#ifndef LOGGING_H
#define LOGGING_H

#include <Arduino.h>
#include <WebServer.h>

extern WebServer server;
extern const long gmtOffset_sec;
extern const int daylightOffset_sec;

void addError(String msg);
void handleErrors();

#endif