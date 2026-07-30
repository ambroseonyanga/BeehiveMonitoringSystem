#include "connection.h"
#include <logging.h>
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <time.h>

// ======================================================
// VARIABLES
// ======================================================
extern WebServer server;

extern const char *ssid;
extern const char *password;

extern const char *ntpServer;
extern const long gmtOffset_sec;
extern const int daylightOffset_sec;

extern const int LED_PIN;
// ======================================================
// WIFI CONNECT
// ======================================================
void connectWiFi()
{
    Serial.println();
    Serial.println("[WIFI]");

    WiFi.mode(WIFI_AP_STA);

    // Connect to router
    WiFi.begin(ssid, password);

    Serial.print("Connecting");

    while (WiFi.status() != WL_CONNECTED)
    {
        server.handleClient();
        yield();
        delay(500);
        Serial.print(".");
    }

    Serial.println();
    Serial.println("Connected to Router");

    // Create ESP32 WiFi
    WiFi.softAP(
        "HiveMonitor",
        "12345678");

    Serial.println();

    Serial.print("Router IP: ");
    Serial.println(WiFi.localIP());

    Serial.print("Dashboard IP: ");
    Serial.println(WiFi.softAPIP());

    digitalWrite(LED_PIN, HIGH);
}
// ======================================================
// STARTING AP
// ======================================================
void startAP()
{
    WiFi.mode(WIFI_AP);

    WiFi.softAP(
        "HiveMonitor",
        "12345678");

    Serial.print("Dashboard IP: ");
    Serial.println(WiFi.softAPIP());
}

// ======================================================
// CONNECTING TO STA
// ======================================================
bool connectSTA()
{
    Serial.println("Starting STA...");

    WiFi.mode(WIFI_AP_STA);

    WiFi.begin(ssid, password);

    unsigned long start = millis();

    while (WiFi.status() != WL_CONNECTED)
    {
        server.handleClient();
        yield();
        if (millis() - start > 15000)
        {
            Serial.println("STA connection failed");
            return false;
        }

        delay(500);
        Serial.print(".");
    }

    Serial.println();
    Serial.print("STA IP: ");
    Serial.println(WiFi.localIP());

    return true;
}
// ======================================================
// DISCONNECTING FROM STA
// ======================================================
void disconnectSTA()
{
    WiFi.disconnect(true);

    WiFi.mode(WIFI_AP);

    Serial.println("STA disconnected");
}

// ======================================================
// WIFI MONITOR
// ======================================================

void checkWiFiConnection()
{

    if (WiFi.status() == WL_CONNECTED)
    {
        digitalWrite(LED_PIN, HIGH);
        return;
    }

    Serial.println();
    // Serial.println("WiFi Lost!");
    addError("[WARN] WiFi connection lost");
    digitalWrite(LED_PIN, LOW);
    connectWiFi();
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
}

// ======================================================
// TIME SETUP
// ======================================================

void setupTime()
{

    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

    Serial.println();
    Serial.println("[TIME]");

    struct tm timeinfo;

    while (!getLocalTime(&timeinfo))
    {
        server.handleClient();
        yield();
        Serial.println("Waiting for NTP time...");
        delay(1000);
    }

    // addError("[INFO] Time synchronized successfully");
    Serial.println("Time synchronized successfully");
}

// ======================================================
// GET TIMESTAMP
// ======================================================

String getTimestamp()
{

    struct tm timeinfo;

    if (!getLocalTime(&timeinfo))
    {
        return "NO_TIME";
    }

    char buffer[40];

    sprintf(buffer,
            "%04d-%02d-%02d_%02d%02d%02d",
            timeinfo.tm_year + 1900,
            timeinfo.tm_mon + 1,
            timeinfo.tm_mday,
            timeinfo.tm_hour,
            timeinfo.tm_min,
            timeinfo.tm_sec);

    return String(buffer);
}