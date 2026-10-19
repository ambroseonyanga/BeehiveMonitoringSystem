// ============================================================================
//  Hive monitor firmware  -  ESP32  -  AdEMNEA / AWS data delivery
//
//  Data path (per "API doc and Usage guide"):
//    sensors  -> POST <gateway>/ingest          (202 = queued, not "stored")
//    health   -> POST <gateway>/heartbeat       (202)
//    media    -> POST <ademnea>/api/iot/media/upload-url (200)
//             -> PUT  <signed S3 url>           (binary, 200/204)
//             -> POST <gateway>/media/confirm   (202)
//
//  Reliability design:
//   * Every reading is written to SD first; it is deleted only after the
//     gateway answered 202. Power loss / network loss never loses data.
//   * recorded_at / captured_at is fixed when the reading is taken and reused
//     byte-for-byte on every retry (it is the server's de-duplication key).
//   * Media sits in a persistent queue (/media_q.txt). A file that already
//     reached S3 is never re-uploaded just because /media/confirm failed.
//   * Task watchdog, reset-reason reporting, heap guard, exponential backoff,
//     SD auto-remount, bounded queues. Nothing can block the loop forever.
//
//  2.0.1 changes (from field log review)
//   * Unuploadable media (e.g. ADEMNEA_BASE_URL still localhost) no longer counts
//     as "backlog": the radio was waking every 15 s forever. Only healthy files
//     left over by MAX_MEDIA_PER_WINDOW trigger the fast retry.
//   * Gateway POSTs retry transient errors (HTTP -1..-5, -10, -11) once on a fresh
//     TLS connection (idempotent by recorded_at); longer connect/IO timeouts.
//   * No SD.remove() on missing files (VFS error spam); CSV rename failure is
//     detected; rows that fail to parse are quarantined instead of silently lost.
//   * Mic is not powered up when the recording would be refused (queue full,
//     clock unsynced). Audio peak is measured after a settling period, with DC
//     removed, over ~100 ms (it was 6 ms right after power-up).
//   * SCD41: data-ready is checked first, so "no new sample" is not a bus error.
//   * millis() rollover-safe upload budget; WDT is fed during large S3 PUTs;
//     SD usage scan is rare and WDT-protected.
//   * connection.cpp / logging.cpp: bounded NTP wait (was an endless loop), a failed
//     STA connect now switches the radio off, no 5 s getLocalTime() stalls while the
//     clock is unset, WiFi.persistent(false) (no flash writes on every mode change),
//     JSON-escaped /errors, duplicate Serial lines removed.
//   * Battery: constants match the real 3S2P Li-ion pack; piecewise SoC curve instead of a
//     linear 9-12.6 V map; low-battery resume raised to 11.1 V.
//   * SCD41 init order fixed (stop -> wait -> serial -> start) with self-heal; per-message
//     error rate limiter; solar INA219 absence handled.
//  2.0.2 changes (from a field log: clock unset for minutes after boot)
//   * Readings taken while the clock is unset are buffered in RAM and written with
//     corrected timestamps once it is set (they used to be dropped while the console
//     still said "saved").
//   * Clock fallback: NTP now asks three servers; if NTP still fails the clock is set
//     from the Date header of an HTTPS reply from the AWS gateway.
//   * SD card dropped out: remount retried after 15 s, backing off to 5 min (was a fixed
//     5 min); an offline SD no longer counts as a network failure in the upload back-off.
//   * Debug tracing is compile-time (-DHIVE_DEBUG=1), off by default.
// ============================================================================

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <time.h>
#include <sys/time.h>
#include <vector>
#include <algorithm>
#include <driver/i2s.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_task_wdt.h>
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
#include "hive_util.h"
#include "secrets.h" // WIFI_SSID, WIFI_PASS, THINGSPEAK_WRITE_KEY, AWS_DEVICE_KEY_VALUE, DEVICE_ID_VALUE, ADEMNEA_BASE_URL

// ===== DEBUG BUILD SWITCH ====================================================
// 1 = verbose "[DBG <uptime ms>]" tracing of every step (this file's default).
// 0 = production: every DBG() compiles to nothing. Set with -DHIVE_DEBUG=0 or edit here.
#ifndef HIVE_DEBUG
#define HIVE_DEBUG 1
#endif
#if HIVE_DEBUG
#define DBG(...) do { Serial.printf("[DBG %lu] ", (unsigned long)millis()); Serial.printf(__VA_ARGS__); Serial.println(); } while (0)
#define DBG_T0(v) const uint32_t v = millis() // start of a timed section
#else
#define DBG(...) do { } while (0)
#define DBG_T0(v) do { } while (0)
#endif

#ifndef FW_VERSION
#if HIVE_DEBUG
#define FW_VERSION "2.0.2-dbg"
#else
#define FW_VERSION "2.0.2"
#endif
#endif

// The default 8 KB loop stack is tight with TLS + SD + WebServer on top.
#ifdef SET_LOOP_TASK_STACK_SIZE
SET_LOOP_TASK_STACK_SIZE(16 * 1024);
#endif

Preferences prefs;

// ============================================================================
// CONFIGURATION
// ============================================================================

// ---- credentials (values live in secrets.h, which is not committed) --------
const char *ssid        = WIFI_SSID;
const char *password    = WIFI_PASS;
const char *writeAPIKey = THINGSPEAK_WRITE_KEY;

// ---- endpoints --------------------------------------------------------------
const char *AWS_API_BASE   = "https://nyw6l52jgf.execute-api.eu-north-1.amazonaws.com/prod";
const char *ADEMNEA_BASE   = ADEMNEA_BASE_URL;     // app server (upload-url) - NOT the gateway
const char *AWS_DEVICE_KEY = AWS_API_KEY_VALUE; // the key authenticates; device_id in the body is informational
static const char *DEVICE_ID = DEVICE_ID_VALUE;


// The single on-board temp/humidity sensor is reported as this hive zone. The
// doc's payload has exterior / brood_section / honey_section; we only send
// zones that are really measured (never invented values).
#define HIVE_ZONE "brood_section"

// ---- ThingSpeak -------------------------------------------------------------
unsigned long channelNumber = 3384879;
String hiveNo = "1";

// ---- default schedule (used only until the dashboard saves its own values) --
#define DEFAULT_SENSOR_S   60    // read + log sensors
#define DEFAULT_TS_S       15    // ThingSpeak (normal mode only; free tier limit)
#define DEFAULT_UPLOAD_S   300   // one WiFi session: CSV rows + media + heartbeat
#define DEFAULT_RECORD_S   900   // audio clip. A 10 s clip is ~320 KB: every 30 s would be ~27 GB/month

// ---- limits applied to dashboard input and stored values --------------------
#define SENSOR_MIN_S 10
#define SENSOR_MAX_S 3600
#define TS_MIN_S     15
#define TS_MAX_S     3600
#define RECORD_MIN_S 30
#define RECORD_MAX_S 86400
#define UPLOAD_MIN_S 30
#define UPLOAD_MAX_S 86400

// ---- delivery behaviour -----------------------------------------------------
#define HEARTBEAT_INTERVAL_MS  300000UL // at most one heartbeat per 5 min
#define UPLOAD_BUDGET_MS       60000UL  // max time spent pushing CSV rows per window
#define BACKLOG_RETRY_MS       15000UL  // come back quickly while a backlog is draining
#define BACKOFF_MAX_MS         3600000UL
#define MAX_MEDIA_PER_WINDOW   2
#define MEDIA_MAX_ATTEMPTS     8
#define MEDIA_QUEUE_MAX        40       // stop recording beyond this many waiting files
#define FAILED_MEDIA_MAX       20       // files kept in /failed for manual recovery
#define REJECTED_CSV_MAX_BYTES 200000UL
#define NTP_RESYNC_MS          43200000UL // 12 h (ESP32 RTC drifts)
#define MIN_TLS_HEAP           45000    // largest free block needed for a TLS session
#define LOW_HEAP_RESTARTS      3        // consecutive windows without TLS heap -> reboot

// ---- power ------------------------------------------------------------------
// Pack: Doublepow 18650 "12V" 5000 mAh, 3S2P Li-ion (label: nominal 11.1 V, 55.5 Wh,
// charge limit 12.6 V). Per cell: 4.20 V full, ~3.6 V nearly empty, 3.0 V absolute floor.
// The heartbeat percentage uses a piecewise Li-ion discharge curve (see batteryPercent()),
// NOT a straight line: a linear 9.0-12.6 V mapping reported 50% at 10.8 V, when the
// cells were really at about 10-15%.
#define BATT_CELLS        3
#define BATT_LOW_CUTOFF_V 10.2f // 3.4 V/cell: below this no audio and no media upload (WiFi TX peaks sag the pack). 0 = disabled
// Resume well above the cutoff: while charging the terminal voltage reads higher than
// the resting voltage, and the pack rebounds when the load drops. 11.1 V = 3.7 V/cell.
#define BATT_LOW_RESUME_V 11.1f

// ---- misc -------------------------------------------------------------------
#define WDT_TIMEOUT_S  120   // longest legitimate blocking step is one 60 s HTTP call
#define AWAKE_GRACE_MS 30000UL
#define MIC_SHIFT      14

#define GATEWAY_CONNECT_TIMEOUT_MS 8000
#define GATEWAY_IO_TIMEOUT_MS      15000
#define GATEWAY_RETRIES_PER_WINDOW 3     // transient-error retries shared by one upload window
#define SD_USAGE_REFRESH_MS        21600000UL // usedBytes() scans the FAT: slow on big cards
#define MIC_SETTLE_SAMPLES         2400  // ~150 ms of start-up transient discarded after power-on
#define MIC_PEAK_SAMPLES           1600  // ~100 ms analysis window

// ============================================================================
// AWS SERVER CERTIFICATE - Amazon Root CA 1 (valid until 2038-01-17)
// Trust anchor for the API Gateway endpoint AND the eu-north-1 S3 endpoint
// (S3 uses Amazon Trust Services certificates in this region).
// SHA-256: 8E:CD:E6:88:4F:3D:87:B1:12:5B:A3:1A:C3:FC:B1:3D:70:16:DE:7F:57:CC:90:4F:E1:CB:97:C6:AE:98:19:6E
// ============================================================================
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

// ============================================================================
// AdEMNEA APP-SERVER CERTIFICATE
// Trust anchor for https://smarthive.iot-ra.net (the /api/iot/media/upload-url
// endpoint). This is NOT an AWS service.
//
// Verified with:
//   openssl s_client -connect smarthive.iot-ra.net:443 -showcerts
// The leaf (CN=iot-ra.net) is issued by "Let's Encrypt YR2", which chains up
// through "ISRG Root YR" to "ISRG Root X1". Pinning X1 (the real, long-lived
// Let's Encrypt root) lets mbedTLS verify the whole chain.
//
// Do NOT use setInsecure() here: the upload-url response carries a presigned
// S3 URL, and a MITM could substitute it if the TLS peer is unauthenticated.
// ============================================================================
static const char ADEMNEA_ROOT_CA[] PROGMEM = R"PEM(
-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
)PEM";

// ============================================================================
// PINS / HARDWARE
// ============================================================================
#define SDA_PIN 21
#define SCL_PIN 22
#define SD_MISO 2
#define SD_MOSI 15
#define SD_SCK  14
#define SD_CS   13
#define I2S_WS  25
#define I2S_SCK 26
#define I2S_SD  27
#define MIC_POWER_PIN 32
#define LED_PIN 33
#define SAMPLE_RATE 16000
#define RECORD_TIME_SEC 10

const char *ntpServer = "pool.ntp.org";
const long  gmtOffset_sec      = 3 * 3600; // EAT (UTC+3), no DST in Uganda
const int   daylightOffset_sec = 0;
#define LOCAL_OFFSET_SEC ((long)(gmtOffset_sec + daylightOffset_sec))

Adafruit_INA219 batterySensor(0x40);
Adafruit_INA219 solarSensor(0x41);
SensirionI2cScd4x scd4x;
WiFiClient client;
WebServer server(80);

// ============================================================================
// STATE
// ============================================================================
long          lastAudioPeak      = 0;
unsigned long sensorInterval     = DEFAULT_SENSOR_S * 1000UL;
unsigned long thingSpeakInterval = DEFAULT_TS_S * 1000UL;
unsigned long recordInterval     = DEFAULT_RECORD_S;          // seconds
unsigned long uploadInterval     = DEFAULT_UPLOAD_S * 1000UL; // milliseconds
bool          fullDutyCycle      = false;

unsigned long lastSensorTime     = 0;
unsigned long lastThingSpeakTime = 0;
unsigned long lastRecordTime     = 0;
unsigned long lastUploadTime     = 0;
unsigned long lastDisplayTime    = 0;
unsigned long lastHeartbeatTime  = 0;
unsigned long lastNtpSyncTime    = 0;
unsigned long lastSdRetryTime    = 0;
unsigned long awakeSince         = 0;
unsigned long sensorReadyAt      = 0; // SCD41 needs ~5 s after (re)start
bool          heartbeatEverSent  = false;

static uint16_t co2 = 0;
static float    temp = 0;
static float    humidity = 0;
static float    batteryVoltage = 0;
static long     audioPeak = 0;

bool mic_ok = false;
bool sd_ok  = false;

