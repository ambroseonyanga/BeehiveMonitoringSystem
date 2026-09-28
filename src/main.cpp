#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <time.h>
#include <driver/i2s.h>
#include <WebServer.h>
#include <Adafruit_INA219.h>
#include <SensirionI2cScd4x.h>
#include "ThingSpeak.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include "dashboard.h"
#include "settings.h"
#include "connection.h"
#include "logging.h"

Preferences prefs; // This one is for storing duty cycle settings in non-volatile memory

// ======================================================
// CREDENTIALS
// ======================================================
// Keep this file private — it contains the WiFi password and device keys.

// WiFi
// const char *ssid = "IoT-ra";
// const char *password = "P9$y#F5x!b&";

const char *ssid = "RENU";
const char *password = "renu12345";

// ThingSpeak
const char *writeAPIKey = "HEHEZST627ICEU7E";

// AdEMNEA on AWS — paste this board's key from the dashboard (shown once).
const char *AWS_API_BASE = "https://erwfjmnjog.execute-api.eu-north-1.amazonaws.com";
const char *AWS_DEVICE_KEY = "M86ZFZMaG6WdPjR0FdZgRkJITFvqsM02kRqTBhCz7EYgJYJHOZNFh9nNnVVPpfVE";

// ======================================================
// AWS: SERVER CERTIFICATE
// ======================================================
// Amazon Root CA 1 — trust anchor for the AdEMNEA AWS endpoint (API Gateway,
// chain: *.execute-api… → Amazon RSA 2048 M04 → Amazon Root CA 1). Valid until 2038-01-17.
// Source: https://www.amazontrust.com/repository/AmazonRootCA1.pem
// SHA-256 fingerprint: 8E:CD:E6:88:4F:3D:87:B1:12:5B:A3:1A:C3:FC:B1:3D:70:16:DE:7F:57:CC:90:4F:E1:CB:97:C6:AE:98:19:6E

static const char AWS_ROOT_CA[] PROGMEM = R"PEM(
-----BEGIN CERTIFICATE-----
MIIDQTCCAimgAwIBAgITBmyfz5m/jAo54vB4ikPmljZbyjANBgkqhkiG9w0BAQsF
ADA5MQswCQYDVQQGEwJVUzEPMA0GA1UEChMGQW1hem9uMRkwFwYDVQQDExBBbWF6
b24gUm9vdCBDQSAxMB4XDTE1MDUyNjAwMDAwMFoXDTM4MDExNzAwMDAwMFowOTEL
MAkGA1UEBhMCVVMxDzANBgNVBAoTBkFtYXpvbjEZMBcGA1UEAxMQQW1hem9uIFJv
b3QgQ0EgMTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBALJ4gHHKeNXj
ca9HgFB0fW7Y14h29Jlo91ghYPl0hAEvrAIthtOgQ3pOsqTQNroBvo3bSMgHFzZM
9O6II8c+6zf1tRn4SWiw3te5djgdYZ6k/oI2peVKVuRF4fn9tBb6dNqcmzU5L/qw
IFAGbHrQgLKm+a/sRxmPUDgH3KKHOVj4utWp+UhnMJbulHheb4mjUcAwhmahRWa6
VOujw5H5SNz/0egwLX0tdHA114gk957EWW67c4cX8jJGKLhD+rcdqsq08p8kDi1L
93FcXmn/6pUCyziKrlA4b9v7LWIbxcceVOF34GfID5yHI9Y/QCB/IIDEgEw+OyQm
jgSubJrIqg0CAwEAAaNCMEAwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMC
AYYwHQYDVR0OBBYEFIQYzIU07LwMlJQuCFmcx7IQTgoIMA0GCSqGSIb3DQEBCwUA
A4IBAQCY8jdaQZChGsV2USggNiMOruYou6r4lK5IpDB/G/wkjUu0yKGX9rbxenDI
U5PMCCjjmCXPI6T53iHTfIUJrU6adTrCC2qJeHZERxhlbI1Bjjt/msv0tadQ1wUs
N+gDS63pYaACbvXy8MWy7Vu33PqUXHeeE6V/Uq2V8viTO96LXFvKWlJbYK8U90vv
o/ufQJVtMVT8QtPHRh8jrdkPSHCa2XV4cdFyQzR1bldZwgJcJmApzyMZFo6IQ6XU
5MsI+yMRQ+hDKXJioaldXgjUkK642M4UwtBV8ob2xJNDd2ZhwLnoQdeXeGADbkpy
rqXRfboQnoZsG4q5WTP468SQvvG5
-----END CERTIFICATE-----
)PEM";

