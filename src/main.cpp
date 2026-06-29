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
// SETTINGS HTML
// ======================================================
const char SETTINGS_HTML[] PROGMEM = R"rawliteral(

<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport"
      content="width=device-width, initial-scale=1">

<title>Settings</title>

<style>

body{
    font-family:Arial;
    background:#f3f4f6;
    padding:20px;
}

.card{
    background:white;
    max-width:500px;
    margin:auto;
    padding:20px;
    border-radius:12px;
    box-shadow:0 2px 6px rgba(0,0,0,.1);
}

input{
    width:100%;
    padding:8px;
    margin-top:5px;
    margin-bottom:15px;
}

button{
    padding:10px 20px;
}

</style>
</head>

<body>

<div class="card">

<h2>⚙ Duty Cycle Settings</h2>

<label>Sensor Interval (sec)</label>
<input id="sensorInterval" type="number">

<label>ThingSpeak Interval (sec)</label>
<input id="tsInterval" type="number">

<label>Audio Record Interval (sec)</label>
<input id="recordInterval" type="number">

<label>Upload Interval (sec)</label>
<input id="uploadInterval" type="number">

<button onclick="saveSettings()">
Save
</button>

<a href="/">
<button>
Dashboard
</button>
</a>

</div>

<script>

async function loadSettings()
{
    const r = await fetch('/settings');
    const s = await r.json();

    sensorInterval.value = s.sensor;
    tsInterval.value = s.ts;
    recordInterval.value = s.record;
    uploadInterval.value = s.upload;
}

loadSettings();

async function saveSettings()
{
    const sensor = sensorInterval.value;
    const ts = tsInterval.value;
    const record = recordInterval.value;
    const upload = uploadInterval.value;

    const response =
        await fetch(
        `/setDuty?sensor=${sensor}&ts=${ts}&record=${record}&upload=${upload}`);

    alert(await response.text());
}

</script>

</body>
</html>

)rawliteral";
// ======================================================
// DASHBOARD HTML
// ======================================================
const char INDEX_HTML[] PROGMEM = R"rawliteral(

<!DOCTYPE html>

<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">

<title>Smart Beehive Monitor</title>

<style>

*{
    margin:0;
    padding:0;
    box-sizing:border-box;
}

body{
    font-family:Arial, sans-serif;
    background:#f3f4f6;
    padding:10px;
}

.container{
    max-width:900px;
    margin:auto;
}

.header{
    background:#f59e0b;
    color:white;
    text-align:center;
    padding:15px;
    border-radius:12px;
    margin-bottom:10px;
}

.header h1{
    font-size:26px;
}

.header p{
    font-size:14px;
    margin-top:5px;
}

.status-bar{
    display:flex;
    flex-wrap:wrap;
    justify-content:center;
    gap:6px;
    margin-bottom:10px;
}

.badge{
    padding:6px 10px;
    border-radius:20px;
    color:white;
    font-size:12px;
    font-weight:bold;
}

.ok{
    background:#10b981;
}

.err{
    background:#ef4444;
}

.grid{
    display:grid;
    grid-template-columns:repeat(3,1fr);
    gap:8px;
}

.card{
    background:white;
    border-radius:12px;
    padding:12px;
    text-align:center;
    box-shadow:0 2px 6px rgba(0,0,0,0.1);
}

.icon{
    font-size:24px;
}

.value{
    font-size:22px;
    font-weight:bold;
    margin-top:5px;
    color:#111827;
}

.label{
    font-size:12px;
    color:#6b7280;
    margin-top:3px;
}

.audio-card{
    grid-column:span 3;
}

.footer{
    background:white;
    margin-top:10px;
    border-radius:12px;
    padding:10px;
    text-align:center;
    font-size:14px;
    box-shadow:0 2px 6px rgba(0,0,0,0.1);
}

@media(max-width:600px)
{
    .grid{
        grid-template-columns:repeat(2,1fr);
    }

    .audio-card{
        grid-column:span 2;
    }

    .header h1{
        font-size:20px;
    }

    .value{
        font-size:18px;
    }
}

</style>

</head>

<body>

<div class="container">

<div class="header">
    <h1>🐝 Smart Beehive Monitor</h1>
    <p>Hive 12 Live Dashboard</p>
</div>

