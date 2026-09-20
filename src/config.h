#pragma once

// Super MiniTV Ultra hardware / media configuration.
// The display wiring is the proven foundation and MUST stay unchanged.

#define TFT_SCK   4
#define TFT_MOSI  6
#define TFT_DC    2
#define TFT_RST   3

#define MINITV_WIDTH   240
#define MINITV_HEIGHT  240
#define MINITV_FPS     24

// Media is stored in LittleFS so the GMT130 (no-CS) display can keep
// its proven four-wire SPI bus without a second physical SPI bus.
#define MINITV_MEDIA_ROOT "/Videos"
#define MINITV_AUDIO_ENABLED 1

// External I2S DAC pins. Change only if your DAC is wired differently.
#define MINITV_I2S_BCLK  7
#define MINITV_I2S_LRCK  0
#define MINITV_I2S_DOUT  1
#define MINITV_AUDIO_VOLUME 80

#define MINITV_MAX_WIDTH 240
#define MINITV_MJPEG_BUFFER_SIZE (MINITV_MAX_WIDTH * MINITV_HEIGHT * 2 / 8)

#define MINITV_DECODE_BUFFERS 3
#define MINITV_DRAW_BUFFERS   6
#define MINITV_READ_BUFFER    1024
#define MINITV_MAX_UPLOAD     900000UL
