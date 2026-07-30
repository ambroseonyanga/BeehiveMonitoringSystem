#ifndef CONNECTION_H
#define CONNECTION_H

#include <WiFi.h>

void connectWiFi();
void startAP();
bool connectSTA();
void disconnectSTA();
void checkWiFiConnection();
void setupTime();
String getTimestamp();

#endif