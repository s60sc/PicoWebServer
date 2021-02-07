/*
  Provides an example of using PicoWebServer to display the content of PicoWSpage.h on a browser. 
  The web page refreshes every 10 seconds using AJAX and JSON.

  s60sc 2021
*/

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/adc.h"
extern "C" {
#include "hardware/watchdog.h"
}
#include <string.h>
#include <string>

#include "PicoWebServer.h"
#include "PicoWSpage.h"
#include "blinkLed.pio.h"

static bool setup();
static void loop();
static void customWebAServer(const char* url, const char* json);
static void extractJsonVal (const char* json, const char* key, char* val);
static void configESP8266gpio();
static void configPico();
static void pollESP8266gpio(int64_t pollTime);

static float gotVolt = 0;

int main() {
  if (setup()) while(true) loop();
  else return 0;
}

static bool setup() {
  blinkLed(0.1); 
  setupUART();

  // allow user time to start USB monitor
  int i = 10;
  while (i--) {
    printf("Countdown %i\n", i);
    sleep_ms(1000);
  }

  setupESP8266(); // connection to ESP8266
  configESP8266gpio();
  configPico();
  blinkLed(BLINKRATE); // using PIO
  sleep_ms(1000); // ensure core0 setup finished before start core1
  return startWebServer();
}

static void loop() {
  // check for web client input from core 1
  uintptr_t* webIn = webInput();
  if (webIn) {
    // got input
    char* webInStr = (char*)webIn;
    int32_t webInLen = strlen(webInStr); 

    //  extract url and json from received data
    int urlLen = webInLen;
    int jsonLen = 0;
    char* jsonSep = strstr(webInStr,","); 
    if (jsonSep != NULL) {
      // have json data after first comma
      urlLen = jsonSep - webInStr;
      jsonLen = webInLen - urlLen;
    } 
    char url[urlLen+1] = {0};
    strncpy(url, webInStr, urlLen);
    char json[jsonLen+1] = {0};
    strncpy(json, webInStr+urlLen+1, jsonLen);
    customWebAServer(url, json);
  } 
  pollESP8266gpio(5); // poll per 5 seconds
}

/* ----------------------- user customised functions ----------------------------- */

static void customWebAServer(const char* url, const char* jsonIn) {
  // setup custom web server
  static char jsonOut[100]; // buffer to holde json response
  static float blinkRate = BLINKRATE;
  // switch on url value to build and return response to core1
  if (strcmp(url, "/") == 0) {
    appResponse(index_html); // initial request, send web page content
  }
  else if (strcmp(url, "/update") == 0)  {
    // blink value is key 4
    char blinkValStr[strlen(jsonIn)+1] = {0};
    extractJsonVal(jsonIn, "\"4\":", blinkValStr);
    blinkRate = strtof(blinkValStr, nullptr);
    blinkLed(blinkRate);
    appResponse(""); // send 200 OK
  }
  else if (strcmp(url, "/refresh") == 0)  {
    // obtain and build json output
    getTOD(); // get latest time and date
    // get internal temp, 12-bit conversion, assume max value is ADC_VREF @ 3V3
    float temperature = 27.0 - ((adc_read() * 3.3 / 4096.0) - 0.706) / 0.001721;
    sprintf(jsonOut, "{\"1\":\"%s\",\"2\":\"%0.1fC\",\"3\":\" %0.4fV\",\"4\":\"%0.2f\"}", datetimeStr, temperature,  gotVolt, blinkRate);
    appResponse(jsonOut); 
  }
  else if (strcmp(url, "/reset") == 0)  {
    // force reset
    watchdog_reboot(0, 0, 0); 
  }
  else {
    appResponse(""); // url not found (send 200, but should send 404)
  }
}

static void extractJsonVal (const char* json, const char* key, char* val) {
  // dirty way to obtain value for supplied key in json
  char* s = strstr(json, key);   
  s += strlen(key) + 1; // skip over opening "
  char* e = strstr(s, "\""); // value bounded by closing "
  strncpy(val, s, e-s); 
}

static void configPico() {
  // setup adc for internal temperature
  adc_init();
  adc_set_temp_sensor_enabled	(true);
  adc_select_input(4); // internal adc
}

static void configESP8266gpio() {
  // configure any required ESP8266 gpio pins
  ESP8266pinMode(2, ESP_OUTPUT, ESP_NOPULLUP);
  ESP8266pinMode(14, ESP_INPUT, ESP_NOPULLUP);
}

static void pollESP8266gpio(int64_t pollTime) {
  // set or get any required ESP8266 gpio pins 
  // usage should not be time critical
  pollTime *= MICROS; // convert to micro secs
  static bool toggle = false;
  static absolute_time_t start = get_absolute_time();

  if (absolute_time_diff_us(start, get_absolute_time()) > pollTime) {
    start = get_absolute_time();
    if (ESP8266digitalWrite(2, toggle)) toggle = !toggle; // blink ESP8266 led at polling rate
    int gotDigi = ESP8266digitalRead(14);
    float adcVal = ESP8266analogRead(); 
    if (adcVal >= 0) gotVolt = adcVal;
  }
}

