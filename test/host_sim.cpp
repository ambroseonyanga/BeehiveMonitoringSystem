// Host simulation of the delivery logic in main.cpp: a real on-disk fake SD
// card and a scripted fake gateway. Verifies the data-loss-critical paths.
//   cd test && ./run_tests.sh
#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ThingSpeak.h>
#include <driver/i2s.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_task_wdt.h>
#include <stdlib.h>
#include <fstream>
#include <sstream>

// ---------- platform shims ----------
static unsigned long g_millis = 1000000;
unsigned long millis() { return g_millis; }
void delay(uint32_t) {} void yield() {} void pinMode(int,int) {} void digitalWrite(int,int) {}
SerialC Serial; EspC ESP; TwoWire Wire; SPIC SPI; SDC SD; WiFiC WiFi; TS ThingSpeak;
std::vector<HttpCall> g_calls; std::deque<int> g_codes; std::deque<std::string> g_bodies; int g_defaultCode = 202;
std::string sd_map(const std::string &p) { const char *r = getenv("SD_ROOT"); return std::string(r ? r : "/tmp/sdroot") + p; }
esp_err_t i2s_driver_install(i2s_port_t,const i2s_config_t*,int,void*){return 0;} esp_err_t i2s_driver_uninstall(i2s_port_t){return 0;}
esp_err_t i2s_set_pin(i2s_port_t,const i2s_pin_config_t*){return 0;} esp_err_t i2s_zero_dma_buffer(i2s_port_t){return 0;}
esp_err_t i2s_read(i2s_port_t,void*,size_t,size_t*,uint32_t){return -1;}
esp_reset_reason_t esp_reset_reason() { return ESP_RST_POWERON; }
int64_t esp_timer_get_time() { return (int64_t)g_millis * 1000; } int esp_light_sleep_start() { return 0; }
int esp_sleep_enable_timer_wakeup(uint64_t) { return 0; }
int esp_task_wdt_init(uint32_t,bool){return 0;} int esp_task_wdt_add(void*){return 0;} int esp_task_wdt_reset(){return 0;}
const char INDEX_HTML[] = "x"; const char SETTINGS_HTML[] = "x";
static std::vector<std::string> g_errors;
void addError(String s) { g_errors.push_back(s.s); }
void startAP() {} bool connectSTA() { return true; } void disconnectSTA() {} void setupTime() {}
String getTimestamp() { return String("t"); } void handleErrors() {}

#include "../main.cpp"   // the firmware under test

