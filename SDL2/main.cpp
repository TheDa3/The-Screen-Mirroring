#define ENABLE_AUDIO 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sstream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>

#include <orbis/libkernel.h>
#include <orbis/Sysmodule.h>
#if ENABLE_AUDIO
#include <orbis/AudioOut.h>
#include <orbis/UserService.h>
#ifndef ORBIS_AUDIO_OUT_PORT_TYPE_MAIN
#define ORBIS_AUDIO_OUT_PORT_TYPE_MAIN 0
#endif
#ifndef ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO
#define ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO 1
#endif
#endif
#include <SDL2/SDL.h>

#include "../../_common/log.h"
#include "ui_embedded.h"

// ---------------------------------------------------------------------------
// Thread-safe CACHING allocator for stb_image.
// The JPEG decoder runs on several threads and allocates/frees a few MB per tile for EVERY frame.
// Plain malloc/free hammered from many threads is risky on this libc (heap corruption / fragmentation,
// crash after a few seconds), so freed blocks are kept and reused instead of being returned.
// Pooling is switched on only after startup (g_poolOn) so the startup image decoding is not cached.
// ---------------------------------------------------------------------------
#define POOL_SLOTS 128
struct PoolBlock { void* p; size_t size; bool used; };
static PoolBlock g_pool[POOL_SLOTS];
static pthread_mutex_t g_poolAllocMtx = PTHREAD_MUTEX_INITIALIZER;
static volatile bool g_poolOn = false;

static void* poolMalloc(size_t n)
{
    if (!g_poolOn) return malloc(n);
    pthread_mutex_lock(&g_poolAllocMtx);
    for (int i = 0; i < POOL_SLOTS; i++)
    {
        if (g_pool[i].p && !g_pool[i].used && g_pool[i].size >= n && g_pool[i].size <= n + (n >> 3) + 256)
        {
            g_pool[i].used = true;
            void* r = g_pool[i].p;
            pthread_mutex_unlock(&g_poolAllocMtx);
            return r;
        }
    }
    size_t sz = (n + 255) & ~(size_t)255;
    void* p = malloc(sz);
    if (p)
    {
        for (int i = 0; i < POOL_SLOTS; i++)
            if (!g_pool[i].p) { g_pool[i].p = p; g_pool[i].size = sz; g_pool[i].used = true; break; }
    }
    pthread_mutex_unlock(&g_poolAllocMtx);
    return p;
}

static void poolFree(void* p)
{
    if (!p) return;
    pthread_mutex_lock(&g_poolAllocMtx);
    for (int i = 0; i < POOL_SLOTS; i++)
    {
        if (g_pool[i].p == p) { g_pool[i].used = false; pthread_mutex_unlock(&g_poolAllocMtx); return; }
    }
    pthread_mutex_unlock(&g_poolAllocMtx);
    free(p);                                    // not one of ours (allocated before pooling was on)
}

static void* poolRealloc(void* p, size_t oldsz, size_t newsz)
{
    if (!p) return poolMalloc(newsz);
    void* q = poolMalloc(newsz);
    if (!q) return NULL;
    memcpy(q, p, oldsz < newsz ? oldsz : newsz);
    poolFree(p);
    return q;
}

// stb_image kept private to this file (JPEG for video, PNG for the UI images)
#define STBIDEF static
#define STBI_MALLOC(sz)                 poolMalloc(sz)
#define STBI_REALLOC_SIZED(p,oldsz,newsz) poolRealloc(p,oldsz,newsz)
#define STBI_FREE(p)                    poolFree(p)
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

#define STREAM_PORT     9000
#define DISCOVERY_PORT  9001
#define AUDIO_PORT      9002
#define FRAME_WIDTH     1920
#define FRAME_HEIGHT    1080
#define MAX_W           1920
#define MAX_H           1088
#define MAX_TILES       6
#define MAX_TILE_BYTES  (1024 * 1024)
#define DECODE_WORKERS  4
#define AUDIO_GRAIN     256                  // samples per output call (same as the audio-wav sample)
#define AUDIO_BYTES     4096                 // one PC packet = 1024 frames = 4 grains (16-bit stereo)

// ---------------------------------------------------------------------------
// CONTROLLER BUTTON NUMBERS (SDL joystick indices).  If a button does the wrong thing,
// press it, read the klog line "JOY button N", and change the number here (-1 = disabled).
// The D-Pad is read as a "hat"; the left stick also works.
// ---------------------------------------------------------------------------
#define BTN_CROSS      0
#define BTN_CIRCLE     1
#define BTN_SQUARE     2
#define BTN_TRIANGLE   3
#define BTN_DPAD_UP    -1
#define BTN_DPAD_DOWN  -1
#define BTN_DPAD_LEFT  -1
#define BTN_DPAD_RIGHT -1

// Logging  (several threads log, so every message goes through a mutex: a std::stringstream is not thread-safe)
std::stringstream debugLogStream;
static pthread_mutex_t g_logMtx = PTHREAD_MUTEX_INITIALIZER;
#define TLOG(expr) do { pthread_mutex_lock(&g_logMtx); DEBUGLOG << expr; pthread_mutex_unlock(&g_logMtx); } while (0)

SDL_Window* window;
SDL_Renderer* renderer;
static SDL_Surface* windowSurface = NULL;
static SDL_Joystick* pad = NULL;

// ---------------------------------------------------------------------------
// Pixel format of the window surface (decoded video is converted to it directly)
// ---------------------------------------------------------------------------
static int      g_rs = 16, g_gs = 8, g_bs = 0;
static uint32_t g_amask = 0xFF000000;
static bool     g_fast = false;      // true if the surface is 32-bit

