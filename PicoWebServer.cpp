/*
  This program runs on a Raspberry Pico to provide a web server when connected to an Espressif ESP8266. 
  This allows the Pico to be monitored and controlled from a browser. 
  The Pico RTC can also be updated with the current time from NTP servers and the ESP8266 GPIOs can be accessed from the Pico. 
  
  The user configuration must be completed in PicoWebServer.h

  s60sc 2021
*/

#include <stdio.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/uart.h"
#include "pico/multicore.h"
#include "hardware/irq.h"
#include <string.h>
#include <iomanip>
extern "C" {
#include <hardware/rtc.h>
#include "hardware/watchdog.h"
#include "pico/util/datetime.h"
}

#include "PicoWebServer.h"


static char sendBuffer[SENDBUFFERLEN];
static char responseBuffer[RESPONSEBUFFERLEN];

// HTTP response wrapper
static const char httpHeader[] = "HTTP/1.0 200 OK\r\nAccess-Control-Allow-Origin: *\r\nHost:Pico\r\n"; 
static const char contentHeader[] = "Content-type: text/html\r\n\r\n";
static const char jsonHeader[] = "Content-type: application/json\r\n\r\n";
static const char httpFooter[] = "\r\n";
static const char serverError[] = "HTTP/1.0 500 Internal Server Error\r\n\r\n";

// used for IRQs
static uintptr_t* webIn = 0;
static uintptr_t* webOut = 0;
static bool webRequest = false;

static mutex_t ESP8266mutex; // prevent both cores accessing ESP8266 at same time
static mutex_t uartIrq; // gate servicing clients on uart irq 
static mutex_t core0resp; // core1 gate on response from core0
char datetimeStr[50];

// forward refs
static bool processATcommand(const char* command, int64_t allowTime, const char* successMsg);
static bool processATcommandOK(const char* command, int64_t allowTime);
static int getParam(int &valOffset, const char* startStr, const char* endStr);
static int getATdata(int buffPtr);
static void sendResponse(const char* id);
void sendResponsePart(const char* id, const char* responseData);
static void setTOD();
static void core0_sio_irq() ;
static void uartRXirq();

/* ----------------------------- uart and cores setup -------------------------------- */

void setupUART() {
  // Initialise UART 0
  uart_set_format(uart0, 8, 1, UART_PARITY_NONE);
  uart_init(uart0, 115200); // default ESP8266 baud rate
  // Set the GPIO pin mux to the UART - 0 is TX, 1 is RX
  gpio_set_function(0, GPIO_FUNC_UART);
  gpio_set_function(1, GPIO_FUNC_UART);

  // use mutexes to control access
  mutex_init(&ESP8266mutex);
  mutex_init(&core0resp); 
  mutex_init(&uartIrq);
  mutex_try_enter(&uartIrq, NULL); // start off blocked
  mutex_try_enter(&core0resp, NULL); 

  // Set up UART to use RX interrupt
  irq_set_exclusive_handler(UART0_IRQ, uartRXirq);
  irq_set_enabled(UART0_IRQ, true);

  stdio_init_all();
  rtc_init(); 
}

void setupESP8266() {
  // reset ESP8266
  if (processATcommandOK("GMR", 2)) {
    processATcommandOK("RST", 2); 
    processATcommand("", 5, ""); // flush ESP8266 boot messages
    uart_puts(uart0, "ATE0\r\n"); // stop command echo;
    processATcommandOK("", 2); // flush previous response

  } else {
    puts("ESP8266 not available, check connections, restart in 10 secs");
    sleep_ms(10000);
    watchdog_reboot(0, 0, 0); 
    sleep_ms(10000);
  }
}

// ISRs in RAM fro speed
static void __not_in_flash_func (uartRXirq)() {
  // UART RX interrupt handler
  if (mutex_try_enter(&ESP8266mutex, NULL)) {
    // not being used for GPIOs, so can take it
    irq_set_enabled(UART0_IRQ, false); // stop further interrupts
    mutex_exit(&uartIrq); // open gate for client servicing
  }  // in use so ignore
}