<div id="statusBar" class="status-bar"></div>

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

    <div class="card audio-card">
        <div class="icon">🎤</div>
        <div id="audio" class="value">--</div>
        <div class="label">Audio Peak</div>
    </div>

</div>
<!-- <div class="card" style="margin-top:10px">

<h3>Duty Cycle Settings</h3>

<label>Sensor Interval (sec)</label><br>
<input id="sensorInterval" type="number"><br><br>

<label>ThingSpeak Interval (sec)</label><br>
<input id="tsInterval" type="number"><br><br>

<label>Audio Record Interval (sec)</label><br>
<input id="recordInterval" type="number"><br><br>

<label>Upload Interval (sec)</label><br>
<input id="uploadInterval" type="number"><br><br>

<button onclick="saveSettings()">Save</button>

</div> -->


<div style="text-align:center;margin-top:15px">

<a href="/settingsPage">
<button
style="
padding:10px 20px;
background:#f59e0b;
color:white;
border:none;
border-radius:8px;">
⚙ Settings
</button>
</a>

</div>

<div class="card" style="margin-top:10px">
    <h3>⚠ Error Log</h3>

    <div id="errorLog"
         style="
         text-align:left;
         max-height:200px;
         overflow-y:auto;
         margin-top:10px;
         font-size:13px;">
    Loading...
    </div>
</div>

<div class="footer">
    Last Update:
    <span id="lastUpdated">--</span>
</div>


</div>

<script>
async function updateErrors()
{
    try
    {
        const response =
            await fetch('/errors');

        const errors =
            await response.json();

        const div =
            document.getElementById("errorLog");

        if(errors.length === 0)
        {
            div.innerHTML =
                "<span style='color:green'>No errors</span>";
            return;
        }

div.innerHTML =
    errors.reverse()
          .map(e =>
          {
              let color = "#ef4444";

              if(e.includes("[WARN]"))
                  color = "#f59e0b";

              if(e.includes("[INFO]"))
                  color = "#10b981";

              return `
                  <div style="
                      padding:4px;
                      border-bottom:1px solid #ddd;
                      color:${color};">
                      ${e}
                  </div>`;
          })
          .join("");
    }
    catch(e)
    {
        console.log(e);
    }
}

async function loadSettings()
{
    const r = await fetch('/settings');
    const s = await r.json();

    document.getElementById("sensorInterval").value =
        s.sensor;

    document.getElementById("tsInterval").value =
        s.ts;

    document.getElementById("recordInterval").value =
        s.record;

    document.getElementById("uploadInterval").value =
        s.upload;
}

loadSettings();

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
            ["Mic", data.mic_ok],
            ["SD", data.sd_ok],
            ["Cloud", data.cloud_ok]
        ];

        sensors.forEach(sensor =>
        {
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
setInterval(updateData, 2000);
updateErrors();
setInterval(updateErrors,5000);

async function saveSettings()
{
    const sensor =
        document.getElementById("sensorInterval").value;

    const ts =
        document.getElementById("tsInterval").value;

    const record =
        document.getElementById("recordInterval").value;

    const upload =
        document.getElementById("uploadInterval").value;

    const response =
        await fetch(
            `/setDuty?sensor=${sensor}&ts=${ts}&record=${record}&upload=${upload}`
        );

    alert(await response.text());
}
</script>

</body>
</html>
)rawliteral";

// ======================================================
// ERROR LOGGING
// ======================================================

#define MAX_ERRORS 20

String errorLogs[MAX_ERRORS];
int errorIndex = 0;

void addError(String msg)
{
    struct tm timeinfo;
    char ts[25];

    if (getLocalTime(&timeinfo))
    {
        sprintf(ts,
                "%02d:%02d:%02d",
                timeinfo.tm_hour,
                timeinfo.tm_min,
                timeinfo.tm_sec);
    }
    else
    {
        strcpy(ts, "--:--:--");
    }

    errorLogs[errorIndex] =
        "[" + String(ts) + "] " + msg;

    errorIndex++;

    if (errorIndex >= MAX_ERRORS)
        errorIndex = 0;

    Serial.println(errorLogs[(errorIndex - 1 + MAX_ERRORS) % MAX_ERRORS]);
}

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
    prefs.putULong("sensor", sensorInterval);
    prefs.putULong("ts", thingSpeakInterval);
    prefs.putULong("record", recordInterval);
    prefs.putULong("upload", uploadInterval);

    Serial.println("Duty cycle updated from UI");

    server.send(
        200,
        "text/plain",
        "Settings saved");
}
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

    // if (!file)
    // {
    //     Serial.println("Failed to open file:");
    //     Serial.println(filepath);
    //     return false;
    // }

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
            String apiKey = "7747ea95-9682-42a7-b0aa-44eb374b4300";

            String hiveName = "Hive 01";
            String hiveNameEncoded = hiveName;
            hiveNameEncoded.replace(" ", "%20");

            String path =
                "/conditions/hives/" +
                hiveNameEncoded +
                "/upload";

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

            //--------------------------------------------------
            // Optional: delete uploaded file
            //--------------------------------------------------
            /*
            if (SD.remove("/" + filename))
            {
                Serial.println("Uploaded WAV deleted from SD.");
            }
            else
            {
                Serial.println("Failed to delete WAV.");
            }
            */

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

