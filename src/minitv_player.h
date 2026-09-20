#pragma once

#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <JPEGDEC.h>
#include <AnimatedGIF.h>
#include "config.h"

extern void lcdSetWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
extern SPISettings lcdSettings;
extern int16_t minitvImageX;
extern int16_t minitvImageY;
extern void minitvPush565Line(const uint16_t *pixels, int count);

static AnimatedGIF minitvGif;
static JPEGDEC minitvJpeg;
static File minitvFile;
static String minitvCurrent = "";
static String minitvPending = "";
static bool minitvRunning = false;
static bool minitvGifActive = false;
static bool minitvJpegShown = false;
static uint32_t minitvStaticUntil = 0;
static int minitvChannel = 1;
static int minitvChannelCount = 0;
static bool minitvHasRandom = false;
static bool minitvRandomMode = false;
static uint32_t minitvFrameCount = 0;
static uint32_t minitvLastFrame = 0;

static bool minitvIsImage(const String &n) {
  String s=n; s.toLowerCase();
  return s.endsWith(".gif") || s.endsWith(".jpg") || s.endsWith(".jpeg");
}
static bool minitvIsGif(const String &n) {
  String s=n; s.toLowerCase(); return s.endsWith(".gif");
}
static bool minitvIsJpeg(const String &n) {
  String s=n; s.toLowerCase(); return s.endsWith(".jpg") || s.endsWith(".jpeg");
}
static bool minitvSafePath(const String &p) {
  return p.startsWith(MINITV_MEDIA_ROOT) && !p.startsWith(String(MINITV_MEDIA_ROOT)+"//") &&
         p.indexOf("..")<0;
}
static void minitvEnsureDir(const String &path) {
  int start=1;
  while(true){
    int slash=path.indexOf('/',start);
    if(slash<0) break;
    String d=path.substring(0,slash);
    if(d.length() && !LittleFS.exists(d)) LittleFS.mkdir(d);
    start=slash+1;
  }
}
static int minitvCountImages(const String &dir) {
  File root=LittleFS.open(dir);
  if(!root || !root.isDirectory()) return 0;
  int n=0; File f=root.openNextFile();
  while(f){ if(!f.isDirectory() && minitvIsImage(String(f.name()))) ++n; f.close(); f=root.openNextFile(); }
  root.close(); return n;
}
static String minitvNthImage(const String &dir, int wanted) {
  File root=LittleFS.open(dir);
  if(!root || !root.isDirectory()) return "";
  int n=0; String selected=""; File f=root.openNextFile();
  while(f){
    String name=String(f.name());
    if(!f.isDirectory() && minitvIsImage(name)){
      if(n++==wanted){ selected=name; f.close(); break; }
    }
    f.close(); f=root.openNextFile();
  }
  root.close(); return selected;
}
static int minitvScanChannels() {
  minitvChannelCount=0;
  minitvHasRandom=LittleFS.exists(String(MINITV_MEDIA_ROOT)+"/random");
  for(int n=1;n<=99;n++){
    String d=String(MINITV_MEDIA_ROOT)+"/"+String(n);
    if(!LittleFS.exists(d)) break;
    if(minitvCountImages(d)>0) ++minitvChannelCount;
  }
  if(minitvChannelCount==0 && minitvCountImages(MINITV_MEDIA_ROOT)>0) minitvChannelCount=1;
  if(minitvChannel<1 || minitvChannel>max(1,minitvChannelCount)) minitvChannel=1;
  return minitvChannelCount;
}

static void *minitvGifOpen(const char *fname, int32_t *size) {
  minitvFile=LittleFS.open(fname,FILE_READ);
  if(!minitvFile) return nullptr;
  *size=(int32_t)minitvFile.size();
  return &minitvFile;
}
static void minitvGifClose(void *handle) {
  File *f=static_cast<File*>(handle); if(f) f->close();
}
static int32_t minitvGifRead(GIFFILE *file,uint8_t *buf,int32_t len) {
  File *f=static_cast<File*>(file->fHandle); if(!f) return 0;
  int32_t n=(int32_t)f->read(buf,len); file->iPos=(int32_t)f->position(); return n;
}
static int32_t minitvGifSeek(GIFFILE *file,int32_t pos) {
  File *f=static_cast<File*>(file->fHandle); if(!f) return 0;
  if(pos<0) pos=0; if(pos>file->iSize) pos=file->iSize;
  f->seek(pos); file->iPos=(int32_t)f->position(); return file->iPos;
}

