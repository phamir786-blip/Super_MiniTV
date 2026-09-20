#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <Update.h>
#include <LittleFS.h>
#include <time.h>
#include "config.h"
#include "minitv_player.h"

#define C_BLACK 0x0000
#define C_WHITE 0xFFFF
#define C_DIM 0x7BEF
#define C_RED 0xF800
#define C_GREEN 0x07E0
#define C_CYAN 0x07FF
#define C_BLUE 0x001F
#define C_YELLOW 0xFFE0
#define C_ORANGE 0xFD20
#define C_MAGENTA 0xF81F
#define C_GRAY 0x4208
#define C_DARK 0x1082

SPISettings lcdSettings(40000000,MSBFIRST,SPI_MODE3);
int16_t minitvImageX=0,minitvImageY=0;
static const int W=240,H=240;
static WiFiServer server(80);
static Preferences prefs;
static const char* HOSTNAME="minitv";
static const char* WIFI_SSID="YOUR_WIFI_SSID";
static const char* WIFI_PASSWORD="YOUR_WIFI_PASSWORD";
static bool mdnsOK=false;
static size_t contentLength=0;
static String lastRequest="-";
static uint32_t bootMs=0,lastUi=0,lastRotate=0;
static bool autoRotate=true;
static uint16_t rotateSeconds=12;
static uint8_t screen=0;
static String weatherPlace="Davao City";
static float weatherTemp=NAN;
static int weatherCode=-1,weatherHumidity=-1;
static float weatherWind=NAN;
static bool weatherOK=false;

