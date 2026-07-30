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
// WIFI
// ======================================================

const char *ssid = "IoT-ra";
const char *password = "P9$y#F5x!b&";

// ======================================================
// THINGSPEAK CONFIGURATION
// ======================================================

unsigned long channelNumber = 3384879;
const char *writeAPIKey = "HEHEZST627ICEU7E";
String hiveNo = "12";

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

const char *SERVER_HOST = "196.43.168.57";
const int SERVER_PORT = 8005;
const char *API_ENDPOINT = "/upload";
const char *BEARER_TOKEN = "my_secure_hive_key_123";

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
//=====================================================
// UPLOADING FILES TO SERVER
//=====================================================

bool uploadFileToServer(String filepath)
{
    if (!sd_ok)
        return false;

    if (WiFi.status() != WL_CONNECTED)
        return false;

    // Ensure SD path starts with /
    if (!filepath.startsWith("/"))
    {
        filepath = "/" + filepath;
    }

    File file = SD.open(filepath, FILE_READ);

    if (!file)
    {
        Serial.println("Failed to open file:");
        Serial.println(filepath);
        return false;
    }

    Serial.print("Size: ");
    Serial.println(file.size());

    Serial.print("Uploading: ");
    Serial.println(filepath);

    String filename = filepath;
    filename.replace("/", "");

    String boundary = "----ESP32Boundary7MA4YWxkTrZu0gW";

    String head =
        "--" + boundary + "\r\n"
                          "Content-Disposition: form-data; name=\"file\"; filename=\"" +
        filename + "\"\r\n";

    if (filename.endsWith(".wav"))
        head += "Content-Type: audio/wav\r\n\r\n";
    else
        head += "Content-Type: text/csv\r\n\r\n";

    String tail =
        "\r\n--" + boundary + "--\r\n";

    uint32_t totalLength =
        head.length() +
        file.size() +
        tail.length();

    WiFiClient client;

    if (!client.connect(SERVER_HOST, SERVER_PORT))
    {
        Serial.println("Connection failed");
        file.close();
        return false;
    }

    client.print("POST ");
    client.print(API_ENDPOINT);
    client.println(" HTTP/1.1");

    client.print("Host: ");
    client.println(SERVER_HOST);

    client.print("Authorization: Bearer ");
    client.println(BEARER_TOKEN);
    client.println("Content-Type: multipart/form-data; boundary=" + boundary);
    client.print("Content-Length: ");
    client.println(totalLength);
    client.println();

    client.print(head);

    uint8_t buffer[1024];

    while (file.available())
    {
        size_t len = file.read(buffer, sizeof(buffer));
        client.write(buffer, len);
    }

    client.print(tail);
    size_t fileSize = file.size();

    file.close();

    unsigned long start = millis();

    while (!client.available())
    {
        server.handleClient();
        yield();
        if (millis() - start > 15000)
        {
            // Serial.println("Upload timeout");
            addError("Server upload timeout");
            client.stop();

            Serial.print("Uploading: ");
            Serial.println(filepath);

            Serial.print("Size: ");
            // Serial.println(file.size());
            Serial.println(fileSize);

            return false;
        }
    }

    String response = "";

    while (client.available())
    {
        response += client.readString();
    }

    client.stop();

    Serial.println("Server response:");
    Serial.println(response);

    bool success =
        response.indexOf("200 OK") >= 0 ||
        response.indexOf("201 Created") >= 0;

    if (success)
    {
        Serial.print("Deleting uploaded file: ");
        Serial.println(filepath);

        if (SD.remove(filepath))
        {
            Serial.println("Deleted successfully");
        }
        else
        {
            Serial.println("Delete failed");
        }
    }

    return success;
}

// =============================================================
// UPLOAD PENDING FILES
// =============================================================

