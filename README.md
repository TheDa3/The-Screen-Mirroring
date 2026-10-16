# PS4 Screen Mirror

Mirror your Windows PC screen **and sound** to your PS4 over your home network.

PS4 Screen Mirror is made of two small programs that work together:

- **PS4 app** (`.pkg`): a homebrew receiver that shows your PC screen full screen on the PS4 and plays your PC audio.
- **PC app** (`.exe`): a Windows sender that captures your screen and system sound and streams them to the PS4.

Everything stays on your local network. No account, no cloud, no internet connection is needed.

## Downloads

Ready-to-use files are in the **[Releases](../../releases)** section:

| File | For | Description |
|------|-----|-------------|
| `*.pkg` | PS4 | The receiver app |
| `*.exe` | Windows PC | The sender app (single file, no installation) |

## Features

- **Screen mirroring** of your whole PC desktop to the PS4, full screen at 1920x1080
- **PC sound streaming** (what you hear on your PC plays on the PS4), 48 kHz 16-bit stereo
- **Automatic PS4 discovery**: the PC finds the PS4 on the network by itself, no IP typing required (manual IP is optional)
- **Any PC resolution**: the picture is scaled to the PS4's 1080p output on the PS4 side, with an option to keep the original aspect ratio (black bars)
- **Adjustable quality, frame rate and resolution** from the PC app (960 / 1280 / 1920 px wide and more)
- **Adaptive quality**: the PC lowers the image quality automatically if the network cannot keep up, and raises it again when it can
- **Live statistics** in the PC app: FPS, bandwidth (Mbps), size per frame and current quality
- **Controller friendly UI** on the PS4, with an on-screen FPS counter and options for sound and aspect ratio
- **Fast screen capture** on the PC with GPU capture (DXGI), with an automatic fallback to a slower capture method

## Requirements

**PS4**
- A PS4 that can run homebrew `.pkg` apps
- Connected to the same local network as the PC (Ethernet strongly recommended)

**PC**
- Windows 10 or 11
- Connected to the same local network as the PS4 (wired connection recommended)
- A free network port range: `9000` (TCP), `9001` (UDP) and `9002` (TCP) must not be blocked by a firewall

## How it works

```
 PC (Windows)                                   PS4
 ┌────────────────────────┐                     ┌─────────────────────────────┐
 │ Screen capture (DXGI)  │                     │ Network thread (receive)    │
 │   -> resize            │   TCP 9000 (video)  │   -> 4 decoder threads      │
 │   -> 4 JPEG strips     │ ──────────────────► │   -> scale to 1920x1080     │
 │      (parallel encode) │                     │   -> direct copy to screen  │
 │                        │                     │                             │
 │ WASAPI loopback audio  │   TCP 9002 (audio)  │ Audio output (48 kHz, S16)  │
 │   -> raw PCM           │ ──────────────────► │                             │
 │                        │                     │                             │
 │ Discovery broadcast    │ ◄─────────────────► │ Discovery responder         │
 └────────────────────────┘   UDP 9001          └─────────────────────────────┘
```

- **Video.** Each frame is split into 4 horizontal JPEG strips. The PC encodes them in parallel and the PS4 decodes them in parallel on separate CPU threads. The PS4 receives the next frame while it decodes the current one, and scales the picture to 1080p while decoding, so the final copy to the screen is a single fast memory copy.
- **Audio.** The PC captures its own output with WASAPI loopback and sends raw 48 kHz 16-bit stereo PCM. The PS4 plays it with its audio output API.
- **Discovery.** The PC broadcasts a short UDP message and the PS4 app answers with its video and audio ports.
- **Adaptive quality.** The sender measures how long each frame takes to send. If the link is slow, JPEG quality is reduced step by step (down to a minimum); when there is room again, it goes back up to the quality you selected.

### Network protocol

All numbers are big-endian.

| Purpose | Transport | Port |
|---------|-----------|------|
| Video | TCP | 9000 |
| Discovery | UDP | 9001 |
| Audio | TCP | 9002 |

**Video packet**

```
[u8 tile count][u16 frame width][u16 frame height]
  then, for every tile:
  [u16 y][u16 tile height][u32 JPEG length][JPEG bytes]
```

A packet with a tile count of `0` is a keep-alive.

**Discovery**

- PC to network (UDP broadcast): `PS4MIRROR_DISCOVER`
- PS4 reply: `PS4MIRROR_HERE|<video port>|<name>|<audio port>`

**Audio**

Raw interleaved signed 16-bit little-endian stereo PCM at 48 kHz, sent in packets of 1024 frames (4096 bytes).

## Performance notes

Frame rate depends mostly on three things:

1. **Network quality.** A wired connection on both devices gives the most stable result. Wi-Fi works, but throughput and latency vary, and some PS4 models only support the 2.4 GHz band.
2. **Resolution and quality.** Higher resolution and quality mean larger frames: more bandwidth to send and more CPU time on the PS4 to decode. Lower values give a higher frame rate.
3. **PC load.** Capturing and JPEG-encoding the screen uses PC CPU time, which competes with games or other heavy programs.

## Limitations

- Windows PC only for the sender
- The video is JPEG-based, so it is not lossless and is not meant for competitive, latency-critical gaming
- The stream is **not encrypted or authenticated**. Anyone on the same local network who can reach the PS4 ports could send it video. Use it only on a network you trust
- Mirrors the whole desktop, not a single window

## Built with

- [OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain), the PS4 homebrew toolchain
- [SDL2](https://www.libsdl.org/), display and controller input on the PS4
- [stb_image](https://github.com/nothings/stb), JPEG decoding on the PS4
- [Python](https://www.python.org/), [Pillow](https://python-pillow.org/), [NumPy](https://numpy.org/), [dxcam](https://github.com/ra1nty/DXcam), [mss](https://github.com/BoboTiG/python-mss) and [PyAudioWPatch](https://github.com/s0d3s/PyAudioWPatch) for the PC app

## Disclaimer

This is an independent, community homebrew project. It is not affiliated with, endorsed by, or sponsored by Sony Interactive Entertainment. "PlayStation" and "PS4" are trademarks of Sony Interactive Entertainment Inc. Use it at your own risk and only on hardware you own.

## License

This project is licensed under the MIT License — see the [LICENSE](LICENSE) file for details.