static inline uint32_t packPx(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint32_t)r << g_rs) | ((uint32_t)g << g_gs) | ((uint32_t)b << g_bs) | g_amask;
}

static void copyToSurface(const uint32_t* src, int w, int h)
{
    for (int y = 0; y < h; y++)
        memcpy((uint8_t*)windowSurface->pixels + (size_t)y * windowSurface->pitch, src + (size_t)y * w, (size_t)w * 4);
}

// ---------------------------------------------------------------------------
// Image assets
// ---------------------------------------------------------------------------
struct Img
{
    SDL_Texture* t; int w, h;
    Img() : t(NULL), w(0), h(0) {}
};

// Finds an image that was compiled into the app (see ui_embedded.h)
static bool getAsset(const char* name, const uint8_t** data, int* len)
{
    for (int i = 0; i < g_uiAssetCount; i++)
    {
        if (strcmp(g_uiAssets[i].name, name) == 0)
        {
            *data = g_uiAssets[i].data;
            *len = (int)g_uiAssets[i].len;
            return true;
        }
    }
    return false;
}

static Img loadImg(const char* name)
{
    Img img;
    const uint8_t* buf = NULL; int n = 0;
    if (!getAsset(name, &buf, &n)) { TLOG("MISSING ASSET: " << name); return img; }

    int w, h, c;
    uint8_t* px = stbi_load_from_memory(buf, n, &w, &h, &c, 4);
    if (!px) { TLOG("Bad image: " << name); return img; }

    SDL_Texture* t = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (t)
    {
        SDL_UpdateTexture(t, NULL, px, w * 4);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        img.t = t; img.w = w; img.h = h;
    }
    stbi_image_free(px);
    return img;
}

static uint32_t* g_bgBuf = NULL;        // background, already in window pixel format

static void loadBackground()
{
    const uint8_t* buf = NULL; int n = 0;
    if (!getAsset("bg.jpg", &buf, &n)) { TLOG("MISSING ASSET: bg.jpg"); return; }
    int w, h, c;
    uint8_t* px = stbi_load_from_memory(buf, n, &w, &h, &c, 4);
    if (!px) { TLOG("Bad image: bg.jpg"); return; }
    if (w == FRAME_WIDTH && h == FRAME_HEIGHT && g_fast)
    {
        g_bgBuf = (uint32_t*)malloc((size_t)w * h * 4);
        for (int i = 0; i < w * h; i++) g_bgBuf[i] = packPx(px[i * 4], px[i * 4 + 1], px[i * 4 + 2]);
    }
    else TLOG("Background not used: size " << w << "x" << h << " fast=" << (int)g_fast);
    stbi_image_free(px);
}

static void drawBackground()
{
    if (g_bgBuf) copyToSurface(g_bgBuf, FRAME_WIDTH, FRAME_HEIGHT);
    else
    {
        SDL_SetRenderDrawColor(renderer, 4, 20, 70, 0xFF);
        SDL_RenderClear(renderer);
    }
}

static void drawImg(const Img& i, int x, int y, int alpha = 255)
{
    if (!i.t) return;
    SDL_SetTextureAlphaMod(i.t, (uint8_t)alpha);
    SDL_Rect d = { x, y, i.w, i.h };
    SDL_RenderCopy(renderer, i.t, NULL, &d);
}

static void drawImgC(const Img& i, int cx, int y, int alpha = 255) { drawImg(i, cx - i.w / 2, y, alpha); }

static Img iHeader, iCard[3][2], iHintMenu, iHintBack, iHintStream, iHintOpt, iDigits, iIpPanel, iFpsBg;
static Img iWaitTitle, iConnTitle, iOptTitle, iRow[3][2], iSwOn, iSwOff, iDot, iMsg[2];

static void loadAll()
{
    iHeader = loadImg("header.png");
    const char* kinds[3] = { "pc", "android", "ios" };
    for (int i = 0; i < 3; i++)
        for (int s = 0; s < 2; s++)
        {
            char n[64];
            snprintf(n, sizeof(n), "card_%s%s.png", kinds[i], s ? "_sel" : "");
            iCard[i][s] = loadImg(n);
            snprintf(n, sizeof(n), "row%d%s.png", i, s ? "_sel" : "");
            iRow[i][s] = loadImg(n);
        }
    iHintMenu = loadImg("hint_menu.png");
    iHintBack = loadImg("hint_back.png");
    iHintStream = loadImg("hint_stream.png");
    iHintOpt = loadImg("hint_options.png");
    iDigits = loadImg("digits.png");
    iIpPanel = loadImg("ip_panel.png");
    iFpsBg = loadImg("fps_bg.png");
    iWaitTitle = loadImg("wait_title.png");
    iConnTitle = loadImg("conn_title.png");
    iOptTitle = loadImg("opt_title.png");
    iSwOn = loadImg("sw_on.png");
    iSwOff = loadImg("sw_off.png");
    iDot = loadImg("dot.png");
    iMsg[0] = loadImg("msg_android.png");
    iMsg[1] = loadImg("msg_ios.png");
}

// Digits / dots drawn from the atlas (cells 34x64: "0123456789.:")
static int digitsWidth(const char* s, float scale)
{
    int w = 0;
    for (; *s; s++) w += (int)(((*s == '.' || *s == ':') ? 16 : 34) * scale);
    return w;
}