// ======================================================
// THINGSPEAK CONFIGURATION
// ======================================================

unsigned long channelNumber = 3384879;
String hiveNo = "1";

// ======================================================
// THINGSPEAK TIMING - SEND EVERY 15 SECONDS
// ======================================================

#define THINGSPEAK_INTERVAL 15000 // 15 seconds exactly (free tier limit)

//=====================================================
// DUTY CYCLING VARIABLES
//=====================================================
#define SENSOR_INTERVAL 60000
long lastAudioPeak = 0;
unsigned long sensorInterval = 60000;
unsigned long thingSpeakInterval = 15000;
unsigned long recordInterval = 30;
unsigned long uploadInterval = 30000;
bool fullDutyCycle = false;

unsigned long lastSensorTime = 0;
unsigned long lastThingSpeakTime = 0;
unsigned long lastRecordTime = 0;
unsigned long lastUploadTime = 0;
unsigned long lastDisplayTime = 0;

static uint16_t co2 = 0;
static float temp = 0;
static float humidity = 0;
static float batteryVoltage = 0;
static long audioPeak = 0;

void normalLoop();
void dutyCycleLoop();

// ======================================================
// TIME SERVER
// ======================================================

const char *ntpServer = "pool.ntp.org";
// const char* ntpServer = "time.google.com";
// const char* ntpServer = "time.windows.com";
const long gmtOffset_sec = 3 * 3600;
const int daylightOffset_sec = 0;

//=======================================================
// API ENDPOINT CONFIGURATION
//=======================================================

// AWS: sensor data (the CSV rows) goes to the AdEMNEA platform on AWS as JSON,
// authenticated with this board's own key (AWS_DEVICE_KEY above).

// ======================================================
// I2C PINS
// ======================================================

#define SDA_PIN 21
#define SCL_PIN 22

// ======================================================
// SD CARD PINS
// ======================================================

#define SD_MISO 2
#define SD_MOSI 15
#define SD_SCK 14
#define SD_CS 13

// ======================================================
// INMP441 MICROPHONE PINS
// ======================================================

#define I2S_WS 25
#define I2S_SCK 26
#define I2S_SD 27
#define MIC_POWER_PIN 32

// ======================================================
// LED
// ======================================================

#define LED_PIN 33

// ======================================================
// AUDIO SETTINGS
// ======================================================

#define SAMPLE_RATE 16000
#define RECORD_TIME_SEC 10

// ======================================================
// OBJECTS
// ======================================================

Adafruit_INA219 batterySensor(0x40);
Adafruit_INA219 solarSensor(0x41);
SensirionI2cScd4x scd4x;
WiFiClient client;
WebServer server(80);

// ======================================================
// STATUS FLAGS
// ======================================================

bool mic_ok = false;
bool sd_ok = false;

// ======================================================
// SMOOTHED READINGS
// ======================================================

uint16_t lastCO2 = 400;
float lastTemp = 25.0;
float lastHumidity = 50.0;
float lastBatteryVoltage = 0.0;
// ======================================================
// SETTINGS HANDLER
// ======================================================
void handleSettingsPage()
{
    server.send_P(
        200,
        "text/html",
        SETTINGS_HTML);
}
//=====================================================
// HANDLE IN ESP32 WEB SERVER
//=====================================================
void handleSetDuty()
{
    if (server.hasArg("sensor"))
    {
        sensorInterval =
            server.arg("sensor").toInt() * 1000UL;
    }

    if (server.hasArg("ts"))
    {
        thingSpeakInterval =
            server.arg("ts").toInt() * 1000UL;
    }

    if (server.hasArg("record"))
    {
        recordInterval =
            server.arg("record").toInt();
    }

    if (server.hasArg("upload"))
    {
        uploadInterval =
            server.arg("upload").toInt() * 1000UL;
    }
    if (server.hasArg("fullDuty"))
    {
        fullDutyCycle = server.arg("fullDuty").toInt() == 1;
    }

    prefs.putULong("sensor", sensorInterval);
    prefs.putULong("ts", thingSpeakInterval);
    prefs.putULong("record", recordInterval);
    prefs.putULong("upload", uploadInterval);
    prefs.putBool("fullDuty", fullDutyCycle);

    Serial.println("Duty cycle updated from UI");

    server.send(
        200,
        "text/plain",
        "Settings saved");
}
// =============================================================
// AWS: SEND CSV ROWS TO THE ADEMNEA PLATFORM (AWS) AS JSON
// =============================================================