static void __not_in_flash_func (core0_sio_irq)() {
  // pointer to incoming input from core1 on interrupt
  while (multicore_fifo_rvalid()) webIn = (uintptr_t(*)) multicore_fifo_pop_blocking();
  multicore_fifo_clear_irq();
}

static void __not_in_flash_func (core1_sio_irq)() {
  // pointer to outgoing response from core0 on interrupt
  while (multicore_fifo_rvalid()) webOut = (uintptr_t(*)) multicore_fifo_pop_blocking();
  mutex_exit(&core0resp); // open gate for server response
  multicore_fifo_clear_irq();
}

/* ----------------------------- Web Server setup -------------------------------- */

bool startWebServer() {
  // start wifi and web server on ESP8266, and get time from NTP server
  bool isInit = false;
  mutex_enter_blocking(&ESP8266mutex);
  processATcommandOK("CWMODE_CUR=1", 2);
  snprintf(sendBuffer, SENDBUFFERLEN, "CIPSTA_CUR=\"%s\",\"%s\",\"255.255.255.0\"", STATICIP, GATEWAY);
  processATcommandOK(sendBuffer, 2); 
  //processATcommandOK("CWLAP", 10); // list of SSIDs

  snprintf(sendBuffer, SENDBUFFERLEN, "CWJAP_CUR=\"%s\",\"%s\"", WIFISSID, WIFIPASS);
  if (processATcommandOK(sendBuffer, 10)) { 
    // have wifi connection
    processATcommandOK("CIFSR", 2); 
    snprintf(sendBuffer, SENDBUFFERLEN, "CIPSNTPCFG=1,%d,\"pool.ntp.org\"", TIMEZOME);
    processATcommandOK(sendBuffer, 2);

    // loop until get current time or timeout
    int retries = NTPRETRIES;
    do {
      sleep_ms(1000);
      // wait for 'unexpected' response
      if (!processATcommand("CIPSNTPTIME?", 2, "1970")) { 
        // have a valid time if not default 1970
        setTOD();
        break;
      }
    } while (--retries);
    if (!retries) puts("*** failed to get time from NTP");

    // start web server
    processATcommandOK("CIPMUX=1", 2); 
    processATcommandOK("CIPSERVERMAXCONN=1", 2); // for simplicity, only one concurrent connection
    processATcommandOK("CIPSERVER=1,80", 2); 
    processATcommandOK("SYSRAM?", 2); // available RAM on ESP8266 
    getTOD(); // get current time
    printf("\nWeb server available on %s at %s\n\n", STATICIP, datetimeStr);

    // setup core1, and core0 IRQ
    multicore_launch_core1(serveClients);
    irq_set_exclusive_handler(SIO_IRQ_PROC0, core0_sio_irq);
    irq_set_enabled(SIO_IRQ_PROC0, true);
    uart_set_irq_enables(uart0, true, false);
    isInit = true;
  } else puts("*** Failed to setup wifi connection");

  mutex_exit(&ESP8266mutex);
  return isInit;
}

static void setTOD () {
  // set current system time and date
  datetime_t dt;

  // extract received time value
  int todOffset = 0;
  int todLen = getParam(todOffset, ":", "\r");
  char tod[todLen+1] = {0};
  strncpy(tod, responseBuffer+todOffset, todLen);

  // update RTC with NTP time
  std::tm t = {};
  std::istringstream ss(tod);
  ss >> std::get_time(&t, "%a %b %d %H:%M:%S %Y"); // format of received time string
  dt = {
    .year  = (int16_t)(t.tm_year+1900),
    .month = (int8_t)(t.tm_mon+1),
    .day   = (int8_t)t.tm_mday,
    .dotw  = (int8_t)t.tm_wday, 
    .hour  = (int8_t)(t.tm_hour),
    .min   = (int8_t)t.tm_min,
    .sec   = (int8_t)t.tm_sec
  };
  rtc_set_datetime(&dt);
}

 void getTOD() {
  // get current local time and date
  datetime_t dt;
  rtc_get_datetime(&dt);
  datetime_to_str(datetimeStr, sizeof(datetimeStr), &dt);
}