// ======================================================
// READ SMOOTHED CO2
// ======================================================

bool readCO2Smooth(uint16_t &co2, float &temp, float &humidity)
{
    uint16_t error;
    uint16_t rawCO2 = 0;
    float rawTemp = 0, rawHumidity = 0;

    error = scd4x.readMeasurement(rawCO2, rawTemp, rawHumidity);

    // if (error != 0 || rawCO2 == 0)
    // {
    //     addError("CO2 sensor read failed");
    //     co2 = lastCO2;
    //     temp = lastTemp;
    //     humidity = lastHumidity;
    //     return false;
    // }

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

// =====================================================
// API ENDPOINT FOR ERRORS
//=====================================================
void handleErrors()
{
    String json = "[";

    bool first = true;

    for (int i = 0; i < MAX_ERRORS; i++)
    {
        int idx =
            (errorIndex + i) % MAX_ERRORS;

        if (errorLogs[idx].length() == 0)
            continue;

        if (!first)
            json += ",";

        json += "\"" +
                errorLogs[idx] +
                "\"";

        first = false;
    }

    json += "]";

    server.send(
        200,
        "application/json",
        json);
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
        delay(1);
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

    json += "\"temp\":" + String(lastTemp, 1);
    json += ",\"humidity\":" + String(lastHumidity, 1);
    json += ",\"co2\":" + String(lastCO2);

    json += ",\"battV\":" + String(lastBatteryVoltage, 2);

    // json += ",\"battA\":0";

    // json += ",\"solW\":0";

    float battCurrent =
        batterySensor.getCurrent_mA() / 1000.0;

    // float solarVoltage =
    //     solarSensor.getBusVoltage_V();

    // float solarCurrent =
    //     solarSensor.getCurrent_mA() / 1000.0;

    // float solarPower =
    //     solarVoltage * solarCurrent;

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
    // json += ",\"solar_ok\":true";

    // float solarVoltage = solarSensor.getBusVoltage_V();

    // bool solarConnected =
    //     solarVoltage > 1.0;
    bool solarConnected =
        solarVoltage > 2.0 &&
        abs(solarCurrent) > 0.005;

    json += ",\"solar_ok\":";
    json += solarConnected ? "true" : "false";

    // json += ",\"mic_ok\":true";

    json += ",\"mic_ok\":";
    json += mic_ok ? "true" : "false";

    // json += ",\"sd_ok\":true";

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
    String json = "{";

    json += "\"sensor\":" +
            String(sensorInterval / 1000);

    json += ",\"ts\":" +
            String(thingSpeakInterval / 1000);

    json += ",\"record\":" +
            String(recordInterval);

    json += ",\"upload\":" +
            String(uploadInterval / 1000);

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

// ======================================================
// LOOP
// ======================================================

void loop()
{

    static unsigned long lastSensorTime = millis() - sensorInterval; // Initialize to trigger immediate reading on startup

    static uint16_t co2 = 0;
    static float temp = 0;
    static float humidity = 0;
    static float batteryVoltage = 0;
    static long audioPeak = 0;

    server.handleClient();
    static unsigned long lastThingSpeakTime = 0;
    static unsigned long lastRecordTime = 0;
    static unsigned long lastDisplayTime = 0;
    // static unsigned long lastCSVTime = 0;
    static unsigned long lastUploadTime = 0;

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