// UTC offset of the CSV timestamps, e.g. "+03:00" (they are written in local time).
String csvUtcOffset()
{
    long offset = gmtOffset_sec + daylightOffset_sec;
    long absOffset = labs(offset);
    char buf[8];
    snprintf(buf, sizeof(buf), "%c%02ld:%02ld",
             offset < 0 ? '-' : '+', absOffset / 3600, (absOffset % 3600) / 60);
    return String(buf);
}

// Parses one row written by logToCSV():
//   "2026-09-24 13:00:00","34.90*2*2","58.20*2*2","812",2.00,2
// Returns false for blank rows and rows written before the clock was synced.
bool parseCsvRow(const String &line, String &atIso, float &tempValue, float &humidityValue, int &co2Value)
{
    String fields[6];
    int count = 0;
    int start = 0;

    for (int i = 0; i <= (int)line.length() && count < 6; i++)
    {
        if (i == (int)line.length() || line[i] == ',')
        {
            fields[count] = line.substring(start, i);
            fields[count].replace("\"", "");
            fields[count].trim();
            count++;
            start = i + 1;
        }
    }

    if (count < 4 || fields[0].length() != 19 || fields[0].startsWith("0000"))
        return false;

    atIso = fields[0];          // "2026-09-24 13:00:00"
    atIso.replace(" ", "T");    // "2026-09-24T13:00:00"
    atIso += csvUtcOffset();    // "2026-09-24T13:00:00+03:00"

    tempValue = fields[1].toFloat();     // "34.90*2*2" -> 34.90 (stops at '*')
    humidityValue = fields[2].toFloat();
    co2Value = fields[3].toInt();
    return true;
}

// Sends every row of <hiveNo>.csv to AWS: one JSON message per sensor type,
// with this board's key in the X-Api-Key header, over verified HTTPS.
// Stops at the first network/auth failure and keeps the file, so the rows are
// retried next window (re-sent rows are safe: the server ignores duplicates).
// When everything went through and deleteWhenDone is true, the CSV is deleted.
bool uploadCsvToAws(bool deleteWhenDone)
{
    String path = "/" + hiveNo + ".csv";

    if (!sd_ok || WiFi.status() != WL_CONNECTED || !SD.exists(path))
        return false;

    File csv = SD.open(path, FILE_READ);

    if (!csv)
        return false;

    WiFiClientSecure tls;
    tls.setCACert(AWS_ROOT_CA); // verify it really is AWS (no setInsecure)

    HTTPClient http;
    http.setReuse(true); // one HTTPS connection for all rows
    http.setTimeout(10000);

    int rowsSent = 0;
    bool ok = true;

    Serial.println("===== SENDING CSV TO AWS =====");

    while (ok && csv.available())
    {
        String line = csv.readStringUntil('\n');
        line.trim();

        String at;
        float rowTemp, rowHumidity;
        int rowCo2;

        if (!parseCsvRow(line, at, rowTemp, rowHumidity, rowCo2))
            continue;

        String messages[3] = {
            "{\"sensor_type\":\"temperature\",\"recorded_at\":\"" + at +
                "\",\"reading\":{\"brood_section\":" + String(rowTemp, 2) + "}}",
            "{\"sensor_type\":\"humidity\",\"recorded_at\":\"" + at +
                "\",\"reading\":{\"brood_section\":" + String(rowHumidity, 2) + "}}",
            "{\"sensor_type\":\"co2\",\"recorded_at\":\"" + at +
                "\",\"reading\":{\"co2_level\":" + String(rowCo2) + "}}",
        };

        for (int i = 0; i < 3; i++)
        {
            http.begin(tls, String(AWS_API_BASE) + "/ingest");
            http.addHeader("Content-Type", "application/json");
            http.addHeader("X-Api-Key", AWS_DEVICE_KEY);

            int code = http.POST(messages[i]);
            http.end();

            server.handleClient(); // keep the local dashboard responsive
            yield();

            if (code == 202)
                continue; // queued in AWS

            if (code == 400)
            {
                Serial.println("AWS rejected malformed row, skipped: " + messages[i]);
                continue; // retrying would fail the same way
            }

            addError("AWS upload failed: HTTP " + String(code));
            ok = false; // no network / key missing / throttled — retry next window
            break;
        }

        if (ok)
            rowsSent++;
    }

    csv.close();

    Serial.printf("AWS: %d CSV rows sent%s\n", rowsSent, ok ? "" : " (stopped early, will retry)");

    if (ok && deleteWhenDone)
    {
        SD.remove(path);
        Serial.println("AWS: CSV cleared after successful upload");
    }

    Serial.println("=============================");

    return ok;
}