/* ----------------------------- Web Client servicing runs on core 1-------------------------------- */

void serveClients() {
  // set up core 1 interrupt
  multicore_fifo_clear_irq();
  irq_set_exclusive_handler(SIO_IRQ_PROC1, core1_sio_irq);
  irq_set_enabled(SIO_IRQ_PROC1, true);

  while (true) {
    // handle incoming web client requests, gate on interrupt
    mutex_enter_blocking(&uartIrq);
      if (uart_is_readable(uart0)) {
        processATcommand("", 2, "");
        // request available
        if (strstr(responseBuffer, "+IPD") != NULL) {

          // +IPD,<link	ID>,<len>:<method> <path> HTTP/1.1
          // received client request, get client id
          int valOffset = 0;
          int valLen = getParam(valOffset, "+IPD,", ","); 
          char id[valLen+1] = {0};
          strncpy(id, responseBuffer+valOffset, valLen);

          // get length of data to return
          valOffset += valLen;
          valLen = getParam(valOffset, ",", ":"); 
          char reqLen[valLen+1] = {0};
          strncpy(reqLen, responseBuffer+valOffset, valLen);
          int requestLen = atoi(reqLen);

          // received payload, so process response   
          if (strlen(responseBuffer) > requestLen) sendResponse(id);       
          else printf("*** truncated input, expected %u, got %u: %s\n", requestLen, strlen(responseBuffer), responseBuffer);
        }  // unexpected content, ignore
    }
    mutex_exit(&ESP8266mutex); // allow gpios
    irq_set_enabled(UART0_IRQ, true); // reenable interrupts
  }
}

static void sendResponse(const char* id) {
  // build response to client request
  // extract whether GET or POST
  int valOffset = 0;
  int valLen = getParam(valOffset, ":", " "); 
  char method[valLen+1] = {0};
  strncpy(method, responseBuffer+valOffset, valLen);

  // extract URL
  int urlOffset = valOffset + valLen;
  int urlLen = getParam(urlOffset, " ", " HTTP"); 
  
  int jsonOffset = urlOffset + urlLen;
  int jsonLen = 0;
  // for POST, also need form content (json)
  if (strncmp(method, "POST", 4) == 0) jsonLen = getParam(jsonOffset, "\r\n\r\n{", "}"); 
    
  char core0msg[urlLen+jsonLen+2] = {0};   
  strncpy(core0msg, responseBuffer+urlOffset, urlLen);
  if (jsonLen > 0) {
    strcat (core0msg, ",");
    strncat (core0msg, responseBuffer+jsonOffset, jsonLen);
  }
  printf("Web client input: %s %s\n", method, core0msg);

  // raise interrupt to send incoming request/data to main app on core 0
  multicore_fifo_push_blocking((uintptr_t)core0msg);
  // block on response from main app via interrupt
  if (mutex_enter_timeout_ms(&core0resp, 1000*20)) {
    // have response
    char* webOutStr = (char*)webOut; 
    int webOutLeft = strlen(webOutStr);
    int webOutStrPtr = 0;
    webOut = 0;

    // send response to client inside HTTP wrapper
    sendResponsePart(id, httpHeader);
    // select which content type to be sent
    if (webOutLeft > 0 && webOutStr[0] == '{') sendResponsePart(id, jsonHeader);
    else sendResponsePart(id, contentHeader);
    
    // send response in chunks if too large
    while (webOutLeft > 0) {
      size_t packetLen =  ((webOutLeft < SENDBUFFERLEN) ? webOutLeft : SENDBUFFERLEN-1);
      webOutLeft -= packetLen;
      snprintf(sendBuffer, SENDBUFFERLEN, "CIPSEND=%s,%d", id, packetLen); 
      processATcommand(sendBuffer, 2, ">"); // ESP8266 ready to receive response
      memcpy(sendBuffer, webOutStr+webOutStrPtr, packetLen);
      webOutStrPtr += packetLen;
      sendBuffer[packetLen] = 0; // terminate string
      uart_puts(uart0, sendBuffer);
      processATcommandOK("", 5); // confirm sent OK
    } 

    // closing footer
    sendResponsePart(id, httpFooter);

  } else {
    // took too long, something wrong, so restart
    puts("*** failed to obtain response for client");
    sleep_ms(10000);
    watchdog_reboot(0, 0, 0); 
  }
  snprintf(sendBuffer, SENDBUFFERLEN, "CIPCLOSE=%s", id);
  processATcommandOK(sendBuffer, 2); // close request
}

