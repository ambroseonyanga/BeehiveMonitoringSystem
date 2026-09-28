#pragma once

// AdEMNEA AWS uplink for the hive ESP32.
//
// Readings are queued in RAM with the time they were taken, then sent in one
// batch whenever the board is online (the existing upload window). Each board
// authenticates with its own device key in the X-Api-Key header over verified
// HTTPS; the server side is API Gateway → SQS → Laravel worker.
//
//   queueReading(temp, humidity, co2)  — after every sensor read
//   flush(idle)                        — while connected to Wi-Fi
//   sendHeartbeat(batteryVoltage)      — while connected, once per window
//
// Message formats: infra/aws/README.md in the ademnea-WBMS repo.

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>

#include "aws_root_ca.h"
#include "aws_secrets.h"  // AWS_API_BASE, AWS_DEVICE_KEY — copy from aws_secrets.example.h

#ifndef AWS_FIRMWARE_VERSION
#define AWS_FIRMWARE_VERSION "hive-fw-1.0.0"
#endif

// Which hive zone the SCD41 sits in: "brood_section", "honey_section" or "exterior".
// The dashboard's temperature/humidity cards and alerts use brood_section.
#ifndef AWS_CLIMATE_ZONE
#define AWS_CLIMATE_ZONE "brood_section"
#endif

// Battery voltage → percentage for the dashboard's battery status. Defaults
// suit a single Li-ion cell (3.30 V empty, 4.20 V full); change for other packs.
#ifndef AWS_BATTERY_EMPTY_V
#define AWS_BATTERY_EMPTY_V 3.30f
#endif
#ifndef AWS_BATTERY_FULL_V
#define AWS_BATTERY_FULL_V 4.20f
#endif

// Readings kept while offline. 180 × 12 bytes ≈ 2 KB, i.e. 3 hours at one per
// minute; when full, the oldest reading is dropped. RAM survives light sleep.
#ifndef AWS_QUEUE_SIZE
#define AWS_QUEUE_SIZE 180
#endif

// Upper bound on one flush so the local web dashboard stays responsive.
#ifndef AWS_FLUSH_BUDGET_MS
#define AWS_FLUSH_BUDGET_MS 20000UL
#endif

namespace AwsUplink {

struct Reading {
    time_t at;
    float temp;
    float humidity;
    uint16_t co2;
};

static Reading queue_[AWS_QUEUE_SIZE];
static size_t head_ = 0;   // index of the oldest reading
static size_t count_ = 0;
static WiFiClientSecure tls_;
static HTTPClient http_;
static bool tlsReady_ = false;

// Before NTP sync the clock reads 1970; such readings would land on the wrong day.
inline bool clockValid() { return time(nullptr) > 1700000000; }

inline size_t pending() { return count_; }

inline String isoUtc(time_t t) {
    struct tm tmUtc;
    gmtime_r(&t, &tmUtc);  // UTC regardless of the gmtOffset given to configTime()
    char buf[21];
    strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tmUtc);
    return String(buf);
}

// Call after every successful sensor read. Returns false if the clock is not
// synced yet (the reading is skipped rather than stored with a wrong time).
inline bool queueReading(float temp, float humidity, uint16_t co2) {
    if (!clockValid()) {
        return false;
    }
    Reading r{time(nullptr), temp, humidity, co2};
    if (count_ == AWS_QUEUE_SIZE) {
        queue_[head_] = r;                       // overwrite the oldest
        head_ = (head_ + 1) % AWS_QUEUE_SIZE;
    } else {
        queue_[(head_ + count_) % AWS_QUEUE_SIZE] = r;
        count_++;
    }
    return true;
}

// HTTP status, or a negative HTTPClient error when the request never completed.
inline int post(const char *path, const String &json) {
    if (!tlsReady_) {
        tls_.setCACert(AWS_ROOT_CA);
        http_.setReuse(true);  // keep the TLS session open across messages
        http_.setTimeout(10000);
        tlsReady_ = true;
    }
    if (!http_.begin(tls_, String(AWS_API_BASE) + path)) {
        return -1;
    }
    http_.addHeader("Content-Type", "application/json");
    http_.addHeader("X-Api-Key", AWS_DEVICE_KEY);
    int code = http_.POST(json);
    http_.end();
    return code;
}

// 202 = queued in AWS. 400/401/413 will fail the same way on every retry, so
// they are dropped; anything else (no network, 429 throttled, 5xx) is retried.
inline bool isPermanentFailure(int code) {
    return code == 400 || code == 401 || code == 413;
}

inline String readingJson(const char *sensorType, const String &at, const String &reading) {
    return String("{\"sensor_type\":\"") + sensorType + "\",\"recorded_at\":\"" + at +
           "\",\"reading\":" + reading + "}";
}

// Send queued readings, oldest first. Stops at the first transient failure
// and keeps the rest for the next window. Re-sending a reading that was
// partly delivered is safe: the server discards exact duplicates.
// `idle` (optional) is called between messages, e.g. to serve the local web UI.
inline size_t flush(void (*idle)() = nullptr) {
    if (WiFi.status() != WL_CONNECTED) {
        return 0;
    }
    size_t sent = 0;
    unsigned long started = millis();

    while (count_ > 0 && millis() - started < AWS_FLUSH_BUDGET_MS) {
        const Reading &r = queue_[head_];
        String at = isoUtc(r.at);
        String climate = String("{\"") + AWS_CLIMATE_ZONE + "\":";

        const String messages[] = {
            readingJson("temperature", at, climate + String(r.temp, 2) + "}"),
            readingJson("humidity", at, climate + String(r.humidity, 2) + "}"),
            readingJson("co2", at, String("{\"co2_level\":") + String(r.co2) + "}"),
        };

        for (const String &body : messages) {
            int code = post("/ingest", body);
            if (idle) {
                idle();
            }
            if (code == 202) {
                continue;
            }
            if (isPermanentFailure(code)) {
                Serial.printf("[AWS] reading rejected (HTTP %d), dropped: %s\n", code, body.c_str());
                continue;
            }
            Serial.printf("[AWS] send failed (%d); %u readings kept for next window\n",
                          code, (unsigned)count_);
            return sent;
        }

        head_ = (head_ + 1) % AWS_QUEUE_SIZE;
        count_--;
        sent++;
    }

    if (sent) {
        Serial.printf("[AWS] %u readings sent, %u pending\n", (unsigned)sent, (unsigned)count_);
    }
    return sent;
}

inline int batteryPercent(float volts) {
    float pct = (volts - AWS_BATTERY_EMPTY_V) / (AWS_BATTERY_FULL_V - AWS_BATTERY_EMPTY_V) * 100.0f;
    return (int)constrain(pct, 0.0f, 100.0f);
}

// Device health for the dashboard (online status, battery, signal).
inline bool sendHeartbeat(float batteryVoltage) {
    if (WiFi.status() != WL_CONNECTED) {
        return false;
    }
    String body = String("{\"battery_level\":") + String(batteryPercent(batteryVoltage)) +
                  ",\"signal_strength\":" + String((int)WiFi.RSSI()) +
                  ",\"uptime\":" + String(millis() / 1000) +
                  ",\"firmware_version\":\"" + AWS_FIRMWARE_VERSION + "\"}";
    int code = post("/heartbeat", body);
    if (code != 202) {
        Serial.printf("[AWS] heartbeat failed (%d)\n", code);
    }
    return code == 202;
}

}  // namespace AwsUplink
