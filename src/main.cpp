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

// ======================================================
// WIFI
// ======================================================

const char* ssid     = "IoT-ra";
const char* password = "P9$y#F5x!b&";

// ======================================================
// THINGSPEAK CONFIGURATION
// ======================================================

unsigned long channelNumber = 3384879;
const char* writeAPIKey = "HEHEZST627ICEU7E";
String hiveNo = "12";

// ======================================================
// THINGSPEAK TIMING - SEND EVERY 15 SECONDS
// ======================================================

#define THINGSPEAK_INTERVAL    15000  // 15 seconds exactly (free tier limit)

// ======================================================
// TIME SERVER
// ======================================================

const char* ntpServer = "pool.ntp.org";
// const char* ntpServer = "time.google.com";
// const char* ntpServer = "time.windows.com";
const long gmtOffset_sec = 3 * 3600;
const int daylightOffset_sec = 0;



//=======================================================
//API ENDPOINT CONFIGURATION
//=======================================================


const char* SERVER_HOST = "196.43.168.57";
const int SERVER_PORT = 8005;
const char* API_ENDPOINT = "/upload";
const char* BEARER_TOKEN = "my_secure_hive_key_123";




// ======================================================
// I2C PINS
// ======================================================

#define SDA_PIN        21
#define SCL_PIN        22

// ======================================================
// SD CARD PINS
// ======================================================

#define SD_MISO        2
#define SD_MOSI        15
#define SD_SCK         14
#define SD_CS          13

// ======================================================
// INMP441 MICROPHONE PINS
// ======================================================

#define I2S_WS         25
#define I2S_SCK        26
#define I2S_SD         27

// ======================================================
// LED
// ======================================================

#define LED_PIN        33

// ======================================================
// AUDIO SETTINGS
// ======================================================

#define SAMPLE_RATE        16000
#define RECORD_TIME_SEC    10
#define RECORD_INTERVAL    30

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


const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">

<title>🐝 Smart Beehive Monitor</title>

<style>
*{
    margin:0;
    padding:0;
    box-sizing:border-box;
}