// ======================================================
// ENSURE 11.CSV EXISTS
// ======================================================

void ensureCSVExists()
{
    if (!sd_ok)
        return;

    if (!SD.exists("/" + hiveNo + ".csv"))
    {
        Serial.println(hiveNo + ".csv not found. Creating...");

        File file = SD.open("/" + hiveNo + ".csv", FILE_WRITE);

        if (!file)
        {
            Serial.println("Failed to create " + hiveNo + ".csv");
            return;
        }

        file.close();

        Serial.println(hiveNo + ".csv created successfully");
    }
}

void logToCSV(float tempReading,
              float humidityReading,
              uint16_t co2Reading)
{
    if (!sd_ok)
        return;

    ensureCSVExists();

    File file = SD.open("/" + hiveNo + ".csv", FILE_APPEND);

    if (!file)
    {
        Serial.println("Failed to open " + hiveNo + ".csv");
        return;
    }

    char tempStr[20];
    sprintf(tempStr, "%.2f*2*2", tempReading);

    char humStr[20];
    sprintf(humStr, "%.2f*2*2", humidityReading);

    float weight = 2.0;
    int gas = 2;

    struct tm timeinfo;
    char timestamp[25];

    if (getLocalTime(&timeinfo))
    {
        sprintf(timestamp,
                "%04d-%02d-%02d %02d:%02d:%02d",
                timeinfo.tm_year + 1900,
                timeinfo.tm_mon + 1,
                timeinfo.tm_mday,
                timeinfo.tm_hour,
                timeinfo.tm_min,
                timeinfo.tm_sec);
    }
    else
    {
        strcpy(timestamp, "0000-00-00 00:00:00");
    }

    file.printf(
        "\"%s\",\"%s\",\"%s\",\"%u\",%.2f,%d\n",
        timestamp,
        tempStr,
        humStr,
        co2Reading,
        weight,
        gas);

    file.close();
}

// ======================================================
// READ SMOOTHED CO2
// ======================================================

bool readCO2Smooth(uint16_t &co2, float &temp, float &humidity)
{
    uint16_t error;
    uint16_t rawCO2 = 0;
    float rawTemp = 0, rawHumidity = 0;

    error = scd4x.readMeasurement(rawCO2, rawTemp, rawHumidity);
    if (error != 0)
    {
        addError("CO2 communication error");

        co2 = lastCO2;
        temp = lastTemp;
        humidity = lastHumidity;

        return false;
    }

    if (rawCO2 == 0)
    {
        addError("[INFO] CO2 measurement not ready");

        co2 = lastCO2;
        temp = lastTemp;
        humidity = lastHumidity;

        return true;
    }

    // Exponential smoothing
    if (lastCO2 > 0)
    {
        co2 = (uint16_t)(0.7 * rawCO2 + 0.3 * lastCO2);
        temp = 0.7 * rawTemp + 0.3 * lastTemp;
        humidity = 0.7 * rawHumidity + 0.3 * lastHumidity;
    }
    else
    {
        co2 = rawCO2;
        temp = rawTemp;
        humidity = rawHumidity;
    }

    lastCO2 = co2;
    lastTemp = temp;
    lastHumidity = humidity;

    return true;
}

// ======================================================
// READ BATTERY VOLTAGE
// ======================================================

float readBatteryVoltage()
{
    float voltage = batterySensor.getBusVoltage_V();

    if (isnan(voltage) || voltage < 0)
    {
        return lastBatteryVoltage;
    }

    // Smooth the reading
    if (lastBatteryVoltage > 0)
    {
        voltage = 0.7 * voltage + 0.3 * lastBatteryVoltage;
    }

    lastBatteryVoltage = voltage;
    return voltage;
}

// ======================================================
// READ MICROPHONE PEAK (AUDIO DATA)
// ======================================================