void sendResponsePart(const char* id, const char* responseData) {
  snprintf(sendBuffer, SENDBUFFERLEN, "CIPSEND=%s,%d", id, strlen(responseData));
  processATcommand(sendBuffer, 2, ">"); // ESP8266 ready to receive response
  uart_puts(uart0, responseData); 
  processATcommandOK("", 5); // confirm sent OK
}

void appResponse(const char* appResp) {
  // interrupt core 1 with data to return
  webIn = 0;
  multicore_fifo_push_blocking((uintptr_t)appResp);
}

uintptr_t* webInput() {
  // called from app to check web input state
  return webIn;
}

/* ----------------------------- Process AT commands -------------------------------- */

static bool processATcommandOK(const char* command, int64_t allowTime) {
  return processATcommand(command, allowTime, "OK");
}

static bool processATcommand(const char* command, int64_t allowTime, const char* successMsg) {
  // send AT command and check response
  allowTime *= MICROS; // convert to micro secs
  absolute_time_t start = get_absolute_time();
  bool runCommand = true;
  int buffPtr = 0;
  char sendBuffer[SENDBUFFERLEN];
  snprintf(sendBuffer, SENDBUFFERLEN, "AT+%s\r\n", command);

  // loop until have required response or exceed allowed time
  while (absolute_time_diff_us(start, get_absolute_time()) < allowTime) {
    if (runCommand && strlen(command) > 0) {
      // send required AT command
      uart_puts(uart0, sendBuffer); 
      printf("AT: %s\n", command);
      runCommand = false;
      buffPtr = 0;
    }
    buffPtr = getATdata(buffPtr);
    if (buffPtr == -1) {
      // abort if response is too long
      printf("*** Response to command %s is too long: [%s]\n", command, responseBuffer);
      return false;
    }

    if ((strlen(successMsg) > 0) && (strstr(responseBuffer, successMsg) != NULL)) {
      // have required response
      // printf("Success: [%s]\n", responseBuffer);
      return true;
    }
    // expected response not found, check if busy
    if (strstr(responseBuffer, "busy p...") != NULL) {
      printf("ESP8266 busy, retry command %s\n", command);
      sleep_ms(1000); // ESP8266 not ready for command, so retry by relooping
      runCommand = true;
    }
  }

  // timed out, required response not found
  if (strlen(successMsg) > 0) {
    if (buffPtr > 0) {
      if (strstr(responseBuffer, "busy p...") != NULL) printf("*** Timed out waiting on ESP8266 busy %s\n", command);
      else printf("*** Command %s got unexpected response: [%s]\n", command, responseBuffer);
    } else printf("*** Timed out waiting for response to %s\n", command);
    return false;
  } else return (buffPtr > 0) ? true : false; // where successMsg is ignored
}

static int getATdata(int buffPtr) {
  // obtain response from ESP8266
  bool tooLong = false;
  while (uart_is_readable(uart0)) {
    responseBuffer[buffPtr] = uart_getc(uart0);  // save response into buffer
    if (buffPtr >= RESPONSEBUFFERLEN-1) tooLong = true; 
    else buffPtr++;
  }
  responseBuffer[buffPtr] = 0; // string terminator
  return (tooLong) ? -1 : buffPtr;
}

