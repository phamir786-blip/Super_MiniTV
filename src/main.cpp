/*
  Super MiniTV Ultra
  ESP32-C3-DevKitM-1 + GMT130-V1.0 240x240 ST7789
  Single-file firmware.

  IMPORTANT: the proven LCD and network foundations are intentionally preserved:
    LCD: SPI mode 3, SCK=4, MOSI=6, DC=2, RST=3
    Network: STA-only, native WiFiServer, mDNS minitv.local, no AP/captive portal.

  Fill in WIFI_SSID / WIFI_PASSWORD before building.
*/

#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include <time.h>
#include <math.h>

// =========================
// Proven hardware foundation
// =========================
#define TFT_SCK   4
#define TFT_MOSI  6
#define TFT_DC    2
#define TFT_RST   3

static SPISettings lcdSettings(1000000, MSBFIRST, SPI_MODE3);

// =========================
// Proven network foundation
// =========================
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* HOSTNAME      = "minitv";

WiFiServer httpServer(80);
Preferences prefs;

// =========================
// Display geometry / colors
// =========================
static const int16_t W = 240;
static const int16_t H = 240;

#define C_BLACK  0x0000
#define C_WHITE  0xFFFF
#define C_DIM    0x7BEF
#define C_RED    0xF800
#define C_GREEN  0x07E0
#define C_CYAN   0x07FF
#define C_BLUE   0x001F
#define C_YELLOW 0xFFE0
#define C_ORANGE 0xFD20
#define C_MAGENTA 0xF81F
#define C_GRAY   0x4208
#define C_DARK   0x1082
#define C_PANEL  0x18E3

// =========================
// Runtime state
// =========================
enum Page : uint8_t {
  PAGE_HOME = 0,
  PAGE_RETRO,
  PAGE_WEATHER,
  PAGE_SYSTEM,
  PAGE_CUSTOM,
  PAGE_COUNT
};

Page page = PAGE_HOME;
bool autoRotate = true;
uint16_t rotateSeconds = 12;
bool crtEffect = true;
bool showSeconds = true;
bool invertScreen = false;

String titleText = "SUPER MINITV";
String messageText = "ON AIR";
String weatherPlace = "Davao City";
float weatherTemp = NAN;
float weatherFeels = NAN;
int weatherCode = -1;
int weatherHumidity = -1;
float weatherWind = NAN;
String weatherUpdated = "--";
bool weatherOK = false;
size_t requestContentLength = 0;

unsigned long bootMillis = 0;
unsigned long lastFrame = 0;
unsigned long lastRotate = 0;
unsigned long lastWifiAttempt = 0;
unsigned long lastWeather = 0;
unsigned long lastNtpPrint = 0;
bool mdnsStarted = false;
bool serverStarted = false;
bool ntpOK = false;
String lastHttp = "-";

// =========================
// Low-level ST7789
// =========================
void lcdCommand(uint8_t cmd) {
  SPI.beginTransaction(lcdSettings);
  digitalWrite(TFT_DC, LOW);
  SPI.transfer(cmd);
  SPI.endTransaction();
}

void lcdCommandData(uint8_t cmd, const uint8_t* data, uint16_t len) {
  SPI.beginTransaction(lcdSettings);
  digitalWrite(TFT_DC, LOW);
  SPI.transfer(cmd);
  digitalWrite(TFT_DC, HIGH);
  while (len--) SPI.transfer(*data++);
  SPI.endTransaction();
}

void lcdData(const uint8_t* data, size_t len) {
  SPI.beginTransaction(lcdSettings);
  digitalWrite(TFT_DC, HIGH);
  while (len--) SPI.transfer(*data++);
  SPI.endTransaction();
}

void lcdSetWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
  uint8_t d[4];
  d[0] = x0 >> 8; d[1] = x0;
  d[2] = x1 >> 8; d[3] = x1;
  lcdCommandData(0x2A, d, 4);
  d[0] = y0 >> 8; d[1] = y0;
  d[2] = y1 >> 8; d[3] = y1;
  lcdCommandData(0x2B, d, 4);
  lcdCommand(0x2C);
}

void lcdInit() {
  pinMode(TFT_DC, OUTPUT);
  pinMode(TFT_RST, OUTPUT);
  SPI.begin(TFT_SCK, -1, TFT_MOSI, -1);

  // EXACT proven initialization
  digitalWrite(TFT_RST, HIGH); delay(20);
  digitalWrite(TFT_RST, LOW); delay(120);
  digitalWrite(TFT_RST, HIGH); delay(120);

  lcdCommand(0x01); delay(150);
  lcdCommand(0x11); delay(150);
  uint8_t pixelFormat = 0x55;
  lcdCommandData(0x3A, &pixelFormat, 1);
  uint8_t madctl = 0x00;
  lcdCommandData(0x36, &madctl, 1);
  lcdCommand(0x21);
  lcdCommand(0x13);
  lcdCommand(0x29);
  delay(100);
}

