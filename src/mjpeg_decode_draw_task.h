#pragma once

#include <Arduino.h>
#include <FS.h>
#include <JPEGDEC.h>
#include "esp_heap_caps.h"
#include "config.h"

typedef struct {
  int32_t size;
  uint8_t *buf;
} MiniTVMjpegBuffer;

typedef struct {
  QueueHandle_t queue;
  JPEG_DRAW_CALLBACK *drawFunc;
} MiniTVDrawTaskParams;

typedef struct {
  QueueHandle_t queue;
  JPEG_DRAW_CALLBACK *drawFunc;
} MiniTVDecodeTaskParams;

static JPEGDRAW minitv_draw_buffers[MINITV_DRAW_BUFFERS];
static int minitv_draw_queue_count = 0;
static JPEGDEC minitv_jpeg;
static QueueHandle_t minitv_draw_queue = nullptr;
static QueueHandle_t minitv_decode_queue = nullptr;
static TaskHandle_t minitv_decode_task_handle = nullptr;
static TaskHandle_t minitv_draw_task_handle = nullptr;
static MiniTVDrawTaskParams minitv_draw_params;
static MiniTVDecodeTaskParams minitv_decode_params;

static MiniTVMjpegBuffer minitv_mjpeg_buffers[MINITV_DECODE_BUFFERS];
static uint8_t minitv_mjpeg_index = 0;
static uint8_t *minitv_current_mjpeg = nullptr;
static int32_t minitv_mjpeg_size = 0;
static uint8_t *minitv_read_buffer = nullptr;
static int32_t minitv_buf_read = 0;
static int32_t minitv_input_index = 0;
static Stream *minitv_input_stream = nullptr;
static int32_t minitv_mjpeg_capacity = 0;
static bool minitv_decoder_ready = false;

static unsigned long minitv_decode_ms = 0;
static unsigned long minitv_draw_ms = 0;

static int minitv_queue_draw(JPEGDRAW *draw) {
  int len = draw->iWidth * draw->iHeight * 2;
  JPEGDRAW *slot = &minitv_draw_buffers[minitv_draw_queue_count % MINITV_DRAW_BUFFERS];
  slot->x = draw->x;
  slot->y = draw->y;
  slot->iWidth = draw->iWidth;
  slot->iHeight = draw->iHeight;
  memcpy(slot->pPixels, draw->pPixels, len);
  ++minitv_draw_queue_count;
  return xQueueSend(minitv_draw_queue, &slot, portMAX_DELAY) == pdTRUE;
}

static void minitv_decode_task(void *arg) {
  MiniTVDecodeTaskParams *p = static_cast<MiniTVDecodeTaskParams *>(arg);
  MiniTVMjpegBuffer *b = nullptr;
  for (;;) {
    if (xQueueReceive(p->queue, &b, portMAX_DELAY) != pdTRUE) continue;
    unsigned long start = millis();
    minitv_jpeg.openRAM(b->buf, b->size, p->drawFunc);
    minitv_jpeg.setPixelType(RGB565_BIG_ENDIAN);
    minitv_jpeg.setMaxOutputSize(MINITV_MAX_WIDTH / 3 / 16);
    minitv_jpeg.decode(0, 0, 0);
    minitv_jpeg.close();
    minitv_decode_ms += millis() - start;
  }
}

static void minitv_draw_task(void *arg) {
  MiniTVDrawTaskParams *p = static_cast<MiniTVDrawTaskParams *>(arg);
  JPEGDRAW *draw = nullptr;
  for (;;) {
    if (xQueueReceive(p->queue, &draw, portMAX_DELAY) != pdTRUE) continue;
    unsigned long start = millis();
    p->drawFunc(draw);
    minitv_draw_ms += millis() - start;
  }
}

