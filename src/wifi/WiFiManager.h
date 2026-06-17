#pragma once

#include <Arduino.h>
#include <WiFi.h>

class WiFiManager
{
private:
    const char* _ssid;
    const char* _password;

    const char* _apName;
    const char* _apPassword;

public:
    WiFiManager(
        const char* ssid,
        const char* password,
        const char* apName,
        const char* apPassword
    );

    void begin();

    void checkConnection();

    bool connected();

    IPAddress localIP();

    IPAddress apIP();
};