static void drawDigits(const char* s, int x, int y, float scale)
{
    if (!iDigits.t) return;
    static const char* set = "0123456789.:";
    SDL_SetTextureAlphaMod(iDigits.t, 255);
    for (; *s; s++)
    {
        const char* p = strchr(set, *s);
        if (!p) continue;
        int idx = (int)(p - set);
        bool narrow = (*s == '.' || *s == ':');
        SDL_Rect src = { idx * 34 + (narrow ? 9 : 0), 0, narrow ? 16 : 34, 64 };
        SDL_Rect dst = { x, y, (int)(src.w * scale), (int)(64 * scale) };
        SDL_RenderCopy(renderer, iDigits.t, &src, &dst);
        x += dst.w;
    }
}

static void drawDigitsC(const char* s, int cx, int y, float scale)
{
    drawDigits(s, cx - digitsWidth(s, scale) / 2, y, scale);
}

static void drawSpinner(int cx, int cy, uint32_t now, uint8_t r, uint8_t g, uint8_t b)
{
    if (!iDot.t) return;
    SDL_SetTextureColorMod(iDot.t, r, g, b);
    int active = (now / 70) % 12;
    for (int i = 0; i < 12; i++)
    {
        int age = (active - i + 12) % 12;
        int a = 255 - age * 20;
        if (a < 30) a = 30;
        float ang = i * 6.2831853f / 12.0f - 1.5707963f;
        int x = cx + (int)(cosf(ang) * 62) - iDot.w / 2;
        int y = cy + (int)(sinf(ang) * 62) - iDot.h / 2;
        drawImg(iDot, x, y, a);
    }
    SDL_SetTextureColorMod(iDot.t, 255, 255, 255);
}

// ---------------------------------------------------------------------------
// Input (polled, edge-triggered)
// ---------------------------------------------------------------------------
struct UiInput { bool up, down, left, right, ok, back, opt, tri; };
static UiInput g_in;

static void handleButton(int b)
{
    if (b == BTN_CROSS)      g_in.ok = true;
    else if (b == BTN_CIRCLE)     g_in.back = true;
    else if (b == BTN_SQUARE)     g_in.opt = true;
    else if (b == BTN_TRIANGLE)   g_in.tri = true;
    else if (b == BTN_DPAD_UP)    g_in.up = true;
    else if (b == BTN_DPAD_DOWN)  g_in.down = true;
    else if (b == BTN_DPAD_LEFT)  g_in.left = true;
    else if (b == BTN_DPAD_RIGHT) g_in.right = true;
}

static void pollInput()
{
    if (!pad) return;
    static bool prevBtn[32];
    static int prevHat = 0, prevX = 0, prevY = 0;

    SDL_JoystickUpdate();

    int nb = SDL_JoystickNumButtons(pad);
    if (nb > 32) nb = 32;
    for (int i = 0; i < nb; i++)
    {
        bool down = SDL_JoystickGetButton(pad, i) != 0;
        if (down && !prevBtn[i]) { TLOG("JOY button " << i); handleButton(i); }
        prevBtn[i] = down;
    }

    int hat = (SDL_JoystickNumHats(pad) > 0) ? SDL_JoystickGetHat(pad, 0) : 0;
    int pressed = hat & ~prevHat;
    if (pressed & SDL_HAT_UP)    g_in.up = true;
    if (pressed & SDL_HAT_DOWN)  g_in.down = true;
    if (pressed & SDL_HAT_LEFT)  g_in.left = true;
    if (pressed & SDL_HAT_RIGHT) g_in.right = true;
    prevHat = hat;

    if (SDL_JoystickNumAxes(pad) >= 2)
    {
        int ax = SDL_JoystickGetAxis(pad, 0), ay = SDL_JoystickGetAxis(pad, 1);
        int dx = ax > 16000 ? 1 : (ax < -16000 ? -1 : 0);
        int dy = ay > 16000 ? 1 : (ay < -16000 ? -1 : 0);
        if (dx != prevX) { if (dx == 1) g_in.right = true; if (dx == -1) g_in.left = true; }
        if (dy != prevY) { if (dy == 1) g_in.down = true;  if (dy == -1) g_in.up = true; }
        prevX = dx; prevY = dy;
    }
}

// ---------------------------------------------------------------------------
// Networking (background threads, never touch SDL)
// ---------------------------------------------------------------------------
static int g_tcp = -1, g_udp = -1, g_aud = -1;

static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static uint32_t* g_decodeBuf = NULL, * g_readyBuf = NULL;   // 2 frame buffers (main thread copies from ready under the lock)
static int       g_readyW = 0, g_readyH = 0;
static bool      g_newFrame = false;
static char      g_clientIp[32] = "";

static volatile bool     g_wantConn = false;     // true while the "Waiting for PC" screen is up
static volatile bool     g_streaming = false;    // true while a PC is connected
static volatile bool     g_stopStream = false;   // main thread asks to drop the connection
static volatile bool     g_soundOn = true;
static volatile bool     g_keepAspect = false;   // mirrors the "keep aspect" option (read by the decoder thread)
static volatile bool     g_geomValid = false;    // set to false on every new connection
static volatile uint32_t g_framesDecoded = 0;

static int recvAll(int fd, uint8_t* buf, int n)
{
    int got = 0, idle = 0;
    while (got < n)
    {
        if (g_stopStream) return -2;
        int r = read(fd, buf + got, n - got);
        if (r > 0) { got += r; idle = 0; continue; }
        if (r == 0) return -1;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        {
            if (++idle > 40) return -1;          // ~8 s without data
            continue;
        }
        return -1;
    }
    return 0;
}

// ---- parallel tile decoding ----
// Every frame is always written into the 1920x1080 buffer. If the PC sends a smaller picture (e.g. 1280x720) the
// tiles are scaled up right here, inside the decoder threads, so the main thread can always use the direct copy.
struct TileJob
{
    const uint8_t* data; int len;
    int y, h, fw;
    uint32_t* dst;
    bool ok;
};

