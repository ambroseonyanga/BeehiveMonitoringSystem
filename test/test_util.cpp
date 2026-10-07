// Host-side tests. Build & run on a PC:
//   g++ -std=c++17 -Wall -Wextra -I.. test_util.cpp -o test_util && ./test_util
#include "hive_util.h"
#include <assert.h>
#include <string>
#include <time.h>

static int fails = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } \
    } while (0)

static std::string iso(int64_t e) { char b[24]; hu::epochToIso(e, b); return b; }

int main()
{
    // --- epoch <-> ISO, checked against the C library over many dates ---
    CHECK(iso(0) == "1970-01-01T00:00:00Z");
    CHECK(iso(1785057120) == "2026-07-26T09:12:00Z"); // the doc's example timestamp
    for (int64_t e = 0; e < 4102444800LL; e += 86399 * 7 + 13)
    {
        time_t t = (time_t)e;
        struct tm g;
        gmtime_r(&t, &g);
        char ref[24];
        strftime(ref, sizeof(ref), "%Y-%m-%dT%H:%M:%SZ", &g);
        if (iso(e) != ref) { printf("ISO mismatch at %lld\n", (long long)e); fails++; break; }

        int64_t back;
        CHECK(hu::civilToEpoch(g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec, back));
        if (back != e) { printf("round-trip mismatch at %lld\n", (long long)e); fails++; break; }
    }

    // --- validation ---
    int64_t e;
    CHECK(!hu::civilToEpoch(2026, 2, 30, 0, 0, 0, e));  // Feb 30
    CHECK(!hu::civilToEpoch(2025, 2, 29, 0, 0, 0, e));  // not a leap year
    CHECK(hu::civilToEpoch(2028, 2, 29, 0, 0, 0, e));   // leap year
    CHECK(!hu::civilToEpoch(2026, 13, 1, 0, 0, 0, e));
    CHECK(!hu::civilToEpoch(2026, 1, 1, 24, 0, 0, e));

    // --- CSV row -> UTC ISO (EAT = UTC+3, so 13:00 local = 10:00Z) ---
    {
        char row[] = "\"2026-09-24 13:00:00\",\"34.90*2*2\",\"58.20*2*2\",\"812\",2.00,2";
        char at[24]; float t, h; int c;
        CHECK(hu::parseCsvRow(row, 3 * 3600, at, t, h, c));
        CHECK(std::string(at) == "2026-09-24T10:00:00Z");
        CHECK(fabsf(t - 34.90f) < 0.001f && fabsf(h - 58.20f) < 0.001f && c == 812);
    }
    {   // local midnight crosses the UTC date boundary
        char row[] = "\"2026-01-01 01:30:00\",\"20.00*2*2\",\"50.00*2*2\",\"400\",2.00,2";
        char at[24]; float t, h; int c;
        CHECK(hu::parseCsvRow(row, 3 * 3600, at, t, h, c));
        CHECK(std::string(at) == "2025-12-31T22:30:00Z");
    }
    {   // clock was never synced
        char row[] = "\"0000-00-00 00:00:00\",\"20.00*2*2\",\"50.00*2*2\",\"400\",2.00,2";
        char at[24]; float t, h; int c;
        CHECK(!hu::parseCsvRow(row, 10800, at, t, h, c));
    }
    {   // garbage / truncated / impossible
        char a[] = "";                                                           char at[24]; float t, h; int c;
        CHECK(!hu::parseCsvRow(a, 10800, at, t, h, c));
        char b[] = "\"2026-09-24 13:00:00\",\"34.9";
        CHECK(!hu::parseCsvRow(b, 10800, at, t, h, c));
        char d[] = "\"2026-09-24 13:00:00\",\"999.00*2*2\",\"58.20*2*2\",\"812\",2.00,2";
        CHECK(!hu::parseCsvRow(d, 10800, at, t, h, c));
        char f[] = "\"2026-09-24 13:00:00\",\"abc\",\"58.20*2*2\",\"812\",2.00,2";
        CHECK(!hu::parseCsvRow(f, 10800, at, t, h, c));
        char g[] = "\"2026-09-24 13:00:00\",\"30.0*2*2\",\"58.2*2*2\",\"-5\",2.00,2";
        CHECK(!hu::parseCsvRow(g, 10800, at, t, h, c));
    }
    {   // CRLF line endings and stray spaces
        char row[] = " \"2026-09-24 13:00:00\" , \"34.90*2*2\" ,\"58.20*2*2\",\"812\",2.00,2\r";
        char at[24]; float t, h; int c;
        CHECK(hu::parseCsvRow(row, 10800, at, t, h, c));
    }

    // --- local stamp is the inverse of the parser's conversion ---
    {
        char stamp[24];
        hu::epochToLocalStamp(1785057120, 10800, stamp);
        CHECK(std::string(stamp) == "2026-07-26 12:12:00");
    }

    // --- JSON extraction, incl. Laravel-style escaped slashes and \u0026 ---
    {
        const char *j = "{\"upload_url\":\"http:\\/\\/127.0.0.1:8000\\/api\\/iot\\/media-mock\\/abc?x=1\\u0026signature=ff\","
                        "\"s3_object_key\" : \"iot-media\\/AEU-UG-005\\/2026-07-26T09-15-00Z.jpg\",\"expires_in_seconds\":900}";
        char url[256], key[128];
        CHECK(hu::jsonGetString(j, "upload_url", url, sizeof(url)));
        CHECK(std::string(url) == "http://127.0.0.1:8000/api/iot/media-mock/abc?x=1&signature=ff");
        CHECK(hu::jsonGetString(j, "s3_object_key", key, sizeof(key)));
        CHECK(std::string(key) == "iot-media/AEU-UG-005/2026-07-26T09-15-00Z.jpg");
        CHECK(!hu::jsonGetString(j, "missing", key, sizeof(key)));
        CHECK(!hu::jsonGetString(j, "expires_in_seconds", key, sizeof(key))); // number, not a string
        char tiny[8];
        CHECK(!hu::jsonGetString(j, "upload_url", tiny, sizeof(tiny)));       // refuses to truncate
        // a value that happens to equal a key name must not confuse the lookup
        const char *tricky = "{\"note\":\"upload_url\",\"upload_url\":\"https://x/y\"}";
        CHECK(hu::jsonGetString(tricky, "upload_url", url, sizeof(url)));
        CHECK(std::string(url) == "https://x/y");
    }

    printf(fails ? "\n%d FAILURE(S)\n" : "\nALL TESTS PASSED\n", fails);
    return fails ? 1 : 0;
}