void fillScreen(uint16_t color) {
  lcdSetWindow(0,0,W-1,H-1);
  uint8_t hi = color >> 8, lo = color;
  uint8_t buf[64];
  for (int i=0;i<64;i+=2) { buf[i]=hi; buf[i+1]=lo; }
  SPI.beginTransaction(lcdSettings);
  digitalWrite(TFT_DC, HIGH);
  size_t pixels = (size_t)W * H;
  while (pixels) {
    size_t n = pixels > 32 ? 32 : pixels;
    SPI.transferBytes(buf, nullptr, n * 2);
    pixels -= n;
  }
  SPI.endTransaction();
}

void drawPixel(int16_t x, int16_t y, uint16_t color) {
  if (x<0||y<0||x>=W||y>=H) return;
  lcdSetWindow(x,y,x,y);
  uint8_t d[2] = { uint8_t(color>>8), uint8_t(color) };
  lcdData(d,2);
}

void drawFastH(int16_t x,int16_t y,int16_t len,uint16_t c) {
  if (y<0||y>=H||len<=0) return;
  if (x<0) { len+=x; x=0; }
  if (x+len>W) len=W-x;
  if (len<=0) return;
  lcdSetWindow(x,y,x+len-1,y);
  uint8_t d[64]; uint8_t hi=c>>8,lo=c;
  for(int i=0;i<64;i+=2){d[i]=hi;d[i+1]=lo;}
  SPI.beginTransaction(lcdSettings); digitalWrite(TFT_DC,HIGH);
  int n=len;
  while(n){int k=n>32?32:n;SPI.transferBytes(d,nullptr,k*2);n-=k;}
  SPI.endTransaction();
}

void drawFastV(int16_t x,int16_t y,int16_t len,uint16_t c) {
  if (x<0||x>=W||len<=0) return;
  if (y<0) {len+=y;y=0;} if(y+len>H)len=H-y; if(len<=0)return;
  lcdSetWindow(x,y,x,y+len-1);
  uint8_t d[64];uint8_t hi=c>>8,lo=c;
  for(int i=0;i<64;i+=2){d[i]=hi;d[i+1]=lo;}
  SPI.beginTransaction(lcdSettings);digitalWrite(TFT_DC,HIGH);
  int n=len;while(n){int k=n>32?32:n;SPI.transferBytes(d,nullptr,k*2);n-=k;}
  SPI.endTransaction();
}

void rect(int16_t x,int16_t y,int16_t w,int16_t h,uint16_t c) {
  drawFastH(x,y,w,c);drawFastH(x,y+h-1,w,c);
  drawFastV(x,y,h,c);drawFastV(x+w-1,y,h,c);
}
void fillRect(int16_t x,int16_t y,int16_t w,int16_t h,uint16_t c) {
  if(w<=0||h<=0)return;
  if(x<0){w+=x;x=0;}if(y<0){h+=y;y=0;}
  if(x+w>W)w=W-x;if(y+h>H)h=H-y;if(w<=0||h<=0)return;
  lcdSetWindow(x,y,x+w-1,y+h-1);
  uint8_t d[64],hi=c>>8,lo=c;
  for(int i=0;i<64;i+=2){d[i]=hi;d[i+1]=lo;}
  SPI.beginTransaction(lcdSettings);digitalWrite(TFT_DC,HIGH);
  int n=w*h;while(n){int k=n>32?32:n;SPI.transferBytes(d,nullptr,k*2);n-=k;}
  SPI.endTransaction();
}

// =========================
// Compact clean bitmap font
// =========================
static const uint8_t font5x7[][5] = {
  {0,0,0,0,0},{0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},
  {0x3E,0x41,0x41,0x41,0x22},{0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},
  {0x7F,0x09,0x09,0x09,0x01},{0x3E,0x41,0x49,0x49,0x7A},{0x7F,0x08,0x08,0x08,0x7F},
  {0,0x41,0x7F,0x41,0},{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},
  {0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},
  {0x3E,0x41,0x41,0x41,0x3E},{0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},
  {0x7F,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7F,0x01,0x01},
  {0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},
  {0x63,0x14,0x08,0x14,0x63},{0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},
  {0x3E,0x45,0x49,0x51,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},
  {0x21,0x41,0x45,0x4B,0x31},{0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},
  {0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},{0x36,0x49,0x49,0x49,0x36},
  {0x06,0x49,0x49,0x29,0x1E}
};

int fontIndex(char c) {
  if(c==' ') return 0;
  if(c>='A'&&c<='Z') return c-'A'+1;
  if(c>='a'&&c<='z') return c-'a'+1;
  if(c>='0'&&c<='9') return c-'0'+27;
  return 0;
}

void text5(const String& s,int x,int y,int scale,uint16_t c) {
  for(size_t k=0;k<s.length();k++) {
    int idx=fontIndex(s[k]);
    for(int col=0;col<5;col++) for(int row=0;row<7;row++)
      if(font5x7[idx][col]&(1<<row)) fillRect(x+col*scale,y+row*scale,scale,scale,c);
    x += 6*scale;
  }
}

void centerText(const String& s,int y,int scale,uint16_t c) {
  int width=s.length()*6*scale- scale;
  text5(s,(W-width)/2,y,scale,c);
}

// =========================
// Segmented digital clock
// =========================
const uint8_t segMap[10]={
  0b1111110,0b0110000,0b1101101,0b1111001,0b0110011,
  0b1011011,0b1011111,0b1110000,0b1111111,0b1111011
};

