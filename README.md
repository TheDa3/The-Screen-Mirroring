# PS4 Screen Mirror

Mirror your **Windows PC or Android device screen to your PS4** over your home network.

PS4 Screen Mirror turns your PS4 into a screen-mirroring receiver, letting you display content from supported devices on your TV.

The project consists of a PS4 homebrew receiver and sender applications for supported platforms:

- **PS4 app** (`.pkg`): Receives the stream and displays it full-screen on your PS4, with support for PC audio.
- **Windows PC app** (`.exe`): Captures your desktop and system audio and streams them to your PS4.
- **Android app** (`.apk`): Mirrors your Android screen to your PS4.
- **iOS support coming soon:** Support for iPhone and iPad is planned for a future release.

Everything is designed to work over your local network, without requiring a cloud service or an online account.

## Downloads

Ready-to-use files are available in the **[Releases](../../releases)** section.

| File | Platform | Description |
|---|---|---|
| `*.pkg` | PS4 | Homebrew receiver app |
| `*.exe` | Windows | Sender application; no installation required |
| `*.apk` | Android | Sender application for Android devices |

* iOS support is coming soon.

## Features

### Screen Mirroring

- **Windows PC mirroring** — Mirror your entire desktop to your PS4.
- **Android mirroring** — Stream your Android device's screen to your PS4.
- **iOS support coming soon** — Planned support for iPhones and iPads.
- **Full-screen output** — Display mirrored content at up to 1920×1080.
- **Adjustable quality** — Configure resolution, frame rate and image quality in the PC app.
- **Aspect ratio options** — Fit the image to your screen or preserve its original aspect ratio with black bars.
- **Adaptive quality** — The PC sender adjusts JPEG quality according to network performance.

### Control Your Device Using Your PS4

The project aims to let you interact with your connected device using your PS4 controller, making your PS4 more than just a screen-mirroring receiver.

Depending on the platform and supported implementation, this could allow you to navigate your device and control compatible applications directly from your couch.

Device-control capabilities may vary by platform and are not guaranteed for every device or application.

### Audio and Performance

- **PC system audio streaming** — Play your Windows PC's audio through your PS4 at 48 kHz, 16-bit stereo.
- **Automatic PS4 discovery** — The PC sender can find your PS4 on the local network without requiring manual IP entry.
- **Live performance statistics** — Monitor FPS, bandwidth, frame size and current quality in the PC app.
- **Optimized screen capture** — Uses GPU-based DXGI capture when available, with a fallback capture method.
- **Parallel video processing** — The PC sender encodes image strips in parallel, while the PS4 decodes them using multiple threads.
- **Controller-friendly interface** — Navigate the PS4 receiver using a compatible controller.

## Requirements

### PS4

- A PS4 capable of running homebrew `.pkg` applications.
- Connected to the same local network as the sending device.
- Ethernet is recommended for the most stable streaming experience.

### Windows PC

- Windows 10 or Windows 11.
- Connected to the same local network as the PS4.
- The following network ports must be allowed through the firewall:
  - TCP `9000` — Video
  - UDP `9001` — Device discovery
  - TCP `9002` — Audio

### Android

- An Android device compatible with the sender application.
- Connected to the same local network as the PS4.
- Any required Android permissions must be granted.

### iOS

- **Coming soon.** Compatibility and setup requirements will be announced when iOS support becomes available.

## How It Works

### Windows PC to PS4

```text
Windows PC                                      PS4
┌─────────────────────────┐                     ┌────────────────────────────┐
│ Screen capture (DXGI)   │                     │ Network receiver           │
│   -> Resize             │   TCP 9000 (video)  │   -> Parallel decoding     │
│   -> Split into strips  │ ──────────────────► │   -> Scale to 1080p        │
│   -> Parallel JPEG      │                     │   -> Display on screen     │
│                         │                     │                            │
│ WASAPI loopback audio   │   TCP 9002 (audio)  │ Audio output               │
│   -> Raw PCM            │ ──────────────────► │   -> 48 kHz stereo         │
│                         │                     │                            │
│ UDP discovery broadcast│ ◄─────────────────► │ Discovery responder        │
└─────────────────────────┘   UDP 9001          └────────────────────────────┘
```

### Android to PS4

The Android sender captures the device's screen and transmits image data to the PS4 receiver over the local network.

