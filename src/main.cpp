#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>

#include "config/Config.h"

#include "wifi/WiFiManager.h"
#include "utils/TimeManager.h"

#include "storage/SDManager.h"

#include "sensors/CO2Sensor.h"
#include "sensors/BatteryMonitor.h"
#include "sensors/AudioMonitor.h"

#include "audio/AudioRecorder.h"

#include "cloud/ThingSpeakManager.h"
#include "cloud/FileUploader.h"

#include "web/DashboardServer.h"

// ======================================================
// GLOBAL OBJECTS
// ======================================================

WiFiManager wifiManager(
    WIFI_SSID,
    WIFI_PASSWORD,
    "HiveMonitor",
    "12345678"
);

TimeManager timeManager;

SDManager sdManager(hiveNo);

CO2Sensor co2Sensor;

BatteryMonitor batteryMonitor;

AudioMonitor audioMonitor(
    25,     // WS
    26,     // SCK
    27,     // SD
    SAMPLE_RATE
);

AudioRecorder audioRecorder(
    25,     // WS
    26,     // SCK
    27,     // SD
    SAMPLE_RATE
);

WiFiClient thingSpeakClient;

ThingSpeakManager thingSpeakManager(
    3384879,
    "HEHEZST627ICEU7E"
);

FileUploader fileUploader(
    "196.43.168.57",
    8005,
    "/upload",
    "my_secure_hive_key_123"
);

DashboardServer dashboard;

// ======================================================
// TIMERS
// ======================================================

unsigned long lastDisplayTime    = 0;
unsigned long lastCSVTime        = 0;
unsigned long lastUploadTime     = 0;
unsigned long lastRecordTime     = 0;
unsigned long lastThingSpeakTime = 0;

#define THINGSPEAK_INTERVAL 15000

// ======================================================
// SETUP
// ======================================================

void setup()
{
    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("================================");
    Serial.println("SMART BEEHIVE MONITOR");
    Serial.println("================================");

    pinMode(LED_PIN, OUTPUT);

    // --------------------------------
    // WIFI
    // --------------------------------
    wifiManager.begin();

    // --------------------------------
    // TIME
    // --------------------------------
    timeManager.begin();

    // --------------------------------
    // DASHBOARD
    // --------------------------------
    dashboard.begin();

    // --------------------------------
    // SD CARD
    // --------------------------------
    sdManager.begin(
        SD_CS,
        SD_SCK,
        SD_MISO,
        SD_MOSI
    );

    // --------------------------------
    // I2C
    // --------------------------------
    Wire.begin(
        SDA_PIN,
        SCL_PIN
    );

    Wire.setClock(50000);

    // --------------------------------
    // SENSORS
    // --------------------------------
    co2Sensor.begin(Wire);

    batteryMonitor.begin();

    audioMonitor.begin();

    audioRecorder.begin();

    // --------------------------------
    // CLOUD
    // --------------------------------
    thingSpeakManager.begin(
        thingSpeakClient
    );

    Serial.println();
    Serial.println("SYSTEM READY");
}

// ======================================================
// LOOP
// ======================================================

void loop()
{
    dashboard.handleClient();

    wifiManager.checkConnection();

    // --------------------------------
    // SENSOR READINGS
    // --------------------------------

    uint16_t co2 = 0;
    float temperature = 0;
    float humidity = 0;

    co2Sensor.read(
        co2,
        temperature,
        humidity
    );

    float batteryVoltage =
        batteryMonitor.readVoltage();

    long audioPeak =
        audioMonitor.readPeak();

    // --------------------------------
    // DASHBOARD UPDATE
    // --------------------------------

    dashboard.updateReadings(
        temperature,
        humidity,
        co2,
        batteryVoltage,
        audioPeak
    );

    dashboard.updateStatus(
        wifiManager.connected(),
        sdManager.isReady(),
        audioMonitor.isReady()
    );

    // --------------------------------
    // SERIAL DISPLAY
    // --------------------------------

    if (millis() - lastDisplayTime >= 10000)
    {
        Serial.println();
        Serial.println("================================");

        Serial.printf(
            "TIME: %s\n",
            timeManager.getTimestamp().c_str()
        );

        Serial.printf(
            "TEMP: %.1f C\n",
            temperature
        );

        Serial.printf(
            "HUM: %.1f %%\n",
            humidity
        );

        Serial.printf(
            "CO2: %u ppm\n",
            co2
        );

        Serial.printf(
            "BATTERY: %.2f V\n",
            batteryVoltage
        );

        Serial.printf(
            "AUDIO: %ld\n",
            audioPeak
        );

        Serial.println("================================");

        lastDisplayTime = millis();
    }

    // --------------------------------
    // THINGSPEAK
    // --------------------------------

    if (
        millis() - lastThingSpeakTime >=
        THINGSPEAK_INTERVAL
    )
    {
        thingSpeakManager.sendData(
            temperature,
            humidity,
            co2,
            batteryVoltage,
            audioPeak
        );

        lastThingSpeakTime = millis();
    }

    // --------------------------------
    // CSV LOGGING
    // --------------------------------

    if (
        millis() - lastCSVTime >=
        15000
    )
    {
        sdManager.logToCSV(
            temperature,
            humidity,
            co2
        );

        lastCSVTime = millis();
    }

    // --------------------------------
    // FILE UPLOADS
    // --------------------------------

    if (
        millis() - lastUploadTime >=
        30000
    )
    {
        fileUploader.uploadPendingFiles(
            hiveNo,
            sdManager.isReady()
        );

        lastUploadTime = millis();
    }

    // --------------------------------
    // AUDIO RECORDING
    // --------------------------------

    if (
        millis() - lastRecordTime >=
        (RECORD_INTERVAL * 1000UL)
    )
    {
        String filename =
            "/" +
            hiveNo +
            "_" +
            timeManager.getTimestamp() +
            ".wav";

        audioRecorder.record(
            filename,
            RECORD_TIME_SEC
        );

        lastRecordTime = millis();
    }

    delay(10);
}