void segH(int x,int y,int w,int t,uint16_t c){fillRect(x+t,y,w-2*t,t,c);}
void segV(int x,int y,int h,int t,uint16_t c){fillRect(x,y+t,t,h-2*t,c);}
void digit(int x,int y,int s,int n,uint16_t c) {
  int w=22*s,h=40*s,t=5*s;
  if(segMap[n]&64)segH(x,y,w,t,c);
  if(segMap[n]&32)segV(x+w-t,y,t==0?h/2:h/2,t,c);
  if(segMap[n]&16)segV(x+w-t,y+h/2,h/2,t,c);
  if(segMap[n]&8)segH(x,y+h-t,w,t,c);
  if(segMap[n]&4)segV(x,y+h/2,h/2,t,c);
  if(segMap[n]&2)segV(x,y,t==0?h/2:h/2,t,c);
  if(segMap[n]&1)segH(x,y,w,t,c);
}

void colon(int x,int y,int s,uint16_t c,bool on=true){
  if(on){fillRect(x,y+10*s,4*s,4*s,c);fillRect(x,y+27*s,4*s,4*s,c);}
}

void drawClock() {
  struct tm ti;
  if(!getLocalTime(&ti,20)) return;
  int hh=ti.tm_hour,mm=ti.tm_min,ss=ti.tm_sec;
  int s=2, dw=44, gap=8, total=4*dw+gap*3+12;
  int x=(W-total)/2,y=56;
  digit(x,y,s,hh/10,C_CYAN);x+=dw+gap;
  digit(x,y,s,hh%10,C_CYAN);x+=dw+6;
  colon(x,y,s,C_ORANGE,(ss%2)==0);x+=12;
  digit(x,y,s,mm/10,C_CYAN);x+=dw+gap;
  digit(x,y,s,mm%10,C_CYAN);
  if(showSeconds){
    String sec=String(ss<10?"0":"")+String(ss);
    centerText(sec,146,2,C_ORANGE);
  }
}

// =========================
// UI helpers
// =========================
String two(int n){return n<10?"0"+String(n):String(n);}
String dayName(int d){static const char* a[]={"SUN","MON","TUE","WED","THU","FRI","SAT"};return a[d%7];}
String monthName(int m){static const char* a[]={"JAN","FEB","MAR","APR","MAY","JUN","JUL","AUG","SEP","OCT","NOV","DEC"};return a[m%12];}

void topBar(const String& label,uint16_t accent) {
  fillRect(0,0,W,30,0x0820);
  fillRect(0,29,W,1,accent);
  text5(label,10,8,2,C_WHITE);
  int bars=WiFi.status()==WL_CONNECTED?map(WiFi.RSSI(),-90,-35,1,5):0;
  bars=constrain(bars,0,5);
  for(int i=0;i<5;i++) fillRect(193+i*7,22-(i+1)*3,5,(i+1)*3,i<bars?C_GREEN:0x294A);
  fillRect(229,9,7,12,WiFi.status()==WL_CONNECTED?C_GREEN:C_RED);
}

void footer(const String& s) {
  fillRect(0,222,W,18,0x0610);
  fillRect(0,222,W,1,C_DARK);
  text5(s,8,227,1,0xBDF7);
  String p=String((int)page+1)+"/"+String((int)PAGE_COUNT);
  text5(p,216,227,1,C_CYAN);
}

void crt() {
  if(!crtEffect)return;
  for(int y=31;y<222;y+=4) fillRect(0,y,W,1,0x0204);
  uint32_t seed=millis()/80;
  for(int i=0;i<10;i++){
    int x=(seed*17+i*31)%W;
    int y=32+(seed+i*17)%186;
    fillRect(x,y,1,1,0x4A69);
  }
}



// =========================
// Pages
// =========================
String weatherCondition();

void pageHome() {
  fillScreen(0x02050A);
  topBar("MINITV ULTRA",C_CYAN);

  // Cinematic background grid / horizon.
  for(int y=42;y<222;y+=18) fillRect(0,y,W,1,0x0820);
  for(int x=20;x<W;x+=40) fillRect(x,42,1,180,0x0615);

  // Status pill.
  fillRect(10,38,72,18,0x102A); rect(10,38,72,18,C_CYAN);
  text5(WiFi.status()==WL_CONNECTED?"ONLINE":"OFFLINE",18,44,1,
        WiFi.status()==WL_CONNECTED?C_GREEN:C_RED);

  // Large clock, deliberately spaced for the 240x240 panel.
  struct tm ti;
  if(getLocalTime(&ti,20)) {
    String hh=two(ti.tm_hour), mm=two(ti.tm_min);
    centerText(hh+":"+mm,64,4,C_WHITE);
    if(showSeconds) {
      String ss=two(ti.tm_sec);
      centerText(ss,112,2,C_ORANGE);
    }
    String d=dayName(ti.tm_wday)+"  "+monthName(ti.tm_mon)+" "+String(ti.tm_mday);
    centerText(d,139,2,C_YELLOW);
    centerText(String(ti.tm_year+1900),158,1,0x7BEF);
  } else {
    centerText("--:--",78,4,C_GRAY);
    centerText("WAITING FOR NTP",142,1,C_DIM);
  }

  // Weather card.
  fillRect(12,181,216,34,0x0B1724);
  rect(12,181,216,34,0x21435A);
  if(weatherOK) {
    text5("WEATHER",20,189,1,C_CYAN);
    text5(String(weatherTemp,0)+"C",80,187,2,C_WHITE);
    text5(weatherCondition(),134,190,1,C_YELLOW);
  } else {
    text5("WEATHER",20,189,1,C_CYAN);
    text5("WAITING...",80,190,1,C_DIM);
  }

  crt();
  footer("01  HOME  •  LIVE");
}

