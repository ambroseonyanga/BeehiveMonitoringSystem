#include "DashboardServer.h"
#include "DashboardHTML.h"

DashboardServer::DashboardServer()
    : server(80)
{
    temperature = 0;
    humidity = 0;
    co2 = 0;
    batteryVoltage = 0;
    audioPeak = 0;

    cloudConnected = false;
    sdReady = false;
    micReady = false;
}

void DashboardServer::begin()
{
    server.on("/",
              [this]()
              {
                  handleRoot();
              });

    server.on("/data",
              [this]()
              {
                  handleData();
              });

    server.begin();

    Serial.println("Dashboard Ready");
}

void DashboardServer::handleClient()
{
    server.handleClient();
}

void DashboardServer::handleRoot()
{
    server.send_P(
        200,
        "text/html",
        INDEX_HTML
    );
}

void DashboardServer::handleData()
{
    String json = "{";

    json += "\"temp\":";
    json += String(temperature, 1);

    json += ",\"humidity\":";
    json += String(humidity, 1);

    json += ",\"co2\":";
    json += String(co2);

    json += ",\"battV\":";
    json += String(batteryVoltage, 2);

    json += ",\"audio\":";
    json += String(audioPeak);

    json += ",\"cloud_ok\":";
    json += (cloudConnected ? "true" : "false");

    json += ",\"sd_ok\":";
    json += (sdReady ? "true" : "false");

    json += ",\"mic_ok\":";
    json += (micReady ? "true" : "false");

    json += "}";

    server.send(
        200,
        "application/json",
        json
    );
}

void DashboardServer::updateReadings(
    float temp,
    float hum,
    uint16_t co2Value,
    float batt,
    long audio)
{
    temperature = temp;
    humidity = hum;
    co2 = co2Value;
    batteryVoltage = batt;
    audioPeak = audio;
}

void DashboardServer::updateStatus(
    bool cloud,
    bool sd,
    bool mic)
{
    cloudConnected = cloud;
    sdReady = sd;
    micReady = mic;
}