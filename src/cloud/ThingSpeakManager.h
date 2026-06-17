#ifndef THINGSPEAK_MANAGER_H
#define THINGSPEAK_MANAGER_H

#include <Arduino.h>
#include <WiFi.h>
#include "ThingSpeak.h"

class ThingSpeakManager
{
private:
    unsigned long channelNumber;
    const char* writeAPIKey;

public:
    ThingSpeakManager(
        unsigned long channel,
        const char* apiKey
    );

    void begin(WiFiClient& client);

    bool sendData(
        float temp,
        float humidity,
        uint16_t co2,
        float batteryVoltage,
        long audioPeak
    );
};

#endif