long readMicrophonePeak(int numSamples = 100)
{
    if (!mic_ok)
        return 0;

    int32_t samples[64];
    size_t bytesRead = 0;
    long peak = 0;
    int samplesRead = 0;

    while (samplesRead < numSamples)
    {
        esp_err_t err = i2s_read(
            I2S_NUM_0,
            samples,
            sizeof(samples),
            &bytesRead,
            pdMS_TO_TICKS(100));

        if (err == ESP_OK && bytesRead > 0)
        {
            int samplesInChunk = bytesRead / sizeof(int32_t);

            for (int i = 0; i < samplesInChunk && samplesRead < numSamples; i++)
            {
                // Scale 32-bit to 16-bit and get absolute value
                long level = abs(samples[i] >> 14);
                if (level > peak)
                    peak = level;
                samplesRead++;
            }
        }
        else
        {
            break;
        }
    }

    return peak;
}

// ======================================================
// SEND DATA TO THINGSPEAK (EVERY 15 SECONDS)
// ======================================================

void sendToThingSpeak(float temp, float humidity, uint16_t co2,
                      float batteryVoltage, long audioPeak)
{

    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("⚠️ WiFi not connected, cannot send to ThingSpeak");
        return;
    }

    Serial.println();
    Serial.println("[THINGSPEAK]");

    // Set the 5 fields
    ThingSpeak.setField(1, temp);           // Temperature
    ThingSpeak.setField(2, humidity);       // Humidity
    ThingSpeak.setField(3, co2);            // CO2
    ThingSpeak.setField(4, batteryVoltage); // Battery Voltage
    ThingSpeak.setField(5, audioPeak);      // Audio Data

    // Write to ThingSpeak
    int httpCode = ThingSpeak.writeFields(channelNumber, writeAPIKey);

    if (httpCode == 200)
    {
        Serial.println("✅ Data sent to ThingSpeak successfully!");
        Serial.printf("   Field 1 (Temp): %.1f°C\n", temp);
        Serial.printf("   Field 2 (Humidity): %.1f%%\n", humidity);
        Serial.printf("   Field 3 (CO2): %d ppm\n", co2);
        Serial.printf("   Field 4 (Battery): %.2f V\n", batteryVoltage);
        Serial.printf("   Field 5 (Audio Peak): %ld\n", audioPeak);
    }
    else
    {
        // Serial.printf("❌ ThingSpeak error: HTTP code %d\n", httpCode);
        addError(
            "ThingSpeak HTTP error " +
            String(httpCode));
    }
}

// ======================================================
// MICROPHONE SETUP
// ======================================================

void setupMicrophone()
{

    Serial.println();
    Serial.println("[MICROPHONE]");
    if (mic_ok)
    {
        i2s_driver_uninstall(I2S_NUM_0);
        mic_ok = false;
    }

    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 64,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0};

    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_SCK,
        .ws_io_num = I2S_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_SD};

    esp_err_t err;

    err = i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);

    if (err != ESP_OK)
    {
        // Serial.println("MIC FAILED");
        addError("Microphone driver install failed");
        mic_ok = false;
        return;
    }

    err = i2s_set_pin(I2S_NUM_0, &pin_config);

    if (err != ESP_OK)
    {
        // Serial.println("PIN FAILED");
        addError("Microphone pin configuration failed");
        mic_ok = false;
        return;
    }

    i2s_zero_dma_buffer(I2S_NUM_0);
    mic_ok = true;
    Serial.println("MIC READY");
}

void setupSDCard()
{

    Serial.println();
    Serial.println("[SD CARD]");

    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

    if (!SD.begin(SD_CS, SPI, 4000000))
    {
        // Serial.println("FAILED");
        addError("[ERROR] SD card initialization failed");
        sd_ok = false;
        return;
    }

    sd_ok = true;
    Serial.println("READY");

    uint64_t cardSize = SD.cardSize() / (1024 * 1024);
    Serial.printf("Card Size: %llu MB\n", cardSize);

    ensureCSVExists();
}

// ======================================================
// WAV HEADER
// ======================================================