The exact capture method, transport protocol, audio support and control capabilities depend on the Android sender implementation.

### iOS to PS4

iOS support is planned for a future release. Further details about screen capture, audio streaming and device control will be provided when development progresses.

## Windows PC Streaming Details

### Video Processing

Each PC video frame is divided into four horizontal JPEG strips. The PC encodes the strips in parallel, and the PS4 decodes them using separate CPU threads.

The receiver can process incoming frames while decoding previously received data. Scaling is performed during decoding to reduce the work required to display the final image.

### Audio Streaming

The Windows sender captures system output using WASAPI loopback and sends raw 48 kHz, 16-bit stereo PCM audio to the PS4.

### Device Discovery

The PC broadcasts a UDP discovery message. The PS4 receiver responds with its connection information, allowing the PC app to locate the console without requiring manual IP entry.

### Adaptive Quality

The sender monitors transmission performance and adjusts JPEG quality to help maintain a stable stream. Quality decreases when the network cannot keep up and can increase again when sufficient bandwidth becomes available.

## Network Protocol

The following protocol details apply to the existing Windows PC sender and PS4 receiver.

All video packet header numbers are big-endian unless otherwise specified.

| Purpose | Transport | Port |
|---|---|---|
| Video | TCP | `9000` |
| Discovery | UDP | `9001` |
| Audio | TCP | `9002` |

### Video Packet

```text
[u8 tile count][u16 frame width][u16 frame height]

For each tile:
[u16 y][u16 tile height][u32 JPEG length][JPEG bytes]
```

A packet with a tile count of `0` is used as a keep-alive.

### Discovery Messages

- PC broadcast: `PS4MIRROR_DISCOVER`
- PS4 response: `PS4MIRROR_HERE|<video port>|<name>|<audio port>`

### Audio Format

Raw interleaved, signed 16-bit little-endian stereo PCM at 48 kHz, transmitted in packets containing 1024 audio frames (4096 bytes).

**Note:** These protocol details describe the Windows sender. They should not be assumed to describe the Android or future iOS senders unless they use the same protocol.

## Performance Notes

Streaming performance depends primarily on the following factors:

1. **Network quality:** Ethernet generally provides more consistent throughput and latency than Wi-Fi.
2. **Resolution and image quality:** Higher settings increase bandwidth requirements and decoding workload.
3. **Sender performance:** Screen capture and image encoding consume CPU and GPU resources.
4. **Receiver performance:** The PS4 must decode incoming frames quickly enough to keep the display smooth.
5. **Platform limitations:** Capture performance, audio support and background behavior vary across Windows, Android and iOS.

For the best experience, connect the PS4 and sending device to the same router, preferably using Ethernet wherever possible.

## Limitations and Security

- The Windows sender requires Windows 10 or Windows 11.
- Video uses JPEG compression and is not lossless.
- The project is intended for screen mirroring rather than competitive gaming requiring extremely low latency.
- The Windows PC sender mirrors the entire desktop rather than a single application window.
- Android and future iOS capabilities depend on the respective sender implementations and operating-system restrictions.
- Device control depends on platform support and may not work with every application.
- The existing PC-to-PS4 stream is **not encrypted or authenticated**. Devices on the same network that can access the relevant ports may be able to send data to the receiver.
- Use the application only on networks you trust.

## Built With

### PS4 Receiver

- [OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) — PS4 homebrew development.
- [SDL2](https://www.libsdl.org/) — Display and controller input.
- [stb_image](https://github.com/nothings/stb) — JPEG decoding.

### Windows PC Sender

- [Python](https://www.python.org/)
- [Pillow](https://python-pillow.org/)
- [NumPy](https://numpy.org/)
- [DXcam](https://github.com/ra1nty/DXcam)
- [mss](https://github.com/BoboTiG/python-mss)
- [PyAudioWPatch](https://github.com/s0d3s/PyAudioWPatch)

## Contributing

Contributions, bug reports and performance improvements are welcome. Feel free to explore the source code, suggest optimizations or help extend support to additional platforms.

## Disclaimer

This is an independent community homebrew project. It is not affiliated with, endorsed by or sponsored by Sony Interactive Entertainment. PlayStation and PS4 are trademarks of Sony Interactive Entertainment Inc.

Use this software at your own risk and only on hardware you own or are authorized to use.

## License

This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.