static TileJob g_jobs[MAX_TILES];
static int g_njobs = 0, g_nextJob = 0, g_doneJobs = 0;
static pthread_mutex_t g_poolMtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_poolWake = PTHREAD_COND_INITIALIZER, g_poolDone = PTHREAD_COND_INITIALIZER;

// Where the picture goes inside the 1920x1080 buffer (only touched by the decoder thread, read by the workers)
static int      g_srcW = 0, g_srcH = 0;
static int      g_dx0 = 0, g_dy0 = 0, g_dw = FRAME_WIDTH, g_dh = FRAME_HEIGHT;
static bool     g_geomKeep = false;
static uint16_t g_xmap[FRAME_WIDTH];            // destination column -> source column

static void setupGeometry(int fw, int fh)
{
    bool keep = g_keepAspect;
    if (g_geomValid && fw == g_srcW && fh == g_srcH && keep == g_geomKeep) return;

    int dw = FRAME_WIDTH, dh = FRAME_HEIGHT;
    if (keep)
    {
        if ((int64_t)fw * FRAME_HEIGHT > (int64_t)fh * FRAME_WIDTH) dh = (int)((int64_t)fh * FRAME_WIDTH / fw);
        else                                                        dw = (int)((int64_t)fw * FRAME_HEIGHT / fh);
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
    }
    g_dw = dw; g_dh = dh;
    g_dx0 = (FRAME_WIDTH - dw) / 2;
    g_dy0 = (FRAME_HEIGHT - dh) / 2;
    for (int x = 0; x < dw; x++) g_xmap[x] = (uint16_t)((int64_t)x * fw / dw);
    g_srcW = fw; g_srcH = fh; g_geomKeep = keep; g_geomValid = true;

    if (dw != FRAME_WIDTH || dh != FRAME_HEIGHT)    // black bars: clear both buffers once
    {
        uint32_t black = packPx(0, 0, 0);
        pthread_mutex_lock(&g_mtx);
        for (int i = 0; i < FRAME_WIDTH * FRAME_HEIGHT; i++) { g_decodeBuf[i] = black; g_readyBuf[i] = black; }
        pthread_mutex_unlock(&g_mtx);
    }
}

static void decodeTile(TileJob* j)
{
    int w, h, c;
    uint8_t* px = stbi_load_from_memory(j->data, j->len, &w, &h, &c, 3);      // RGB only: less work than RGBA
    if (!px) { j->ok = false; return; }
    if (w != j->fw || h != j->h || w != g_srcW) { stbi_image_free(px); j->ok = false; return; }

    const int rs = g_rs, gs = g_gs, bs = g_bs;                                // locals: keeps the inner loop tight
    const uint32_t am = g_amask;
    const int fh = g_srcH, dw = g_dw, dh = g_dh;

    int dyA = g_dy0 + (int)((int64_t)j->y * dh / fh);
    int dyB = g_dy0 + (int)((int64_t)(j->y + j->h) * dh / fh);
    int prevSy = -1;
    uint32_t* prevRow = NULL;

    for (int dy = dyA; dy < dyB; dy++)
    {
        int sy = (int)((int64_t)(dy - g_dy0) * fh / dh);
        if (sy < j->y) sy = j->y;
        if (sy >= j->y + j->h) sy = j->y + j->h - 1;

        uint32_t* d = j->dst + (size_t)dy * FRAME_WIDTH + g_dx0;
        if (sy == prevSy)
        {
            memcpy(d, prevRow, (size_t)dw * 4);                               // same source row as the line above
        }
        else
        {
            const uint8_t* s = px + (size_t)(sy - j->y) * w * 3;
            if (dw == w)
            {
                for (int x = 0; x < dw; x++)
                    d[x] = ((uint32_t)s[x * 3] << rs) | ((uint32_t)s[x * 3 + 1] << gs) | ((uint32_t)s[x * 3 + 2] << bs) | am;
            }
            else if (dw * 2 == w * 3)                                         // 1280 -> 1920 (the usual case): 2 pixels become 3
            {
                for (int k = 0; k < w / 2; k++)
                {
                    const uint8_t* p = s + k * 6;
                    uint32_t a = ((uint32_t)p[0] << rs) | ((uint32_t)p[1] << gs) | ((uint32_t)p[2] << bs) | am;
                    uint32_t b = ((uint32_t)p[3] << rs) | ((uint32_t)p[4] << gs) | ((uint32_t)p[5] << bs) | am;
                    d[k * 3] = a; d[k * 3 + 1] = a; d[k * 3 + 2] = b;
                }
            }
            else if (dw == w * 2)                                             // 960 -> 1920: every pixel twice
            {
                for (int x = 0; x < w; x++)
                {
                    uint32_t a = ((uint32_t)s[x * 3] << rs) | ((uint32_t)s[x * 3 + 1] << gs) | ((uint32_t)s[x * 3 + 2] << bs) | am;
                    d[x * 2] = a; d[x * 2 + 1] = a;
                }
            }
            else                                                              // any other size
            {
                uint32_t row[FRAME_WIDTH];                                    // convert the source row once ...
                for (int x = 0; x < w; x++)
                    row[x] = ((uint32_t)s[x * 3] << rs) | ((uint32_t)s[x * 3 + 1] << gs) | ((uint32_t)s[x * 3 + 2] << bs) | am;
                for (int x = 0; x < dw; x++) d[x] = row[g_xmap[x]];           // ... then stretch it with plain copies
            }
            prevSy = sy; prevRow = d;
        }
    }
    stbi_image_free(px);
    j->ok = true;
}