void writeWavHeader(File &file, int sampleRate, int totalSamples)
{

    int dataSize = totalSamples * 2;
    int fileSize = dataSize + 36;

    file.write((const uint8_t *)"RIFF", 4);
    file.write((const uint8_t *)&fileSize, 4);
    file.write((const uint8_t *)"WAVE", 4);

    file.write((const uint8_t *)"fmt ", 4);

    int subChunk1Size = 16;
    short audioFormat = 1;
    short numChannels = 1;
    int byteRate = sampleRate * 2;
    short blockAlign = 2;
    short bitsPerSample = 16;

    file.write((const uint8_t *)&subChunk1Size, 4);
    file.write((const uint8_t *)&audioFormat, 2);
    file.write((const uint8_t *)&numChannels, 2);
    file.write((const uint8_t *)&sampleRate, 4);
    file.write((const uint8_t *)&byteRate, 4);
    file.write((const uint8_t *)&blockAlign, 2);
    file.write((const uint8_t *)&bitsPerSample, 2);

    file.write((const uint8_t *)"data", 4);
    file.write((const uint8_t *)&dataSize, 4);
}

// ======================================================
// RECORD AUDIO (Normal rate - 10 seconds every 30 seconds)
// ======================================================

bool recordAudio()
{

    if (!mic_ok || !sd_ok)
        return false;

    String filename = "/" + hiveNo + "_" + getTimestamp() + ".wav";

    Serial.println();
    Serial.println("========================");
    Serial.println("RECORDING AUDIO");
    Serial.println("========================");
    Serial.println(filename);

    File file = SD.open(filename.c_str(), FILE_WRITE);

    if (!file)
    {
        Serial.println("FILE CREATE FAILED");
        return false;
    }

    int totalSamples = SAMPLE_RATE * RECORD_TIME_SEC;
    writeWavHeader(file, SAMPLE_RATE, totalSamples);

    int32_t samples32[256];
    int16_t samples16[256];
    int samplesWritten = 0;

    while (samplesWritten < totalSamples)
    {
        server.handleClient();
        yield();

        size_t bytesRead;

        esp_err_t err = i2s_read(
            I2S_NUM_0,
            samples32,
            sizeof(samples32),
            &bytesRead,
            portMAX_DELAY);

        if (err != ESP_OK)
            continue;

        int count = bytesRead / sizeof(int32_t);

        for (int i = 0; i < count; i++)
        {
            int32_t sample = samples32[i] >> 14;
            if (sample > 32767)
                sample = 32767;
            if (sample < -32768)
                sample = -32768;
            samples16[i] = (int16_t)sample;
        }

        file.write((uint8_t *)samples16, count * sizeof(int16_t));
        samplesWritten += count;
    }

    file.close();
    Serial.println("AUDIO SAVED");
    return true;
}

void handleRoot()
{
    server.send_P(200, "text/html", INDEX_HTML);
}

void handleData()
{
    String json;
    json.reserve(512);
    json = "{";

    json += "\"temp\":" + String(lastTemp, 1);
    json += ",\"humidity\":" + String(lastHumidity, 1);
    json += ",\"co2\":" + String(lastCO2);

    json += ",\"battV\":" + String(lastBatteryVoltage, 2);

    float battCurrent =
        batterySensor.getCurrent_mA() / 1000.0;

    float solarVoltage = solarSensor.getBusVoltage_V();
    float solarCurrent = solarSensor.getCurrent_mA() / 1000.0;

    if (isnan(solarVoltage))
    {
        solarVoltage = 0;
    }

    if (isnan(solarCurrent))
    {
        solarCurrent = 0;
    }

    float solarPower = solarVoltage * solarCurrent;

    json += ",\"battA\":" + String(battCurrent, 3);
    json += ",\"solW\":" + String(solarPower, 2);

    // json += ",\"audio\":" + String(readMicrophonePeak(50));
    json += ",\"audio\":" + String(lastAudioPeak);

    json += ",\"co2_ok\":true";
    json += ",\"batt_ok\":true";

    bool solarConnected =
        solarVoltage > 2.0 &&
        abs(solarCurrent) > 0.005;

    json += ",\"solar_ok\":";
    json += solarConnected ? "true" : "false";

    json += ",\"mic_ok\":";
    json += mic_ok ? "true" : "false";

    json += ",\"sd_ok\":";
    json += sd_ok ? "true" : "false";

    json += ",\"cloud_ok\":";

    json += (WiFi.status() == WL_CONNECTED ? "true" : "false");

    json += "}";

    server.send(200, "application/json", json);
}

//=====================================================
// POWER FUNCTIONS
//=====================================================
void powerOnMicrophone()
{
    digitalWrite(MIC_POWER_PIN, HIGH);
    delay(100);

    setupMicrophone();
}

