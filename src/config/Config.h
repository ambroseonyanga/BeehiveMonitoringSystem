#include <Arduino.h>
#pragma once

#define SAMPLE_RATE 16000
#define RECORD_TIME_SEC 10
#define RECORD_INTERVAL 30

#define SDA_PIN 21
#define SCL_PIN 22

#define SD_CS 13
#define SD_MOSI 15
#define SD_MISO 2
#define SD_SCK 14

#define LED_PIN 33

extern const char* WIFI_SSID;
extern const char* WIFI_PASSWORD;

extern String hiveNo;