static void* poolWorker(void*)
{
    for (;;)
    {
        pthread_mutex_lock(&g_poolMtx);
        while (g_nextJob >= g_njobs) pthread_cond_wait(&g_poolWake, &g_poolMtx);
        int idx = g_nextJob++;
        pthread_mutex_unlock(&g_poolMtx);

        decodeTile(&g_jobs[idx]);

        pthread_mutex_lock(&g_poolMtx);
        if (++g_doneJobs >= g_njobs) pthread_cond_signal(&g_poolDone);
        pthread_mutex_unlock(&g_poolMtx);
    }
    return NULL;
}

static void runJobs(int n)
{
    pthread_mutex_lock(&g_poolMtx);
    g_njobs = n; g_nextJob = 0; g_doneJobs = 0;
    pthread_cond_broadcast(&g_poolWake);
    while (g_doneJobs < n) pthread_cond_wait(&g_poolDone, &g_poolMtx);
    g_njobs = 0; g_nextJob = 0;
    pthread_mutex_unlock(&g_poolMtx);
}

// ---- receive / decode pipeline ----
// The network thread fills one FrameSet while the decoder thread decodes the other one, so the time spent
// waiting for the network and the time spent decoding overlap instead of adding up.
struct FrameSet
{
    uint8_t* buf[MAX_TILES];
    int len[MAX_TILES], ty[MAX_TILES], th[MAX_TILES];
    int nt, fw, fh;
    bool busy;                       // handed to the decoder and not finished yet
};

static FrameSet g_sets[2];
static FrameSet* g_box = NULL;       // frame waiting for the decoder
static pthread_mutex_t g_boxMtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_boxCv = PTHREAD_COND_INITIALIZER;

static void decodeFrame(FrameSet* fs)
{
    setupGeometry(fs->fw, fs->fh);
    int nt = fs->nt;
    for (int i = 0; i < nt; i++)
    {
        g_jobs[i].data = fs->buf[i]; g_jobs[i].len = fs->len[i];
        g_jobs[i].y = fs->ty[i];     g_jobs[i].h = fs->th[i];
        g_jobs[i].fw = fs->fw;       g_jobs[i].dst = g_decodeBuf; g_jobs[i].ok = false;
    }
    runJobs(nt);

    for (int i = 0; i < nt; i++) if (!g_jobs[i].ok) return;                   // corrupt frame: drop it

    pthread_mutex_lock(&g_mtx);
    uint32_t* t = g_decodeBuf; g_decodeBuf = g_readyBuf; g_readyBuf = t;       // publish
    g_readyW = FRAME_WIDTH; g_readyH = FRAME_HEIGHT; g_newFrame = true;
    pthread_mutex_unlock(&g_mtx);
    g_framesDecoded++;
    if ((g_framesDecoded % 300) == 0) TLOG("frames decoded: " << (unsigned)g_framesDecoded);
}

static void* decoderThread(void*)
{
    for (;;)
    {
        pthread_mutex_lock(&g_boxMtx);
        while (!g_box) pthread_cond_wait(&g_boxCv, &g_boxMtx);
        FrameSet* fs = g_box;
        g_box = NULL;
        pthread_cond_broadcast(&g_boxCv);                                      // network thread may hand over the next one
        pthread_mutex_unlock(&g_boxMtx);

        decodeFrame(fs);

        pthread_mutex_lock(&g_boxMtx);
        fs->busy = false;
        pthread_cond_broadcast(&g_boxCv);
        pthread_mutex_unlock(&g_boxMtx);
    }
    return NULL;
}

static void* tcpThread(void*)
{
    for (;;)
    {
        struct sockaddr_in ca;
        socklen_t cl = sizeof(ca);
        int fd = accept(g_tcp, (struct sockaddr*)&ca, &cl);
        if (fd < 0) { usleep(100000); continue; }
        if (!g_wantConn) { close(fd); continue; }

        struct timeval tv;
        tv.tv_sec = 0; tv.tv_usec = 200000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        // make sure the decoder is idle before a new connection starts
        pthread_mutex_lock(&g_boxMtx);
        while (g_box || g_sets[0].busy || g_sets[1].busy) pthread_cond_wait(&g_boxCv, &g_boxMtx);
        pthread_mutex_unlock(&g_boxMtx);
        g_geomValid = false;

        pthread_mutex_lock(&g_mtx);
        snprintf(g_clientIp, sizeof(g_clientIp), "%s", inet_ntoa(ca.sin_addr));
        g_newFrame = false;
        pthread_mutex_unlock(&g_mtx);

        g_stopStream = false;
        g_streaming = true;
        TLOG("PC connected: " << g_clientIp);

        int cur = 0;
        for (;;)
        {
            uint8_t hdr[5];
            if (recvAll(fd, hdr, 5) < 0) break;
            int nt = hdr[0];
            int fw = (hdr[1] << 8) | hdr[2];
            int fh = (hdr[3] << 8) | hdr[4];
            if (nt == 0) continue;                                   // keep-alive
            if (nt > MAX_TILES || fw <= 0 || fh <= 0 || fw > MAX_W || fh > MAX_H) break;

            FrameSet* fs = &g_sets[cur];
            pthread_mutex_lock(&g_boxMtx);                           // wait until the decoder is done with this set
            while (fs->busy) pthread_cond_wait(&g_boxCv, &g_boxMtx);
            pthread_mutex_unlock(&g_boxMtx);

            bool bad = false;
            for (int i = 0; i < nt; i++)
            {
                uint8_t th[8];
                if (recvAll(fd, th, 8) < 0) { bad = true; break; }
                int ty = (th[0] << 8) | th[1];
                int tH = (th[2] << 8) | th[3];
                uint32_t len = ((uint32_t)th[4] << 24) | ((uint32_t)th[5] << 16) | ((uint32_t)th[6] << 8) | th[7];
                if (len == 0 || len > MAX_TILE_BYTES || ty + tH > fh) { bad = true; break; }
                if (recvAll(fd, fs->buf[i], (int)len) < 0) { bad = true; break; }
                fs->len[i] = (int)len; fs->ty[i] = ty; fs->th[i] = tH;
            }
            if (bad) break;
            fs->nt = nt; fs->fw = fw; fs->fh = fh;

            pthread_mutex_lock(&g_boxMtx);                           // hand it to the decoder, keep receiving
            fs->busy = true;
            while (g_box) pthread_cond_wait(&g_boxCv, &g_boxMtx);
            g_box = fs;
            pthread_cond_broadcast(&g_boxCv);
            pthread_mutex_unlock(&g_boxMtx);
            cur ^= 1;
        }

        close(fd);
        g_streaming = false;
        TLOG("PC disconnected");
    }
    return NULL;
}