static uint16_t rgb(uint32_t c){if(c<=0xffff)return(uint16_t)c;return(uint16_t)(((c>>19)<<11)|(((c>>10)&63)<<5)|(c>>3));}
void lcdCommand(uint8_t c){SPI.beginTransaction(lcdSettings);digitalWrite(TFT_DC,LOW);SPI.transfer(c);SPI.endTransaction();}
void lcdCommandData(uint8_t c,const uint8_t*d,uint16_t n){SPI.beginTransaction(lcdSettings);digitalWrite(TFT_DC,LOW);SPI.transfer(c);digitalWrite(TFT_DC,HIGH);while(n--)SPI.transfer(*d++);SPI.endTransaction();}
void lcdSetWindow(uint16_t x0,uint16_t y0,uint16_t x1,uint16_t y1){
  uint8_t d[4]={uint8_t(x0>>8),uint8_t(x0),uint8_t(x1>>8),uint8_t(x1)};lcdCommandData(0x2a,d,4);
  d[0]=y0>>8;d[1]=y0;d[2]=y1>>8;d[3]=y1;lcdCommandData(0x2b,d,4);lcdCommand(0x2c);
}
void minitvPush565Line(const uint16_t*p,int n){SPI.beginTransaction(lcdSettings);digitalWrite(TFT_DC,HIGH);for(int i=0;i<n;i++){SPI.transfer(p[i]>>8);SPI.transfer(p[i]);}SPI.endTransaction();}
void fillRect(int x,int y,int w,int h,uint32_t raw){
  if(w<=0||h<=0)return;if(x<0){w+=x;x=0;}if(y<0){h+=y;y=0;}if(x+w>W)w=W-x;if(y+h>H)h=H-y;if(w<=0||h<=0)return;
  uint16_t c=rgb(raw);lcdSetWindow(x,y,x+w-1,y+h-1);uint8_t b[64];for(int i=0;i<64;i+=2){b[i]=c>>8;b[i+1]=c;}
  SPI.beginTransaction(lcdSettings);digitalWrite(TFT_DC,HIGH);int n=w*h;while(n){int k=min(n,32);SPI.transferBytes(b,nullptr,k*2);n-=k;}SPI.endTransaction();
}
void fillScreen(uint32_t c){fillRect(0,0,W,H,c);}
void rect(int x,int y,int w,int h,uint32_t c){fillRect(x,y,w,1,c);fillRect(x,y+h-1,w,1,c);fillRect(x,y,1,h,c);fillRect(x+w-1,y,1,h,c);}
void lcdInit(){
  pinMode(TFT_DC,OUTPUT);pinMode(TFT_RST,OUTPUT);SPI.begin(TFT_SCK,-1,TFT_MOSI,-1);
  digitalWrite(TFT_RST,HIGH);delay(20);digitalWrite(TFT_RST,LOW);delay(120);digitalWrite(TFT_RST,HIGH);delay(120);
  lcdCommand(0x01);delay(150);lcdCommand(0x11);delay(120);
  uint8_t pf=0x55,mc=0x00;lcdCommandData(0x3a,&pf,1);lcdCommandData(0x36,&mc,1);lcdCommand(0x21);lcdCommand(0x13);lcdCommand(0x29);delay(80);
}
static const uint8_t font[][5]={{0,0,0,0,0},{0x7e,0x11,0x11,0x11,0x7e},{0x7f,0x49,0x49,0x49,0x36},{0x3e,0x41,0x41,0x41,0x22},{0x7f,0x41,0x41,0x22,0x1c},{0x7f,0x49,0x49,0x49,0x41},{0x7f,9,9,9,1},{0x3e,0x41,0x49,0x49,0x7a},{0x7f,8,8,8,0x7f},{0,0x41,0x7f,0x41,0},{0x20,0x40,0x41,0x3f,1},{0x7f,8,0x14,0x22,0x41},{0x7f,0x40,0x40,0x40,0x40},{0x7f,2,0x0c,2,0x7f},{0x7f,4,8,0x10,0x7f},{0x3e,0x41,0x41,0x41,0x3e},{0x7f,9,9,9,6},{0x3e,0x41,0x51,0x21,0x5e},{0x7f,9,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},{1,1,0x7f,1,1},{0x3f,0x40,0x40,0x40,0x3f},{0x1f,0x20,0x40,0x20,0x1f},{0x3f,0x40,0x38,0x40,0x3f},{0x63,0x14,8,0x14,0x63},{7,8,0x70,8,7},{0x61,0x51,0x49,0x45,0x43},{0x3e,0x45,0x49,0x51,0x3e},{0,0x42,0x7f,0x40,0},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4b,0x31},{0x18,0x14,0x12,0x7f,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3c,0x4a,0x49,0x49,0x30},{1,0x71,9,5,3},{0x36,0x49,0x49,0x49,0x36},{6,0x49,0x49,0x29,0x1e}};
int fi(char c){if(c==' ')return 0;if(c>='A'&&c<='Z')return c-'A'+1;if(c>='a'&&c<='z')return c-'a'+1;if(c>='0'&&c<='9')return c-'0'+27;return 0;}
void text5(const String&s,int x,int y,int sc,uint16_t c){for(char ch:s){int i=fi(ch);for(int a=0;a<5;a++)for(int b=0;b<7;b++)if(font[i][a]&(1<<b))fillRect(x+a*sc,y+b*sc,sc,sc,c);x+=6*sc;}}
void center(const String&s,int y,int sc,uint16_t c){int w=s.length()*6*sc-sc;text5(s,(W-w)/2,y,sc,c);}
String two(int n){return n<10?"0"+String(n):String(n);}
String day(int n){static const char*a[]={"SUN","MON","TUE","WED","THU","FRI","SAT"};return a[n%7];}
String mon(int n){static const char*a[]={"JAN","FEB","MAR","APR","MAY","JUN","JUL","AUG","SEP","OCT","NOV","DEC"};return a[n%12];}
void top(const String&s,uint16_t a){fillRect(0,0,W,30,0x0820);fillRect(0,29,W,1,a);text5(s,8,8,2,C_WHITE);fillRect(229,9,7,12,WiFi.status()==WL_CONNECTED?C_GREEN:C_RED);}
void footer(const String&s){fillRect(0,222,W,18,0x0610);text5(s,8,227,1,C_DIM);}
void retroTransition(){for(int y=0;y<120;y+=8){fillRect(0,y,W,4,C_ORANGE);delay(8);fillRect(0,y,W,4,C_BLACK);}}
String weatherName(){if(weatherCode==0)return"CLEAR";if(weatherCode<=2)return"CLOUDY";if(weatherCode==3)return"OVERCAST";if(weatherCode>=95)return"STORM";if(weatherCode>=80)return"SHOWERS";if(weatherCode>=60)return"RAIN";return"CLOUDY";}
void drawHome(){
  fillScreen(0x02050a);top("MINITV ULTRA",C_CYAN);struct tm t;
  if(getLocalTime(&t,10)){center(two(t.tm_hour)+":"+two(t.tm_min),60,4,C_WHITE);center(two(t.tm_sec),110,2,C_ORANGE);center(day(t.tm_wday)+" "+mon(t.tm_mon)+" "+two(t.tm_mday)+" "+String(t.tm_year+1900),140,1,C_YELLOW);}
  else center("--:--",80,4,C_GRAY);
  fillRect(12,178,216,38,0x0b1724);rect(12,178,216,38,0x21435a);
  text5("WEATHER",20,187,1,C_CYAN);text5(weatherOK?String(weatherTemp,0)+"C":"--C",88,184,2,C_WHITE);text5(weatherOK?weatherName():"WAITING",145,189,1,C_YELLOW);footer("CLOCK / DATE / WEATHER");
}
void drawRetro(){fillScreen(0x020308);top("RETRO TV",C_ORANGE);fillRect(16,42,208,132,0x1a1c24);rect(16,42,208,132,C_ORANGE);fillRect(28,54,184,96,0x001018);rect(28,54,184,96,C_YELLOW);int p=(millis()/70)%34;for(int y=60;y<146;y+=6)fillRect(34,y,172,1,(y+p)%18==0?C_CYAN:0x08304a);fillRect(45+p,78,42,28,C_MAGENTA);fillRect(100-p/2,84,64,18,C_BLUE);center("ON AIR",116,2,C_WHITE);footer("RETRO BROADCAST");}
void drawWeather(){fillScreen(0x03070b);top("WEATHER",C_YELLOW);fillRect(10,42,220,72,0x0b1620);rect(10,42,220,72,0x254052);if(weatherOK){text5(String(weatherTemp,0)+"C",24,58,4,C_WHITE);text5(weatherName(),28,100,1,C_CYAN);text5("HUM "+String(weatherHumidity)+"%",135,64,1,C_DIM);text5("WIND "+String(weatherWind,0),135,80,1,C_DIM);}else center("NO DATA",70,2,C_RED);fillRect(10,124,220,74,0x0a111a);text5("LOCATION",18,134,1,C_DIM);text5(weatherPlace.substring(0,28),18,151,2,C_YELLOW);footer("OPEN-METEO");}
void drawScreen(){if(screen==0)drawHome();else if(screen==1)drawRetro();else drawWeather();}
void loadSettings(){prefs.begin("mini",true);screen=prefs.getUChar("screen",0);autoRotate=prefs.getBool("rotate",true);rotateSeconds=prefs.getUShort("rsec",12);weatherPlace=prefs.getString("place","Davao City");prefs.end();}
void saveSettings(){prefs.begin("mini",false);prefs.putUChar("screen",screen);prefs.putBool("rotate",autoRotate);prefs.putUShort("rsec",rotateSeconds);prefs.putString("place",weatherPlace);prefs.end();}
String enc(const String&s){String o;const char*h="0123456789ABCDEF";for(char c:s){if(isalnum((unsigned char)c)||c=='/'||c=='_'||c=='-'||c=='.')o+=c;else{o+='%';o+=h[((uint8_t)c)>>4];o+=h[((uint8_t)c)&15];}}return o;}
String dec(String s){s.replace("%2F","/");s.replace("%2f","/");s.replace("%20"," ");s.replace("+"," ");return s;}
String queryValue(const String&p,const String&key){String k=key+"=";int a=p.indexOf(k);if(a<0)return"";a+=k.length();int b=p.indexOf('&',a);if(b<0)b=p.length();return dec(p.substring(a,b));}
String html(){
  String h=R"HTML(<!doctype html><meta name=viewport content="width=device-width,initial-scale=1"><title>MiniTV Ultra</title><style>body{background:#070a10;color:#eef;font:16px system-ui;margin:0}main{max-width:760px;margin:auto;padding:18px}.c{background:#111827;border:1px solid #29344a;border-radius:16px;padding:16px;margin:12px 0}button,input,select{padding:11px;border-radius:9px;background:#0a1020;color:#fff;border:1px solid #34415c}button{font-weight:700}.p{background:#00d9ff;color:#001015}.grid{display:grid;grid-template-columns:1fr 1fr;gap:8px}small{color:#98a7c0}</style><main><div class=c><h1>SUPER MINITV ULTRA</h1><small>ESP32-C3 • ST7789 240×240 • image/animation only</small></div>
  <div class=c><b>MEDIA PLAYER</b><p>Current: )HTML"+(minitvCurrent.length()?minitvCurrent:"idle")+R"HTML(</p><p>Channel )HTML"+String(minitvChannel)+R"HTML( / )HTML"+String(minitvChannelCount)+R"HTML(</p>
  <a href="/media?cmd=start"><button class=p>PLAY</button></a> <a href="/media?cmd=stop"><button>STOP</button></a> <a href="/media?cmd=next"><button>NEXT</button></a> <a href="/media?cmd=random"><button>RANDOM</button></a></div>
  <div class=c><b>UPLOAD MEDIA</b><p><small>GIF, JPG, JPEG. Path example: /Videos/1/logo.gif</small></p><form id=u><input id=f type=file required><input id=path placeholder="/Videos/1/file.gif" required><button class=p>UPLOAD</button></form><p id=s></p></div>
  <div class=c><b>MEDIA FILES</b><pre>)HTML"+minitvMediaList()+R"HTML(</pre><small>Use DELETE below with an exact path.</small><form id=d><input id=dp placeholder="/Videos/1/file.gif"><button>DELETE</button></form></div>
  <div class=c><b>SCREENS</b><form method=post action=/screen><button name=s value=0>CLOCK</button><button name=s value=1>RETRO</button><button name=s value=2>WEATHER</button></form><form method=post action=/settings><label>Weather place</label><input name=place value=")HTML"+weatherPlace+R"HTML("><br><button class=p>SAVE</button></form></div>
  <div class=c><b>OTA</b><input id=fw type=file accept=.bin><button onclick=ota()>UPLOAD FIRMWARE</button><p id=o></p></div>
  <div class=c><b>SYSTEM</b><p>mDNS: <b>minitv.local</b><br>Wi-Fi: )HTML"+String(WiFi.status()==WL_CONNECTED?"connected":"offline")+R"HTML(<br>Heap: )HTML"+String(ESP.getFreeHeap())+R"HTML(</p></div>
  <script>
  u.onsubmit=async e=>{e.preventDefault();let f=document.querySelector('#f').files[0],p=document.querySelector('#path').value;let r=await fetch('/upload?path='+encodeURIComponent(p),{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:f});s.textContent=await r.text();location.reload()};
  d.onsubmit=async e=>{e.preventDefault();let p=document.querySelector('#dp').value;let r=await fetch('/delete?path='+encodeURIComponent(p),{method:'POST'});s.textContent=await r.text();location.reload()};
  async function ota(){let f=document.querySelector('#fw').files[0];if(!f)return;o.textContent='Uploading...';let r=await fetch('/ota',{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:f});o.textContent=await r.text();}
  </script></main>)HTML";
  return h;
}
void reply(WiFiClient&c,const String&b,int code=200,const char*t="text/html"){const char*s=code==200?"200 OK":code==303?"303 See Other":code==404?"404 Not Found":"400 Bad Request";c.printf("HTTP/1.1 %s\r\nContent-Type: %s; charset=utf-8\r\nConnection: close\r\nContent-Length: %u\r\n\r\n",s,t,(unsigned)b.length());c.print(b);}
void redirect(WiFiClient&c){c.print("HTTP/1.1 303 See Other\r\nLocation: /\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");}
void headers(WiFiClient&c){String line;while(c.connected()){line=c.readStringUntil('\n');if(line=="\r"||line.length()==0)break;String l=line;l.toLowerCase();if(l.startsWith("content-length:"))contentLength=(size_t)l.substring(15).toInt();}}
String request(WiFiClient&c){String r=c.readStringUntil('\n');r.trim();lastRequest=r;contentLength=0;headers(c);return r;}
String readBody(WiFiClient&c){String b;size_t left=contentLength;uint32_t until=millis()+5000;while(left&&c.connected()&&millis()<until){while(c.available()&&left){char ch=(char)c.read();b+=ch;--left;}if(left)delay(1);}return b;}
void handleUpload(WiFiClient&c,const String&path){
  if(!minitvSafePath(path)||!minitvIsImage(path)||contentLength==0||contentLength>MINITV_MAX_UPLOAD){reply(c,"Invalid media path or size",400,"text/plain");return;}
  minitvEnsureDir(path);File f=LittleFS.open(path,FILE_WRITE);if(!f){reply(c,"Open failed",500,"text/plain");return;}
  uint8_t buf[1024];size_t left=contentLength;uint32_t until=millis()+30000;
  while(left && c.connected() && millis()<until){int n=c.read(buf,min((size_t)sizeof(buf),left));if(n>0){f.write(buf,n);left-=n;}else delay(1);}
  f.close();if(left){LittleFS.remove(path);reply(c,"Upload incomplete",400,"text/plain");return;}minitvScanChannels();reply(c,"UPLOAD OK",200,"text/plain");
}
void handleOta(WiFiClient&c){
  if(!contentLength||contentLength>1900000UL){reply(c,"Invalid firmware size",400,"text/plain");return;}
  if(!Update.begin(contentLength)){reply(c,"OTA begin failed",500,"text/plain");return;}
  uint8_t b[1024];size_t left=contentLength;uint32_t until=millis()+60000;
  while(left&&c.connected()&&millis()<until){int n=c.read(b,min((size_t)sizeof(b),left));if(n>0){if(Update.write(b,n)!=(size_t)n){Update.abort();reply(c,"OTA write failed",500,"text/plain");return;}left-=n;}else delay(1);}
  if(left||!Update.end(true)){Update.abort();reply(c,"OTA failed",500,"text/plain");return;}
  reply(c,"OTA OK - rebooting",200,"text/plain");delay(500);ESP.restart();
}
void http(){
  WiFiClient c=server.available();if(!c)return;c.setTimeout(500);
  String req=request(c);int a=req.indexOf(' '),b=req.indexOf(' ',a+1);String path=(a>=0&&b>a)?req.substring(a+1,b):"/";String route=path;int q=route.indexOf('?');if(q>=0)route=route.substring(0,q);
  if(route=="/upload"&&req.startsWith("POST")){handleUpload(c,queryValue(path,"path"));c.stop();return;}
  if(route=="/ota"&&req.startsWith("POST")){handleOta(c);c.stop();return;}
  if(route=="/delete"&&req.startsWith("POST")){String p=queryValue(path,"path");bool ok=minitvDelete(p);reply(c,ok?"DELETE OK":"DELETE FAILED",ok?200:400,"text/plain");c.stop();return;}
  if(req.startsWith("GET /media?")){String cmd=queryValue(path,"cmd");if(cmd=="start")minitvStartPlayback();else if(cmd=="stop")minitvStopPlayback();else if(cmd=="next")minitvNextChannel(1);else if(cmd=="random"){minitvRandomMode=!minitvRandomMode;minitvStopPlayback();minitvStartPlayback();}reply(c,minitvStatusJson(),200,"application/json");c.stop();return;}
  if(route=="/"&&req.startsWith("GET"))reply(c,html());
  else if(route=="/screen"&&req.startsWith("POST")){String body=c.readStringUntil('\r');(void)body;screen=(uint8_t)constrain(queryValue(path,"s").toInt(),0,2);saveSettings();redirect(c);}
  else if(route=="/settings"&&req.startsWith("POST")){String body=readBody(c);int p=body.indexOf("place=");if(p>=0){p+=6;int e=body.indexOf('&',p);weatherPlace=dec(e<0?body.substring(p):body.substring(p,e));saveSettings();}redirect(c);}
  else reply(c,"Not found",404,"text/plain");
  c.stop();
}
void weather(){
  if(WiFi.status()!=WL_CONNECTED)return;WiFiClient cl;HTTPClient h;
  String g="http://geocoding-api.open-meteo.com/v1/search?name="+weatherPlace+"&count=1&format=json";if(!h.begin(cl,g))return;int code=h.GET();if(code!=200){h.end();return;}String s=h.getString();h.end();
  int a=s.indexOf("\"latitude\":"),b=s.indexOf("\"longitude\":");if(a<0||b<0)return;float lat=s.substring(a+11).toFloat(),lon=s.substring(b+12).toFloat();
  String u="http://api.open-meteo.com/v1/forecast?latitude="+String(lat,5)+"&longitude="+String(lon,5)+"&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m&timezone=auto";
  if(!h.begin(cl,u))return;code=h.GET();if(code!=200){h.end();return;}s=h.getString();h.end();
  a=s.indexOf("\"temperature_2m\":");if(a>=0)weatherTemp=s.substring(a+17).toFloat();a=s.indexOf("\"relative_humidity_2m\":");if(a>=0)weatherHumidity=s.substring(a+23).toInt();a=s.indexOf("\"weather_code\":");if(a>=0)weatherCode=s.substring(a+15).toInt();a=s.indexOf("\"wind_speed_10m\":");if(a>=0)weatherWind=s.substring(a+17).toFloat();weatherOK=!isnan(weatherTemp);
}
void network(){
  if(WiFi.status()!=WL_CONNECTED){WiFi.mode(WIFI_STA);WiFi.setHostname(HOSTNAME);WiFi.setSleep(false);WiFi.begin(WIFI_SSID,WIFI_PASSWORD);return;}
  if(!mdnsOK){if(MDNS.begin(HOSTNAME)){MDNS.addService("http","tcp",80);mdnsOK=true;}}
}
void setup(){
  Serial.begin(115200);delay(200);bootMs=millis();loadSettings();lcdInit();fillScreen(C_BLACK);retroTransition();minitvBegin();network();delay(1000);weather();server.begin();drawScreen();
}
void loop(){
  network();http();minitvTick();
  if(WiFi.status()==WL_CONNECTED && millis()%900000<50)weather();
  if(!minitvRunning){
    if(autoRotate&&millis()-lastRotate>rotateSeconds*1000UL){screen=(screen+1)%3;lastRotate=millis();retroTransition();}
    if(millis()-lastUi>1000/10){lastUi=millis();drawScreen();}
  }
}