static int getParam(int &valOffset, const char* startStr, const char* endStr) {
  // obtain location of parameter from ESP8266 AT response bounded by start and end strings
  char* s = strstr(responseBuffer+valOffset, startStr);   
  s += strlen(startStr); 
  char* e = strstr(s, endStr);  
  valOffset = s-responseBuffer;   
  // return length of param, and update supplied arg with offset to param
  return e-s; 
}

/* ---------------------- ESP8266 GPIO -------------------------------------- */

// as GPIO routines are called from core 0, a mutex is used to prevent conflict with web server on core 1
// so GPIO routines dont block on mutex otherwise a deadlock could occur so web server has priority
// therefore while web server is busy, gpio routines will fail

bool ESP8266pinMode(int pin, int direction, int pullup) {
  // define how pin to be used (configured as simple input or output, no peripherals)
  // direction: 0 for input, 1 for output
  // pullup: 1 for on, 0 for off
  // Useable: pins 4, 5, 12, 13, 14 are general purpose IO, pins 0, 2, 15 have restrictions
  // Not useable: pins 1, 3 are UART, pins 6-11 are flash, pin 16 not accessible via AT commands
  if (pin > 15) printf("*** Pin %u not accessible\n", pin);
  else {
    if (mutex_enter_timeout_ms(&ESP8266mutex, MUTEXWAIT)) {
      int mode = (pin == 1 || pin == 3 || pin > 6) ? 3 : 0; // FUNC_GPIO mode
      snprintf(sendBuffer, SENDBUFFERLEN, "SYSIOSETCFG=%u,%u,%u", pin, mode, pullup);
      processATcommandOK(sendBuffer, 1);  
      snprintf(sendBuffer, SENDBUFFERLEN, "SYSGPIODIR=%u,%u", pin, direction); 
      processATcommandOK(sendBuffer, 1); 
      mutex_exit(&ESP8266mutex);
      return true;
    }
  }
  return false; // failed to set
}

int ESP8266digitalRead(int pin) {
  // read from ESP8266 IO pin
  if (mutex_enter_timeout_ms(&ESP8266mutex, MUTEXWAIT)) {
    snprintf(sendBuffer, SENDBUFFERLEN, "SYSGPIOREAD=%u", pin);
    processATcommandOK(sendBuffer, 1); // +SYSGPIOREAD:14,0,1
    int valOffset = 0;
    int valLen = getParam(valOffset, ",", ","); // skip over direction param
    valOffset += valLen;
    valLen = getParam(valOffset, ",", "\r");  // final param is read value
    char pinVal[valLen+1] = {0};
    strncpy(pinVal, responseBuffer+valOffset, valLen);
    mutex_exit(&ESP8266mutex);
    return atoi(pinVal); 
  }
  return -1; // failed to read
}

bool ESP8266digitalWrite(int pin, bool value) {
  // write to ESP8266 IO pin
  if (mutex_enter_timeout_ms(&ESP8266mutex, MUTEXWAIT)) {
    snprintf(sendBuffer, SENDBUFFERLEN, "SYSGPIOWRITE=%u,%u", pin, value);
    processATcommandOK(sendBuffer, 1);  
    mutex_exit(&ESP8266mutex);
    return true;
  }
  return false; // failed to write
}

float ESP8266analogRead() {
  // read value from single analog pin and return as voltage
  if (mutex_enter_timeout_ms(&ESP8266mutex, MUTEXWAIT)) {
    processATcommandOK("SYSADC?", 1);
    int valOffset = 0;
    int valLen = getParam(valOffset, ":", "\r");
    char adcVal[valLen+1] = {0};
    strncpy(adcVal, responseBuffer+valOffset, valLen); // extract value from response
    mutex_exit(&ESP8266mutex);
    return (float)(atoi(adcVal)/1024.0); // as a voltage 0 - 1V
  } 
  return -1.0; // failed to read
}