static bool minitv_decoder_init(JPEG_DRAW_CALLBACK *drawFunc) {
  if (minitv_decoder_ready) return true;

  minitv_read_buffer = static_cast<uint8_t *>(malloc(MINITV_READ_BUFFER));
  if (!minitv_read_buffer) return false;

  for (int i = 0; i < MINITV_DECODE_BUFFERS; ++i) {
    minitv_mjpeg_buffers[i].buf = static_cast<uint8_t *>(malloc(MINITV_MJPEG_BUFFER_SIZE));
    if (!minitv_mjpeg_buffers[i].buf) return false;
  }

  minitv_draw_queue = xQueueCreate(MINITV_DRAW_BUFFERS, sizeof(JPEGDRAW *));
  minitv_decode_queue = xQueueCreate(MINITV_DECODE_BUFFERS, sizeof(MiniTVMjpegBuffer *));
  if (!minitv_draw_queue || !minitv_decode_queue) return false;

  const size_t drawBytes = (MINITV_MAX_WIDTH / 3 / 16) * 16 * 16 * 2;
  for (int i = 0; i < MINITV_DRAW_BUFFERS; ++i) {
    minitv_draw_buffers[i].pPixels =
      static_cast<uint16_t *>(heap_caps_malloc(drawBytes, MALLOC_CAP_DMA));
    if (!minitv_draw_buffers[i].pPixels) return false;
  }

  minitv_draw_params.queue = minitv_draw_queue;
  minitv_draw_params.drawFunc = drawFunc;
  minitv_decode_params.queue = minitv_decode_queue;
  minitv_decode_params.drawFunc = minitv_queue_draw;

  if (xTaskCreate(minitv_decode_task, "MiniTV JPEG", 4096, &minitv_decode_params, 5,
                  &minitv_decode_task_handle) != pdPASS) return false;
  if (xTaskCreate(minitv_draw_task, "MiniTV Draw", 4096, &minitv_draw_params, 6,
                  &minitv_draw_task_handle) != pdPASS) return false;

  minitv_decoder_ready = true;
  return true;
}

static void minitv_decoder_set_input(Stream *input) {
  minitv_input_stream = input;
  minitv_input_index = 0;
  minitv_buf_read = 0;
  minitv_mjpeg_size = 0;
  minitv_mjpeg_index = 0;
  minitv_current_mjpeg = minitv_mjpeg_buffers[0].buf;
}

static bool minitv_read_frame() {
  if (!minitv_input_stream) return false;

  if (minitv_input_index == 0) {
    minitv_buf_read = minitv_input_stream->readBytes(minitv_read_buffer, MINITV_READ_BUFFER);
    minitv_input_index += minitv_buf_read;
  }

  minitv_mjpeg_size = 0;
  int i = 0;
  bool foundStart = false;

  while (minitv_buf_read > 0 && !foundStart) {
    for (i = 0; i + 1 < minitv_buf_read; ++i) {
      if (minitv_read_buffer[i] == 0xFF && minitv_read_buffer[i + 1] == 0xD8) {
        foundStart = true;
        break;
      }
    }
    if (!foundStart) {
      minitv_buf_read = minitv_input_stream->readBytes(minitv_read_buffer, MINITV_READ_BUFFER);
      minitv_input_index += minitv_buf_read;
    }
  }

  if (!foundStart) return false;

  uint8_t *p = minitv_read_buffer + i;
  minitv_buf_read -= i;

  bool foundEnd = false;
  while (minitv_buf_read > 0 && !foundEnd) {
    int take = minitv_buf_read;
    for (int j = 1; j + 1 < minitv_buf_read; ++j) {
      if (p[j] == 0xFF && p[j + 1] == 0xD9) {
        take = j + 2;
        foundEnd = true;
        break;
      }
    }

    if (minitv_mjpeg_size + take > MINITV_MJPEG_BUFFER_SIZE) {
      return false;
    }

    memcpy(minitv_current_mjpeg + minitv_mjpeg_size, p, take);
    minitv_mjpeg_size += take;

    int remain = minitv_buf_read - take;
    if (remain > 0) {
      memmove(minitv_read_buffer, p + take, remain);
    }

    int next = minitv_input_stream->readBytes(minitv_read_buffer + remain,
                                               MINITV_READ_BUFFER - remain);
    minitv_buf_read = remain + next;
    minitv_input_index += next;
    p = minitv_read_buffer;

    if (foundEnd) break;
  }

  return foundEnd;
}

static bool minitv_decode_frame() {
  if (!minitv_decoder_ready || minitv_mjpeg_size <= 0) return false;
  MiniTVMjpegBuffer *b = &minitv_mjpeg_buffers[minitv_mjpeg_index];
  b->size = minitv_mjpeg_size;
  if (xQueueSend(minitv_decode_queue, &b, pdMS_TO_TICKS(250)) != pdTRUE) return false;
  minitv_mjpeg_index = (minitv_mjpeg_index + 1) % MINITV_DECODE_BUFFERS;
  minitv_current_mjpeg = minitv_mjpeg_buffers[minitv_mjpeg_index].buf;
  return true;
}