void uploadPendingFiles()
{
    if (!sd_ok)
        return;

    if (WiFi.status() != WL_CONNECTED)
        return;

    File root = SD.open("/");

    if (!root)
        return;

    File file = root.openNextFile();

    while (file)
    {
        String filename = file.name();
        Serial.print("Found file: ");
        Serial.println(filename);
        bool upload = false;

        if (filename == hiveNo + ".csv")
        {
            // String apiKey = "7747ea95-9682-42a7-b0aa-44eb374b4300";

            // String hiveName = "Hive 01";
            // String hiveNameEncoded = hiveName;
            // hiveNameEncoded.replace(" ", "%20");

            // String path =
            //     "/conditions/hives/" +
            //     hiveNameEncoded +
            //     "/upload";

            //--------------------------------------------------
            // Open CSV file from SD
            //--------------------------------------------------
            File csvFile = SD.open("/" + hiveNo + ".csv", FILE_READ);

            if (!csvFile)
            {
                Serial.println("Failed to open CSV file");
                continue;
            }

            //--------------------------------------------------
            // Generate timestamp filename
            //--------------------------------------------------
            struct tm timeinfo;

            if (!getLocalTime(&timeinfo))
            {
                Serial.println("Failed to obtain time");
                csvFile.close();
                continue;
            }

            char uploadFilename[80];

            snprintf(
                uploadFilename,
                sizeof(uploadFilename),
                "%s_%04d_%02d_%02d_%02d%02d%02d.csv",
                hiveNo.c_str(),
                timeinfo.tm_year + 1900,
                timeinfo.tm_mon + 1,
                timeinfo.tm_mday,
                timeinfo.tm_hour,
                timeinfo.tm_min,
                timeinfo.tm_sec);

            Serial.print("SD filename: ");
            Serial.println(hiveNo + ".csv");

            Serial.print("Upload filename: ");
            Serial.println(uploadFilename);

            //--------------------------------------------------
            // Multipart setup
            //--------------------------------------------------
            String boundary = "----ESP32Boundary12345";

            String multipartHeader =
                "--" + boundary + "\r\n"
                                  "Content-Disposition: form-data; name=\"file\"; filename=\"" +
                String(uploadFilename) +
                "\"\r\n"
                "Content-Type: text/csv\r\n\r\n";

            String multipartFooter =
                "\r\n--" + boundary + "--\r\n";

            size_t contentLength =
                multipartHeader.length() +
                csvFile.size() +
                multipartFooter.length();

            //--------------------------------------------------
            // HTTPS connection
            //--------------------------------------------------
            WiFiClientSecure client;
            client.setInsecure();

            Serial.println("Connecting to server...");

            if (!client.connect("swarming.ademneaproject.net", 443))
            {
                Serial.println("Connection failed");
                csvFile.close();
                continue;
            }

            Serial.println("Connected");

            //--------------------------------------------------
            // HTTP request
            //--------------------------------------------------
            client.print("POST ");
            client.print(path);
            client.println(" HTTP/1.1");

            client.println("Host: swarming.ademneaproject.net");
            client.println("Accept: application/json");
            client.println("x-api-key: " + apiKey);
            client.println("Content-Type: multipart/form-data; boundary=" + boundary);
            client.println("Content-Length: " + String(contentLength));
            client.println("Connection: close");
            client.println();

            //--------------------------------------------------
            // Multipart header
            //--------------------------------------------------
            client.print(multipartHeader);

            //--------------------------------------------------
            // Stream CSV
            //--------------------------------------------------
            uint8_t buffer[1024];

            size_t totalSent = 0;

            while (csvFile.available())
            {
                server.handleClient();
                yield();
                size_t len = csvFile.read(buffer, sizeof(buffer));

                if (len > 0)
                {
                    size_t written = client.write(buffer, len);
                    totalSent += written;
                }
            }

            csvFile.close();

            //--------------------------------------------------
            // Multipart footer
            //--------------------------------------------------
            client.print(multipartFooter);

            Serial.print("CSV bytes uploaded: ");
            Serial.println(totalSent);

            Serial.println("Upload sent");
            Serial.println("Waiting for response...");

            //--------------------------------------------------
            // Wait for response
            //--------------------------------------------------
            unsigned long start = millis();

            while (!client.available())
            {
                server.handleClient();
                yield();
                if (millis() - start > 15000)
                {
                    Serial.println("Response timeout");
                    client.stop();
                    break;
                }

                delay(10);
            }

            //--------------------------------------------------
            // Print response
            //--------------------------------------------------
            Serial.println("===== SERVER RESPONSE =====");

            while (client.available())
            {
                String line = client.readStringUntil('\n');
                Serial.println(line);
            }

            Serial.println("===========================");

            client.stop();

            upload = true;
        }
        else if (
            filename.startsWith(hiveNo + "_") &&
            filename.endsWith(".wav"))
        {
            String apiKey = "7747ea95-9682-42a7-b0aa-44eb374b4300";

            //--------------------------------------------------
            // Generate hive name for endpoint
            //--------------------------------------------------
            String hiveName = hiveNo;
            hiveName.replace("_", " "); // Hive_01 -> Hive 01

            String hiveNameEncoded = hiveName;
            hiveNameEncoded.replace(" ", "%20");

            String path =
                "/recordings/hives/" +
                hiveNameEncoded +
                "/upload";

            //--------------------------------------------------
            // Generate timestamp filename
            //--------------------------------------------------
            struct tm timeinfo;

            if (!getLocalTime(&timeinfo))
            {
                Serial.println("Failed to obtain time");
                continue;
            }

            char uploadFilename[80];

            snprintf(
                uploadFilename,
                sizeof(uploadFilename),
                "%s_%04d_%02d_%02d_%02d%02d%02d.wav",
                hiveNo.c_str(),
                timeinfo.tm_year + 1900,
                timeinfo.tm_mon + 1,
                timeinfo.tm_mday,
                timeinfo.tm_hour,
                timeinfo.tm_min,
                timeinfo.tm_sec);

            Serial.println("\n=================================");
            Serial.println("STARTING WAV UPLOAD");
            Serial.println("=================================");

            Serial.print("SD File: ");
            Serial.println(filename);

            Serial.print("Upload Name: ");
            Serial.println(uploadFilename);

            //--------------------------------------------------
            // Open WAV file
            //--------------------------------------------------
            File wavFile = SD.open("/" + filename, FILE_READ);

            if (!wavFile)
            {
                Serial.println("Failed to open WAV file");
                continue;
            }

            //--------------------------------------------------
            // Multipart setup
            //--------------------------------------------------
            String boundary = "----ESP32Boundary12345";

            String multipartHeader =
                "--" + boundary + "\r\n"
                                  "Content-Disposition: form-data; name=\"file\"; filename=\"" +
                String(uploadFilename) +
                "\"\r\n"
                "Content-Type: audio/wav\r\n\r\n";

            String multipartFooter =
                "\r\n--" + boundary + "--\r\n";

            size_t contentLength =
                multipartHeader.length() +
                wavFile.size() +
                multipartFooter.length();

            Serial.print("WAV Size: ");
            Serial.println(wavFile.size());

            Serial.print("Request Size: ");
            Serial.println(contentLength);

            //--------------------------------------------------
            // HTTPS Connection
            //--------------------------------------------------
            WiFiClientSecure client;
            client.setInsecure();

            Serial.println("Connecting to server...");

            if (!client.connect("swarming.ademneaproject.net", 443))
            {
                Serial.println("Connection failed");
                wavFile.close();
                continue;
            }

            Serial.println("Connected");

            //--------------------------------------------------
            // HTTP Request
            //--------------------------------------------------
            client.print("POST ");
            client.print(path);
            client.println(" HTTP/1.1");

            client.println("Host: swarming.ademneaproject.net");
            client.println("Accept: application/json");
            client.println("x-api-key: " + apiKey);
            client.println(
                "Content-Type: multipart/form-data; boundary=" + boundary);
            client.println("Content-Length: " + String(contentLength));
            client.println("Connection: close");
            client.println();

            //--------------------------------------------------
            // Multipart Header
            //--------------------------------------------------
            client.print(multipartHeader);

            //--------------------------------------------------
            // Stream WAV File
            //--------------------------------------------------
            uint8_t buffer[2048];

            size_t totalSent = 0;

            while (wavFile.available())
            {
                server.handleClient();
                yield();
                size_t bytesRead =
                    wavFile.read(buffer, sizeof(buffer));

                if (bytesRead > 0)
                {
                    size_t bytesWritten =
                        client.write(buffer, bytesRead);

                    totalSent += bytesWritten;

                    if (totalSent % 32768 < bytesRead)
                    {
                        Serial.print("Uploaded ");
                        Serial.print(totalSent);
                        Serial.print(" / ");
                        Serial.println(wavFile.size());
                    }
                }
            }

            wavFile.close();

            //--------------------------------------------------
            // Multipart Footer
            //--------------------------------------------------
            client.print(multipartFooter);

            Serial.println("Upload complete.");
            Serial.println("Waiting for response...");

            //--------------------------------------------------
            // Wait for response
            //--------------------------------------------------
            unsigned long start = millis();

            while (!client.available())
            {
                server.handleClient();
                yield();
                if (millis() - start > 30000)
                {
                    Serial.println("Response timeout");
                    client.stop();
                    break;
                }

                delay(10);
            }

            //--------------------------------------------------
            // Print response
            //--------------------------------------------------
            Serial.println("\n===== SERVER RESPONSE =====");

            while (client.available())
            {
                String line = client.readStringUntil('\n');
                Serial.println(line);
            }

            Serial.println("===========================");

            client.stop();

            upload = true;
        }

        if (upload)
        {
            Serial.println();
            Serial.print("Uploading: ");
            Serial.println(filename);

            uploadFileToServer(filename);
        }

        file.close();
        file = root.openNextFile();
    }

    root.close();
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
    Serial.println("WIFI AUDIO LOGGER + THINGSPEAK");
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
            uploadPendingFiles();

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
        Serial.println("===== STARTING FILE UPLOAD =====");
        if (connectSTA())
        {
            uploadPendingFiles();

            disconnectSTA();
        }
        Serial.println("===== FILE UPLOAD COMPLETE =====");
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