// Answers "PS4MIRROR_DISCOVER" broadcasts with "PS4MIRROR_HERE|videoPort|name|audioPort"
static void* udpThread(void*)
{
    char buf[128];
    for (;;)
    {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(g_udp, buf, sizeof(buf) - 1, 0, (struct sockaddr*)&from, &fl);
        if (n <= 0) { usleep(50000); continue; }
        buf[n] = 0;
        if (!g_wantConn || g_streaming) continue;
        if (strncmp(buf, "PS4MIRROR_DISCOVER", 18) != 0) continue;

        char rep[96];
        snprintf(rep, sizeof(rep), "PS4MIRROR_HERE|%d|PS4|%d", STREAM_PORT, AUDIO_PORT);
        sendto(g_udp, rep, strlen(rep), 0, (struct sockaddr*)&from, fl);
        TLOG("Answered discovery from " << inet_ntoa(from.sin_addr));
    }
    return NULL;
}

#if ENABLE_AUDIO
// Receives raw PCM from the PC and plays it with sceAudioOut (set up exactly like samples/audio-wav)
static void* audioThread(void*)
{
    sceUserServiceInitialize(NULL);                    // may already be initialized by SDL: ignore the result
    int rc = sceAudioOutInit();
    if (rc != 0) { TLOG("sceAudioOutInit failed: " << rc); return NULL; }

    int h = sceAudioOutOpen(ORBIS_USER_SERVICE_USER_ID_SYSTEM, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0,
        AUDIO_GRAIN, 48000, ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    if (h <= 0) { TLOG("sceAudioOutOpen failed: " << h); return NULL; }
    TLOG("Audio output ready");

    static uint8_t buf[AUDIO_BYTES] __attribute__((aligned(256)));
    for (;;)
    {
        struct sockaddr_in ca;
        socklen_t cl = sizeof(ca);
        int fd = accept(g_aud, (struct sockaddr*)&ca, &cl);
        if (fd < 0) { usleep(100000); continue; }
        if (!g_wantConn && !g_streaming) { close(fd); continue; }

        struct timeval tv;
        tv.tv_sec = 0; tv.tv_usec = 200000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        TLOG("Audio connected");

        while (recvAll(fd, buf, AUDIO_BYTES) == 0)
        {
            if (!g_soundOn) continue;
            for (int k = 0; k < AUDIO_BYTES / (AUDIO_GRAIN * 4); k++)
                if (sceAudioOutOutput(h, buf + k * AUDIO_GRAIN * 4) < 0) break;   // blocks one grain: real-time pacing
        }
        close(fd);
        TLOG("Audio disconnected");
    }
    return NULL;
}
#endif

static bool netInit()
{
    int one = 1;
    struct sockaddr_in a;

    g_tcp = socket(AF_INET, SOCK_STREAM, 0);
    if (g_tcp < 0) { TLOG("TCP socket failed: " << strerror(errno)); return false; }
    setsockopt(g_tcp, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons(STREAM_PORT);
    if (bind(g_tcp, (struct sockaddr*)&a, sizeof(a)) != 0) { TLOG("TCP bind failed: " << strerror(errno)); return false; }
    if (listen(g_tcp, 1) != 0) { TLOG("TCP listen failed: " << strerror(errno)); return false; }

    g_aud = socket(AF_INET, SOCK_STREAM, 0);
    if (g_aud >= 0)
    {
        setsockopt(g_aud, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons(AUDIO_PORT);
        if (bind(g_aud, (struct sockaddr*)&a, sizeof(a)) != 0 || listen(g_aud, 1) != 0)
        {
            TLOG("Audio socket setup failed: " << strerror(errno)); g_aud = -1;
        }
    }

    g_udp = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_udp < 0) { TLOG("UDP socket failed: " << strerror(errno)); return false; }
    setsockopt(g_udp, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons(DISCOVERY_PORT);
    if (bind(g_udp, (struct sockaddr*)&a, sizeof(a)) != 0) { TLOG("UDP bind failed: " << strerror(errno)); return false; }

    // frame buffers + tile buffers (checked: a failed malloc must not crash later)
    size_t bytes = (size_t)MAX_W * MAX_H * 4;
    g_decodeBuf = (uint32_t*)malloc(bytes);
    g_readyBuf = (uint32_t*)malloc(bytes);
    if (!g_decodeBuf || !g_readyBuf) { TLOG("OUT OF MEMORY (frame buffers)"); return false; }
    for (int s = 0; s < 2; s++)
        for (int i = 0; i < MAX_TILES; i++)
        {
            g_sets[s].buf[i] = (uint8_t*)malloc(MAX_TILE_BYTES);
            if (!g_sets[s].buf[i]) { TLOG("OUT OF MEMORY (tile buffer " << s << "/" << i << ")"); return false; }
        }
    TLOG("Buffers allocated");

    g_poolOn = true;                      // from here on stb_image uses the caching allocator

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1024 * 1024);

    pthread_t t;
    for (int i = 0; i < DECODE_WORKERS; i++) pthread_create(&t, &attr, poolWorker, NULL);
    pthread_create(&t, &attr, decoderThread, NULL);
    pthread_create(&t, &attr, tcpThread, NULL);
    pthread_create(&t, &attr, udpThread, NULL);
#if ENABLE_AUDIO
    if (g_aud >= 0) pthread_create(&t, &attr, audioThread, NULL);
#endif
    return true;
}

// Finds this PS4's LAN IP (connecting a UDP socket sends nothing, it only picks the route)
static void detectLocalIp(char* out, size_t n)
{
    snprintf(out, n, "unknown");
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_port = htons(53); a.sin_addr.s_addr = inet_addr("8.8.8.8");
    if (connect(s, (struct sockaddr*)&a, sizeof(a)) == 0)
    {
        struct sockaddr_in me;
        socklen_t l = sizeof(me);
        if (getsockname(s, (struct sockaddr*)&me, &l) == 0) snprintf(out, n, "%s", inet_ntoa(me.sin_addr));
    }
    close(s);
}

// ---------------------------------------------------------------------------
// Screens
// ---------------------------------------------------------------------------
enum UiState { ST_MENU, ST_OPTIONS, ST_WAIT, ST_STREAM };

static UiState state = ST_MENU;
static int  menuSel = 0, optSel = 0;
static bool optShowFps = false, optKeepAspect = false, optSound = true;
static int  devMsg = -1;               // 0 = android, 1 = ios
static uint32_t devMsgStart = 0;
static char localIp[32] = "unknown";
static char clientIp[32] = "";

static const int CARD_CX[3] = { 584, 960, 1336 };
#define CARD_TOP 429
#define CARD_PAD 70

static void drawMenu(uint32_t now)
{
    drawBackground();
    drawImg(iHeader, 510, 100);
    for (int i = 0; i < 3; i++)
    {
        const Img& c = iCard[i][i == menuSel ? 1 : 0];
        drawImg(c, CARD_CX[i] - c.w / 2, CARD_TOP - CARD_PAD);
    }
    if (devMsg >= 0)
    {
        uint32_t t = now - devMsgStart;
        if (t > 2800) devMsg = -1;
        else
        {
            int a = 255;
            if (t < 200) a = (int)(t * 255 / 200);
            else if (t > 2300) a = (int)((2800 - t) * 255 / 500);
            drawImgC(iMsg[devMsg], FRAME_WIDTH / 2, 790, a);
        }
    }
    drawImgC(iHintMenu, FRAME_WIDTH / 2, 958 - iHintMenu.h / 2);
}

static void drawOptions()
{
    drawBackground();
    drawImg(iOptTitle, 510, 90);
    bool vals[3] = { optShowFps, optKeepAspect, optSound };
    for (int i = 0; i < 3; i++)
    {
        int y = 330 + i * 150;
        drawImg(iRow[i][i == optSel ? 1 : 0], 510 - 40, y - 40);
        drawImg(vals[i] ? iSwOn : iSwOff, 510 + 900 - 112 - 36, y + (120 - 56) / 2);
    }
    drawImgC(iHintOpt, FRAME_WIDTH / 2, 958 - iHintOpt.h / 2);
}

static void drawWait(uint32_t now)
{
    drawBackground();
    drawSpinner(FRAME_WIDTH / 2, 255, now, 120, 190, 255);
    drawImg(iWaitTitle, 260, 350);
    drawImg(iIpPanel, 610, 640);
    drawDigitsC(localIp, FRAME_WIDTH / 2, 640 + 52, 1.0f);
    drawImgC(iHintBack, FRAME_WIDTH / 2, 958 - iHintBack.h / 2);
}

static void drawConnecting(uint32_t now)
{
    drawBackground();
    drawSpinner(FRAME_WIDTH / 2, 290, now, 90, 230, 170);
    drawImg(iConnTitle, 260, 385);
    drawDigitsC(clientIp, FRAME_WIDTH / 2, 560, 0.85f);
    drawImgC(iHintStream, FRAME_WIDTH / 2, 958 - iHintStream.h / 2);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char* args[])
{
    int rc;
    setvbuf(stdout, NULL, _IONBF, 0);

    TLOG("Initializing SDL");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) != 0)
    {
        TLOG("Failed to initialize SDL: " << SDL_GetError());
        for (;;);
    }

    // The project links FreeType; the module must be loaded or it crashes on unresolved NIDs
    rc = sceSysmoduleLoadModule(0x009A);
    if (rc < 0) { TLOG("Failed to load freetype module"); for (;;); }

    window = SDL_CreateWindow("main", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, FRAME_WIDTH, FRAME_HEIGHT, 0);
    if (!window) { TLOG("Failed to create window: " << SDL_GetError()); for (;;); }

    windowSurface = SDL_GetWindowSurface(window);
    renderer = SDL_CreateSoftwareRenderer(windowSurface);
    if (!renderer) { TLOG("Failed to create software renderer: " << SDL_GetError()); for (;;); }

    SDL_PixelFormat* pf = windowSurface->format;
    g_rs = pf->Rshift; g_gs = pf->Gshift; g_bs = pf->Bshift; g_amask = pf->Amask;
    g_fast = (pf->BytesPerPixel == 4);
    TLOG("Surface " << windowSurface->w << "x" << windowSurface->h << " bpp " << (int)pf->BytesPerPixel);

    loadBackground();
    loadAll();
    TLOG("Assets loaded");

    if (!netInit()) TLOG("Network setup failed");
    TLOG("Network ready");

    SDL_Texture* tex = NULL;
    int texW = 0, texH = 0;
    bool haveFrame = false;
    uint32_t lastFpsTick = 0, lastFpsCount = 0, fpsValue = 0;
    uint32_t lastPadTry = 0;
    bool running = true;

    while (running)
    {
        uint32_t now = SDL_GetTicks();

        if (!pad && now - lastPadTry > 1000)
        {
            lastPadTry = now;
            if (SDL_NumJoysticks() > 0) pad = SDL_JoystickOpen(0);
        }
        pollInput();
        UiInput in = g_in;
        memset(&g_in, 0, sizeof(g_in));

        bool present = true;

        switch (state)
        {
        case ST_MENU:
            if (in.left && menuSel > 0) menuSel--;
            if (in.right && menuSel < 2) menuSel++;
            if (in.opt) { state = ST_OPTIONS; optSel = 0; }
            if (in.back) running = false;
            if (in.ok)
            {
                if (menuSel == 0)
                {
                    detectLocalIp(localIp, sizeof(localIp));
                    g_wantConn = true;
                    state = ST_WAIT;
                }
                else { devMsg = menuSel - 1; devMsgStart = now; }
            }
            drawMenu(now);
            break;

        case ST_OPTIONS:
            if (in.up && optSel > 0) optSel--;
            if (in.down && optSel < 2) optSel++;
            if (in.ok)
            {
                if (optSel == 0) optShowFps = !optShowFps;
                else if (optSel == 1) { optKeepAspect = !optKeepAspect; g_keepAspect = optKeepAspect; }
                else { optSound = !optSound; g_soundOn = optSound; }
            }
            if (in.back || in.opt) state = ST_MENU;
            drawOptions();
            break;

        case ST_WAIT:
            if (in.back) { g_wantConn = false; state = ST_MENU; drawMenu(now); break; }
            if (g_streaming)
            {
                g_wantConn = false;
                pthread_mutex_lock(&g_mtx);
                snprintf(clientIp, sizeof(clientIp), "%s", g_clientIp);
                g_newFrame = false;
                pthread_mutex_unlock(&g_mtx);
                haveFrame = false;
                lastFpsTick = now; lastFpsCount = g_framesDecoded; fpsValue = 0;
                state = ST_STREAM;
                drawConnecting(now);
                break;
            }
            drawWait(now);
            break;

        case ST_STREAM:
        {
            if (in.back)
            {
                g_stopStream = true;
                haveFrame = false;
                state = ST_MENU;
                drawMenu(now);
                break;
            }
            if (!g_streaming)
            {
                detectLocalIp(localIp, sizeof(localIp));
                g_wantConn = true;
                haveFrame = false;
                state = ST_WAIT;
                drawWait(now);
                break;
            }
            if (in.tri) optShowFps = !optShowFps;

            bool got = false;
            int pw = 0, ph = 0;
            pthread_mutex_lock(&g_mtx);
            if (g_newFrame)
            {
                pw = g_readyW; ph = g_readyH; g_newFrame = false; got = true;
                if (g_fast && pw == FRAME_WIDTH && ph == FRAME_HEIGHT && windowSurface->w >= pw)
                {
                    copyToSurface(g_readyBuf, pw, ph);                  // 1:1, fastest path
                }
                else
                {
                    if (!tex || pw != texW || ph != texH)
                    {
                        if (tex) SDL_DestroyTexture(tex);
                        tex = SDL_CreateTexture(renderer, windowSurface->format->format, SDL_TEXTUREACCESS_STREAMING, pw, ph);
                        texW = pw; texH = ph;
                    }
                    if (tex) SDL_UpdateTexture(tex, NULL, g_readyBuf, pw * 4);
                }
            }
            pthread_mutex_unlock(&g_mtx);

            if (!got && !haveFrame) { drawConnecting(now); break; }
            if (!got) { present = false; SDL_Delay(1); break; }

            haveFrame = true;
            bool native = (g_fast && pw == FRAME_WIDTH && ph == FRAME_HEIGHT && windowSurface->w >= pw);
            if (!native && tex)
            {
                if (optKeepAspect)
                {
                    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0xFF);
                    SDL_RenderClear(renderer);
                    float sc = (float)FRAME_WIDTH / pw, sc2 = (float)FRAME_HEIGHT / ph;
                    if (sc2 < sc) sc = sc2;
                    int dw = (int)(pw * sc), dh = (int)(ph * sc);
                    SDL_Rect dst = { (FRAME_WIDTH - dw) / 2, (FRAME_HEIGHT - dh) / 2, dw, dh };
                    SDL_RenderCopy(renderer, tex, NULL, &dst);
                }
                else SDL_RenderCopy(renderer, tex, NULL, NULL);
            }

            if (optShowFps)
            {
                if (now - lastFpsTick >= 1000)
                {
                    uint32_t c = g_framesDecoded;
                    fpsValue = c - lastFpsCount;
                    lastFpsCount = c; lastFpsTick = now;
                }
                char t[16];
                snprintf(t, sizeof(t), "%u", (unsigned)fpsValue);
                drawImg(iFpsBg, 24, 24);
                drawDigits(t, 24 + 84, 24 + 9, 0.62f);
            }
            break;
        }
        }

        if (present) SDL_UpdateWindowSurface(window);
        if (state != ST_STREAM) SDL_Delay(16);
    }

    SDL_Quit();
    return 0;
}