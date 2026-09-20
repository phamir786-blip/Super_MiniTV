#pragma once

#include <Arduino.h>
#include "config.h"

#if MINITV_AUDIO_ENABLED
#include "driver/i2s.h"
#include "AACDecoderHelix.h"
#include "MP3DecoderHelix.h"

static i2s_port_t minitv_i2s_num = I2S_NUM_0;
static int minitv_sample_rate = 0;
static bool minitv_i2s_ready = false;
static volatile bool minitv_audio_muted = false;

static void minitv_audio_silence() {
  if (!minitv_i2s_ready) return;
  int16_t silence[160] = {};
  size_t written = 0;
  for (int i = 0; i < 4; ++i) {
    i2s_write(minitv_i2s_num, silence, sizeof(silence), &written, portMAX_DELAY);
  }
}

static bool minitv_audio_begin() {
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = 44100;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 160;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;
  cfg.fixed_mclk = 0;
  cfg.mclk_multiple = I2S_MCLK_MULTIPLE_DEFAULT;
  cfg.bits_per_chan = I2S_BITS_PER_CHAN_16BIT;

  esp_err_t e = i2s_driver_install(minitv_i2s_num, &cfg, 0, nullptr);
  if (e != ESP_OK) return false;

  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = MINITV_I2S_BCLK;
  pins.ws_io_num = MINITV_I2S_LRCK;
  pins.data_out_num = MINITV_I2S_DOUT;
  pins.data_in_num = I2S_PIN_NO_CHANGE;

  e = i2s_set_pin(minitv_i2s_num, &pins);
  if (e != ESP_OK) {
    i2s_driver_uninstall(minitv_i2s_num);
    return false;
  }

  minitv_i2s_ready = true;
  minitv_audio_silence();
  return true;
}

static void minitv_audio_end() {
  if (!minitv_i2s_ready) return;
  minitv_audio_silence();
  i2s_driver_uninstall(minitv_i2s_num);
  minitv_i2s_ready = false;
  minitv_sample_rate = 0;
}

static void minitv_write_pcm(int16_t *pcm, size_t samples, int sampleRate, int channels) {
  if (!minitv_i2s_ready) return;

  if (minitv_sample_rate != sampleRate) {
    i2s_set_clk(minitv_i2s_num, sampleRate, I2S_BITS_PER_SAMPLE_16BIT,
                channels == 2 ? I2S_CHANNEL_STEREO : I2S_CHANNEL_MONO);
    minitv_sample_rate = sampleRate;
  }

  if (minitv_audio_muted || MINITV_AUDIO_VOLUME < 100) {
    for (size_t i = 0; i < samples; ++i) {
      if (minitv_audio_muted) pcm[i] = 0;
      else pcm[i] = (int16_t)((int32_t)pcm[i] * MINITV_AUDIO_VOLUME / 100);
    }
  }

  size_t written = 0;
  i2s_write(minitv_i2s_num, pcm, samples * sizeof(int16_t), &written, portMAX_DELAY);
}

static void minitv_aac_callback(AACFrameInfo &info, int16_t *pcm, size_t len) {
  minitv_write_pcm(pcm, len, info.sampRateOut, info.nChans);
}

static void minitv_mp3_callback(MP3FrameInfo &info, int16_t *pcm, size_t len) {
  minitv_write_pcm(pcm, len, info.samprate, info.nChans);
}

static libhelix::AACDecoderHelix minitv_aac(minitv_aac_callback);
static libhelix::MP3DecoderHelix minitv_mp3(minitv_mp3_callback);

static volatile bool minitv_audio_task_done = true;

static void minitv_aac_task(void *arg) {
  Stream *in = static_cast<Stream *>(arg);
  uint8_t *buf = static_cast<uint8_t *>(malloc(2100));
  if (!buf) {
    minitv_audio_task_done = true;
    vTaskDelete(nullptr);
    return;
  }
  minitv_aac.begin();
  while (in && in->available()) {
    int n = in->readBytes(buf, 2100);
    if (n <= 0) break;
    int left = n;
    uint8_t *p = buf;
    while (left > 0) {
      int used = minitv_aac.write(p, left);
      if (used <= 0) break;
      p += used;
      left -= used;
    }
  }
  free(buf);
  minitv_audio_task_done = true;
  vTaskDelete(nullptr);
}

static void minitv_mp3_task(void *arg) {
  Stream *in = static_cast<Stream *>(arg);
  uint8_t *buf = static_cast<uint8_t *>(malloc(2048));
  if (!buf) {
    minitv_audio_task_done = true;
    vTaskDelete(nullptr);
    return;
  }
  minitv_mp3.begin();
  while (in && in->available()) {
    int n = in->readBytes(buf, 2048);
    if (n <= 0) break;
    int left = n;
    uint8_t *p = buf;
    while (left > 0) {
      int used = minitv_mp3.write(p, left);
      if (used <= 0) break;
      p += used;
      left -= used;
    }
  }
  free(buf);
  minitv_audio_task_done = true;
  vTaskDelete(nullptr);
}

static bool minitv_start_aac(Stream *in) {
  minitv_audio_task_done = false;
  return xTaskCreate(minitv_aac_task, "MiniTV AAC", 4096, in, 4, nullptr) == pdPASS;
}

static bool minitv_start_mp3(Stream *in) {
  minitv_audio_task_done = false;
  return xTaskCreate(minitv_mp3_task, "MiniTV MP3", 4096, in, 4, nullptr) == pdPASS;
}

#else
static bool minitv_audio_begin() { return true; }
static void minitv_audio_end() {}
static void minitv_audio_silence() {}
static void minitv_set_audio_mute(bool) {}
#endif