uint16_t lastCO2 = 400;
float lastTemp = 25.0;
float lastHumidity = 50.0;
float lastBatteryVoltage = 0.0;
static bool haveSensorSample = false;
static bool solarPresent     = false; // INA219 at 0x41 answered at boot

// upload scheduling / health
static uint8_t  uploadFailStreak = 0;
static bool     uploadBacklog    = false;
static uint8_t  lowHeapStreak    = 0;
static bool     battLow          = false;

// heartbeat metrics
static uint32_t rebootCount = 0;
static uint32_t sensorReadOk = 0, sensorReadTotal = 0;
static uint64_t idleUs = 0, hbWindowStartUs = 0;
static const char *bootResetCode = nullptr; // set after a crash/WDT/brownout, reported once

void normalLoop();
void dutyCycleLoop();

// ============================================================================
// ERROR FLAGS -> heartbeat "error_codes"
// ============================================================================
enum ErrFlag : uint32_t
{
    ERR_SCD41       = 1u << 0,
    ERR_BATT        = 1u << 1,
    ERR_SD          = 1u << 2,
    ERR_MIC         = 1u << 3,
    ERR_CLOCK       = 1u << 4,
    ERR_AWS_UPLOAD  = 1u << 5,
    ERR_MEDIA       = 1u << 6,
    ERR_AUTH        = 1u << 7,
    ERR_HEAP_LOW    = 1u << 8,
    ERR_BATT_LOW    = 1u << 9,
    ERR_MEDIA_FULL  = 1u << 10,
    ERR_RESET       = 1u << 11,
};
static uint32_t errFlags = 0;
#if HIVE_DEBUG
static const char *errFlagName(uint32_t f)
{
    switch (f)
    {
    case ERR_SCD41:      return "SCD41_READ";
    case ERR_BATT:       return "BATT_SENSOR";
    case ERR_SD:         return "SD_FAIL";
    case ERR_MIC:        return "MIC_FAIL";
    case ERR_CLOCK:      return "CLOCK_UNSYNCED";
    case ERR_AWS_UPLOAD: return "AWS_UPLOAD";
    case ERR_MEDIA:      return "MEDIA_UPLOAD";
    case ERR_AUTH:       return "AUTH_REJECTED";
    case ERR_HEAP_LOW:   return "HEAP_LOW";
    case ERR_BATT_LOW:   return "BATT_LOW";
    case ERR_MEDIA_FULL: return "MEDIA_QUEUE_FULL";
    case ERR_RESET:      return "RESET_REPORTED";
    default:             return "?";
    }
}
#endif
// Every change of an error flag is traced: this is the quickest way to see WHEN and
// in which order things start failing.
static inline void setErr(uint32_t f, bool on)
{
#if HIVE_DEBUG
    if (((errFlags & f) != 0) != on) DBG("ERRFLAG %s -> %s", errFlagName(f), on ? "SET" : "cleared");
#endif
    if (on) errFlags |= f; else errFlags &= ~f;
}

static void buildErrorCodes(char *out, size_t n)
{
    static const struct { uint32_t f; const char *name; } map[] = {
        {ERR_SCD41, "SCD41_READ"},     {ERR_BATT, "BATT_SENSOR"},   {ERR_SD, "SD_FAIL"},
        {ERR_MIC, "MIC_FAIL"},         {ERR_CLOCK, "CLOCK_UNSYNCED"}, {ERR_AWS_UPLOAD, "AWS_UPLOAD"},
        {ERR_MEDIA, "MEDIA_UPLOAD"},   {ERR_AUTH, "AUTH_REJECTED"}, {ERR_HEAP_LOW, "HEAP_LOW"},
        {ERR_BATT_LOW, "BATT_LOW"},    {ERR_MEDIA_FULL, "MEDIA_QUEUE_FULL"},
    };
    out[0] = 0;
    size_t len = 0;
    auto add = [&](const char *name) {
        int w = snprintf(out + len, n - len, "%s\"%s\"", len ? "," : "", name);
        if (w > 0 && (size_t)w < n - len) len += w;
    };
    for (auto &m : map)
        if (errFlags & m.f) add(m.name);
    if ((errFlags & ERR_RESET) && bootResetCode) add(bootResetCode);
}

// Rate-limited wrapper around the dashboard's addError() so a failure loop
// cannot flood the log buffer.
static void noteError(const String &msg)
{
    // Remember several recent messages: two alternating failures (e.g. "media
    // queue full" and "media host is localhost") used to defeat a last-message
    // check, flood the Serial log and push useful entries out of the 20-line
    // dashboard buffer.
    static const unsigned long QUIET_MS = 600000UL; // same message at most every 10 min
    static struct { uint32_t h; unsigned long at; bool used; } seen[8];
    static uint8_t next = 0;

    uint32_t h = 2166136261u; // FNV-1a
    for (size_t i = 0; i < msg.length(); i++) { h ^= (uint8_t)msg[i]; h *= 16777619u; }

    const unsigned long now = millis();
    for (auto &e : seen)
        if (e.used && e.h == h)
        {
            if (now - e.at < QUIET_MS) { DBG("suppressed repeat: %s", msg.c_str()); return; }
            e.at = now;
            addError(msg); // also prints to Serial
            return;
        }

    seen[next] = {h, now, true};
    next = (next + 1) % 8;
    addError(msg);
}

// ============================================================================
// WATCHDOG / HEAP / POWER GUARDS
// ============================================================================
static void wdtBegin()
{
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    esp_task_wdt_config_t cfg = {};
    cfg.timeout_ms = WDT_TIMEOUT_S * 1000;
    cfg.idle_core_mask = 0;
    cfg.trigger_panic = true;
    esp_task_wdt_deinit();
    esp_task_wdt_init(&cfg);
#else
    esp_task_wdt_init(WDT_TIMEOUT_S, true);
#endif
    esp_task_wdt_add(NULL); // watch the Arduino loop task
}
static inline void wdtFeed() { esp_task_wdt_reset(); }

static bool heapOkForTls() { return ESP.getMaxAllocHeap() >= MIN_TLS_HEAP; }

static void updateBatteryGuard()
{
    // (a plain if: the preprocessor cannot compare floating-point constants)
    if (BATT_LOW_CUTOFF_V > 0.0f && lastBatteryVoltage > 0)
    {
        if (!battLow && lastBatteryVoltage < BATT_LOW_CUTOFF_V)
        {
            battLow = true;
            DBG("BATT GUARD: LOW (%.2f V < %.2f V): audio and media uploads paused", lastBatteryVoltage, BATT_LOW_CUTOFF_V);
        }
        else if (battLow && lastBatteryVoltage > BATT_LOW_RESUME_V)
        {
            battLow = false;
            DBG("BATT GUARD: recovered (%.2f V > %.2f V): audio and media uploads resume", lastBatteryVoltage, BATT_LOW_RESUME_V);
        }
    }
    setErr(ERR_BATT_LOW, battLow);
}

// delay() that is counted as idle time for the cpu_usage estimate.
static void idleDelay(uint32_t ms)
{
    delay(ms);
    idleUs += (uint64_t)ms * 1000ULL;
}

// ============================================================================
// TIME (UTC from time(): always UTC regardless of TZ; no mktime anywhere)
// ============================================================================
static bool clockSynced() { return time(nullptr) > 1735689600; } // after 2025-01-01

static bool getUtcIso(char *out /*>=21*/)
{
    if (!clockSynced()) return false;
    hu::epochToIso((int64_t)time(nullptr), out);
    return true;
}

static bool syncClockFromHttpDate();   // defined after httpDefaults()
static void flushPendingReadings();     // defined next to logToCSV()

static void syncClockIfNeeded()
{
    if (clockSynced() && millis() - lastNtpSyncTime < NTP_RESYNC_MS) return;
    DBG("NTP: sync needed (clockSynced=%d, last sync %lu s ago)", (int)clockSynced(),
        (unsigned long)((millis() - lastNtpSyncTime) / 1000UL));
    setupTime(); // from connection.h; needs WiFi up
    if (!clockSynced())
    {
        DBG("CLOCK: NTP gave no answer, trying the HTTPS Date header");
        if (syncClockFromHttpDate()) Serial.println("Clock set from HTTPS Date header");
    }
    if (clockSynced()) { lastNtpSyncTime = millis(); flushPendingReadings(); }
    setErr(ERR_CLOCK, !clockSynced());
#if HIVE_DEBUG
    {
        char isoNow[24];
        DBG("NTP: result clockSynced=%d utc=%s", (int)clockSynced(), getUtcIso(isoNow) ? isoNow : "(unset)");
    }
#endif
}

// ============================================================================
// DASHBOARD SETTINGS
// ============================================================================
static unsigned long clampUL(long v, long lo, long hi)
{
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return (unsigned long)v;
}

#if HIVE_DEBUG
#define DBG_HTTP() DBG("HTTP: %s from %s", server.uri().c_str(), server.client().remoteIP().toString().c_str())
#else
#define DBG_HTTP() do { } while (0)
#endif

void handleSettingsPage() { DBG_HTTP(); server.send_P(200, "text/html", SETTINGS_HTML); }

void handleSetDuty()
{
    DBG_HTTP();
    DBG("HTTP: setDuty args sensor=%s ts=%s record=%s upload=%s fullDuty=%s", server.arg("sensor").c_str(),
        server.arg("ts").c_str(), server.arg("record").c_str(), server.arg("upload").c_str(),
        server.arg("fullDuty").c_str());
    if (server.hasArg("sensor"))
        sensorInterval = clampUL(server.arg("sensor").toInt(), SENSOR_MIN_S, SENSOR_MAX_S) * 1000UL;
    if (server.hasArg("ts"))
        thingSpeakInterval = clampUL(server.arg("ts").toInt(), TS_MIN_S, TS_MAX_S) * 1000UL;
    if (server.hasArg("record"))
        recordInterval = clampUL(server.arg("record").toInt(), RECORD_MIN_S, RECORD_MAX_S);
    if (server.hasArg("upload"))
        uploadInterval = clampUL(server.arg("upload").toInt(), UPLOAD_MIN_S, UPLOAD_MAX_S) * 1000UL;
    if (server.hasArg("fullDuty"))
        fullDutyCycle = server.arg("fullDuty").toInt() == 1;

    prefs.putULong("sensor", sensorInterval);
    prefs.putULong("ts", thingSpeakInterval);
    prefs.putULong("record", recordInterval);
    prefs.putULong("upload", uploadInterval);
    prefs.putBool("fullDuty", fullDutyCycle);

    Serial.println("Duty cycle updated from UI");
    DBG("SETTINGS: sensor=%lus ts=%lus record=%lus upload=%lus fullDuty=%d", sensorInterval / 1000UL,
        thingSpeakInterval / 1000UL, recordInterval, uploadInterval / 1000UL, (int)fullDutyCycle);
    server.send(200, "text/plain", "Settings saved");
}

void handleSettings()
{
    DBG_HTTP();
    String json;
    json.reserve(256);
    json = "{";
    json += "\"sensor\":" + String(sensorInterval / 1000);
    json += ",\"ts\":"     + String(thingSpeakInterval / 1000);
    json += ",\"record\":" + String(recordInterval);
    json += ",\"upload\":" + String(uploadInterval / 1000);
    json += ",\"fullDuty\":";
    json += fullDutyCycle ? "true" : "false";
    json += "}";
    server.send(200, "application/json", json);
}

// ============================================================================
// SD CARD
// ============================================================================
static String csvPath() { return "/" + hiveNo + ".csv"; }
static const char *CSV_TMP      = "/upload.tmp";
static const char *REJECTED_CSV = "/rejected.csv";
static const char *MEDIA_Q      = "/media_q.txt";
static const char *MEDIA_Q_TMP  = "/media_q.tmp";
static const char *FAILED_DIR   = "/failed";

// SD.remove() on a missing file logs a VFS error: only remove what exists.
static void sdRemoveIfExists(const char *p)
{
    if (SD.exists(p)) SD.remove(p);
}

#if HIVE_DEBUG
static size_t fileSizeOf(const String &p)
{
    if (!SD.exists(p)) return 0;
    File f = SD.open(p, FILE_READ);
    if (!f) return 0;
    size_t n = f.size();
    f.close();
    return n;
}
#endif

// Any SD error that suggests the card dropped out: mark it so the main loop
// re-initialises it (connectors on a moving/vibrating hive do glitch).
static void sdFault(const char *why)
{
    DBG("SD FAULT: %s (heap=%u)", why, (unsigned)ESP.getFreeHeap());
    setErr(ERR_SD, true);
    sd_ok = false;
    noteError(String("SD fault: ") + why);
}

// Crash safety for the "write tmp, delete original, rename tmp" pattern:
//  original missing + tmp present -> the rename never ran: finish it
//  both present                   -> tmp may be partial: keep the original
//                                    (worst case a harmless duplicate resend)
static void recoverTmp(const char *path, const char *tmp)
{
    bool hasMain = SD.exists(path), hasTmp = SD.exists(tmp);
    DBG("RECOVER %s: main=%d tmp=%d", path, (int)hasMain, (int)hasTmp);
    if (!hasMain && hasTmp) { DBG("RECOVER: finishing interrupted rename %s -> %s", tmp, path); SD.rename(tmp, path); }
    else if (hasMain && hasTmp) { DBG("RECOVER: dropping stale %s", tmp); SD.remove(tmp); }
}