void drawTV(int ox,int oy,int scale) {
  fillRect(ox,oy,160*scale,132*scale,0x294A);
  rect(ox,oy,160*scale,132*scale,C_ORANGE);
  fillRect(ox+9*scale,oy+9*scale,142*scale,94*scale,C_BLACK);
  rect(ox+9*scale,oy+9*scale,142*scale,94*scale,C_YELLOW);
  // antenna
  drawFastH(ox+65*scale,oy-7*scale,30*scale,C_ORANGE);
  drawFastV(ox+80*scale,oy-20*scale,14*scale,C_ORANGE);
  // screen content
  int wave=(millis()/80)%18;
  for(int yy=0;yy<82;yy+=8) drawFastH(ox+14*scale,oy+(15+yy)*scale,132*scale,yy%16?C_BLUE:C_CYAN);
  fillRect(ox+(22+wave)*scale,oy+30*scale,34*scale,24*scale,C_MAGENTA);
  text5("ON AIR",ox+35*scale,oy+70*scale,2,C_WHITE);
  // knobs
  fillRect(ox+22*scale,oy+111*scale,8*scale,8*scale,C_GRAY);
  fillRect(ox+40*scale,oy+111*scale,8*scale,8*scale,C_GRAY);
  fillRect(ox+112*scale,oy+110*scale,8*scale,8*scale,C_ORANGE);
}

void pageRetro() {
  fillScreen(0x020308);
  topBar("RETRO TV",C_ORANGE);

  // Bezel.
  fillRect(17,40,206,137,0x1A1C24);
  rect(17,40,206,137,0xD67A16);
  rect(21,44,198,129,0x6B3C12);
  fillRect(29,52,182,101,0x001018);
  rect(29,52,182,101,C_ORANGE);

  // Animated broadcast raster.
  int phase=(millis()/90)%24;
  for(int y=58;y<148;y+=5) fillRect(34,y,172,1,(y+phase)%15==0?C_CYAN:0x08304A);
  fillRect(43+phase,76,46,30,C_MAGENTA);
  fillRect(97-phase/2,84,71,18,C_BLUE);
  text5("ON AIR",89,112,2,C_WHITE);

  // Channel badge + signal.
  fillRect(28,160,72,25,0x101C26); rect(28,160,72,25,C_YELLOW);
  text5("CH 07",38,168,2,C_YELLOW);
  text5("SIGNAL",112,162,1,C_DIM);
  for(int i=0;i<6;i++)
    fillRect(112+i*14,174,9,6,(i<((millis()/250)%7))?C_GREEN:C_GRAY);

  crt();
  footer("02  RETRO  •  BROADCAST");
}

void weatherIcon(int x,int y) {
  if(weatherCode<0){centerText("?",y,5,C_GRAY);return;}
  if(weatherCode<=1){
    fillRect(x+20,y+15,28,28,C_YELLOW);
    for(int i=0;i<8;i++){int dx=(i%4)*15,dy=(i/4)*22;drawFastH(x+7+dx,y+2+dy,8,C_YELLOW);}
  } else if(weatherCode<=3) {
    fillRect(x+16,y+14,48,24,C_DIM);fillRect(x+28,y+8,28,24,C_WHITE);
    fillRect(x+12,y+37,55,7,C_CYAN);
  } else {
    fillRect(x+12,y+17,55,7,C_DIM);fillRect(x+24,y+9,30,18,C_DIM);
    for(int i=0;i<4;i++) drawFastV(x+18+i*13,y+30,16,C_CYAN);
  }
}

String weatherCondition() {
  if(weatherCode==0)return "CLEAR";
  if(weatherCode<=2)return "PARTLY CLOUDY";
  if(weatherCode==3)return "OVERCAST";
  if(weatherCode>=95)return "STORM";
  if(weatherCode>=80)return "SHOWERS";
  if(weatherCode>=60)return "RAIN";
  return "CLOUDY";
}

void pageWeather() {
  fillScreen(0x03070B);
  topBar("WEATHER DESK",C_YELLOW);

  fillRect(10,40,220,72,0x0B1620);
  rect(10,40,220,72,0x254052);
  weatherIcon(18,47);

  if(weatherOK) {
    text5(String(weatherTemp,0)+"C",92,51,4,C_WHITE);
    text5(weatherCondition(),94,92,1,C_CYAN);
  } else {
    text5("--C",100,55,4,C_GRAY);
    text5("NO DATA",101,94,1,C_RED);
  }

  fillRect(10,120,105,42,0x0A111A); rect(10,120,105,42,0x203448);
  text5("HUMIDITY",18,128,1,C_DIM);
  text5(weatherOK?String(weatherHumidity)+"%":"--",18,143,2,C_WHITE);

  fillRect(120,120,110,42,0x0A111A); rect(120,120,110,42,0x203448);
  text5("WIND",128,128,1,C_DIM);
  text5(weatherOK?String(weatherWind,0)+" KM/H":"--",128,143,2,C_WHITE);

  fillRect(10,171,220,39,0x0B1620); rect(10,171,220,39,0x254052);
  text5("LOCATION",18,178,1,C_DIM);
  text5(weatherPlace.substring(0,25),18,192,1,C_YELLOW);
  text5(weatherOK?("UPDATED "+weatherUpdated):"NETWORK REQUIRED",132,192,1,C_CYAN);

  footer("03  WEATHER  •  OPEN-METEO");
}