void powerOffMicrophone()
{
    if (mic_ok)
    {
        i2s_driver_uninstall(I2S_NUM_0);
        mic_ok = false;
    }

    mic_ok = false;

    digitalWrite(MIC_POWER_PIN, LOW);
}
//=====================================================
// HANDLE SETTINGS IMPLEMENTATION
//=====================================================
void handleSettings()
{
    String json;
    json.reserve(512);
    json = "{";

    json += "\"sensor\":" +
            String(sensorInterval / 1000);

    json += ",\"ts\":" +
            String(thingSpeakInterval / 1000);

    json += ",\"record\":" +
            String(recordInterval);

    json += ",\"upload\":" +
            String(uploadInterval / 1000);

    json += ",\"fullDuty\":";
    json += fullDutyCycle ? "true" : "false";

    json += "}";

    server.send(
        200,
        "application/json",
        json);
}
// ======================================================
// SETUP
// ======================================================
void setup()
{

    Serial.begin(115200);
    delay(1000);

    prefs.begin("duty", false);

    sensorInterval =
        prefs.getULong("sensor", 60000);

    thingSpeakInterval =
        prefs.getULong("ts", 15000);

    recordInterval =
        prefs.getULong("record", 30);

    uploadInterval =
        prefs.getULong("upload", 30000);
    fullDutyCycle =
        prefs.getBool("fullDuty", false);

    Serial.println();
    Serial.println("================================");
    Serial.println("WIFI AUDIO LOGGER + THINGSPEAK + AWS");
    Serial.println("5 FIELDS: Temp, Humidity, CO2, Battery, Audio");
    Serial.println("Sending sensor data EVERY 15 SECONDS");
    Serial.println("Recording audio: 10 seconds every 30 seconds");
    Serial.println("================================");

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
    pinMode(MIC_POWER_PIN, OUTPUT);
    digitalWrite(MIC_POWER_PIN, LOW);

    // AP
    // connectWiFi();
    startAP();

    server.on("/", handleRoot);
    server.on(
        "/settingsPage",
        handleSettingsPage);
    server.on("/data", handleData);
    server.on("/setDuty", handleSetDuty);
    server.on("/settings", handleSettings);
    server.on("/errors", handleErrors);

    server.begin();

    Serial.println("Dashboard Ready");

    // Time
    if (connectSTA())
    {
        setupTime();
        disconnectSTA();
    }
    else
    {
        Serial.println("Failed to connect to WiFi for time sync");
    }

    // ThingSpeak
    ThingSpeak.begin(client);

    // I2C
    Wire.begin(SDA_PIN, SCL_PIN);

    Serial.println("Scanning I2C...");

    for (byte addr = 1; addr < 127; addr++)
    {

        Wire.beginTransmission(addr);

        if (Wire.endTransmission() == 0)
        {
            Serial.print("Found: 0x");
            Serial.println(addr, HEX);
        }
    }

    Wire.setClock(50000);

    // INA219 Sensors
    if (batterySensor.begin())
    {
        batterySensor.setCalibration_32V_2A();
        Serial.println("Battery Sensor: OK");
    }
    else
    {
        Serial.println("Battery Sensor: FAILED");
    }

    if (solarSensor.begin())
    {
        solarSensor.setCalibration_32V_2A();
        Serial.println("Solar Sensor: OK");
    }
    else
    {
        Serial.println("Solar Sensor: FAILED");
    }

    // SCD41 CO2 Sensor
    scd4x.begin(Wire, 0x62);

    delay(2000);

    uint64_t serialNumber;
    if (scd4x.getSerialNumber(serialNumber) == 0)
    {
        scd4x.startPeriodicMeasurement();
        Serial.println("CO2 Sensor: OK");
        delay(500);
    }
    else
    {
        Serial.println("CO2 Sensor: FAILED");
    }

    // SD Card
    setupSDCard();

    // Microphone
    // setupMicrophone();

    Serial.println();
    Serial.println("SYSTEM READY");
    Serial.println("📊 Sensor data sent to ThingSpeak EVERY 15 seconds");
    Serial.println("☁️ Sensor data (CSV rows) sent to AdEMNEA on AWS every upload window");
    Serial.println("🎤 Audio recorded for 10 seconds EVERY 30 seconds");
    Serial.println("📁 Audio files saved to SD card (not sent to cloud)");
    Serial.println("   Fields: Temp | Humidity | CO2 | Battery | Audio Peak");
}

void loop()
{
    if (fullDutyCycle)
    {
        dutyCycleLoop();
    }
    else
    {
        normalLoop();
    }
}

