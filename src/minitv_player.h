#pragma once

#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include "config.h"
#include "mjpeg_decode_draw_task.h"
#include "esp32_audio_task.h"

extern void minitvDisplayFrame(JPEGDRAW *draw);
extern volatile bool mediaPlaying;

static volatile bool minitv_stop_requested = false;
static volatile bool minitv_autoplay = true;
static volatile bool minitv_engine_ready = false;
static volatile bool minitv_playback_task_running = false;
static TaskHandle_t minitv_playback_handle = nullptr;

static int minitv_channel_count = 0;
static bool minitv_has_random = false;
static int minitv_channel = 1;
static String minitv_now_playing = "";
static unsigned long minitv_frame_counter = 0;
static unsigned long minitv_last_frame_ms = 0;

static bool minitv_isMjpeg(const String &name) {
  String s = name; s.toLowerCase();
  return s.endsWith(".mjpeg") || s.endsWith(".mjpg");
}

static bool minitv_isAudio(const String &name) {
  String s = name; s.toLowerCase();
  return s.endsWith(".aac") || s.endsWith(".mp3");
}

static String minitv_baseName(const String &name) {
  int slash = name.lastIndexOf('/');
  String n = slash >= 0 ? name.substring(slash + 1) : name;
  int dot = n.lastIndexOf('.');
  return dot > 0 ? n.substring(0, dot) : n;
}

static bool minitv_findMediaInDir(const String &dir, String &videoPath, String &audioPath) {
  videoPath = "";
  audioPath = "";

  File root = LittleFS.open(dir);
  if (!root || !root.isDirectory()) return false;

  File f = root.openNextFile();
  while (f) {
    if (!f.isDirectory() && minitv_isMjpeg(String(f.name()))) {
      videoPath = String(f.name());
      f.close();
      break;
    }
    f.close();
    f = root.openNextFile();
  }
  root.close();

  if (!videoPath.length()) return false;

  String base = minitv_baseName(videoPath);
  String aac = dir + "/" + base + ".aac";
  String mp3 = dir + "/" + base + ".mp3";

  if (LittleFS.exists(aac)) audioPath = aac;
  else if (LittleFS.exists(mp3)) audioPath = mp3;
  else {
    root = LittleFS.open(dir);
    f = root.openNextFile();
    while (f) {
      String n = String(f.name());
      if (!f.isDirectory() && minitv_isAudio(n)) {
        audioPath = n;
        f.close();
        break;
      }
      f.close();
      f = root.openNextFile();
    }
    root.close();
  }

  return true;
}

static int minitv_scanChannels() {
  minitv_channel_count = 0;
  minitv_has_random = LittleFS.exists(String(MINITV_MEDIA_ROOT) + "/random");

  for (int n = 1; n <= 99; ++n) {
    String dir = String(MINITV_MEDIA_ROOT) + "/" + String(n);
    if (!LittleFS.exists(dir)) break;
    String v, a;
    if (minitv_findMediaInDir(dir, v, a)) ++minitv_channel_count;
  }

  if (minitv_channel_count == 0 && !minitv_has_random) minitv_channel = 1;
  else if (minitv_channel > minitv_channel_count) minitv_channel = 1;

  Serial.printf("[MiniTV] channels=%d random=%d\n", minitv_channel_count, minitv_has_random ? 1 : 0);
  return minitv_channel_count + (minitv_has_random ? 1 : 0);
}

static bool minitv_pickRandom(String &videoPath, String &audioPath) {
  String dir = String(MINITV_MEDIA_ROOT) + "/random";
  File root = LittleFS.open(dir);
  if (!root || !root.isDirectory()) return false;

  String selected = "";
  int count = 0;
  File f = root.openNextFile();
  while (f) {
    String n = String(f.name());
    if (!f.isDirectory() && minitv_isMjpeg(n)) {
      ++count;
      if ((esp_random() % count) == 0) selected = n;
    }
    f.close();
    f = root.openNextFile();
  }
  root.close();

  if (!selected.length()) return false;

  videoPath = selected;
  String base = minitv_baseName(videoPath);
  String aac = dir + "/" + base + ".aac";
  String mp3 = dir + "/" + base + ".mp3";
  if (LittleFS.exists(aac)) audioPath = aac;
  else if (LittleFS.exists(mp3)) audioPath = mp3;
  else audioPath = "";
  return true;
}