void pageSystem() {
  fillScreen(0x030508);
  topBar("SYSTEM",C_GREEN);

  int y=41;
  String rows[]={
    "WIFI   "+String(WiFi.status()==WL_CONNECTED?"CONNECTED":"OFFLINE"),
    "RSSI   "+String(WiFi.status()==WL_CONNECTED?WiFi.RSSI():0)+" dBm",
    "IP     "+(WiFi.status()==WL_CONNECTED?WiFi.localIP().toString():"0.0.0.0"),
    "MDNS   minitv.local",
    "NTP    "+String(ntpOK?"SYNCED":"WAITING"),
    "UP     "+String((millis()-bootMillis)/1000)+" s",
    "HEAP   "+String(ESP.getFreeHeap()/1024)+" KB",
    "CPU    "+String(getCpuFrequencyMhz())+" MHz"
  };
  for(int i=0;i<8;i++){
    fillRect(10,y,220,20,(i&1)?0x081018:0x0B151E);
    text5(rows[i],16,y+6,1,(i==0)?(WiFi.status()==WL_CONNECTED?C_GREEN:C_RED):C_WHITE);
    y+=21;
  }
  footer("04  SYSTEM  •  DIAGNOSTICS");
}

void pageCustom() {
  fillScreen(0x03020A);
  topBar("CUSTOM CHANNEL",C_MAGENTA);

  int pulse=(millis()/20)%260-10;
  fillRect(10,42,220,118,0x10091A);
  rect(10,42,220,118,C_MAGENTA);
  fillRect(16,48,208,106,0x05030B);
  text5("CHANNEL 99",24,57,1,C_DIM);

  // Moving neon sweep behind the message.
  fillRect(pulse,76,54,54,0x3A0E55);
  fillRect((pulse+90)%250-10,89,42,30,0x143A54);

  text5(messageText.substring(0,30),20,91,2,C_WHITE);
  centerText(titleText.substring(0,22),137,1,C_CYAN);

  fillRect(10,169,220,39,0x0B1019);
  rect(10,169,220,39,0x27334A);
  text5("EDIT LIVE FROM",22,178,1,C_DIM);
  text5("MINITV.LOCAL",118,178,1,C_YELLOW);
  text5("WEB CONSOLE",78,194,1,C_CYAN);

  crt();
  footer("05  CUSTOM  •  USER CHANNEL");
}

void drawPage() {
  switch(page){
    case PAGE_HOME:pageHome();break;
    case PAGE_RETRO:pageRetro();break;
    case PAGE_WEATHER:pageWeather();break;
    case PAGE_SYSTEM:pageSystem();break;
    case PAGE_CUSTOM:pageCustom();break;
  }
}

// =========================
// Preferences
// =========================
void loadSettings() {
  prefs.begin("minitv",true);
  titleText=prefs.getString("title","SUPER MINITV");
  messageText=prefs.getString("msg","ON AIR");
  weatherPlace=prefs.getString("place","Davao City");
  autoRotate=prefs.getBool("rotate",true);
  rotateSeconds=prefs.getUShort("rsec",12);
  crtEffect=prefs.getBool("crt",true);
  showSeconds=prefs.getBool("secs",true);
  invertScreen=prefs.getBool("inv",false);
  prefs.end();
  if(rotateSeconds<3)rotateSeconds=3;
}

void saveSettings() {
  prefs.begin("minitv",false);
  prefs.putString("title",titleText);
  prefs.putString("msg",messageText);
  prefs.putString("place",weatherPlace);
  prefs.putBool("rotate",autoRotate);
  prefs.putUShort("rsec",rotateSeconds);
  prefs.putBool("crt",crtEffect);
  prefs.putBool("secs",showSeconds);
  prefs.putBool("inv",invertScreen);
  prefs.end();
}

void factoryReset() {
  prefs.begin("minitv",false);prefs.clear();prefs.end();loadSettings();
}

// =========================
// Time + weather
// =========================
void updateNtp() {
  configTime(28800,0,"pool.ntp.org","time.nist.gov","time.google.com");
  struct tm t;
  ntpOK=getLocalTime(&t,1000);
}

String urlEncode(const String& s) {
  String o;
  const char* hex="0123456789ABCDEF";
  for(size_t i=0;i<s.length();i++){
    uint8_t c=s[i];
    if(isalnum(c)||c=='-'||c=='_'||c=='.'||c=='~')o+=(char)c;
    else{o+='%';o+=hex[c>>4];o+=hex[c&15];}
  }
  return o;
}