void dutyCycleLoop()
{
    server.handleClient();

    if (millis() - lastSensorTime >= sensorInterval)
    {
        bool co2Ok = readCO2Smooth(co2, temp, humidity);

        batteryVoltage = readBatteryVoltage();

        powerOnMicrophone();
        delay(200);

        audioPeak = readMicrophonePeak(100);
        lastAudioPeak = audioPeak;

        powerOffMicrophone();

        if (co2Ok)
        {
            logToCSV(temp, humidity, co2);
            Serial.println("Sensor data saved to CSV");
        }

        lastSensorTime = millis();
    }

    if (millis() - lastUploadTime >= uploadInterval)
    {
        if (connectSTA())
        {
            sendToThingSpeak(
                temp,
                humidity,
                co2,
                batteryVoltage,
                audioPeak);

            // AWS: send the CSV rows; the file is cleared once all have arrived.
            uploadCsvToAws(true);

            disconnectSTA();
        }

        lastUploadTime = millis();
    }

    if (millis() - lastRecordTime >= (recordInterval * 1000UL))
    {
        powerOnMicrophone();
        delay(200);

        recordAudio();

        powerOffMicrophone();

        lastRecordTime = millis();
    }

    if (WiFi.softAPgetStationNum() == 0)
    {
        Serial.println("Preparing for sleep...");

        scd4x.stopPeriodicMeasurement();

        powerOffMicrophone();

        disconnectSTA();

        esp_sleep_enable_timer_wakeup(
            sensorInterval * 1000ULL);

        Serial.println("Entering Light Sleep");

        esp_light_sleep_start();

        Serial.println("Awake");

        scd4x.startPeriodicMeasurement();

        delay(500);
    }
} // ======================================================
// LOOP
// ======================================================

void normalLoop()
{

    server.handleClient();

    if (millis() - lastSensorTime >= sensorInterval)
    {
        bool co2Ok = readCO2Smooth(co2, temp, humidity);

        batteryVoltage = readBatteryVoltage();

        powerOnMicrophone();
        delay(200);

        audioPeak = readMicrophonePeak(100);
        lastAudioPeak = audioPeak;

        powerOffMicrophone();

        // Save one record for every sensor reading cycle
        if (co2Ok)
        {
            logToCSV(temp, humidity, co2);

            Serial.println("Sensor data saved to CSV");
        }

        lastSensorTime = millis();
    }

    // Display every 10 seconds
    if (millis() - lastDisplayTime >= 10000)
    {
        Serial.println();
        Serial.println("================================");
        Serial.printf("TIME: %s\n", getTimestamp().c_str());
        Serial.println();
        Serial.println("[SENSOR DATA]");
        Serial.printf("🌡️ Temperature: %.1f°C\n", temp);
        Serial.printf("💧 Humidity: %.1f%%\n", humidity);
        Serial.printf("💨 CO2: %u ppm\n", co2);
        Serial.printf("🔋 Battery Voltage: %.2f V\n", batteryVoltage);
        Serial.printf("🎤 Audio Peak: %ld\n", audioPeak);
        Serial.printf("💾 SD Card: %s\n", sd_ok ? "ONLINE" : "OFFLINE");
        Serial.println("================================");
        lastDisplayTime = millis();
    }

    // Send to ThingSpeak EVERY 15 SECONDS (respects free tier limit)
    if (millis() - lastThingSpeakTime >= thingSpeakInterval)
    {
        if (connectSTA())
        {
            sendToThingSpeak(temp, humidity, co2, batteryVoltage, audioPeak);
            disconnectSTA();
        }
        lastThingSpeakTime = millis();
    }

    if (millis() - lastUploadTime >= uploadInterval)
    {
        Serial.println("===== STARTING AWS UPLOAD =====");
        if (connectSTA())
        {
            // AWS: send the CSV rows; the file is cleared once all have arrived.
            uploadCsvToAws(true);

            disconnectSTA();
        }
        Serial.println("===== AWS UPLOAD COMPLETE =====");
        lastUploadTime = millis();
    }

    // Record audio EVERY 30 SECONDS (10 second recordings)
    if (millis() - lastRecordTime >= (recordInterval * 1000UL))
    {
        powerOnMicrophone();
        delay(200); // Allow microphone to stabilize

        recordAudio();

        powerOffMicrophone();

        lastRecordTime = millis();
    }

    delay(10); // Small delay to keep CPU responsive
}