static void minitvGifDraw(GIFDRAW *d) {
  int x=minitvImageX+d->iX;
  int y=minitvImageY+d->iY+d->y;
  int w=d->iWidth;
  if(x<0 || y<0 || x>=MINITV_WIDTH || y>=MINITV_HEIGHT) return;
  if(x+w>MINITV_WIDTH) w=MINITV_WIDTH-x;
  if(w<=0) return;

  const uint8_t *src=d->pPixels;
  if(d->ucHasTransparency){
    // Transparent GIF runs are handled by drawing only opaque runs.
    int i=0;
    while(i<w){
      while(i<w && src[i]==d->ucTransparent) ++i;
      int start=i;
      while(i<w && src[i]!=d->ucTransparent) ++i;
      if(i>start){
        lcdSetWindow(x+start,y,x+i-1,y);
        SPI.beginTransaction(lcdSettings);
        digitalWrite(TFT_DC,HIGH);
        for(int k=start;k<i;k++){
          uint16_t c=d->pPalette[src[k]];
          SPI.transfer((uint8_t)(c>>8)); SPI.transfer((uint8_t)c);
        }
        SPI.endTransaction();
      }
    }
  } else {
    lcdSetWindow(x,y,x+w-1,y);
    SPI.beginTransaction(lcdSettings);
    digitalWrite(TFT_DC,HIGH);
    for(int i=0;i<w;i++){
      uint16_t c=d->pPalette[src[i]];
      SPI.transfer((uint8_t)(c>>8)); SPI.transfer((uint8_t)c);
    }
    SPI.endTransaction();
  }
}

static int minitvJpegDraw(JPEGDRAW *d) {
  int x=minitvImageX+d->x;
  int y=minitvImageY+d->y;
  int w=d->iWidth, h=d->iHeight;
  if(x<0 || y<0 || x+w>MINITV_WIDTH || y+h>MINITV_HEIGHT) return 1;
  lcdSetWindow(x,y,x+w-1,y+h-1);
  SPI.beginTransaction(lcdSettings);
  digitalWrite(TFT_DC,HIGH);
  SPI.transferBytes((uint8_t*)d->pPixels,nullptr,(size_t)w*h*2);
  SPI.endTransaction();
  return 1;
}

static String minitvPickNext() {
  minitvScanChannels();
  if(minitvHasRandom && minitvRandomMode){
    String d=String(MINITV_MEDIA_ROOT)+"/random";
    int count=minitvCountImages(d);
    if(count) return minitvNthImage(d,esp_random()%count);
  }
  if(minitvChannelCount<=0){
    int count=minitvCountImages(MINITV_MEDIA_ROOT);
    if(count) return minitvNthImage(MINITV_MEDIA_ROOT,(int)(minitvFrameCount%count));
    return "";
  }
  String d=String(MINITV_MEDIA_ROOT)+"/"+String(minitvChannel);
  int count=minitvCountImages(d);
  if(!count) return "";
  int index=(int)(minitvFrameCount%count);
  return minitvNthImage(d,index);
}

static bool minitvOpenCurrent(const String &path) {
  if(!minitvIsImage(path)) return false;
  minitvCurrent=path;
  minitvGifActive=false; minitvJpegShown=false;
  minitvImageX=0; minitvImageY=0;

  if(minitvIsGif(path)){
    minitvGif.begin(GIF_PALETTE_RGB565_BE);
    if(!minitvGif.open(path.c_str(),minitvGifOpen,minitvGifClose,minitvGifRead,minitvGifSeek,minitvGifDraw)){
      return false;
    }
    int w=minitvGif.getCanvasWidth(), h=minitvGif.getCanvasHeight();
    minitvImageX=(MINITV_WIDTH-w)/2; if(minitvImageX<0)minitvImageX=0;
    minitvImageY=(MINITV_HEIGHT-h)/2; if(minitvImageY<0)minitvImageY=0;
    minitvGifActive=true;
    return true;
  }

  minitvFile=LittleFS.open(path,FILE_READ);
  if(!minitvFile) return false;
  int w=minitvFile.size()>0?MINITV_WIDTH:0;
  (void)w;
  if(!minitvJpeg.open(minitvFile,minitvJpegDraw)){
    minitvFile.close(); return false;
  }
  minitvJpeg.setPixelType(RGB565_BIG_ENDIAN);
  int jw=minitvJpeg.getWidth(), jh=minitvJpeg.getHeight();
  minitvImageX=(MINITV_WIDTH-jw)/2; if(minitvImageX<0)minitvImageX=0;
  minitvImageY=(MINITV_HEIGHT-jh)/2; if(minitvImageY<0)minitvImageY=0;
  minitvJpeg.decode(minitvImageX,minitvImageY,0);
  minitvJpeg.close(); minitvFile.close();
  minitvJpegShown=true;
  minitvStaticUntil=millis()+MINITV_STATIC_SECONDS*1000UL;
  return true;
}