static void ensureCSVExists()
{
    if (!sd_ok) return;
    String p = csvPath();
    if (SD.exists(p)) return;
    File f = SD.open(p, FILE_WRITE);
    if (!f) { sdFault("cannot create csv"); return; }
    f.close();
}

// Reads one line into buf (NUL-terminated, CR stripped). Returns the length,
// 0 for blank/over-long lines (remainder discarded), -1 at end of file.
static int readLine(File &f, char *buf, size_t n)
{
    size_t len = 0;
    bool any = false, over = false;
    while (f.available())
    {
        int c = f.read();
        if (c < 0) break;
        any = true;
        if (c == '\n') break;
        if (c == '\r') continue;
        if (len + 1 < n) buf[len++] = (char)c;
        else over = true;
    }
    buf[len] = 0;
    if (!any) return -1;
    return over ? 0 : (int)len;
}

static void quarantineRow(const char *line)
{
    File f = SD.open(REJECTED_CSV, FILE_APPEND);
    if (!f) return;
    if (f.size() < REJECTED_CSV_MAX_BYTES) f.println(line);
    f.close();
}

static bool rewriteLines(const char *path, const char *tmp, const std::vector<String> &lines)
{
    sdRemoveIfExists(tmp);
    File f = SD.open(tmp, FILE_WRITE);
    if (!f) return false;
    for (auto &l : lines) f.println(l);
    f.close();
    sdRemoveIfExists(path);
    return SD.rename(tmp, path);
}

static int countLines(const char *path)
{
    if (!sd_ok || !SD.exists(path)) return 0;
    File f = SD.open(path, FILE_READ);
    if (!f) return 0;
    char buf[200];
    int n = 0, r;
    while ((r = readLine(f, buf, sizeof(buf))) >= 0)
        if (r > 0) n++;
    f.close();
    return n;
}

// Returns true only if the row really reached the card.
// Readings taken while the clock is not set yet. With no NTP (router without internet,
// UDP 123 filtered) every reading used to be refused, and the console still claimed
// "saved". They are now kept in RAM with their uptime and written to the CSV with a
// corrected timestamp (epoch = now - age) as soon as the clock is set.
struct PendingReading { uint32_t atMs; float t; float h; uint16_t co2; };
#define PENDING_MAX 360 // 1 hour at a 10 s sensor interval (~6 KB of RAM)
static PendingReading pending[PENDING_MAX];
static uint16_t       pendingCount   = 0;
static uint32_t       pendingDropped = 0;

enum LogResult { LOG_FAILED = 0, LOG_SAVED = 1, LOG_BUFFERED = 2 };

static int formatCsvRow(char *row, size_t cap, int64_t epoch, float t, float h, uint16_t co2)
{
    // Local time derived from the UTC epoch with a fixed offset: the legacy CSV format is
    // kept so existing SD-card tooling still parses it. The last two columns are legacy
    // placeholders (no weight/gas sensor on this board) and are NOT uploaded.
    char stamp[24];
    hu::epochToLocalStamp(epoch, LOCAL_OFFSET_SEC, stamp);
    return snprintf(row, cap, "\"%s\",\"%.2f*2*2\",\"%.2f*2*2\",\"%u\",%.2f,%d\n", stamp, t, h, (unsigned)co2, 2.0f, 2);
}

static void pendingAdd(float t, float h, uint16_t co2)
{
    if (pendingCount >= PENDING_MAX) // full: forget the oldest
    {
        memmove(&pending[0], &pending[1], (PENDING_MAX - 1) * sizeof(PendingReading));
        pendingCount = PENDING_MAX - 1;
        pendingDropped++;
    }
    pending[pendingCount].atMs = millis();
    pending[pendingCount].t    = t;
    pending[pendingCount].h    = h;
    pending[pendingCount].co2  = co2;
    pendingCount++;
}

static void flushPendingReadings()
{
    if (pendingCount == 0 || !sd_ok || !clockSynced()) return;

    ensureCSVExists();
    File file = SD.open(csvPath(), FILE_APPEND);
    if (!file) { sdFault("cannot open csv (backfill)"); return; }

    const int64_t  now   = (int64_t)time(nullptr);
    const uint32_t nowMs = millis();
    uint16_t written = 0;
    for (uint16_t i = 0; i < pendingCount; i++)
    {
        const int64_t epoch = now - (int64_t)((uint32_t)(nowMs - pending[i].atMs) / 1000UL);
        char row[128];
        const int n = formatCsvRow(row, sizeof(row), epoch, pending[i].t, pending[i].h, pending[i].co2);
        if (n <= 0 || file.write((const uint8_t *)row, (size_t)n) != (size_t)n) break;
        written++;
        if ((i & 31) == 31) wdtFeed();
    }
    file.flush();
    file.close();

    DBG("BACKFILL: wrote %u of %u buffered readings (dropped while buffering: %lu)", (unsigned)written,
        (unsigned)pendingCount, (unsigned long)pendingDropped);
    if (written > 0) Serial.printf("Clock is set: %u buffered readings written with corrected timestamps\n", (unsigned)written);

    if (written == pendingCount) pendingCount = 0;
    else
    {
        memmove(&pending[0], &pending[written], (size_t)(pendingCount - written) * sizeof(PendingReading));
        pendingCount = (uint16_t)(pendingCount - written);
        sdFault("short write to csv (backfill)");
    }
}

LogResult logToCSV(float tempReading, float humidityReading, uint16_t co2Reading)
{
    if (!sd_ok) { DBG("CSV: skipped, SD offline"); return LOG_FAILED; }

    if (!clockSynced())
    {
        setErr(ERR_CLOCK, true);
        pendingAdd(tempReading, humidityReading, co2Reading);
        noteError("Clock not set: readings are buffered in RAM until time sync");
        DBG("CSV: clock not set, reading buffered in RAM (%u waiting)", (unsigned)pendingCount);
        return LOG_BUFFERED;
    }

    flushPendingReadings(); // oldest first, so the CSV stays in chronological order

    ensureCSVExists();
    File file = SD.open(csvPath(), FILE_APPEND);
    if (!file) { sdFault("cannot open csv"); return LOG_FAILED; }
    const size_t sizeBefore = file.size();

    char row[128];
    int n = formatCsvRow(row, sizeof(row), (int64_t)time(nullptr), tempReading, humidityReading, co2Reading);
    size_t w = (n > 0) ? file.write((const uint8_t *)row, (size_t)n) : 0;
    file.flush();
    const size_t sizeAfter = file.size();
    file.close();
    DBG("CSV: wrote %u/%d bytes, before=%u after=%u", (unsigned)w, n, (unsigned)sizeBefore, (unsigned)sizeAfter);
    (void)sizeBefore; (void)sizeAfter;

    if (n <= 0 || w != (size_t)n) { sdFault("short write to csv"); return LOG_FAILED; }
    setErr(ERR_SD, false);
    return LOG_SAVED;
}

static float sdUsagePercent()
{
    static float cached = 0;
    static unsigned long last = 0;
    static bool have = false;
    if (!sd_ok) return cached;
    if (!have || millis() - last > SD_USAGE_REFRESH_MS) // usedBytes() can take seconds: cache it
    {
        wdtFeed();
        uint64_t total = SD.totalBytes();
        cached = total ? 100.0f * (float)SD.usedBytes() / (float)total : 0;
        wdtFeed();
        last = millis();
        have = true;
        DBG("SD: usage refreshed: %.2f%% of %llu MB", cached, (unsigned long long)(total / (1024ULL * 1024ULL)));
    }
    return cached;
}

void setupSDCard()
{
    Serial.println("\n[SD CARD]");
    SD.end();
    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

    if (!SD.begin(SD_CS, SPI, 4000000))
    {
        noteError("SD card initialization failed");
        setErr(ERR_SD, true);
        sd_ok = false;
        return;
    }

    sd_ok = true;
    setErr(ERR_SD, false);
    Serial.printf("READY, %llu MB\n", (unsigned long long)(SD.cardSize() / (1024 * 1024)));
    DBG("SD: cardType=%d (1=MMC 2=SD 3=SDHC), cardSize=%llu MB", (int)SD.cardType(),
        (unsigned long long)(SD.cardSize() / (1024 * 1024)));

    // Finish anything a power loss interrupted.
    recoverTmp(csvPath().c_str(), CSV_TMP);
    recoverTmp(MEDIA_Q, MEDIA_Q_TMP);
    ensureCSVExists();
    DBG("SD: csv=%u bytes, media queue=%d items", (unsigned)fileSizeOf(csvPath()), countLines(MEDIA_Q));
}

// ============================================================================
// CSV -> /ingest
// ============================================================================
enum PostResult { POST_OK, POST_REJECTED, POST_RETRY, POST_AUTH };

static void httpDefaults(HTTPClient &http, uint16_t ioTimeoutMs = GATEWAY_IO_TIMEOUT_MS)
{
    http.setConnectTimeout(GATEWAY_CONNECT_TIMEOUT_MS);
    http.setTimeout(ioTimeoutMs);
}

