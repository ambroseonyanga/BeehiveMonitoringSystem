#include "connection.h"
#include "logging.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <time.h>
#include <esp_task_wdt.h>
#include <esp_sntp.h>

// ======================================================
// VARIABLES (defined in main.cpp)
// ======================================================
extern WebServer server;

extern const char *ssid;
extern const char *password;

extern const char *ntpServer;
extern const long gmtOffset_sec;
extern const int daylightOffset_sec;

// main.cpp defines LED_PIN as a macro, which is invisible here, so the old
// "extern const int LED_PIN" could never link (it only built because the
// functions using it were unused and garbage-collected). Keep in sync with main.cpp.
#ifndef LED_PIN
#define LED_PIN 33
#endif

// Local dashboard access point. Override from build_flags, e.g.
//   -DHIVE_AP_SSID=\"Hive-A1\" -DHIVE_AP_PASS=\"a-long-random-pass\"
// (WPA2 needs >= 8 characters.) CHANGE THE DEFAULT PASSWORD before deployment.
#ifndef HIVE_AP_SSID
#define HIVE_AP_SSID "HiveMonitor"
#endif
#ifndef HIVE_AP_PASS
#define HIVE_AP_PASS "12345678"
#endif

#define STA_CONNECT_TIMEOUT_MS 15000UL
// No valid clock yet. NTP answers within ~2-5 s when it works at all; when it does not,
// waiting longer only keeps the radio on (boot took 37 s). main.cpp falls back to the
// HTTPS Date header (~4 s), which has worked every time so far.
#define NTP_FIRST_SYNC_TIMEOUT_MS 8000UL
#define NTP_RESYNC_TIMEOUT_MS 5000UL      // clock already valid: don't hold the radio on

static const time_t MIN_VALID_EPOCH = 1735689600; // 2025-01-01

static inline void wdtPet() { esp_task_wdt_reset(); } // harmless if this task is not subscribed

// ======================================================
// WIFI CONNECT (legacy blocking helper; main.cpp uses connectSTA())
// ======================================================
// void connectWiFi()
// {
//     Serial.println();
//     Serial.println("[WIFI]");

//     WiFi.persistent(false);
//     WiFi.mode(WIFI_AP_STA);
//     WiFi.softAP(HIVE_AP_SSID, HIVE_AP_PASS);
//     WiFi.begin(ssid, password);

//     Serial.print("Connecting");

//     // Bounded: this used to spin forever if the router was away.
//     unsigned long start = millis();
//     while (WiFi.status() != WL_CONNECTED && millis() - start < STA_CONNECT_TIMEOUT_MS)
//     {
//         server.handleClient();
//         wdtPet();
//         yield();
//         delay(500);
//         Serial.print(".");
//     }
//     Serial.println();

//     // WiFi.softAP(HIVE_AP_SSID, HIVE_AP_PASS);

//     if (WiFi.status() == WL_CONNECTED)
//     {
//         Serial.println("Connected to Router");
//         Serial.print("Router IP: ");
//         Serial.println(WiFi.localIP());
//         digitalWrite(LED_PIN, HIGH);
//     }
//     else
//     {
//         Serial.println("Router not reachable");
//         digitalWrite(LED_PIN, LOW);
//     }

//     Serial.print("Dashboard IP: ");
//     Serial.println(WiFi.softAPIP());
// }

// ======================================================
// STARTING AP
// ======================================================
void startAP()
{
    // Never write WiFi settings to flash: this firmware switches modes on every
    // upload window and would otherwise wear the NVS partition.
    WiFi.persistent(false);
    WiFi.mode(WIFI_AP);

    WiFi.softAP(HIVE_AP_SSID, HIVE_AP_PASS);

    Serial.print("Dashboard IP: ");
    Serial.println(WiFi.softAPIP());
}

// ======================================================
// CONNECTING TO STA
// ======================================================
bool connectSTA()
{
    Serial.println("Starting STA...");

    WiFi.persistent(false);
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(ssid, password);

    const unsigned long start = millis();

    while (WiFi.status() != WL_CONNECTED)
    {
        server.handleClient();
        wdtPet();
        yield();

        if (millis() - start > STA_CONNECT_TIMEOUT_MS)
        {
            Serial.println();
            Serial.println("STA connection failed");
            // Stop the background retry loop: otherwise the radio keeps scanning
            // (power drain, SoftAP channel hopping) until the next window.
            WiFi.disconnect(true);
            WiFi.mode(WIFI_AP);
            return false;
        }

        delay(250);
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
// void disconnectSTA()
// {
//     WiFi.disconnect(true);
//     WiFi.mode(WIFI_AP);

//     Serial.println("STA disconnected");
// }
void disconnectSTA()
{
    WiFi.disconnect(true);                         // drops the STA connection
    // Stay in WIFI_AP_STA so the SoftAP never goes down and
    // connected dashboard clients are not kicked on every upload window.
    if (WiFi.getMode() != WIFI_AP_STA) WiFi.mode(WIFI_AP_STA);

    Serial.println("STA disconnected");
}

// ======================================================
// WIFI MONITOR (legacy; not used by the scheduler)
// ======================================================
// void checkWiFiConnection()
// {
//     if (WiFi.status() == WL_CONNECTED)
//     {
//         digitalWrite(LED_PIN, HIGH);
//         return;
//     }

//     addError("[WARN] WiFi connection lost");
//     digitalWrite(LED_PIN, LOW);
//     connectWiFi();
//     configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
// }

// ======================================================
// TIME SETUP
// ======================================================
static volatile bool ntpCompleted = false;
static void onNtpSync(struct timeval *) { ntpCompleted = true; }

// Bounded. It used to loop forever when NTP was unreachable (router online but no
// internet), which ended in a watchdog reset loop. The caller checks the clock
// afterwards and simply retries in the next upload window.
void setupTime()
{
    Serial.println();
    Serial.println("[TIME]");

    ntpCompleted = false;
    sntp_set_time_sync_notification_cb(onNtpSync);
    // Three servers: one blocked or unresolvable name no longer means "no time".
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer, "time.google.com", "time.cloudflare.com");

    const bool hadClock = time(nullptr) > MIN_VALID_EPOCH;
    const unsigned long limit = hadClock ? NTP_RESYNC_TIMEOUT_MS : NTP_FIRST_SYNC_TIMEOUT_MS;
    const unsigned long start = millis();

    while (!ntpCompleted && millis() - start < limit)
    {
        server.handleClient();
        wdtPet();
        yield();
        delay(100);
    }

    if (ntpCompleted)                         Serial.println("Time synchronized successfully");
    else if (time(nullptr) > MIN_VALID_EPOCH) Serial.println("NTP did not answer; keeping the current clock");
    else                                      Serial.println("NTP failed; clock not set");
}

// ======================================================
// GET TIMESTAMP
// ======================================================
// Uses time() instead of getLocalTime(): getLocalTime() blocks for up to 5 s per
// call while the clock is unset, which stalled the main loop and every error log.
String getTimestamp()
{
    const time_t now = time(nullptr);
    if (now < MIN_VALID_EPOCH) return "NO_TIME";

    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    char buffer[24];
    snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d_%02d%02d%02d",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

    return String(buffer);
}