static void minitvStopPlayback() {
  if(minitvGifActive){minitvGif.close();minitvGifActive=false;}
  if(minitvFile) minitvFile.close();
  minitvRunning=false; minitvCurrent="";
}
static void minitvStartPlayback() {
  minitvRunning=true;
  if(!minitvCurrent.length()) {
    String p=minitvPickNext();
    if(p.length()) minitvOpenCurrent(p);
  }
}
static void minitvNextChannel(int direction) {
  minitvStopPlayback();
  if(minitvChannelCount>0){
    minitvChannel+=direction;
    if(minitvChannel>minitvChannelCount)minitvChannel=1;
    if(minitvChannel<1)minitvChannel=minitvChannelCount;
  }
  minitvFrameCount++;
  minitvStartPlayback();
}
static void minitvTick() {
  if(!minitvRunning) return;
  if(minitvGifActive){
    int delayMs=0;
    int more=minitvGif.playFrame(true,&delayMs);
    ++minitvFrameCount;
    if(!more){
      minitvGif.close(); minitvGifActive=false;
      minitvFrameCount++;
      minitvCurrent="";
    }
    return;
  }
  if(minitvJpegShown && (int32_t)(millis()-minitvStaticUntil)>=0){
    minitvJpegShown=false;
    minitvCurrent="";
    ++minitvFrameCount;
  }
  if(!minitvCurrent.length()){
    String p=minitvPickNext();
    if(p.length()) minitvOpenCurrent(p);
  }
}
static String minitvStatusJson() {
  String q=String((char)34);
  String s="{" + q + "running" + q + ":" + String(minitvRunning?"true":"false");
  s+="," + q + "channel" + q + ":" + String(minitvChannel);
  s+="," + q + "channels" + q + ":" + String(minitvChannelCount);
  s+="," + q + "random" + q + ":" + String(minitvRandomMode?"true":"false");
  s+="," + q + "file" + q + ":" + q + minitvCurrent + q;
  s+="," + q + "frames" + q + ":" + String(minitvFrameCount) + "}";
  return s;
}
static String minitvMediaList() {
  String out="";
  File root=LittleFS.open(MINITV_MEDIA_ROOT);
  if(!root || !root.isDirectory()) return out;
  File f=root.openNextFile();
  while(f){
    if(!f.isDirectory() && minitvIsImage(String(f.name()))) out+=String(f.name())+"\n";
    f.close(); f=root.openNextFile();
  }
  root.close();
  for(int n=1;n<=99;n++){
    String d=String(MINITV_MEDIA_ROOT)+"/"+String(n);
    if(!LittleFS.exists(d)) break;
    File r=LittleFS.open(d);
    if(!r || !r.isDirectory()) continue;
    File q=r.openNextFile();
    while(q){if(!q.isDirectory() && minitvIsImage(String(q.name()))) out+=String(q.name())+"\n";q.close();q=r.openNextFile();}
    r.close();
  }
  return out;
}
static bool minitvDelete(const String &path) {
  return minitvSafePath(path) && LittleFS.exists(path) && LittleFS.remove(path);
}
static void minitvBegin() {
  if(!LittleFS.begin(true)){Serial.println("[MiniTV] LittleFS mount failed");return;}
  if(!LittleFS.exists(MINITV_MEDIA_ROOT)) LittleFS.mkdir(MINITV_MEDIA_ROOT);
  minitvScanChannels();
}
