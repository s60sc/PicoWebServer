
// s60sc 2021

#ifndef ESPWEBSERVER
#define ESPWEBSERVER

// user defined values
#define WIFISSID "****" // wifi SSID
#define WIFIPASS "****" // wifi password
#define STATICIP "192.168.1.135" // static IP for PicoWebServer
#define GATEWAY "192.168.1.1" // gateway (eg router)
#define TIMEZOME 0 // +/- local time offset in hours from UTC

// usr modifiable
#define RESETPIN 2  // Pico pin used to connect to ESP8266 RST
#define BLINKRATE 1 // in secs (can be fraction)
#define MUTEXWAIT 100 // time in ms for ESP8266 gpio functions to wait on mutex
#define NTPRETRIES 5 // max attempts to get current time from NTP
#define RESPONSEBUFFERLEN 1000 // size of buffer to receive data from web client(max 2048)
#define SENDBUFFERLEN 500 // size of buffer to send data to web client (max 2048)

// used for ESP8266 gpio 
enum {ESP_INPUT, ESP_OUTPUT};  // ESP8266 pin direction
enum {ESP_PULLUP, ESP_NOPULLUP}; // ESP8266 pin pullup

#define MICROS 1000000 // microseconds per sec
extern char datetimeStr[]; // holds current RTC time

// public functions
void setupUART();
void setupESP8266();
bool startWebServer();
void serveClients();
void appResponse(const char* appResp);
void doRestart(const char* fatalMsg);
uintptr_t* webInput();
void getTOD();
bool ESP8266pinMode(int pin, int direction, int pullup);
int ESP8266digitalRead(int pin);
bool ESP8266digitalWrite(int pin, bool value);
float ESP8266analogRead();

#endif