static bool minitv_playOne(const String &videoPath, const String &audioPath) {
  File video = LittleFS.open(videoPath, FILE_READ);
  if (!video || video.isDirectory()) return false;

  File audio;
  bool hasAudio = false;
#if MINITV_AUDIO_ENABLED
  if (audioPath.length()) {
    audio = LittleFS.open(audioPath, FILE_READ);
    hasAudio = audio && !audio.isDirectory();
  }
#endif

  minitv_now_playing = videoPath;
  mediaPlaying = true;
  minitv_frame_counter = 0;
  minitv_last_frame_ms = millis();

  minitv_decoder_set_input(&video);

#if MINITV_AUDIO_ENABLED
  if (hasAudio && minitv_i2s_ready) {
    String low = audioPath;
    low.toLowerCase();
    if (low.endsWith(".aac")) minitv_start_aac(&audio);
    else if (low.endsWith(".mp3")) minitv_start_mp3(&audio);
  }
#endif

  const uint32_t framePeriod = 1000UL / MINITV_FPS;
  uint32_t nextFrame = millis();

  Serial.printf("[MiniTV] PLAY %s%s%s\n",
                videoPath.c_str(),
                hasAudio ? " + " : "",
                hasAudio ? audioPath.c_str() : "");

  while (video.available() && !minitv_stop_requested) {
    if (!minitv_read_frame()) break;
    minitv_decode_frame();

    while ((int32_t)(nextFrame - millis()) > 0) {
      vTaskDelay(1);
    }
    ++minitv_frame_counter;
    nextFrame += framePeriod;

    if ((int32_t)(millis() - nextFrame) > (int32_t)framePeriod * 3) {
      nextFrame = millis() + framePeriod;
    }
  }

#if MINITV_AUDIO_ENABLED
  uint32_t waitStart = millis();
  while (!minitv_audio_task_done && millis() - waitStart < 3000) vTaskDelay(5);
#endif

  video.close();
  if (hasAudio) audio.close();
  minitv_now_playing = "";
  mediaPlaying = false;
  return true;
}

static void minitv_playback_task(void *) {
  minitv_playback_task_running = true;

  while (minitv_autoplay) {
    if (minitv_stop_requested) break;

    String video, audio;
    bool found = false;

    if (minitv_has_random) {
      found = minitv_pickRandom(video, audio);
    } else if (minitv_channel_count > 0) {
      String dir = String(MINITV_MEDIA_ROOT) + "/" + String(minitv_channel);
      found = minitv_findMediaInDir(dir, video, audio);
    }

    if (!found) {
      minitv_autoplay = false;
      break;
    }

    minitv_playOne(video, audio);

    if (minitv_stop_requested) {
      if (minitv_autoplay) {
        minitv_stop_requested = false;
      } else {
        break;
      }
    }

    if (!minitv_has_random && minitv_channel_count > 0) {
      ++minitv_channel;
      if (minitv_channel > minitv_channel_count) minitv_channel = 1;
    }

    vTaskDelay(1);
  }

  mediaPlaying = false;
  minitv_playback_task_running = false;
  minitv_playback_handle = nullptr;
  vTaskDelete(nullptr);
}

static bool minitvStartPlayback() {
  if (!minitv_engine_ready) return false;
  if (minitv_playback_task_running) {
    minitv_stop_requested = false;
    minitv_autoplay = true;
    return true;
  }
  minitv_stop_requested = false;
  minitv_autoplay = true;
  return xTaskCreate(minitv_playback_task, "MiniTV Player", 6144, nullptr, 3,
                     &minitv_playback_handle) == pdPASS;
}

static void minitvStopPlayback() {
  minitv_stop_requested = true;
  minitv_autoplay = false;
}

static bool minitvNextChannel(int direction) {
  if (minitv_channel_count <= 0) return false;
  minitv_channel += direction;
  if (minitv_channel < 1) minitv_channel = minitv_channel_count;
  if (minitv_channel > minitv_channel_count) minitv_channel = 1;
  minitv_stop_requested = true;
  minitv_autoplay = true;
  return true;
}

static bool minitvBegin() {
  if (!LittleFS.begin(true)) {
    Serial.println("[MiniTV] LittleFS mount failed");
    return false;
  }

#if MINITV_AUDIO_ENABLED
  if (!minitv_audio_begin()) Serial.println("[MiniTV] I2S audio init failed");
#endif

  if (!minitv_decoder_init(minitvDisplayFrame)) {
    Serial.println("[MiniTV] MJPEG decoder init failed");
    return false;
  }

  minitv_scanChannels();
  minitv_engine_ready = true;

  if (minitv_channel_count || minitv_has_random) minitvStartPlayback();
  else Serial.println("[MiniTV] No media yet. Web console can upload media.");
  return true;
}

static String minitvStatusJson() {
  String s = "{";
  s += "\"playing\":" + String(mediaPlaying ? "true" : "false");
  s += ",\"channels\":" + String(minitv_channel_count);
  s += ",\"random\":" + String(minitv_has_random ? "true" : "false");
  s += ",\"channel\":" + String(minitv_channel);
  s += ",\"frames\":" + String(minitv_frame_counter);
  s += ",\"file\":\"" + minitv_now_playing + "\"";
  s += "}";
  return s;
}
