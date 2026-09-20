# Super MiniTV Ultra

ESP32-C3-DevKitM-1 + GMT130-V1.0 240x240 ST7789.

This repository keeps the proven MiniTV Ultra display/network foundation and adds the real media engine architecture from the selected DynaMight1124 MiniTV Player: MJPEG/video-frame playback, dynamic numbered channels, random playback, image screens, persistent Ultra UI settings, weather, mDNS, native HTTP, and OTA.

## Hardware kept unchanged

- Display: GMT130-V1.0 / ST7789 / 240x240
- SCK: GPIO4
- MOSI/SDA: GPIO6
- DC: GPIO2
- RESET: GPIO3
- SPI mode: 3
- Wi-Fi: STA only
- mDNS: minitv.local
- HTTP: native WiFiServer on port 80

The GMT130-V1.0 has no exposed CS pin, so this C3 build does **not** attempt to bolt an ordinary second SPI SD bus onto the proven display wiring. Instead, MiniTV media is stored in the dedicated LittleFS partition. This preserves the display timing while still providing real local MJPEG playback.

## Media tree

Put media in:

```
/Videos/
  1/
    show.mjpeg
  2/
    another.mjpeg
  random/
    clip01.mjpeg
    clip02.mjpeg
```

Numbered channels play sequentially. The random folder continuously selects MJPEG clips at random. Video-only playback is used; no audio decoder, I2S audio task, or audio dependency is included.

## Recommended encoding

240x240 MJPEG at 24 fps is the target for this C3 build.

Example video:

```
ffmpeg -i input.mp4 -pix_fmt yuvj420p -q:v 8 -vf "fps=24,scale=240:240:flags=lanczos" output.mjpeg
```

Example MP3:

```
ffmpeg -i input.mp4 -ar 44100 -ac 1 -b:a 32k output.mp3
```

AAC is also supported.

## Build

PlatformIO is configured for LittleFS and the two OTA application slots.

Build:

```
pio run
```

Build the LittleFS image:

```
pio run --target buildfs
```

Upload the filesystem separately when you have media files in `data/`:

```
pio run --target uploadfs
```

PlatformIO supports LittleFS directly when `board_build.filesystem = littlefs` is selected.

## Web console

After Wi-Fi connects:

- `http://minitv.local/`
- Display pages
- Clock/date
- Weather
- Custom screen
- System diagnostics
- Media play/stop/next controls
- Firmware OTA

## Important

The firmware build is separate from flashing. No hardware flash is authorized by this repository work.