// ---------- tiny test framework ----------
static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { printf("  FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
#define SCENARIO(n) printf("\n== %s\n", n)

static std::string readFile(const std::string &p) { std::ifstream f(sd_map(p)); std::stringstream s; s << f.rdbuf(); return s.str(); }
static void writeFile(const std::string &p, const std::string &c) { std::ofstream f(sd_map(p), std::ios::binary); f << c; }
static int nLines(const std::string &s) { int n = 0; std::stringstream ss(s); std::string l; while (std::getline(ss, l)) if (!l.empty()) n++; return n; }
static bool contains(const std::string &h, const std::string &n) { return h.find(n) != std::string::npos; }
static std::string csvRow(int i) {
    char b[160]; snprintf(b, sizeof(b), "\"2026-09-24 13:%02d:00\",\"%d.90*2*2\",\"58.20*2*2\",\"%d\",2.00,2\n", i, 30 + i, 800 + i); return b; }
static void freshState() {
    system("rm -rf /tmp/sdroot && mkdir -p /tmp/sdroot");
    g_calls.clear(); g_codes.clear(); g_bodies.clear(); g_defaultCode = 202; g_errors.clear();
    errFlags = 0; sd_ok = true; hiveNo = "1";
}
static void makeCsv(int n) { std::string s; for (int i = 0; i < n; i++) s += csvRow(i); writeFile("/1.csv", s); }
static std::vector<std::string> g_allBodies;
static void harvest() { for (auto &c : g_calls) if (c.method == "POST") g_allBodies.push_back(c.body); }
static std::string hdr(const HttpCall &c, const std::string &k) { for (auto &h : c.headers) if (h.first == k) return h.second; return "<none>"; }

int main()
{
    // ------------------------------------------------------------------
    SCENARIO("A. all rows accepted -> 3 POSTs per row, CSV emptied");
    freshState(); makeCsv(5);
    { CsvStats st = uploadCsvToAws(millis() + 60000);
      CHECK(st.sent == 5 && !st.failed && !st.more);
      CHECK(g_calls.size() == 15);
      CHECK(g_calls[0].url == std::string(AWS_API_BASE) + "/ingest");
      CHECK(hdr(g_calls[0], "X-Api-Key") == "TESTKEY123");
      CHECK(g_calls[0].body == "{\"device_id\":\"AEU-UG-001\",\"sensor_type\":\"temperature\",\"recorded_at\":\"2026-09-24T10:00:00Z\",\"reading\":{\"brood_section\":30.90}}");
      CHECK(contains(g_calls[1].body, "\"sensor_type\":\"humidity\"") && contains(g_calls[1].body, "\"brood_section\":58.20"));
      CHECK(contains(g_calls[2].body, "\"sensor_type\":\"co2\"") && contains(g_calls[2].body, "\"reading\":{\"co2_level\":800}"));
      CHECK(readFile("/1.csv").empty()); harvest(); }

    // ------------------------------------------------------------------
    SCENARIO("B. network drops mid-row -> unsent tail kept, retry reuses identical recorded_at");
    freshState(); makeCsv(5);
    g_codes = {202,202,202, 202,202,202, 202,-1};   // rows 1,2 ok; row 3: temp ok, humidity fails
    std::string firstTry;
    { CsvStats st = uploadCsvToAws(millis() + 60000);
      CHECK(st.failed && st.sent == 2);
      std::string left = readFile("/1.csv");
      CHECK(nLines(left) == 3);                              // rows 3,4,5 survive
      CHECK(contains(left, "13:02:00") && !contains(left, "13:01:00"));
      CHECK(ERR_AWS_UPLOAD & errFlags);
      firstTry = g_calls[6].body; harvest(); }
    g_calls.clear();
    { CsvStats st = uploadCsvToAws(millis() + 60000);
      CHECK(!st.failed && st.sent == 3 && readFile("/1.csv").empty());
      CHECK(g_calls[0].body == firstTry);                    // byte-identical resend = server-side dedupe works
      CHECK(!(ERR_AWS_UPLOAD & errFlags)); harvest(); }

    // ------------------------------------------------------------------
    SCENARIO("C. gateway returns 400 for one message -> row quarantined, rest still delivered");
    freshState(); makeCsv(3);
    g_codes = {202,202,202, 400,202,202, 202,202,202};
    { CsvStats st = uploadCsvToAws(millis() + 60000);
      CHECK(st.sent == 2 && st.rejected == 1 && !st.failed);
      CHECK(readFile("/1.csv").empty());
      CHECK(nLines(readFile("/rejected.csv")) == 1 && contains(readFile("/rejected.csv"), "13:01:00")); harvest(); }

    // ------------------------------------------------------------------
    SCENARIO("D. 403 from gateway -> nothing deleted, auth flag raised");
    freshState(); makeCsv(4); g_defaultCode = 403;
    { CsvStats st = uploadCsvToAws(millis() + 60000);
      CHECK(st.failed && st.sent == 0 && nLines(readFile("/1.csv")) == 4 && (errFlags & ERR_AUTH)); }
    g_defaultCode = 202;
    { CsvStats st = uploadCsvToAws(millis() + 60000);
      CHECK(!st.failed && st.sent == 4 && !(errFlags & ERR_AUTH)); harvest(); }

    // ------------------------------------------------------------------
    SCENARIO("E. time budget exhausted -> backlog flagged, file untouched");
    freshState(); makeCsv(5);
    { CsvStats st = uploadCsvToAws(millis()); // budget already spent
      CHECK(st.more && st.sent == 0 && nLines(readFile("/1.csv")) == 5 && g_calls.empty()); }

    // ------------------------------------------------------------------
    SCENARIO("F. garbage / unsynced / blank lines are skipped, valid rows delivered");
    freshState();
    writeFile("/1.csv", "\"0000-00-00 00:00:00\",\"20.00*2*2\",\"50.00*2*2\",\"400\",2.00,2\n\n\x01\x02 junk,,,\n" + csvRow(7) + "\"2026-02-30 10:00:00\",\"20.00*2*2\",\"50.00*2*2\",\"400\",2.00,2\n");
    { CsvStats st = uploadCsvToAws(millis() + 60000);
      CHECK(st.sent == 1 && st.skipped == 3 && !st.failed && readFile("/1.csv").empty()); harvest(); }

    // ------------------------------------------------------------------
    SCENARIO("G. media happy path: upload-url -> PUT (no api key) -> confirm");
    freshState();
    writeFile("/1_20260926T091500Z.wav", std::string(2000, 'x'));
    CHECK(enqueueMedia("/1_20260926T091500Z.wav", "2026-09-26T09:15:00Z"));
    g_codes = {200, 200, 202};   // real S3 answers a presigned PUT with 200
    g_bodies = {"{\"upload_url\":\"https:\\/\\/b.s3.eu-north-1.amazonaws.com\\/iot-media\\/AEU-UG-001\\/x.wav?X-Amz-Signature=ab\\u0026X-Amz-Expires=900\","
                "\"s3_object_key\":\"iot-media\\/AEU-UG-001\\/2026-09-26T09-15-00Z.wav\",\"expires_in_seconds\":900}"};
    { MediaStatus ms = processMediaQueue(2);
      CHECK(g_calls.size() == 3 && !ms.retryLater && !ms.pending);
      CHECK(g_calls[0].url == "https://ademnea.example/api/iot/media/upload-url" && hdr(g_calls[0], "X-Api-Key") == "TESTKEY123");
      CHECK(contains(g_calls[0].body, "\"media_type\":\"audio\"") && contains(g_calls[0].body, "\"content_type\":\"audio/wav\""));
      CHECK(g_calls[1].method == "PUT" && g_calls[1].url == "https://b.s3.eu-north-1.amazonaws.com/iot-media/AEU-UG-001/x.wav?X-Amz-Signature=ab&X-Amz-Expires=900");
      CHECK(hdr(g_calls[1], "X-Api-Key") == "<none>" && hdr(g_calls[1], "Content-Type") == "audio/wav");
      CHECK(g_calls[2].url == std::string(AWS_API_BASE) + "/media/confirm");
      CHECK(contains(g_calls[2].body, "\"s3_object_key\":\"iot-media/AEU-UG-001/2026-09-26T09-15-00Z.wav\""));
      CHECK(contains(g_calls[2].body, "\"captured_at\":\"2026-09-26T09:15:00Z\"") && contains(g_calls[2].body, "\"file_size_bytes\":2000"));
      CHECK(!SD.exists("/1_20260926T091500Z.wav") && nLines(readFile("/media_q.txt")) == 0); harvest(); }

    // ------------------------------------------------------------------
    SCENARIO("H. confirm fails after a good PUT -> retried WITHOUT re-uploading the file");
    freshState();
    writeFile("/1_20260926T091500Z.wav", std::string(2000, 'x'));
    enqueueMedia("/1_20260926T091500Z.wav", "2026-09-26T09:15:00Z");
    g_codes = {200, 204, 500};
    g_bodies = {"{\"upload_url\":\"https://b.s3/x?s=1\",\"s3_object_key\":\"iot-media/k.wav\"}"};
    { MediaStatus ms = processMediaQueue(2);
      CHECK(ms.retryLater && ms.pending && SD.exists("/1_20260926T091500Z.wav"));
      CHECK(contains(readFile("/media_q.txt"), "|iot-media/k.wav|")); harvest(); }
    g_calls.clear(); g_codes.clear();
    { MediaStatus ms = processMediaQueue(2);
      CHECK(g_calls.size() == 1 && g_calls[0].url == std::string(AWS_API_BASE) + "/media/confirm");
      CHECK(!ms.pending && !SD.exists("/1_20260926T091500Z.wav")); harvest(); }

    // ------------------------------------------------------------------
    SCENARIO("I. permanently failing upload -> file moved to /failed after max attempts, never deleted");
    freshState();
    writeFile("/1_20260926T091500Z.wav", std::string(2000, 'x'));
    enqueueMedia("/1_20260926T091500Z.wav", "2026-09-26T09:15:00Z");
    for (int i = 0; i < MEDIA_MAX_ATTEMPTS; i++) {
        g_codes = {200, 403};
        g_bodies = {"{\"upload_url\":\"https://b.s3/x?s=1\",\"s3_object_key\":\"iot-media/k.wav\"}"};
        processMediaQueue(2);
    }
    CHECK(!SD.exists("/1_20260926T091500Z.wav") && SD.exists("/failed/1_20260926T091500Z.wav"));
    CHECK(nLines(readFile("/media_q.txt")) == 0);

    // ------------------------------------------------------------------
    SCENARIO("J. boot housekeeping adopts orphaned recordings, drops stubs, caps /failed");
    freshState();
    writeFile("/1_20260926T101500Z.wav", std::string(5000, 'x'));   // orphan: recorded, never queued
    writeFile("/1_20260926T111500Z.wav", std::string(10, 'x'));     // stub from an interrupted start
    writeFile("/other.wav", std::string(5000, 'x'));                 // not ours: untouched
    SD.mkdir("/failed");
    for (int i = 0; i < FAILED_MEDIA_MAX + 5; i++) { char n[64]; snprintf(n, sizeof(n), "/failed/1_202609%02dT000000Z.wav", i + 1); writeFile(n, "x"); }
    mediaHousekeeping();
    { std::string q = readFile("/media_q.txt");
      CHECK(nLines(q) == 1 && contains(q, "/1_20260926T101500Z.wav|2026-09-26T10:15:00Z||0"));
      CHECK(!SD.exists("/1_20260926T111500Z.wav") && SD.exists("/other.wav"));
      int cnt = 0; File d = SD.open("/failed"); for (File f = d.openNextFile(); f; f = d.openNextFile()) cnt++;
      CHECK(cnt == FAILED_MEDIA_MAX);
      CHECK(!SD.exists("/failed/1_20260901T000000Z.wav") && SD.exists("/failed/1_20260925T000000Z.wav")); } // oldest evicted first

    // ------------------------------------------------------------------
    SCENARIO("K. heartbeat payload matches the documented flat schema");
    freshState();
    setErr(ERR_SD, true); setErr(ERR_RESET, true); bootResetCode = "RESET_BROWNOUT"; rebootCount = 7; sensorReadOk = 49; sensorReadTotal = 50;
    CHECK(sendHeartbeat(3.9f, -62));
    { const std::string &b = g_calls[0].body;
      CHECK(g_calls[0].url == std::string(AWS_API_BASE) + "/heartbeat");
      CHECK(contains(b, "\"battery_level\":75.0") && contains(b, "\"signal_strength\":-62") && contains(b, "\"reboot_count\":7"));
      CHECK(contains(b, "\"sensor_read_success_rate\":0.98") && contains(b, "\"error_codes\":[\"SD_FAIL\",\"RESET_BROWNOUT\"]"));
      CHECK(!contains(b, "\"reading\"") && !contains(b, "\"sensor_type\""));
      CHECK(!(errFlags & ERR_RESET)); } harvest();    // reset reason is reported exactly once

    // ------------------------------------------------------------------
    SCENARIO("L. power loss between 'delete original' and 'rename tmp' is repaired at boot");
    freshState();
    writeFile("/upload.tmp", csvRow(3) + csvRow(4));          // original already deleted
    recoverTmp("/1.csv", "/upload.tmp");
    CHECK(nLines(readFile("/1.csv")) == 2 && !SD.exists("/upload.tmp"));
    writeFile("/upload.tmp", "partial");                       // both exist: tmp is suspect, keep original
    recoverTmp("/1.csv", "/upload.tmp");
    CHECK(nLines(readFile("/1.csv")) == 2 && !SD.exists("/upload.tmp"));

    // ------------------------------------------------------------------
    SCENARIO("M. backoff grows on failure, resets on success, honours backlog");
    uploadInterval = 300000; uploadFailStreak = 0; uploadBacklog = false;
    CHECK(uploadDelayMs() == 300000);
    uploadFailStreak = 1; CHECK(uploadDelayMs() == 600000);
    uploadFailStreak = 3; CHECK(uploadDelayMs() == 2400000);
    uploadFailStreak = 9; CHECK(uploadDelayMs() == BACKOFF_MAX_MS);       // capped at 1 h
    uploadFailStreak = 0; uploadBacklog = true; CHECK(uploadDelayMs() == BACKLOG_RETRY_MS);

    // ------------------------------------------------------------------
    SCENARIO("N. logToCSV round-trips through the uploader with a correct UTC timestamp");
    freshState();
    CHECK(clockSynced());
    logToCSV(34.1f, 63.7f, 612);
    { std::string row = readFile("/1.csv");
      CHECK(nLines(row) == 1 && contains(row, "\"34.10*2*2\",\"63.70*2*2\",\"612\""));
      g_calls.clear();
      CsvStats st = uploadCsvToAws(millis() + 60000);
      CHECK(st.sent == 1);
      time_t now = time(nullptr); char want[24]; struct tm g; gmtime_r(&now, &g); strftime(want, sizeof(want), "%Y-%m-%dT%H:", &g);
      CHECK(contains(g_calls[0].body, want));                      // same UTC hour as the host clock (offset round-trips)
      harvest(); }

    // dump every body POSTed during the run for external JSON validation
    { std::ofstream f("/tmp/bodies.jsonl"); for (auto &b : g_allBodies) f << b << "\n"; }

    printf("\n%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