void updateWeather() {
  if(WiFi.status()!=WL_CONNECTED)return;
  // Resolve coordinates from the place name. This keeps the web UI simple.
  WiFiClient client;
  HTTPClient http;
  String geo="http://geocoding-api.open-meteo.com/v1/search?name="+urlEncode(weatherPlace)+"&count=1&language=en&format=json";
  if(!http.begin(client,geo))return;
  int code=http.GET();
  if(code!=200){http.end();return;}
  String body=http.getString();http.end();
  int latPos=body.indexOf("\"latitude\":");
  int lonPos=body.indexOf("\"longitude\":");
  if(latPos<0||lonPos<0)return;
  float lat=body.substring(latPos+11).toFloat();
  float lon=body.substring(lonPos+12).toFloat();
  String api="http://api.open-meteo.com/v1/forecast?latitude="+String(lat,5)+"&longitude="+String(lon,5)+
             "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m&timezone=auto";
  if(!http.begin(client,api))return;
  code=http.GET();
  if(code!=200){http.end();return;}
  body=http.getString();http.end();

  int p=body.indexOf("\"temperature_2m\":");
  if(p>=0)weatherTemp=body.substring(p+17).toFloat();
  p=body.indexOf("\"relative_humidity_2m\":");
  if(p>=0)weatherHumidity=body.substring(p+23).toInt();
  p=body.indexOf("\"apparent_temperature\":");
  if(p>=0)weatherFeels=body.substring(p+22).toFloat();
  p=body.indexOf("\"weather_code\":");
  if(p>=0)weatherCode=body.substring(p+15).toInt();
  p=body.indexOf("\"wind_speed_10m\":");
  if(p>=0)weatherWind=body.substring(p+17).toFloat();
  weatherOK=!isnan(weatherTemp);
  struct tm t;if(getLocalTime(&t,20))weatherUpdated=two(t.tm_hour)+":"+two(t.tm_min);
}

// =========================
// Wi-Fi / mDNS
// =========================
void startNetworkServices() {
  if(!serverStarted){httpServer.begin();serverStarted=true;}
  if(!mdnsStarted && MDNS.begin(HOSTNAME)){
    MDNS.addService("http","tcp",80);
    mdnsStarted=true;
  }
}

void ensureWiFi() {
  if(WiFi.status()==WL_CONNECTED){
    startNetworkServices();
    return;
  }
  if(millis()-lastWifiAttempt<10000)return;
  lastWifiAttempt=millis();
  Serial.printf("[WiFi] reconnecting to %s\n",WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID,WIFI_PASSWORD);
}

// =========================
// HTML / HTTP
// =========================
String htmlEscape(String s) {
  s.replace("&","&amp;");s.replace("<","&lt;");s.replace(">","&gt;");
  s.replace("\"","&quot;");s.replace("'","&#39;");return s;
}