// ---- clock fallback ---------------------------------------------------------
// Parses an HTTP-date such as "Tue, 02 Oct 2026 14:03:37 GMT".
static bool parseHttpDate(const char *d, int64_t &epoch)
{
    char mon[4] = {0};
    int day = 0, year = 0, hh = 0, mm = 0, ss = 0;
    if (sscanf(d, "%*3s, %d %3s %d %d:%d:%d", &day, mon, &year, &hh, &mm, &ss) != 6) return false;
    static const char MONTHS[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *p = strstr(MONTHS, mon);
    if (!p || (p - MONTHS) % 3 != 0) return false;
    return hu::civilToEpoch(year, (int)((p - MONTHS) / 3) + 1, day, hh, mm, ss, epoch);
}

// When NTP does not answer (UDP 123 filtered on the network, flaky DNS), every HTTPS
// reply from the AWS gateway still carries a Date header. TLS does not need a valid
// clock on the ESP32, so the clock can be set from it (accurate to ~1-2 s, plenty for
// 10-second readings). Any HTTP status will do: only the header matters.
static bool syncClockFromHttpDate()
{
    if (!heapOkForTls()) { DBG("CLOCK: HTTPS Date fallback skipped, heap too low"); return false; }

    WiFiClientSecure tls;
    tls.setCACert(AWS_ROOT_CA);
    HTTPClient http;
    httpDefaults(http, 8000);
    static const char *keys[] = {"Date"};
    http.collectHeaders(keys, 1);

    bool ok = false;
    if (http.begin(tls, String(AWS_API_BASE) + "/"))
    {
        const int code = http.GET();
        const String date = http.header("Date");
        int64_t epoch = 0;
        DBG("CLOCK: HTTPS Date fallback: GET -> %d (%s), Date='%s'", code, HTTPClient::errorToString(code).c_str(),
            date.c_str());
        if (code > 0 && date.length() > 0 && parseHttpDate(date.c_str(), epoch) && epoch > 1735689600LL)
        {
            struct timeval tv;
            tv.tv_sec = (time_t)epoch;
            tv.tv_usec = 0;
            settimeofday(&tv, nullptr);
            ok = clockSynced();
        }
        http.end();
    }
    return ok;
}

// Transient-error retries left in the current upload window (reset per window).
static uint8_t gatewayRetries = 0;

// HTTPC_ERROR_*: -1 refused, -2/-3 send failed, -4 not connected, -5 connection
// lost, -10 stream write, -11 read timeout. Others (-6..-9) will not heal by retrying.
static bool transientHttpError(int code)
{
    return code < 0 && code != -6 && code != -7 && code != -8 && code != -9;
}

// One POST to the gateway.
//   200/202      accepted into the queue                       -> POST_OK
//   400/422      the payload itself is bad: retrying is futile -> POST_REJECTED
//   401/403      key problem: keep the data, try again later   -> POST_AUTH
//   <=0/429/5xx  network or throttling: keep data, retry       -> POST_RETRY
static PostResult gatewayPost(HTTPClient &http, WiFiClientSecure &tls, const char *path,
                              const char *body, int &codeOut)
{
    wdtFeed();
    const String url = String(AWS_API_BASE) + path;
    const size_t len = strlen(body);

    // Every gateway message is idempotent (recorded_at / captured_at key), so a
    // transient network error is retried on a fresh connection before giving up.
    for (;;)
    {
        DBG_T0(t0);
        http.begin(tls, url);
        http.addHeader("Content-Type", "application/json");
        http.addHeader("X-Api-Key", AWS_DEVICE_KEY);
        codeOut = http.POST((uint8_t *)body, len);
#if HIVE_DEBUG
        String respBody;
        if (codeOut > 0 && codeOut != 200 && codeOut != 202) respBody = http.getString();
#endif
        http.end();
        DBG("POST %s -> %d %s in %lu ms (heap=%u maxBlock=%u)", path, codeOut,
            codeOut < 0 ? HTTPClient::errorToString(codeOut).c_str() : "", (unsigned long)(millis() - t0),
            (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
#if HIVE_DEBUG
        if (codeOut != 200 && codeOut != 202)
        {
            DBG("  request body : %s", body);
            if (respBody.length()) DBG("  response body: %.300s", respBody.c_str());
        }
#endif
        wdtFeed();

        if (!transientHttpError(codeOut) || gatewayRetries == 0) break;
        gatewayRetries--;
        DBG("POST: transient error %d, retrying on a fresh connection (%u retries left)", codeOut,
            (unsigned)gatewayRetries);
        tls.stop(); // drop the possibly half-dead connection
        server.handleClient();
        delay(500);
    }

    server.handleClient(); // keep the local dashboard alive during long uploads
    wdtFeed();

    if (codeOut == 202 || codeOut == 200) return POST_OK;
    if (codeOut == 400 || codeOut == 422) return POST_REJECTED;
    if (codeOut == 401 || codeOut == 403) return POST_AUTH;
    return POST_RETRY;
}

static void buildEnvelope(char *out, size_t n, const char *sensorType, const char *atIso,
                          const char *readingJson)
{
    snprintf(out, n,
             "{\"device_id\":\"%s\",\"sensor_type\":\"%s\",\"recorded_at\":\"%s\",\"reading\":%s}",
             DEVICE_ID, sensorType, atIso, readingJson);
}

// Sends the three messages (temperature, humidity, co2) for one CSV row.
// Messages already accepted are simply re-sent on a retry: same recorded_at,
// so the server discards the duplicates (documented idempotency).
static PostResult sendRow(HTTPClient &http, WiFiClientSecure &tls, const char *atIso,
                          float t, float h, int c)
{
    char reading[56], body[256];
    bool anyRejected = false;

    for (int i = 0; i < 3; i++)
    {
        const char *type;
        if (i == 0)      { type = "temperature"; snprintf(reading, sizeof(reading), "{\"" HIVE_ZONE "\":%.2f}", t); }
        else if (i == 1) { type = "humidity";    snprintf(reading, sizeof(reading), "{\"" HIVE_ZONE "\":%.2f}", h); }
        else             { type = "co2";         snprintf(reading, sizeof(reading), "{\"co2_level\":%.1f}", (float)c); }

        buildEnvelope(body, sizeof(body), type, atIso, reading);

        int code;
        PostResult r = gatewayPost(http, tls, "/ingest", body, code);
        if (r == POST_OK) continue;
        if (r == POST_REJECTED)
        {
            Serial.printf("AWS rejected %s (HTTP %d): %s\n", type, code, body);
            anyRejected = true;
            continue;
        }
        noteError("AWS ingest failed: HTTP " + String(code));
        return r; // network/auth problem: stop, keep the row
    }
    return anyRejected ? POST_REJECTED : POST_OK;
}

struct CsvStats
{
    int sent = 0, rejected = 0, skipped = 0;
    bool failed = false; // network/auth problem
    bool more = false;   // backlog left because of the time budget
};

static CsvStats uploadCsvToAws(unsigned long budgetEndMs)
{
    CsvStats st;
    String path = csvPath();
    // No SD card = nothing to upload. That is not a network failure and must not stretch
    // the upload back-off (the heartbeat still reports SD_FAIL to the server).
    if (!sd_ok) { DBG("CSV: SD offline, nothing to upload (not counted as a network failure)"); return st; }
    if (WiFi.status() != WL_CONNECTED) { st.failed = true; return st; }
    if (!SD.exists(path)) return st;

    File csv = SD.open(path, FILE_READ);
    if (!csv) { sdFault("cannot read csv"); st.failed = true; return st; }

    WiFiClientSecure tls;
    tls.setCACert(AWS_ROOT_CA);
    HTTPClient http;
    http.setReuse(true);
    httpDefaults(http);

    char raw[160], work[160], at[24];
    char stopLine[160] = "";
    bool haveStop = false;

    Serial.println("===== SENDING CSV TO AWS =====");
    DBG("CSV: file=%s size=%u bytes, time budget=%lu ms", path.c_str(), (unsigned)csv.size(),
        (unsigned long)(budgetEndMs - millis()));

    int r;
    while ((r = readLine(csv, raw, sizeof(raw))) >= 0)
    {
        if (r == 0) { DBG("CSV: blank/over-long line ignored"); continue; }

        strcpy(work, raw); // parseCsvRow edits in place; keep raw for retry/quarantine
        float t, h;
        int c;
        if (!hu::parseCsvRow(work, LOCAL_OFFSET_SEC, at, t, h, c)) { DBG("CSV: unparseable row quarantined: %s", raw); quarantineRow(raw); st.skipped++; continue; }

        if ((long)(millis() - budgetEndMs) >= 0) { DBG("CSV: time budget used up, rest stays queued"); st.more = true; strcpy(stopLine, raw); haveStop = true; break; }

        PostResult pr = sendRow(http, tls, at, t, h, c);
        DBG("CSV: row %s T=%.2f H=%.2f CO2=%d -> result %d (0=OK 1=REJECTED 2=RETRY 3=AUTH)", at, t, h, c, (int)pr);
        if (pr == POST_OK) st.sent++;
        else if (pr == POST_REJECTED) { quarantineRow(raw); st.rejected++; }
        else
        {
            st.failed = true;
            strcpy(stopLine, raw);
            haveStop = true;
            setErr(ERR_AUTH, pr == POST_AUTH);
            break;
        }
    }

    if (!st.failed) setErr(ERR_AUTH, false);
    setErr(ERR_AWS_UPLOAD, st.failed);

    if (haveStop)
    {
        DBG("CSV: keeping unsent tail starting at: %s", stopLine);
        // Keep the unsent tail (stopLine + everything after it): write it to a
        // temp file, then swap. recoverTmp() repairs a power cut in between.
        sdRemoveIfExists(CSV_TMP);
        File tmp = SD.open(CSV_TMP, FILE_WRITE);
        if (tmp)
        {
            tmp.println(stopLine);
            uint8_t buf[512];
            while (csv.available())
            {
                size_t n = csv.read(buf, sizeof(buf));
                if (n == 0) break;
                tmp.write(buf, n);
            }
            tmp.close();
            csv.close();
            SD.remove(path);
            // If this rename fails the tail survives in CSV_TMP; recoverTmp()
            // restores it on the next SD (re)initialisation.
            if (!SD.rename(CSV_TMP, path)) sdFault("csv rename failed");
        }
        else
        {
            csv.close(); // can't trim: keep the whole file, duplicates are harmless
            sdFault("cannot write csv tmp");
        }
    }
    else
    {
        csv.close();
        DBG("CSV: all rows consumed, file reset");
        SD.remove(path);
        ensureCSVExists();
    }

    Serial.printf("AWS: %d sent, %d quarantined, %d skipped%s\n", st.sent, st.rejected, st.skipped,
                  st.failed ? " (will retry)" : st.more ? " (more pending)" : "");
    Serial.println("=============================");
    return st;
}

// ============================================================================
// /heartbeat   (flat payload per the API doc - NOT the /ingest envelope)
// ============================================================================
// State of charge from the pack voltage: piecewise-linear 18650 (NMC) curve per cell.
// Voltage-based SoC is an ESTIMATE (+/- ~10 %, load and temperature dependent, flat
// between 3.7 and 3.9 V/cell); it is good enough to see "low / medium / full" remotely.
static float batteryPercent(float packV)
{
    static const struct { float v, pct; } curve[] = {
        {3.00f, 0},  {3.30f, 3},  {3.50f, 6},  {3.60f, 12}, {3.70f, 25},
        {3.80f, 42}, {3.90f, 60}, {4.00f, 76}, {4.10f, 90}, {4.20f, 100},
    };
    const int n = sizeof(curve) / sizeof(curve[0]);
    const float v = packV / BATT_CELLS;

    if (v <= curve[0].v)     return 0;
    if (v >= curve[n - 1].v) return 100;
    for (int i = 1; i < n; i++)
        if (v <= curve[i].v)
            return curve[i - 1].pct + (v - curve[i - 1].v) * (curve[i].pct - curve[i - 1].pct) /
                                          (curve[i].v - curve[i - 1].v);
    return 0;
}

static bool sendHeartbeat(float battV, int wifiRssi)
{
    if (WiFi.status() != WL_CONNECTED) return false;

    uint64_t nowUs  = esp_timer_get_time();
    uint64_t wallUs = nowUs - hbWindowStartUs;
    float cpu = 0;
    if (wallUs > 0)
    {
        cpu = 100.0f * (1.0f - (float)idleUs / (float)wallUs);
        if (cpu < 0) cpu = 0;
        if (cpu > 100) cpu = 100;
    }
    float readRate = sensorReadTotal ? (float)sensorReadOk / (float)sensorReadTotal : 1.0f;

    char codes[256];
    buildErrorCodes(codes, sizeof(codes));

    char body[640];
    snprintf(body, sizeof(body),
             "{\"battery_level\":%.1f,\"signal_strength\":%d,\"uptime\":%lu,"
             "\"firmware_version\":\"%s\",\"cpu_usage\":%.1f,\"storage_usage\":%.1f,"
             "\"reboot_count\":%lu,\"sensor_read_success_rate\":%.2f,\"error_codes\":[%s]}",
             batteryPercent(battV), wifiRssi, (unsigned long)(nowUs / 1000000ULL),
             FW_VERSION, cpu, sdUsagePercent(),
             (unsigned long)rebootCount, readRate, codes);

    WiFiClientSecure tls;
    tls.setCACert(AWS_ROOT_CA);
    HTTPClient http;
    httpDefaults(http);

    DBG("HB body: %s", body);
    int code;
    PostResult r = gatewayPost(http, tls, "/heartbeat", body, code);
    if (r == POST_OK)
    {
        Serial.println("Heartbeat: 202 queued");
        sensorReadOk = sensorReadTotal = 0; // start a fresh reporting window
        idleUs = 0;
        hbWindowStartUs = nowUs;
        setErr(ERR_AUTH, false);
        setErr(ERR_RESET, false); // the reset reason has now been reported
        return true;
    }
    if (r == POST_AUTH) setErr(ERR_AUTH, true);
    noteError("Heartbeat failed: HTTP " + String(code));
    return false;
}

// ============================================================================
// MEDIA: persistent upload queue + the doc's 3-step flow
// ============================================================================
// /media_q.txt, one item per line:   path|captured_at|s3_key|attempts
//  - captured_at is fixed at recording time (duplicate-detection key).
//  - s3_key is filled once the PUT to S3 succeeded, so a failed /media/confirm
//    is retried WITHOUT uploading the file again.

struct MediaItem
{
    String path, capturedAt, s3Key;
    uint8_t attempts = 0;
};

enum MediaResult { MEDIA_DONE, MEDIA_GIVE_UP, MEDIA_RETRY_LATER, MEDIA_RETRY_ITEM };

static bool ademneaIsLocalhost()
{
    return strstr(ADEMNEA_BASE, "127.0.0.1") || strstr(ADEMNEA_BASE, "localhost");
}

static bool mediaInfoForPath(const String &p, const char *&type, const char *&mime)
{
    String l = p;
    l.toLowerCase();
    if (l.endsWith(".wav"))                        { type = "audio"; mime = "audio/wav";  return true; }
    if (l.endsWith(".jpg") || l.endsWith(".jpeg")) { type = "photo"; mime = "image/jpeg"; return true; }
    if (l.endsWith(".mp4"))                        { type = "video"; mime = "video/mp4";  return true; }
    return false;
}

static bool parseMediaLine(const String &line, MediaItem &it)
{
    int a = line.indexOf('|');
    int b = line.indexOf('|', a + 1);
    int c = line.indexOf('|', b + 1);
    if (a < 0 || b < 0 || c < 0) return false;
    it.path       = line.substring(0, a);
    it.capturedAt = line.substring(a + 1, b);
    it.s3Key      = line.substring(b + 1, c);
    it.attempts   = (uint8_t)line.substring(c + 1).toInt();
    return it.path.length() && it.capturedAt.length();
}

static String formatMediaLine(const MediaItem &it)
{
    return it.path + "|" + it.capturedAt + "|" + it.s3Key + "|" + String(it.attempts);
}

static bool enqueueMedia(const String &path, const String &capturedAt)
{
    File f = SD.open(MEDIA_Q, FILE_APPEND);
    if (!f) { sdFault("cannot open media queue"); return false; }
    f.println(path + "|" + capturedAt + "||0");
    f.close();
    DBG("MEDIA: enqueued %s captured=%s", path.c_str(), capturedAt.c_str());
    return true;
}

// Unrecoverable files are moved aside rather than deleted: a misconfigured
// server must not destroy recordings. /failed is capped at boot.
static void moveToFailed(const String &path)
{
    if (!SD.exists(path)) return;
    DBG("MEDIA: moving %s to %s", path.c_str(), FAILED_DIR);
    SD.mkdir(FAILED_DIR);
    int slash = path.lastIndexOf('/');
    String name = slash >= 0 ? path.substring(slash + 1) : path;
    if (!SD.rename(path, String(FAILED_DIR) + "/" + name)) SD.remove(path);
}

static void beginHttp(HTTPClient &http, WiFiClient &plain, WiFiClientSecure &tls, const String &url)
{
    if (url.startsWith("https://")) http.begin(tls, url);
    else                            http.begin(plain, url);
}

// File stream that feeds the watchdog on every chunk, so a slow link cannot
// trip the 120 s task watchdog during a large PUT.
class WdtFileStream : public Stream
{
  public:
    explicit WdtFileStream(File &file) : f(file) {}
    using Stream::readBytes;
    int    available() override { return f.available(); }
    int    read() override { return f.read(); }
    int    peek() override { return f.peek(); }
    size_t write(uint8_t) override { return 0; }
    size_t readBytes(char *b, size_t n) override { wdtFeed(); return f.readBytes(b, n); }
    void   flush() override {}

  private:
    File &f;
};

static MediaResult uploadMediaItem(MediaItem &it)
{
    const char *mtype, *mime;
    if (!mediaInfoForPath(it.path, mtype, mime)) { noteError("Media: unknown type " + it.path); return MEDIA_GIVE_UP; }
    if (!SD.exists(it.path))                     { noteError("Media: file missing " + it.path); return MEDIA_GIVE_UP; }

    File probe = SD.open(it.path, FILE_READ);
    uint32_t fileSize = probe ? (uint32_t)probe.size() : 0;
    if (probe) probe.close();
    DBG("MEDIA: upload %s type=%s mime=%s size=%u s3Key=%s", it.path.c_str(), mtype, mime, (unsigned)fileSize,
        it.s3Key.length() ? it.s3Key.c_str() : "(none yet)");
    if (fileSize < 1024) { noteError("Media: file too small " + it.path); return MEDIA_GIVE_UP; }

    // A failure that is not about the network counts against the item.
    auto itemFailed = [&]() -> MediaResult {
        it.attempts++;
        return it.attempts >= MEDIA_MAX_ATTEMPTS ? MEDIA_GIVE_UP : MEDIA_RETRY_ITEM;
    };

    // ---- Steps 1 + 2 (skipped when the file is already in S3) ----
    if (it.s3Key.length() == 0)
    {
        // Step 1: ask the AdEMNEA app server for a one-time upload URL.
        // Presigned URLs that carry an STS session token run 1.5-2 KB; jsonGetString()
        // fails (never truncates) if the buffer is too small.
        static char uploadUrl[2304]; // static: keep the big buffer off the task stack
        char s3Key[200];
        int code;
        String resp;
        {
            WiFiClient plain;
            WiFiClientSecure tls;
            tls.setCACert(ADEMNEA_ROOT_CA);
            HTTPClient http;
            httpDefaults(http);
            wdtFeed();
            DBG("MEDIA step1: POST %s/api/iot/media/upload-url (heap=%u maxBlock=%u)", ADEMNEA_BASE,
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
            DBG_T0(s1T0);
            beginHttp(http, plain, tls, String(ADEMNEA_BASE) + "/api/iot/media/upload-url");
            http.addHeader("Content-Type", "application/json");
            http.addHeader("X-Api-Key", ADEMNEA_API_KEY);

            char body[96];
            snprintf(body, sizeof(body), "{\"media_type\":\"%s\",\"content_type\":\"%s\"}", mtype, mime);
            code = http.POST((uint8_t *)body, strlen(body));
            if (code == 200 || code == 201)
            {
                int len = http.getSize();
                if (len > 6144) code = -100; // refuse absurd responses (JSON escaping inflates a 2 KB URL)
                else resp = http.getString();
            }
#if HIVE_DEBUG
            else if (code > 0) resp = http.getString(); // error body: shows WHY the server refused
#endif
            http.end();
            DBG("MEDIA step1: HTTP %d %s in %lu ms, %u bytes: %.200s", code,
                code < 0 ? HTTPClient::errorToString(code).c_str() : "", (unsigned long)(millis() - s1T0),
                (unsigned)resp.length(), resp.c_str());
            wdtFeed();
        }

        if (code == 401 || code == 403) { setErr(ERR_AUTH, true); noteError("Media step1: key rejected"); return MEDIA_RETRY_LATER; }
        if (code == 400 || code == 422) { noteError("Media step1 rejected: HTTP " + String(code)); return itemFailed(); }
        if (code != 200 && code != 201) { noteError("Media step1 failed: HTTP " + String(code)); return MEDIA_RETRY_LATER; }

        if (!hu::jsonGetString(resp.c_str(), "upload_url", uploadUrl, sizeof(uploadUrl)) ||
            !hu::jsonGetString(resp.c_str(), "s3_object_key", s3Key, sizeof(s3Key)))
        {
            noteError("Media step1: bad response");
            return itemFailed();
        }
        Serial.printf("Media step1 OK. Key: %s\n", s3Key);
        DBG("MEDIA step1: upload_url length=%u, key=%s", (unsigned)strlen(uploadUrl), s3Key);

        // // Step 2: PUT the raw bytes. No X-Api-Key: the signed URL is the auth.
        // {
        //     WiFiClient plain;
        //     WiFiClientSecure tls;
        //     tls.setCACert(AWS_ROOT_CA);
        //     HTTPClient http;
        //     httpDefaults(http, 60000);
        //     beginHttp(http, plain, tls, String(uploadUrl));
                // Step 2: PUT the raw bytes. No X-Api-Key: the signed URL is the auth.
        {
            WiFiClient plain;
            WiFiClientSecure tls;
            // Real S3 lives on *.amazonaws.com and chains to Amazon Root CA 1.
            // The dev/mock S3 lives on the AdEMNEA host (smarthive.iot-ra.net).
            // Pick the right anchor from the URL itself.
            if (String(uploadUrl).indexOf("amazonaws.com") >= 0)
                tls.setCACert(AWS_ROOT_CA);
            else
                tls.setCACert(ADEMNEA_ROOT_CA);
            HTTPClient http;
            httpDefaults(http, 60000);
            beginHttp(http, plain, tls, String(uploadUrl));
            http.addHeader("Content-Type", mime); // same value the URL was requested for

            File f = SD.open(it.path, FILE_READ);
            if (!f) { sdFault("cannot reopen media"); return MEDIA_RETRY_LATER; }
            wdtFeed();
            WdtFileStream ws(f);
            DBG("MEDIA step2: PUT %u bytes (heap=%u maxBlock=%u)", (unsigned)f.size(),
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
            DBG_T0(putT0);
            code = http.sendRequest("PUT", &ws, f.size());
#if HIVE_DEBUG
            String putBody;
            if (code >= 300) putBody = http.getString(); // S3 explains in XML (e.g. SignatureDoesNotMatch)
            DBG("MEDIA step2: HTTP %d %s in %lu ms%s%.300s", code,
                code < 0 ? HTTPClient::errorToString(code).c_str() : "", (unsigned long)(millis() - putT0),
                putBody.length() ? " body: " : "", putBody.c_str());
#endif
            f.close();
            http.end();
            wdtFeed();
        }

        // The doc says 204 (mock). Real S3 answers a presigned PUT with 200.
        if (code != 204 && code != 200)
        {
            noteError("Media step2 (PUT) failed: HTTP " + String(code));
            setErr(ERR_MEDIA, true);
            // 4xx = expired/invalid signature: request a fresh URL next time.
            return (code >= 400 && code < 500) ? itemFailed() : MEDIA_RETRY_LATER;
        }
        it.s3Key = s3Key; // persisted to the queue even if step 3 fails
        Serial.println("Media step2 OK");
    }

    // ---- Step 3: confirm (gateway -> SQS -> DB). Only after a good PUT. ----
    char atIso[24];
    strlcpy(atIso, it.capturedAt.c_str(), sizeof(atIso));
    char body[384];
    snprintf(body, sizeof(body),
             "{\"device_id\":\"%s\",\"media_type\":\"%s\",\"s3_object_key\":\"%s\","
             "\"captured_at\":\"%s\",\"file_size_bytes\":%lu}",
             DEVICE_ID, mtype, it.s3Key.c_str(), atIso, (unsigned long)fileSize);

    WiFiClientSecure tls;
    tls.setCACert(AWS_ROOT_CA);
    HTTPClient http;
    httpDefaults(http);
    DBG("MEDIA step3: POST /media/confirm body: %s", body);
    int code;
    PostResult r = gatewayPost(http, tls, "/media/confirm", body, code);

    if (r == POST_OK)   { Serial.println("Media step3 OK: reference queued"); return MEDIA_DONE; }
    if (r == POST_AUTH) { setErr(ERR_AUTH, true); return MEDIA_RETRY_LATER; }
    if (r == POST_REJECTED)
    {
        noteError("Media confirm rejected: HTTP " + String(code));
        it.s3Key = ""; // upload again with a fresh key next time
        return itemFailed();
    }
    noteError("Media step3 failed: HTTP " + String(code));
    return MEDIA_RETRY_LATER;
}

// retryLater: a network/auth problem stopped the pass (back off).
// pending:    files are still waiting (informational).
// more:       healthy files were left over only because of MAX_MEDIA_PER_WINDOW
//             (come back soon). Files that cannot upload right now are NOT
//             "more": they must not keep the radio awake every 15 s.
struct MediaStatus { bool retryLater = false; bool pending = false; bool more = false; };

static MediaStatus processMediaQueue(int maxItems)
{
    MediaStatus ms;
    if (!sd_ok || WiFi.status() != WL_CONNECTED || !SD.exists(MEDIA_Q)) return ms;

    if (ademneaIsLocalhost())
    {
        noteError("Media: ADEMNEA_BASE_URL is localhost; set the real host in secrets.h. Files stay queued.");
        setErr(ERR_MEDIA, true);
        ms.pending = true;
        return ms;
    }

    DBG("MEDIA: processing queue (%d items, up to %d this window)", countLines(MEDIA_Q), maxItems);
    File q = SD.open(MEDIA_Q, FILE_READ);
    if (!q) { sdFault("cannot read media queue"); return ms; }

    std::vector<String> keep;
    int attempted = 0;
    bool stop = false, anyProblem = false;
    char buf[400];
    int r;

    while ((r = readLine(q, buf, sizeof(buf))) >= 0)
    {
        if (r == 0) continue;
        String line(buf);

        if (stop || attempted >= maxItems) { if (!stop) ms.more = true; keep.push_back(line); continue; }

        MediaItem it;
        if (!parseMediaLine(line, it)) { DBG("MEDIA: corrupt queue line dropped: %s", line.c_str()); continue; }

        attempted++;
        DBG("MEDIA: item #%d %s captured=%s attempts=%u", attempted, it.path.c_str(), it.capturedAt.c_str(),
            (unsigned)it.attempts);
        MediaResult res = uploadMediaItem(it);
        DBG("MEDIA: item result=%d (0=DONE 1=GIVE_UP 2=RETRY_LATER 3=RETRY_ITEM)", (int)res);

        if (res == MEDIA_DONE)
        {
            sdRemoveIfExists(it.path.c_str()); // server confirmed: reclaim the space
        }
        else if (res == MEDIA_GIVE_UP)
        {
            anyProblem = true;
            noteError("Media given up, moved to /failed: " + it.path);
            moveToFailed(it.path);
        }
        else
        {
            anyProblem = true;
            keep.push_back(formatMediaLine(it));
            if (res == MEDIA_RETRY_LATER) { stop = true; ms.retryLater = true; }
        }
        server.handleClient();
        wdtFeed();
    }
    q.close();

    if (!rewriteLines(MEDIA_Q, MEDIA_Q_TMP, keep)) sdFault("cannot rewrite media queue");
    DBG("MEDIA: queue rewritten, %u kept, attempted=%d, stopped early=%d", (unsigned)keep.size(), attempted, (int)stop);
    if (!anyProblem) setErr(ERR_MEDIA, false);
    ms.pending = !keep.empty();
    setErr(ERR_MEDIA_FULL, (int)keep.size() >= MEDIA_QUEUE_MAX);
    return ms;
}

// Boot housekeeping: cap /failed, and adopt WAV files that exist on the card
// but never reached the queue (power cut between recording and enqueue).
static void mediaHousekeeping()
{
    if (!sd_ok) return;

    // 1) cap /failed (oldest first; names embed the capture time)
    if (SD.exists(FAILED_DIR))
    {
        std::vector<String> names;
        File d = SD.open(FAILED_DIR);
        if (d)
        {
            for (File f = d.openNextFile(); f; f = d.openNextFile())
            {
                String n = f.name();
                int s = n.lastIndexOf('/');
                if (s >= 0) n = n.substring(s + 1);
                if (!f.isDirectory()) names.push_back(n);
                f.close();
            }
            d.close();
        }
        if ((int)names.size() > FAILED_MEDIA_MAX)
        {
            std::sort(names.begin(), names.end());
            for (size_t i = 0; i + FAILED_MEDIA_MAX < names.size(); i++)
                SD.remove(String(FAILED_DIR) + "/" + names[i]);
        }
    }

    // 2) adopt orphaned recordings
    std::vector<String> queued;
    if (SD.exists(MEDIA_Q))
    {
        File q = SD.open(MEDIA_Q, FILE_READ);
        char buf[400];
        int r;
        while (q && (r = readLine(q, buf, sizeof(buf))) >= 0)
        {
            MediaItem it;
            if (r > 0 && parseMediaLine(String(buf), it)) queued.push_back(it.path);
        }
        if (q) q.close();
    }

    File root = SD.open("/");
    if (!root) return;
    std::vector<String> orphans;
    String prefix = hiveNo + "_";
    for (File f = root.openNextFile(); f; f = root.openNextFile())
    {
        String n = f.name();
        int s = n.lastIndexOf('/');
        if (s >= 0) n = n.substring(s + 1);
        bool isWav = n.endsWith(".wav") && n.startsWith(prefix);
        size_t sz = f.size();
        bool dir = f.isDirectory();
        f.close();
        if (dir || !isWav) continue;
        String path = "/" + n;
        if (std::find(queued.begin(), queued.end(), path) != queued.end()) continue;
        if (sz < 1024) { SD.remove(path); continue; } // stub from an interrupted start
        orphans.push_back(path);
    }
    root.close();
    DBG("HOUSEKEEPING: %u files already queued, %u orphan recordings found", (unsigned)queued.size(),
        (unsigned)orphans.size());

    for (auto &path : orphans)
    {
        // "/1_20260926T091500Z.wav" -> "2026-09-26T09:15:00Z"
        int u = path.indexOf('_');
        String st = path.substring(u + 1, path.length() - 4);
        if (st.length() != 16 || st[8] != 'T' || st[15] != 'Z') continue;
        String iso = st.substring(0, 4) + "-" + st.substring(4, 6) + "-" + st.substring(6, 8) + "T" +
                     st.substring(9, 11) + ":" + st.substring(11, 13) + ":" + st.substring(13, 15) + "Z";
        enqueueMedia(path, iso);
        Serial.println("Adopted orphan recording: " + path);
    }
}

// ============================================================================
// SENSORS
// ============================================================================
// The SCD41 keeps measuring across an ESP32 reset whenever its supply stays up
// (USB re-flash, MCU-only reset). In periodic mode it rejects every command except
// read/stop, so getSerialNumber() failed and the sensor was reported FAILED even
// though it was delivering data. Sensirion's init order: stop, wait 500 ms, then talk.
static bool initScd41()
{
    scd4x.stopPeriodicMeasurement(); // error ignored: fine if it was idle
    delay(500);

    uint64_t sn = 0;
    int16_t err = scd4x.getSerialNumber(sn);
    if (err != 0) { delay(100); err = scd4x.getSerialNumber(sn); } // one retry
    DBG("SCD41 init: serial read err=%d serial=0x%08lX%08lX", (int)err, (unsigned long)(sn >> 32),
        (unsigned long)(sn & 0xFFFFFFFFULL));
    if (err == 0) err = scd4x.startPeriodicMeasurement();
    DBG("SCD41 init: %s (err=%d)", err == 0 ? "periodic measurement started" : "FAILED", (int)err);
    if (err != 0) { setErr(ERR_SCD41, true); return false; }

    setErr(ERR_SCD41, false);
    sensorReadyAt = millis() + 5500; // first sample takes ~5 s
    return true;
}

// Returns true ONLY for a fresh, valid measurement. "Not ready" is not an
// error and must not be logged again with a new timestamp.
bool readCO2Smooth(uint16_t &co2, float &temp, float &humidity)
{
    uint16_t rawCO2 = 0;
    float rawTemp = 0, rawHumidity = 0;

    // Ask first: "no new sample yet" is normal and must not be reported as a bus error.
    bool ready = false;
    int16_t error = scd4x.getDataReadyStatus(ready);
    DBG("SCD41: dataReady err=%d ready=%d", (int)error, (int)ready);
    if (error == 0 && !ready)
    {
        co2 = lastCO2; temp = lastTemp; humidity = lastHumidity;
        return false;
    }
    if (error == 0) error = scd4x.readMeasurement(rawCO2, rawTemp, rawHumidity);
    if (error != 0)
    {
        DBG("SCD41: read error code %d", (int)error);
        setErr(ERR_SCD41, true);
        noteError("CO2 communication error");
        co2 = lastCO2; temp = lastTemp; humidity = lastHumidity;
        return false;
    }
    if (rawCO2 == 0)
    {
        DBG("SCD41: rawCO2==0 (sensor reports an invalid sample)");
        co2 = lastCO2; temp = lastTemp; humidity = lastHumidity;
        return false;
    }

    setErr(ERR_SCD41, false);
    DBG("SCD41: raw co2=%u temp=%.2f hum=%.2f", (unsigned)rawCO2, rawTemp, rawHumidity);

    if (haveSensorSample) // exponential smoothing
    {
        co2      = (uint16_t)(0.7 * rawCO2 + 0.3 * lastCO2);
        temp     = 0.7 * rawTemp + 0.3 * lastTemp;
        humidity = 0.7 * rawHumidity + 0.3 * lastHumidity;
    }
    else // first sample: don't blend with the 400 / 25 / 50 placeholders
    {
        co2 = rawCO2; temp = rawTemp; humidity = rawHumidity;
        haveSensorSample = true;
    }

    lastCO2 = co2; lastTemp = temp; lastHumidity = humidity;
    return true;
}

float readBatteryVoltage()
{
    float voltage = batterySensor.getBusVoltage_V();
    DBG("BATT: raw bus voltage=%.3f V (previous smoothed=%.3f V)", voltage, lastBatteryVoltage);
    if (isnan(voltage) || voltage <= 0)
    {
        setErr(ERR_BATT, true);
        return lastBatteryVoltage;
    }
    setErr(ERR_BATT, false);

    if (lastBatteryVoltage > 0) voltage = 0.7 * voltage + 0.3 * lastBatteryVoltage;
    lastBatteryVoltage = voltage;
    return voltage;
}

// Throws away `frames` samples: the MEMS mic needs ~100 ms after power-up and
// its first samples carry a DC step / start-up noise.
static void micDiscard(int frames)
{
    if (!mic_ok) return;
    int32_t tmp[64];
    size_t  got = 0;
    const unsigned long deadline = millis() + 1000UL;
    while (frames > 0 && millis() < deadline)
    {
        if (i2s_read(I2S_NUM_0, tmp, sizeof(tmp), &got, pdMS_TO_TICKS(100)) != ESP_OK || got == 0) break;
        frames -= (int)(got / sizeof(int32_t));
    }
    DBG("MIC: settle finished, %d frames not discarded (0 = full settle)", frames > 0 ? frames : 0);
}

// Peak amplitude (16-bit scale) over ~100 ms AFTER the settling period, with
// the DC offset removed.
long readMicrophonePeak(int numSamples = MIC_PEAK_SAMPLES)
{
    if (!mic_ok) return 0;
    if (numSamples > MIC_PEAK_SAMPLES) numSamples = MIC_PEAK_SAMPLES;

    static int16_t buf[MIC_PEAK_SAMPLES]; // static: keep it off the task stack
    int32_t samples[64];
    size_t  bytesRead = 0;
    int     n = 0;

    micDiscard(MIC_SETTLE_SAMPLES);

    const unsigned long deadline = millis() + 1000UL;
    while (n < numSamples && millis() < deadline)
    {
        esp_err_t err = i2s_read(I2S_NUM_0, samples, sizeof(samples), &bytesRead, pdMS_TO_TICKS(100));
        if (err != ESP_OK || bytesRead == 0) break;
        int cnt = (int)(bytesRead / sizeof(int32_t));
        for (int i = 0; i < cnt && n < numSamples; i++)
        {
            int32_t v = samples[i] >> MIC_SHIFT;
            if (v > 32767)  v = 32767;
            if (v < -32768) v = -32768;
            buf[n++] = (int16_t)v;
        }
    }
    if (n == 0) { DBG("MIC: no samples read (mic dead or I2S stalled)"); return 0; }

    int64_t sum = 0;
    for (int i = 0; i < n; i++) sum += buf[i];
    const long mean = (long)(sum / n);

    long peak = 0;
    for (int i = 0; i < n; i++)
    {
        long level = labs((long)buf[i] - mean);
        if (level > peak) peak = level;
    }
    DBG("MIC: peak=%ld, DC mean=%ld, samples=%d", peak, mean, n);
    return peak;
}

void sendToThingSpeak(float temp, float humidity, uint16_t co2, float batteryVoltage, long audioPeak)
{
    if (WiFi.status() != WL_CONNECTED) { Serial.println("WiFi not connected, cannot send to ThingSpeak"); return; }
    if (!haveSensorSample) return; // never publish the placeholder zeros

    Serial.println("\n[THINGSPEAK]");
    ThingSpeak.setField(1, temp);
    ThingSpeak.setField(2, humidity);
    ThingSpeak.setField(3, co2);
    ThingSpeak.setField(4, batteryVoltage);
    ThingSpeak.setField(5, audioPeak);

    wdtFeed();
    DBG_T0(tsT0);
    int httpCode = ThingSpeak.writeFields(channelNumber, writeAPIKey);
    DBG("ThingSpeak -> %d in %lu ms", httpCode, (unsigned long)(millis() - tsT0));
    wdtFeed();
    if (httpCode == 200)
        Serial.printf("ThingSpeak OK: T=%.1fC H=%.1f%% CO2=%d Batt=%.2fV Audio=%ld\n",
                      temp, humidity, co2, batteryVoltage, audioPeak);
    else
        noteError("ThingSpeak HTTP error " + String(httpCode));
}

// ============================================================================
// MICROPHONE
// ============================================================================
void setupMicrophone()
{
    if (mic_ok) { i2s_driver_uninstall(I2S_NUM_0); mic_ok = false; }

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

    esp_err_t ie = i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    if (ie != ESP_OK)
    { DBG("MIC: i2s_driver_install -> %s", esp_err_to_name(ie)); noteError("Microphone driver install failed"); setErr(ERR_MIC, true); return; }

    ie = i2s_set_pin(I2S_NUM_0, &pin_config);
    if (ie != ESP_OK)
    { DBG("MIC: i2s_set_pin -> %s", esp_err_to_name(ie)); i2s_driver_uninstall(I2S_NUM_0); noteError("Microphone pin configuration failed"); setErr(ERR_MIC, true); return; }

    i2s_zero_dma_buffer(I2S_NUM_0);
    mic_ok = true;
    setErr(ERR_MIC, false);
}

void powerOnMicrophone()
{
    DBG_T0(micT0);
    digitalWrite(MIC_POWER_PIN, HIGH);
    delay(100);
    setupMicrophone();
    DBG("MIC: power ON, ready=%d (%lu ms)", (int)mic_ok, (unsigned long)(millis() - micT0));
}

void powerOffMicrophone()
{
    DBG("MIC: power OFF");
    if (mic_ok) i2s_driver_uninstall(I2S_NUM_0);
    mic_ok = false;
    digitalWrite(MIC_POWER_PIN, LOW);
}

// ============================================================================
// WAV RECORDING
// ============================================================================
static void writeWavHeader(File &file, int sampleRate, int totalSamples)
{
    int   dataSize = totalSamples * 2;
    int   fileSize = dataSize + 36;
    int   subChunk1Size = 16;
    short audioFormat = 1, numChannels = 1, blockAlign = 2, bitsPerSample = 16;
    int   byteRate = sampleRate * 2;

    file.write((const uint8_t *)"RIFF", 4);
    file.write((const uint8_t *)&fileSize, 4);
    file.write((const uint8_t *)"WAVE", 4);
    file.write((const uint8_t *)"fmt ", 4);
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

// captured_at = the START time in UTC ISO, fixed here once and reused on every
// retry. The file name embeds it, which lets housekeeping recover orphans.
static bool recordAudio(String &outPath, String &outCapturedAt)
{
    outPath = "";
    outCapturedAt = "";

    if (!mic_ok || !sd_ok) { DBG("REC: refused, mic_ok=%d sd_ok=%d", (int)mic_ok, (int)sd_ok); return false; }

    char iso[24];
    if (!getUtcIso(iso)) { setErr(ERR_CLOCK, true); Serial.println("Recording skipped: clock not synced"); return false; }
    setErr(ERR_CLOCK, false);
    outCapturedAt = iso;

    if (countLines(MEDIA_Q) >= MEDIA_QUEUE_MAX)
    {
        setErr(ERR_MEDIA_FULL, true);
        noteError("Media queue full, recording skipped");
        return false;
    }

    char stamp[17]; // 20260926T091500Z
    snprintf(stamp, sizeof(stamp), "%.4s%.2s%.2sT%.2s%.2s%.2sZ", iso, iso + 5, iso + 8, iso + 11, iso + 14, iso + 17);
    outPath = "/" + hiveNo + "_" + String(stamp) + ".wav";

    Serial.println("RECORDING AUDIO -> " + outPath);
    DBG_T0(recT0);

    File file = SD.open(outPath.c_str(), FILE_WRITE);
    if (!file) { sdFault("cannot create wav"); outPath = ""; return false; }

    const int totalSamples = SAMPLE_RATE * RECORD_TIME_SEC;
    writeWavHeader(file, SAMPLE_RATE, totalSamples);

    micDiscard(MIC_SETTLE_SAMPLES); // skip the power-on transient

    int32_t samples32[256];
    int16_t samples16[256];
    int     samplesWritten = 0, readFailures = 0;
    bool    writeFailed = false;
    const unsigned long deadline = millis() + RECORD_TIME_SEC * 1000UL + 3000UL;

    while (samplesWritten < totalSamples && millis() < deadline)
    {
        server.handleClient();
        wdtFeed();

        size_t bytesRead = 0;
        // Bounded wait (a dead mic must not hang the firmware).
        esp_err_t err = i2s_read(I2S_NUM_0, samples32, sizeof(samples32), &bytesRead, pdMS_TO_TICKS(500));
        if (err != ESP_OK || bytesRead == 0)
        {
            if (++readFailures > 10) break;
            continue;
        }

        int count = bytesRead / sizeof(int32_t);
        if (samplesWritten + count > totalSamples) count = totalSamples - samplesWritten;

        for (int i = 0; i < count; i++)
        {
            int32_t s = samples32[i] >> MIC_SHIFT;
            if (s > 32767)  s = 32767;
            if (s < -32768) s = -32768;
            samples16[i] = (int16_t)s;
        }
        size_t want = count * sizeof(int16_t);
        if (file.write((uint8_t *)samples16, want) != want) { writeFailed = true; break; }
        samplesWritten += count;
    }

    if (writeFailed || samplesWritten == 0)
    {
        DBG("REC: failed, writeFailed=%d samples=%d readFailures=%d", (int)writeFailed, samplesWritten, readFailures);
        file.close();
        SD.remove(outPath);
        if (writeFailed) sdFault("write error while recording");
        else setErr(ERR_MIC, true);
        outPath = "";
        return false;
    }

    // Stopped early: fix the header so the WAV matches its real length.
    if (samplesWritten != totalSamples)
    {
        file.seek(0);
        writeWavHeader(file, SAMPLE_RATE, samplesWritten);
    }
    file.close();
    File check = SD.open(outPath.c_str(), FILE_READ);
    uint32_t sz = check ? check.size() : 0;
    if (check) check.close();
    DBG("REC: %d/%d samples, readFailures=%d, %lu ms, %u bytes on card", samplesWritten, totalSamples,
        readFailures, (unsigned long)(millis() - recT0), (unsigned)sz);

    Serial.printf("AUDIO SAVED: %s (%u bytes)\n", outPath.c_str(), (unsigned)sz);
    return true;
}

static void recordAndEnqueue()
{
    if (battLow) { Serial.println("Recording skipped: battery low"); return; }
    if (!sd_ok) { DBG("RECORD: skipped, SD offline"); return; }

    // Decide BEFORE powering the microphone: no point waking it for nothing.
    if (!clockSynced()) { setErr(ERR_CLOCK, true); noteError("Recording skipped: clock not synced"); return; }
    if (countLines(MEDIA_Q) >= MEDIA_QUEUE_MAX)
    {
        setErr(ERR_MEDIA_FULL, true);
        noteError("Media queue full, recording skipped");
        return;
    }

    // DBG("RECORD: start (mediaQueue=%d/%d, heap=%u)", countLines(MEDIA_Q), MEDIA_QUEUE_MAX, (unsigned)ESP.getFreeHeap());
    // powerOnMicrophone();
    // delay(150);

    // String wavPath, capturedAt;
    // bool recOk = recordAudio(wavPath, capturedAt);

    // // The first SD write of a recording occasionally fails: the mic's power-on
    // // transient pulls the 3.3V rail down right as the card starts its first flash
    // // program, and the card rejects the write. The failure always happens at
    // // exactly 3,840 samples (15 sectors).
    // //
    // // The SD driver has by now torn itself down (sdFault() ran, sd_ok is false).
    // // Bring it back up explicitly, pause briefly, and try once more. The retry
    // // has a clean, freshly-initialised SD to write to and usually succeeds.
    // if (!recOk && countLines(MEDIA_Q) < MEDIA_QUEUE_MAX)
    // {
    //     DBG("RECORD: first attempt failed, reinitialising SD and retrying");
    //     delay(1000);
    //     setupSDCard();
    //     lastSdRetryTime = millis();   // avoid a second remount from the main loop
    //     if (sd_ok)
    //     {
    //         recOk = recordAudio(wavPath, capturedAt);
    //         if (recOk) DBG("RECORD: retry succeeded");
    //         else       DBG("RECORD: retry also failed, giving up this cycle");
    //     }
    //     else
    //     {
    //         DBG("RECORD: SD still offline after remount, skipping retry");
    //     }
    // }

    // if (recOk) enqueueMedia(wavPath, capturedAt); // uploaded in the next upload window
    // DBG("RECORD: done ok=%d path=%s heap=%u", (int)recOk, wavPath.c_str(), (unsigned)ESP.getFreeHeap());

    // powerOffMicrophone();

    DBG("RECORD: start (mediaQueue=%d/%d, heap=%u)", countLines(MEDIA_Q), MEDIA_QUEUE_MAX, (unsigned)ESP.getFreeHeap());
    powerOnMicrophone();
    delay(150);

    String wavPath, capturedAt;
    const bool recOk = recordAudio(wavPath, capturedAt);
    if (recOk) enqueueMedia(wavPath, capturedAt); // uploaded in the next upload window
    DBG("RECORD: done ok=%d path=%s heap=%u", (int)recOk, wavPath.c_str(), (unsigned)ESP.getFreeHeap());

    powerOffMicrophone();
}

// ============================================================================
// DASHBOARD HANDLERS
// ============================================================================
void handleRoot() { DBG_HTTP(); server.send_P(200, "text/html", INDEX_HTML); }

void handleData()
{
#if HIVE_DEBUG
    {   // the dashboard polls /data every few seconds: log it at most every 10 s
        static unsigned long lastHttpDbg = 0;
        if (millis() - lastHttpDbg > 10000UL) { lastHttpDbg = millis(); DBG_HTTP(); }
    }
#endif
    String json;
    json.reserve(512);
    json = "{";
    json += "\"temp\":"      + String(lastTemp, 1);
    json += ",\"humidity\":" + String(lastHumidity, 1);
    json += ",\"co2\":"      + String(lastCO2);
    json += ",\"battV\":"    + String(lastBatteryVoltage, 2);

    float battCurrent  = batterySensor.getCurrent_mA() / 1000.0;
    float solarVoltage = solarPresent ? solarSensor.getBusVoltage_V() : 0;
    float solarCurrent = solarPresent ? solarSensor.getCurrent_mA() / 1000.0 : 0;
    if (isnan(solarVoltage)) solarVoltage = 0;
    if (isnan(solarCurrent)) solarCurrent = 0;

    json += ",\"battA\":" + String(battCurrent, 3);
    json += ",\"solW\":"  + String(solarVoltage * solarCurrent, 2);
    json += ",\"audio\":" + String(lastAudioPeak);

    // Real sensor status (these used to be hard-coded to true).
    json += ",\"co2_ok\":";  json += (errFlags & ERR_SCD41) ? "false" : "true";
    json += ",\"batt_ok\":"; json += (errFlags & ERR_BATT)  ? "false" : "true";

    bool solarConnected = solarVoltage > 2.0 && fabs(solarCurrent) > 0.005;
    json += ",\"solar_ok\":"; json += solarConnected ? "true" : "false";
    json += ",\"mic_ok\":";   json += mic_ok ? "true" : "false";
    json += ",\"sd_ok\":";    json += sd_ok ? "true" : "false";
    json += ",\"cloud_ok\":"; json += (WiFi.status() == WL_CONNECTED ? "true" : "false");
    json += "}";

    server.send(200, "application/json", json);
}

// ============================================================================
// SCHEDULING
// ============================================================================
static void sampleSensors()
{
    sensorReadTotal++;
    bool fresh = readCO2Smooth(co2, temp, humidity);
    if (fresh) sensorReadOk++;

    // Self-heal: five failed samples in a row while the error flag is set -> re-run
    // the init sequence (a glitched bus or a sensor that lost power recovers).
    static uint8_t scdFailStreak = 0;
    if (fresh) scdFailStreak = 0;
    else if ((errFlags & ERR_SCD41) && ++scdFailStreak >= 5)
    {
        scdFailStreak = 0;
        Serial.println(initScd41() ? "CO2 Sensor: recovered" : "CO2 Sensor: still failing");
    }
    batteryVoltage = readBatteryVoltage();
    updateBatteryGuard();
    DBG("SAMPLE: fresh=%d T=%.2f H=%.2f CO2=%u batt=%.2fV (%.0f%%) battLow=%d", (int)fresh, temp, humidity,
        (unsigned)co2, batteryVoltage, batteryPercent(batteryVoltage), (int)battLow);

    if (!battLow)
    {
        powerOnMicrophone();
        audioPeak = readMicrophonePeak();
        lastAudioPeak = audioPeak;
        powerOffMicrophone();
        DBG("SAMPLE: audioPeak=%ld", audioPeak);
    }
    else DBG("SAMPLE: microphone skipped (battery low)");

    if (fresh)
    {
        switch (logToCSV(temp, humidity, co2))
        {
        case LOG_SAVED:    Serial.println("Sensor data saved to CSV"); break;
        case LOG_BUFFERED: Serial.printf("Sensor data buffered in RAM, waiting for the clock (%u waiting)\n", (unsigned)pendingCount); break;
        default:           Serial.println("Sensor data NOT saved (see errors)"); break;
        }
    }
    else DBG("SAMPLE: no new SCD41 sample, nothing logged");
}

// Milliseconds until the next upload window, honouring backoff and backlog.
static unsigned long uploadDelayMs()
{
    unsigned long d = uploadInterval;
    if (uploadFailStreak > 0)
    {
        uint8_t s = uploadFailStreak > 5 ? 5 : uploadFailStreak;
        d = uploadInterval << s;
        unsigned long cap = uploadInterval > BACKOFF_MAX_MS ? uploadInterval : BACKOFF_MAX_MS;
        if (d > cap) d = cap;
    }
    else if (uploadBacklog && BACKLOG_RETRY_MS < d)
        d = BACKLOG_RETRY_MS; // keep draining quickly after a successful window
    return d;
}

static uint32_t msUntil(unsigned long last, unsigned long intervalMs)
{
    unsigned long el = millis() - last;
    return el >= intervalMs ? 0 : (uint32_t)(intervalMs - el);
}

// One WiFi session: clock check, CSV rows, queued media, heartbeat.
static void runUploadWindow(bool alsoThingSpeak)
{
    wdtFeed();
    Serial.println("===== UPLOAD WINDOW =====");
    bool ok = true;
    uploadBacklog = false;
    DBG_T0(winT0);
    gatewayRetries = GATEWAY_RETRIES_PER_WINDOW;
    DBG("WIN: start heap=%u maxBlock=%u failStreak=%u", (unsigned)ESP.getFreeHeap(),
        (unsigned)ESP.getMaxAllocHeap(), (unsigned)uploadFailStreak);

    DBG_T0(connT0);
    const bool staUp = connectSTA();
    DBG("WIN: STA connect %s in %lu ms", staUp ? "OK" : "FAILED", (unsigned long)(millis() - connT0));
    if (staUp)
    {
        DBG("WIN: connected ssid=%s ch=%d rssi=%d bssid=%s ip=%s gw=%s dns=%s heap=%u", WiFi.SSID().c_str(),
            (int)WiFi.channel(), (int)WiFi.RSSI(), WiFi.BSSIDstr().c_str(), WiFi.localIP().toString().c_str(),
            WiFi.gatewayIP().toString().c_str(), WiFi.dnsIP().toString().c_str(), (unsigned)ESP.getFreeHeap());
        // Checked after connecting: the WiFi stack itself consumes heap, and a
        // TLS handshake needs one large contiguous block.
        if (!heapOkForTls())
        {
            setErr(ERR_HEAP_LOW, true);
            noteError("Heap too low for TLS: " + String((unsigned)ESP.getMaxAllocHeap()));
            disconnectSTA();
            if (++lowHeapStreak >= LOW_HEAP_RESTARTS)
            {
                Serial.println("Heap fragmented for too long: restarting");
                Serial.flush();
                ESP.restart(); // self-heal; the reset is not reported as a crash
            }
            if (uploadFailStreak < 250) uploadFailStreak++;
            lastUploadTime = millis();
            return;
        }
        lowHeapStreak = 0;
        setErr(ERR_HEAP_LOW, false);

        syncClockIfNeeded();

        if (alsoThingSpeak) sendToThingSpeak(temp, humidity, co2, batteryVoltage, audioPeak);

        CsvStats cs = uploadCsvToAws(millis() + UPLOAD_BUDGET_MS);
        if (cs.failed) ok = false;
        if (cs.more) uploadBacklog = true;
        DBG("WIN: CSV sent=%d rejected=%d skipped=%d failed=%d more=%d", cs.sent, cs.rejected, cs.skipped,
            (int)cs.failed, (int)cs.more);
        if (cs.failed || battLow) DBG("WIN: media skipped (csvFailed=%d battLow=%d)", (int)cs.failed, (int)battLow);

        if (!cs.failed && !battLow)
        {
            MediaStatus ms = processMediaQueue(MAX_MEDIA_PER_WINDOW);
            if (ms.retryLater) ok = false;
            if (ms.more) uploadBacklog = true;
            DBG("WIN: media retryLater=%d pending=%d more=%d", (int)ms.retryLater, (int)ms.pending, (int)ms.more);
        }

        if (!heartbeatEverSent || millis() - lastHeartbeatTime >= HEARTBEAT_INTERVAL_MS)
        {
            DBG("WIN: heartbeat due (everSent=%d)", (int)heartbeatEverSent);
            if (sendHeartbeat(batteryVoltage, WiFi.RSSI()))
            {
                heartbeatEverSent = true;
                lastHeartbeatTime = millis();
            }
        }
        disconnectSTA();
    }
    else
    {
        ok = false;
        DBG("WIN: STA connect failed (wifi status=%d, dashboard clients=%d)", (int)WiFi.status(),
            (int)WiFi.softAPgetStationNum());
        setErr(ERR_AWS_UPLOAD, true);
    }

    if (ok) uploadFailStreak = 0;
    else if (uploadFailStreak < 250) uploadFailStreak++;

    lastUploadTime = millis();
    DBG("WIN: done ok=%d failStreak=%u backlog=%d heap=%u, window took %lu ms", (int)ok,
        (unsigned)uploadFailStreak, (int)uploadBacklog, (unsigned)ESP.getFreeHeap(),
        (unsigned long)(millis() - winT0));
    Serial.printf("===== WINDOW DONE (%s), next in %lu s =====\n", ok ? "ok" : "problems",
                  uploadDelayMs() / 1000UL);
}

// ============================================================================
// SETUP
// ============================================================================
void setup()
{
    Serial.begin(115200);
    delay(500);

    // --- why did we (re)start? ---
    switch (esp_reset_reason())
    {
    case ESP_RST_PANIC:    bootResetCode = "RESET_PANIC"; break;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      bootResetCode = "RESET_WDT"; break;
    case ESP_RST_BROWNOUT: bootResetCode = "RESET_BROWNOUT"; break;
    default:               bootResetCode = nullptr; break;
    }
    if (bootResetCode) setErr(ERR_RESET, true); // reported with the first heartbeat

    wdtBegin();

    prefs.begin("duty", false);
    sensorInterval     = clampUL(prefs.getULong("sensor", DEFAULT_SENSOR_S * 1000UL) / 1000, SENSOR_MIN_S, SENSOR_MAX_S) * 1000UL;
    thingSpeakInterval = clampUL(prefs.getULong("ts", DEFAULT_TS_S * 1000UL) / 1000, TS_MIN_S, TS_MAX_S) * 1000UL;
    recordInterval     = clampUL(prefs.getULong("record", DEFAULT_RECORD_S), RECORD_MIN_S, RECORD_MAX_S);
    uploadInterval     = clampUL(prefs.getULong("upload", DEFAULT_UPLOAD_S * 1000UL) / 1000, UPLOAD_MIN_S, UPLOAD_MAX_S) * 1000UL;
    fullDutyCycle      = prefs.getBool("fullDuty", false);

    Serial.printf("Schedule: sensor %lus  thingspeak %lus  record %lus  upload %lus  mode %s\n",
                  sensorInterval / 1000UL, thingSpeakInterval / 1000UL, recordInterval,
                  uploadInterval / 1000UL, fullDutyCycle ? "duty-cycle" : "normal");
    if (uploadInterval < 120000UL || recordInterval < 300UL)
        Serial.println("WARNING: bench/test schedule saved in flash. Restore upload>=300 s and record>=900 s on the dashboard before field use (battery, data volume).");

    rebootCount = prefs.getUInt("boots", 0) + 1; // cumulative across resets
    prefs.putUInt("boots", rebootCount);
    hbWindowStartUs = esp_timer_get_time();

    Serial.println("\n================================");
    Serial.printf("HIVE LOGGER fw %s  (boot #%lu%s%s)\n", FW_VERSION, (unsigned long)rebootCount,
                  bootResetCode ? ", after " : "", bootResetCode ? bootResetCode : "");
    Serial.println("================================");

    DBG("BOOT: reset reason=%d (1=POWERON 3=SW 4=PANIC 5=INT_WDT 6=TASK_WDT 7=WDT 15=BROWNOUT)", (int)esp_reset_reason());
    DBG("BOOT: chip=%s rev=%d cpu=%u MHz flash=%u KB sdk=%s", ESP.getChipModel(), (int)ESP.getChipRevision(),
        (unsigned)ESP.getCpuFreqMHz(), (unsigned)(ESP.getFlashChipSize() / 1024), ESP.getSdkVersion());
    DBG("BOOT: heap=%u maxBlock=%u minEver=%u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(),
        (unsigned)ESP.getMinFreeHeap());

    if (ademneaIsLocalhost())
        Serial.println("WARNING: ADEMNEA_BASE_URL is localhost. Media uploads cannot work from the ESP32.");

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
    pinMode(MIC_POWER_PIN, OUTPUT);
    digitalWrite(MIC_POWER_PIN, LOW);

    startAP();
    DBG("SETUP: access point up, ip=%s mac=%s", WiFi.softAPIP().toString().c_str(), WiFi.macAddress().c_str());

    server.on("/",             handleRoot);
    server.on("/settingsPage", handleSettingsPage);
    server.on("/data",         handleData);
    server.on("/setDuty",      handleSetDuty);
    server.on("/settings",     handleSettings);
    server.on("/errors",       handleErrors);
    server.begin();
    Serial.println("Dashboard Ready");
    DBG("SETUP: dashboard listening, heap=%u", (unsigned)ESP.getFreeHeap());

    if (connectSTA())
    {
        setupTime();
        if (!clockSynced() && syncClockFromHttpDate()) Serial.println("Clock set from HTTPS Date header");
        disconnectSTA();
    }
    else Serial.println("Failed to connect to WiFi for time sync");
    if (clockSynced()) lastNtpSyncTime = millis();
    setErr(ERR_CLOCK, !clockSynced());
    wdtFeed();

    DBG("BOOT: clock synced=%d epoch=%ld", (int)clockSynced(), (long)time(nullptr));
    ThingSpeak.begin(client);

    Wire.begin(SDA_PIN, SCL_PIN);
    for (byte addr = 1; addr < 127; addr++)
    {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) Serial.printf("I2C device at 0x%02X\n", addr);
    }
    Wire.setClock(50000);

    if (batterySensor.begin()) { batterySensor.setCalibration_32V_2A(); Serial.println("Battery Sensor: OK"); }
    else                       { Serial.println("Battery Sensor: FAILED"); setErr(ERR_BATT, true); }

    if (solarSensor.begin()) { solarSensor.setCalibration_32V_2A(); solarPresent = true; Serial.println("Solar Sensor: OK"); }
    else                     Serial.println("Solar Sensor: not fitted (0x41 absent); solar readings reported as 0");

    scd4x.begin(Wire, 0x62);
    delay(1000);
    Serial.println(initScd41() ? "CO2 Sensor: OK" : "CO2 Sensor: FAILED");
    wdtFeed();

    DBG("SETUP: sensors batteryErr=%d solarPresent=%d scd41Err=%d", (int)((errFlags & ERR_BATT) != 0), (int)solarPresent,
        (int)((errFlags & ERR_SCD41) != 0));

    setupSDCard();
    mediaHousekeeping();
    DBG("SETUP: storage sd_ok=%d errFlags=0x%03lX", (int)sd_ok, (unsigned long)errFlags);

    unsigned long now = millis();
    lastSensorTime = now - sensorInterval; // first sample as soon as the sensor is ready
    lastUploadTime = now;
    lastRecordTime = now;
    awakeSince     = now;

    DBG("BOOT: setup complete, heap=%u maxBlock=%u loopStackFree=%uB", (unsigned)ESP.getFreeHeap(),
        (unsigned)ESP.getMaxAllocHeap(), (unsigned)uxTaskGetStackHighWaterMark(NULL));
    Serial.println("\nSYSTEM READY");
}

#if HIVE_DEBUG
// One block every 30 s: the quickest way to spot leaks, stack exhaustion and stuck queues.
static void dbgHealth()
{
    static unsigned long last = 0;
    if (millis() - last < 30000UL) return;
    last = millis();

    char codes[256];
    buildErrorCodes(codes, sizeof(codes));
    DBG("HEALTH: up=%lus heap=%u minEver=%u maxBlock=%u loopStackFree=%uB", (unsigned long)(millis() / 1000UL),
        (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(), (unsigned)ESP.getMaxAllocHeap(),
        (unsigned)uxTaskGetStackHighWaterMark(NULL));
    DBG("HEALTH: errFlags=0x%03lX [%s] sd=%d mic=%d battLow=%d batt=%.2fV", (unsigned long)errFlags, codes,
        (int)sd_ok, (int)mic_ok, (int)battLow, lastBatteryVoltage);
    DBG("HEALTH: sensor=%lus upload=%lus record=%lus failStreak=%u backlog=%d mediaQueue=%d csv=%uB apClients=%d",
        sensorInterval / 1000UL, uploadInterval / 1000UL, recordInterval, (unsigned)uploadFailStreak,
        (int)uploadBacklog, sd_ok ? countLines(MEDIA_Q) : -1, (unsigned)(sd_ok ? fileSizeOf(csvPath()) : 0),
        (int)WiFi.softAPgetStationNum());
}
#else
static inline void dbgHealth() {}
#endif

void loop()
{
    wdtFeed();
    dbgHealth();

    // SD card dropped out (vibration, a loose connector)? Retry quickly at first and back
    // off to every 5 minutes if the card stays away: a 5-minute fixed wait after a
    // 2-second glitch used to cost ~30 readings.
    static unsigned long sdRetryDelayMs = 15000UL;
    if (!sd_ok && millis() - lastSdRetryTime > sdRetryDelayMs)
    {
        lastSdRetryTime = millis();
        setupSDCard();
        sdRetryDelayMs = sd_ok ? 15000UL : (sdRetryDelayMs * 2 > 300000UL ? 300000UL : sdRetryDelayMs * 2);
        DBG("LOOP: SD remount %s (next attempt in %lu s if it fails)", sd_ok ? "OK" : "FAILED", sdRetryDelayMs / 1000UL);
    }

    if (fullDutyCycle) dutyCycleLoop();
    else               normalLoop();
}

// ============================================================================
// DUTY-CYCLE LOOP (battery / solar operation)
// ============================================================================
static bool sensorReady() { return (int32_t)(millis() - sensorReadyAt) >= 0; }

void dutyCycleLoop()
{
    server.handleClient();

    if (WiFi.softAPgetStationNum() > 0) awakeSince = millis(); // someone is on the dashboard

    if (millis() - lastSensorTime >= sensorInterval && sensorReady())
    {
        sampleSensors();
        lastSensorTime = millis();
    }

    if (millis() - lastUploadTime >= uploadDelayMs())
        runUploadWindow(true);

    if (millis() - lastRecordTime >= (recordInterval * 1000UL))
    {
        recordAndEnqueue();
        lastRecordTime = millis();
    }

    // Light-sleep only after the grace period, and only until the next task is
    // due, so scheduled work never runs late and the dashboard is reachable.
    if (WiFi.softAPgetStationNum() == 0 && millis() - awakeSince > AWAKE_GRACE_MS)
    {
        uint32_t sleepMs = msUntil(lastSensorTime, sensorInterval);
        uint32_t u = msUntil(lastUploadTime, uploadDelayMs());
        uint32_t r = msUntil(lastRecordTime, recordInterval * 1000UL);
        if (u < sleepMs) sleepMs = u;
        if (r < sleepMs) sleepMs = r;

        if (sleepMs > 1000)
        {
            Serial.printf("Light sleep for %lu ms\n", (unsigned long)sleepMs);
            Serial.flush();
            DBG("SLEEP: entering light sleep for %lu ms (upload in %lu ms, record in %lu ms)", (unsigned long)sleepMs,
                (unsigned long)u, (unsigned long)r);
            scd4x.stopPeriodicMeasurement();
            delay(500);
            powerOffMicrophone();
            disconnectSTA();

            wdtFeed();
            esp_sleep_enable_timer_wakeup((uint64_t)sleepMs * 1000ULL);
            uint64_t t0 = esp_timer_get_time();
            esp_light_sleep_start();
            idleUs += esp_timer_get_time() - t0;
            DBG("SLEEP: woke after %lu ms, cause=%d (4=timer)", (unsigned long)((esp_timer_get_time() - t0) / 1000),
                (int)esp_sleep_get_wakeup_cause());
            wdtFeed();

            scd4x.startPeriodicMeasurement();
            sensorReadyAt = millis() + 5500;
            awakeSince = millis();
        }
        else idleDelay(10);
    }
    else idleDelay(10);
}

// ============================================================================
// NORMAL LOOP (mains / bench operation)
// ============================================================================
void normalLoop()
{
    server.handleClient();

    if (millis() - lastSensorTime >= sensorInterval && sensorReady())
    {
        sampleSensors();
        lastSensorTime = millis();
    }

    if (millis() - lastDisplayTime >= 10000)
    {
        Serial.println("\n================================");
        Serial.printf("TIME: %s\n", getTimestamp().c_str());
        Serial.printf("Temp: %.1f C  Hum: %.1f %%  CO2: %u ppm\n", temp, humidity, co2);
        Serial.printf("Battery: %.2f V  Audio peak: %ld  SD: %s  Heap: %u (max block %u)\n",
                      batteryVoltage, audioPeak, sd_ok ? "ONLINE" : "OFFLINE",
                      (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
        Serial.println("================================");
        lastDisplayTime = millis();
    }

    if (millis() - lastThingSpeakTime >= thingSpeakInterval)
    {
        if (connectSTA())
        {
            sendToThingSpeak(temp, humidity, co2, batteryVoltage, audioPeak);
            disconnectSTA();
        }
        lastThingSpeakTime = millis();
    }

    if (millis() - lastUploadTime >= uploadDelayMs())
        runUploadWindow(false);

    if (millis() - lastRecordTime >= (recordInterval * 1000UL))
    {
        recordAndEnqueue();
        lastRecordTime = millis();
    }

    idleDelay(10);
}
