#ifndef DASHBOARD_SERVER_H
#define DASHBOARD_SERVER_H

#include <Arduino.h>
#include <WebServer.h>

class DashboardServer
{
private:
    WebServer server;

    float temperature;
    float humidity;
    uint16_t co2;
    float batteryVoltage;
    long audioPeak;

    bool cloudConnected;
    bool sdReady;
    bool micReady;

    void handleRoot();
    void handleData();

public:
    DashboardServer();

    void begin();

    void handleClient();

    void updateReadings(
        float temp,
        float hum,
        uint16_t co2Value,
        float batt,
        long audio
    );

    void updateStatus(
        bool cloud,
        bool sd,
        bool mic
    );
};

#endif