String pageHTML() {
  String h=R"HTML(<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Super MiniTV Ultra</title><style>
:root{--bg:#090b12;--card:#121725;--line:#28324a;--a:#00e5ff;--g:#32e875;--y:#ffd84d;--r:#ff5964}
*{box-sizing:border-box}body{margin:0;background:radial-gradient(circle at 50% 0,#17233a,#070910 65%);color:#eef5ff;font-family:system-ui,sans-serif}
main{max-width:760px;margin:auto;padding:18px}.hero{padding:20px 4px}.hero h1{margin:0;font-size:30px}.hero p{color:#91a0bb}
.card{background:#111725dd;border:1px solid var(--line);border-radius:18px;padding:16px;margin:12px 0;box-shadow:0 12px 40px #0007}
.grid{display:grid;grid-template-columns:repeat(2,1fr);gap:10px}.stat{background:#0b101c;border:1px solid #202b42;border-radius:12px;padding:12px}
label{display:block;color:#9aa8c2;font-size:12px;margin:12px 0 5px}input,select{width:100%;padding:12px;border-radius:10px;border:1px solid #33405c;background:#080d17;color:white}
button{border:0;border-radius:10px;padding:11px 14px;background:#1c2b43;color:white;font-weight:700;margin:5px 4px 0 0}button.primary{background:var(--a);color:#001016}
small{color:#7f8ca5}.ok{color:var(--g)}.warn{color:var(--y)}a{color:var(--a)}
</style></head><body><main>
<div class="hero"><h1>SUPER MINITV ULTRA</h1><p>ESP32-C3 retro display console</p></div>
<div class="card"><b>LIVE STATUS</b><div class="grid">
<div class="stat">Wi-Fi<br><strong class="ok">)HTML";
  h += (WiFi.status()==WL_CONNECTED?"CONNECTED":"OFFLINE");
  h += R"HTML(</strong><br><small>)HTML"+htmlEscape(WiFi.localIP().toString())+R"HTML(</small></div>
<div class="stat">mDNS<br><strong class="ok">minitv.local</strong></div>
<div class="stat">Page<br><strong>)HTML"+String((int)page+1)+R"HTML( / 5</strong></div>
<div class="stat">Weather<br><strong>)HTML"+(weatherOK?String(weatherTemp,1)+" °C":"OFFLINE")+R"HTML(</strong></div>
</div></div>
<div class="card"><b>DISPLAY</b><form method="POST" action="/save">
<label>Title</label><input name="title" value=")HTML"+htmlEscape(titleText)+R"HTML(" maxlength="30">
<label>Custom message</label><input name="msg" value=")HTML"+htmlEscape(messageText)+R"HTML(" maxlength="40">
<label>Start page</label><select name="page">)HTML";
  for(int i=0;i<PAGE_COUNT;i++) h+="<option value='"+String(i)+"'"+(page==i?" selected":"")+">"+String(i+1)+"</option>";
  h += R"HTML(</select><label>Auto rotate seconds</label><input name="rsec" type="number" min="3" max="120" value=")HTML"+String(rotateSeconds)+R"HTML(">
<label><input style="width:auto" type="checkbox" name="rotate" )HTML"+(autoRotate?"checked":"")+R"HTML(> Auto rotate</label>
<label><input style="width:auto" type="checkbox" name="crt" )HTML"+(crtEffect?"checked":"")+R"HTML(> CRT scanlines/noise</label>
<label><input style="width:auto" type="checkbox" name="secs" )HTML"+(showSeconds?"checked":"")+R"HTML(> Show seconds</label>
<button class="primary">APPLY SETTINGS</button></form></div>
<div class="card"><b>WEATHER</b><form method="POST" action="/weather">
<label>City / place</label><input name="place" value=")HTML"+htmlEscape(weatherPlace)+R"HTML(" maxlength="50">
<button class="primary">SAVE + UPDATE WEATHER</button></form>
<p><small>Provider: Open-Meteo • no API key required</small></p></div>
<div class="card"><b>QUICK CONTROL</b>
<form method="POST" action="/page"><button name="p" value="0">HOME</button><button name="p" value="1">RETRO</button><button name="p" value="2">WEATHER</button><button name="p" value="3">SYSTEM</button><button name="p" value="4">CUSTOM</button></form>
</div>
<div class="card"><b>FIRMWARE</b>
<p><small>Web OTA is available on the same LAN. Upload the PlatformIO firmware.bin directly.</small></p>
<input id="fw" type="file" accept=".bin" required>
<button class="primary" onclick="ota();return false">UPLOAD FIRMWARE</button>
<small id="otaStatus"></small>
<script>
async function ota(){
 const f=document.getElementById('fw').files[0];
 const st=document.getElementById('otaStatus');
 if(!f){st.textContent='Select firmware.bin first';return;}
 if(!f.name.endsWith('.bin')){st.textContent='Select a .bin firmware file';return;}
 st.textContent='Uploading '+f.size+' bytes...';
 try{
   const r=await fetch('/ota',{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:f});
   st.textContent=await r.text();
 }catch(e){st.textContent='OTA connection failed';}
}
</script>
</div>
<div class="card"><b>SYSTEM</b><p>Uptime: )HTML"+String((millis()-bootMillis)/1000)+R"HTML( s<br>Heap: )HTML"+String(ESP.getFreeHeap())+R"HTML( bytes<br>RSSI: )HTML"+String(WiFi.RSSI())+R"HTML( dBm<br>CPU: )HTML"+String(getCpuFrequencyMhz())+R"HTML( MHz</p>
<form method="POST" action="/factory" onsubmit="return confirm('Reset saved MiniTV settings?')"><button>FACTORY RESET SETTINGS</button></form></div>
<div class="card"><small>Firmware: Super MiniTV Ultra • )HTML"+String(__DATE__)+" "+String(__TIME__)+R"HTML(</small></div>
</main></body></html>)HTML";
  return h;
}

String readRequest(WiFiClient& c) {
  String req=c.readStringUntil('\n');
  req.trim();
  lastHttp=req;
  requestContentLength=0;

  while(c.connected()){
    String line=c.readStringUntil('\n');
    if(line=="\r"||line.length()==0)break;
    String lower=line;
    lower.toLowerCase();
    if(lower.startsWith("content-length:")){
      requestContentLength=(size_t)lower.substring(15).toInt();
    }
  }
  return req;
}

String readBody(WiFiClient& c) {
  String body;
  body.reserve(requestContentLength);
  unsigned long deadline=millis()+2000;
  while(body.length()<requestContentLength && c.connected() && millis()<deadline){
    while(c.available() && body.length()<requestContentLength){
      body+=(char)c.read();
    }
    if(body.length()<requestContentLength) delay(1);
  }
  return body;
}

void httpReply(WiFiClient& c,const String& body,int code=200,const char* type="text/html");

void handleOta(WiFiClient& c) {
  if(requestContentLength==0 || requestContentLength > 1900000UL){
    httpReply(c,"Invalid firmware size",400,"text/plain");
    return;
  }

  if(!Update.begin(requestContentLength)){
    httpReply(c,"OTA begin failed",500,"text/plain");
    return;
  }

  uint8_t buffer[1024];
  size_t received=0;
  unsigned long deadline=millis()+30000;

  while(received<requestContentLength && c.connected() && millis()<deadline){
    int available=c.available();
    if(available<=0){ delay(1); continue; }
    size_t want=requestContentLength-received;
    if(want>sizeof(buffer)) want=sizeof(buffer);
    if((size_t)available<want) want=available;
    int n=c.read(buffer,want);
    if(n>0){
      if(Update.write(buffer,n)!=(size_t)n){
        Update.abort();
        httpReply(c,"OTA write failed",500,"text/plain");
        return;
      }
      received+=(size_t)n;
    }
  }

  if(received!=requestContentLength){
    Update.abort();
    httpReply(c,"OTA upload incomplete",400,"text/plain");
    return;
  }

  if(!Update.end(true)){
    httpReply(c,"OTA validation failed",500,"text/plain");
    return;
  }

  httpReply(c,"OTA OK - rebooting",200,"text/plain");
  delay(500);
  ESP.restart();
}

String formValue(const String& body,const String& key) {
  String k=key+"=";int p=body.indexOf(k);if(p<0)return "";
  p+=k.length();int e=body.indexOf('&',p);if(e<0)e=body.length();
  String v=body.substring(p,e);v.replace("+"," ");
  String out;
  for(int i=0;i<(int)v.length();i++){
    if(v[i]=='%'&&i+2<(int)v.length()){
      char hex[3]={v[i+1],v[i+2],0};out+=(char)strtol(hex,nullptr,16);i+=2;
    }else out+=v[i];
  }
  return out;
}

void httpReply(WiFiClient& c,const String& body,int code,const char* type) {
  String status=code==200?"200 OK":(code==303?"303 See Other":"400 Bad Request");
  c.printf("HTTP/1.1 %s\r\nContent-Type: %s; charset=utf-8\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: %u\r\n\r\n",
           status.c_str(),type,(unsigned)body.length());
  c.print(body);
}

void redirect(WiFiClient& c,const char* path="/") {
  c.printf("HTTP/1.1 303 See Other\r\nLocation: %s\r\nConnection: close\r\nContent-Length: 0\r\n\r\n",path);
}

void handleHttp() {
  WiFiClient c=httpServer.available();if(!c)return;
  c.setTimeout(500);
  String req=readRequest(c);
  String method=req.substring(0,req.indexOf(' '));
  int a=req.indexOf(' '),b=req.indexOf(' ',a+1);
  String path=(a>=0&&b>a)?req.substring(a+1,b):"/";
  if(path=="/ota"&&method=="POST"){
    handleOta(c);
    c.stop();
    return;
  }

  String body;
  if(method=="POST")body=readBody(c);

  if(path=="/"){
    httpReply(c,pageHTML());
  } else if(path=="/save"&&method=="POST"){
    String v=formValue(body,"title");if(v.length())titleText=v;
    v=formValue(body,"msg");if(v.length())messageText=v;
    v=formValue(body,"rsec");if(v.length())rotateSeconds=constrain(v.toInt(),3,120);
    autoRotate=body.indexOf("rotate=")>=0;
    crtEffect=body.indexOf("crt=")>=0;
    showSeconds=body.indexOf("secs=")>=0;
    v=formValue(body,"page");if(v.length())page=(Page)constrain(v.toInt(),0,(int)PAGE_COUNT-1);
    saveSettings();redirect(c);
  } else if(path=="/weather"&&method=="POST"){
    String v=formValue(body,"place");if(v.length())weatherPlace=v;
    saveSettings();weatherOK=false;updateWeather();redirect(c);
  } else if(path=="/page"&&method=="POST"){
    String v=formValue(body,"p");if(v.length())page=(Page)constrain(v.toInt(),0,(int)PAGE_COUNT-1);
    lastRotate=millis();redirect(c);
  } else if(path=="/ota"&&method=="GET"){
    httpReply(c,"Use the MiniTV web console to upload firmware.bin.");
  } else if(path=="/factory"&&method=="POST"){
    factoryReset();redirect(c);
  } else {
    httpReply(c,"Not found",404);
  }
  delay(1);c.stop();
}

// =========================
// Setup / loop
// =========================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("========================================");
  Serial.println(" SUPER MINITV ULTRA");
  Serial.println(" ESP32-C3 + ST7789 240x240");
  Serial.println("========================================");

  bootMillis=millis();
  loadSettings();
  lcdInit();
  fillScreen(C_BLACK);

  // Proven STA-only network strategy
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID,WIFI_PASSWORD);

  Serial.printf("[WiFi] connecting to %s\n",WIFI_SSID);
  unsigned long t0=millis();
  while(WiFi.status()!=WL_CONNECTED && millis()-t0<15000){
    delay(250);Serial.print(".");
  }
  Serial.println();

  if(WiFi.status()==WL_CONNECTED){
    Serial.printf("[WiFi] CONNECTED  IP=%s RSSI=%d\n",WiFi.localIP().toString().c_str(),WiFi.RSSI());
    startNetworkServices();
    updateNtp();
    updateWeather();
  }else{
    Serial.println("[WiFi] not connected; will retry in background");
  }

  drawPage();
}

void loop() {
  ensureWiFi();

  if(WiFi.status()==WL_CONNECTED){
    if(!ntpOK || millis()-lastNtpPrint>60000){updateNtp();lastNtpPrint=millis();}
    if(millis()-lastWeather>900000 || (weatherUpdated=="--" && millis()-lastWeather>15000)){
      lastWeather=millis();updateWeather();
    }
  }

  handleHttp();

  if(autoRotate && millis()-lastRotate>rotateSeconds*1000UL){
    page=(Page)(((int)page+1)%PAGE_COUNT);
    lastRotate=millis();
  }

  if(millis()-lastFrame>1000/12){
    lastFrame=millis();
    drawPage();
  }
}
