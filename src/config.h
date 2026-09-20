#pragma once

// Super MiniTV Ultra — image/animation-only media engine.
// Hardware foundation: ESP32-C3-DevKitM-1 + 240x240 ST7789.
#define TFT_SCK   4
#define TFT_MOSI  6
#define TFT_DC    2
#define TFT_RST   3

#define MINITV_WIDTH   240
#define MINITV_HEIGHT  240
#define MINITV_FPS     24

#define MINITV_MEDIA_ROOT "/Videos"
#define MINITV_STATIC_SECONDS 6
#define MINITV_MAX_UPLOAD (2UL * 1024UL * 1024UL)