body{
    font-family:Arial,sans-serif;
    background:linear-gradient(135deg,#fef3c7,#fde68a);
    padding:20px;
}

.container{
    max-width:900px;
    margin:0 auto;
}

header{
    background:#f59e0b;
    color:white;
    padding:20px;
    border-radius:16px;
    text-align:center;
    margin-bottom:20px;
}

header h1{
    margin-bottom:10px;
}

.grid{
    display:grid;
    grid-template-columns:repeat(auto-fit,minmax(180px,1fr));
    gap:15px;
}

.card{
    background:white;
    border-radius:16px;
    padding:20px;
    text-align:center;
    box-shadow:0 4px 8px rgba(0,0,0,0.1);
}

.icon{
    font-size:32px;
}

.value{
    font-size:30px;
    font-weight:bold;
    margin:10px 0;
    color:#222;
}

.label{
    color:#666;
    font-size:14px;
}

.status-bar{
    display:flex;
    flex-wrap:wrap;
    justify-content:center;
    gap:10px;
    margin-top:20px;
}

.badge{
    padding:8px 14px;
    border-radius:20px;
    color:white;
    font-size:13px;
    font-weight:bold;
}

.ok{
    background:#10b981;
}

.err{
    background:#ef4444;
}

.footer{
    text-align:center;
    margin-top:20px;
    color:#555;
    font-size:14px;
}
</style>
</head>

<body>

<div class="container">

<header>
    <h1>🐝 Smart Beehive Monitor</h1>
    <p>Live Hive Monitoring Dashboard</p>
</header>

<div class="grid">

    <div class="card">
        <div class="icon">🌡️</div>
        <div id="temp" class="value">--</div>
        <div class="label">Temperature (°C)</div>
    </div>

    <div class="card">
        <div class="icon">💧</div>
        <div id="hum" class="value">--</div>
        <div class="label">Humidity (%)</div>
    </div>

    <div class="card">
        <div class="icon">💨</div>
        <div id="co2" class="value">--</div>
        <div class="label">CO₂ (ppm)</div>
    </div>

    <div class="card">
        <div class="icon">🔋</div>
        <div id="battery" class="value">--</div>
        <div class="label">Battery (V)</div>
    </div>

    <div class="card">
        <div class="icon">⚡</div>
        <div id="current" class="value">--</div>
        <div class="label">Current (A)</div>
    </div>

    <div class="card">
        <div class="icon">☀️</div>
        <div id="solar" class="value">--</div>
        <div class="label">Solar Power (W)</div>
    </div>

    <div class="card">
        <div class="icon">🎤</div>
        <div id="audio" class="value">--</div>
        <div class="label">Audio Peak</div>
    </div>

</div>

<div class="status-bar" id="statusBar"></div>

<div class="footer">
    Last Update: <span id="lastUpdated">--</span>
</div>

</div>

<script>

async function updateData()
{
    try
    {
        const response = await fetch('/data');
        const data = await response.json();

        document.getElementById("temp").innerHTML =
            Number(data.temp).toFixed(1);

        document.getElementById("hum").innerHTML =
            Number(data.humidity).toFixed(1);

        document.getElementById("co2").innerHTML =
            data.co2;

        document.getElementById("battery").innerHTML =
            Number(data.battV).toFixed(2);

        document.getElementById("current").innerHTML =
            Number(data.battA).toFixed(3);

        document.getElementById("solar").innerHTML =
            Number(data.solW).toFixed(2);

        document.getElementById("audio").innerHTML =
            data.audio;

        const statusBar =
            document.getElementById("statusBar");

        statusBar.innerHTML = "";

        const sensors = [
            ["CO₂", data.co2_ok],
            ["Battery", data.batt_ok],
            ["Solar", data.solar_ok],
            ["Microphone", data.mic_ok],
            ["SD Card", data.sd_ok],
            ["Cloud", data.cloud_ok]
        ];

        sensors.forEach(sensor => {

            const badge =
                document.createElement("span");

            badge.className =
                "badge " +
                (sensor[1] ? "ok" : "err");

            badge.innerHTML =
                (sensor[1] ? "✓ " : "✗ ") +
                sensor[0];

            statusBar.appendChild(badge);
        });

        document.getElementById("lastUpdated")
            .innerHTML =
            new Date().toLocaleTimeString();
    }
    catch(err)
    {
        console.log(err);
    }
}

updateData();
setInterval(updateData,2000);

</script>

</body>
</html>
)rawliteral";

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
        delay(500);
        Serial.print(".");
    }

    Serial.println();
    Serial.println("Connected to Router");

    // Create ESP32 WiFi
    WiFi.softAP(
        "HiveMonitor",
        "12345678"
    );

    Serial.println();

    Serial.print("Router IP: ");
    Serial.println(WiFi.localIP());

    Serial.print("Dashboard IP: ");
    Serial.println(WiFi.softAPIP());

    digitalWrite(LED_PIN, HIGH);
}


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

    Serial.print("Uploading: ");
    Serial.println(filepath);

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

    Serial.print("Size: ");
    Serial.println(file.size());

    if (!file)
    {
        Serial.println("Failed to open file:");
        Serial.println(filepath);
        return false;
    }

    String filename = filepath;
    filename.replace("/", "");

    String boundary = "----ESP32Boundary7MA4YWxkTrZu0gW";

    String head =
        "--" + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"" + filename + "\"\r\n";

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

    file.close();

    unsigned long start = millis();

    while (!client.available())
    {
        if (millis() - start > 15000)
        {
            Serial.println("Upload timeout");
            client.stop();

            Serial.print("Uploading: ");
            Serial.println(filepath);

            Serial.print("Size: ");
            Serial.println(file.size());



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

        // Upload 10.csv
        if (filename == hiveNo + ".csv")
        {
            upload = true;
        }

        // Upload only WAV files beginning with 10_
        else if (
            filename.startsWith(hiveNo + "_") &&
            filename.endsWith(".wav"))
        {
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
        gas
    );

    file.close();
}

// ======================================================
// WIFI MONITOR
// ======================================================

void checkWiFiConnection() {

    if (WiFi.status() == WL_CONNECTED) {
        digitalWrite(LED_PIN, HIGH);
        return;
    }

    Serial.println();
    Serial.println("WiFi Lost!");
    digitalWrite(LED_PIN, LOW);
    connectWiFi();
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
}

// ======================================================
// TIME SETUP
// ======================================================

void setupTime() {

    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

    Serial.println();
    Serial.println("[TIME]");

    struct tm timeinfo;

    while (!getLocalTime(&timeinfo)) {
        Serial.println("Waiting for NTP time...");
        delay(1000);
    }

    Serial.println("Time Synced");
}



// ======================================================
// GET TIMESTAMP
// ======================================================

String getTimestamp() {

    struct tm timeinfo;

    if (!getLocalTime(&timeinfo)) {
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
        timeinfo.tm_sec
    );

    return String(buffer);
}

// ======================================================
// READ SMOOTHED CO2
// ======================================================

bool readCO2Smooth(uint16_t &co2, float &temp, float &humidity) {
    uint16_t error;
    uint16_t rawCO2 = 0;
    float rawTemp = 0, rawHumidity = 0;
    
    error = scd4x.readMeasurement(rawCO2, rawTemp, rawHumidity);
    
    if (error != 0 || rawCO2 == 0) {
        co2 = lastCO2;
        temp = lastTemp;
        humidity = lastHumidity;
        return false;
    }
    
    // Exponential smoothing
    if (lastCO2 > 0) {
        co2 = (uint16_t)(0.7 * rawCO2 + 0.3 * lastCO2);
        temp = 0.7 * rawTemp + 0.3 * lastTemp;
        humidity = 0.7 * rawHumidity + 0.3 * lastHumidity;
    } else {
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

float readBatteryVoltage() {
    float voltage = batterySensor.getBusVoltage_V();
    
    if (isnan(voltage) || voltage < 0) {
        return lastBatteryVoltage;
    }
    
    // Smooth the reading
    if (lastBatteryVoltage > 0) {
        voltage = 0.7 * voltage + 0.3 * lastBatteryVoltage;
    }
    
    lastBatteryVoltage = voltage;
    return voltage;
}

// ======================================================
// READ MICROPHONE PEAK (AUDIO DATA)
// ======================================================

long readMicrophonePeak(int numSamples = 100) {
    if (!mic_ok) return 0;
    
    int32_t samples[64];
    size_t bytesRead = 0;
    long peak = 0;
    int samplesRead = 0;
    
    while (samplesRead < numSamples) {
        esp_err_t err = i2s_read(
            I2S_NUM_0,
            samples,
            sizeof(samples),
            &bytesRead,
            pdMS_TO_TICKS(100)
        );
        
        if (err == ESP_OK && bytesRead > 0) {
            int samplesInChunk = bytesRead / sizeof(int32_t);
            
            for (int i = 0; i < samplesInChunk && samplesRead < numSamples; i++) {
                // Scale 32-bit to 16-bit and get absolute value
                long level = abs(samples[i] >> 14);
                if (level > peak) peak = level;
                samplesRead++;
            }
        } else {
            break;
        }
    }
    
    return peak;
}

// ======================================================
// SEND DATA TO THINGSPEAK (EVERY 15 SECONDS)
// ======================================================

void sendToThingSpeak(float temp, float humidity, uint16_t co2, 
                      float batteryVoltage, long audioPeak) {
    
    if (WiFi.status() != WL_CONNECTED) {
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
    
    if (httpCode == 200) {
        Serial.println("✅ Data sent to ThingSpeak successfully!");
        Serial.printf("   Field 1 (Temp): %.1f°C\n", temp);
        Serial.printf("   Field 2 (Humidity): %.1f%%\n", humidity);
        Serial.printf("   Field 3 (CO2): %d ppm\n", co2);
        Serial.printf("   Field 4 (Battery): %.2f V\n", batteryVoltage);
        Serial.printf("   Field 5 (Audio Peak): %ld\n", audioPeak);
    } else {
        Serial.printf("❌ ThingSpeak error: HTTP code %d\n", httpCode);
    }
}

// ======================================================
// MICROPHONE SETUP
// ======================================================

void setupMicrophone() {

    Serial.println();
    Serial.println("[MICROPHONE]");

    i2s_driver_uninstall(I2S_NUM_0);

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
        .fixed_mclk = 0
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_SCK,
        .ws_io_num = I2S_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_SD
    };

    esp_err_t err;

    err = i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);

    if (err != ESP_OK) {
        Serial.println("MIC FAILED");
        mic_ok = false;
        return;
    }

    err = i2s_set_pin(I2S_NUM_0, &pin_config);

    if (err != ESP_OK) {
        Serial.println("PIN FAILED");
        mic_ok = false;
        return;
    }

    i2s_zero_dma_buffer(I2S_NUM_0);
    mic_ok = true;
    Serial.println("MIC READY");
}

void setupSDCard() {

    Serial.println();
    Serial.println("[SD CARD]");

    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

    if (!SD.begin(SD_CS, SPI, 4000000)) {
        Serial.println("FAILED");
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

void writeWavHeader(File &file, int sampleRate, int totalSamples) {

    int dataSize = totalSamples * 2;
    int fileSize = dataSize + 36;

    file.write((const uint8_t*)"RIFF", 4);
    file.write((const uint8_t*)&fileSize, 4);
    file.write((const uint8_t*)"WAVE", 4);

    file.write((const uint8_t*)"fmt ", 4);

    int subChunk1Size = 16;
    short audioFormat = 1;
    short numChannels = 1;
    int byteRate = sampleRate * 2;
    short blockAlign = 2;
    short bitsPerSample = 16;

    file.write((const uint8_t*)&subChunk1Size, 4);
    file.write((const uint8_t*)&audioFormat, 2);
    file.write((const uint8_t*)&numChannels, 2);
    file.write((const uint8_t*)&sampleRate, 4);
    file.write((const uint8_t*)&byteRate, 4);
    file.write((const uint8_t*)&blockAlign, 2);
    file.write((const uint8_t*)&bitsPerSample, 2);

    file.write((const uint8_t*)"data", 4);
    file.write((const uint8_t*)&dataSize, 4);
}

// ======================================================
// RECORD AUDIO (Normal rate - 10 seconds every 30 seconds)
// ======================================================

bool recordAudio() {

    if (!mic_ok || !sd_ok)
        return false;

    String filename = "/" + hiveNo + "_" + getTimestamp() + ".wav";

    Serial.println();
    Serial.println("========================");
    Serial.println("RECORDING AUDIO");
    Serial.println("========================");
    Serial.println(filename);

    File file = SD.open(filename.c_str(), FILE_WRITE);

    if (!file) {
        Serial.println("FILE CREATE FAILED");
        return false;
    }

    int totalSamples = SAMPLE_RATE * RECORD_TIME_SEC;
    writeWavHeader(file, SAMPLE_RATE, totalSamples);

    int32_t samples32[256];
    int16_t samples16[256];
    int samplesWritten = 0;

    while (samplesWritten < totalSamples) {

        size_t bytesRead;

        esp_err_t err = i2s_read(
            I2S_NUM_0,
            samples32,
            sizeof(samples32),
            &bytesRead,
            portMAX_DELAY
        );

        if (err != ESP_OK)
            continue;

        int count = bytesRead / sizeof(int32_t);

        for (int i = 0; i < count; i++) {
            int32_t sample = samples32[i] >> 14;
            if (sample > 32767) sample = 32767;
            if (sample < -32768) sample = -32768;
            samples16[i] = (int16_t)sample;
        }

        file.write((uint8_t*)samples16, count * sizeof(int16_t));
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
    String json = "{";

    json += "\"temp\":" + String(lastTemp,1);
    json += ",\"humidity\":" + String(lastHumidity,1);
    json += ",\"co2\":" + String(lastCO2);

    json += ",\"battV\":" + String(lastBatteryVoltage,2);

    json += ",\"battA\":0";

    json += ",\"solW\":0";

    json += ",\"audio\":" + String(readMicrophonePeak(50));

    json += ",\"co2_ok\":true";
    json += ",\"batt_ok\":true";
    json += ",\"solar_ok\":true";
    json += ",\"mic_ok\":true";
    json += ",\"sd_ok\":true";
    json += ",\"cloud_ok\":";

    json += (WiFi.status() == WL_CONNECTED ? "true" : "false");

    json += "}";

    server.send(200, "application/json", json);
}
// ======================================================
// SETUP
// ======================================================
void setup() {

    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("================================");
    Serial.println("WIFI AUDIO LOGGER + THINGSPEAK");
    Serial.println("5 FIELDS: Temp, Humidity, CO2, Battery, Audio");
    Serial.println("Sending sensor data EVERY 15 SECONDS");
    Serial.println("Recording audio: 10 seconds every 30 seconds");
    Serial.println("================================");

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    // WiFi
    connectWiFi();

    server.on("/", handleRoot);
    server.on("/data", handleData);

    server.begin();

    Serial.println("Dashboard Ready");

    // Time
    setupTime();

    // ThingSpeak
    ThingSpeak.begin(client);

    // I2C
    Wire.begin(SDA_PIN, SCL_PIN);

    Serial.println("Scanning I2C...");

for (byte addr = 1; addr < 127; addr++) {

    Wire.beginTransmission(addr);

    if (Wire.endTransmission() == 0) {
        Serial.print("Found: 0x");
        Serial.println(addr, HEX);
    }
}

    Wire.setClock(50000);

    // INA219 Sensors
    if (batterySensor.begin()) {
        batterySensor.setCalibration_32V_2A();
        Serial.println("Battery Sensor: OK");
    } else {
        Serial.println("Battery Sensor: FAILED");
    }
    
    if (solarSensor.begin()) {
        solarSensor.setCalibration_32V_2A();
        Serial.println("Solar Sensor: OK");
    } else {
        Serial.println("Solar Sensor: FAILED");
    }

    // SCD41 CO2 Sensor
    scd4x.begin(Wire, 0x62);

    delay(2000);

    uint64_t serialNumber;
    if (scd4x.getSerialNumber(serialNumber) == 0) {
        scd4x.startPeriodicMeasurement();
        Serial.println("CO2 Sensor: OK");
        delay(500);
    } else {
        Serial.println("CO2 Sensor: FAILED");
    }

    // SD Card
    setupSDCard();

    // Microphone
    setupMicrophone();

    Serial.println();
    Serial.println("SYSTEM READY");
    Serial.println("📊 Sensor data sent to ThingSpeak EVERY 15 seconds");
    Serial.println("🎤 Audio recorded for 10 seconds EVERY 30 seconds");
    Serial.println("📁 Audio files saved to SD card (not sent to cloud)");
    Serial.println("   Fields: Temp | Humidity | CO2 | Battery | Audio Peak");
}

// ======================================================
// LOOP
// ======================================================

void loop() {
    server.handleClient();
    static unsigned long lastThingSpeakTime = 0;
    static unsigned long lastRecordTime = 0;
    static unsigned long lastDisplayTime = 0;
    static unsigned long lastCSVTime = 0;
    static unsigned long lastUploadTime = 0;

    // Keep WiFi alive
    checkWiFiConnection();

    // Read CO2, Temperature, Humidity
    uint16_t co2 = 0;
    float temp = 0;
    float humidity = 0;
    readCO2Smooth(co2, temp, humidity);

    // Read Battery Voltage
    float batteryVoltage = readBatteryVoltage();

    // Read microphone peak (audio data)
    long audioPeak = readMicrophonePeak(100);

    // Display every 10 seconds
    if (millis() - lastDisplayTime >= 10000) {
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
    if (millis() - lastThingSpeakTime >= THINGSPEAK_INTERVAL) {
        sendToThingSpeak(temp, humidity, co2, batteryVoltage, audioPeak);
        lastThingSpeakTime = millis();
    }


    if (millis() - lastCSVTime >= 15000)
    {
        logToCSV(temp, humidity, co2);
        lastCSVTime = millis();

        Serial.println("Data appended to " + hiveNo +".csv");
    }



    if (millis() - lastUploadTime >= 300000) // 5 minutes is 300,000 milliseconds
    {
        Serial.println("===== STARTING FILE UPLOAD =====");
        uploadPendingFiles();
        Serial.println("===== FILE UPLOAD COMPLETE =====");
        lastUploadTime = millis();
    }

    // Record audio EVERY 30 SECONDS (10 second recordings)
    if (millis() - lastRecordTime >= (RECORD_INTERVAL * 1000UL)) {
        recordAudio();
        lastRecordTime = millis();
    }

    delay(10);  // Small delay to keep CPU responsive
}