#pragma once
// Pure C++ helpers with no Arduino/ESP32 dependencies, so they can be unit
// tested on a PC (see test/test_util.cpp). No heap allocation anywhere here.

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

namespace hu
{

// ---------------------------------------------------------------------
// Calendar maths (Howard Hinnant's civil-date algorithms). Deterministic and
// independent of TZ / mktime / gmtime, so the same input always produces the
// same recorded_at string. That string is the server's duplicate-detection key.
// ---------------------------------------------------------------------
inline int64_t daysFromCivil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

inline void civilFromDays(int64_t z, int &y, unsigned &m, unsigned &d)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = (int)(yoe + era * 400);
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += (m <= 2);
}

inline bool isLeap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

inline int daysInMonth(int y, int m)
{
    static const int dm[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return (m == 2 && isLeap(y)) ? 29 : dm[m - 1];
}

// Validates every field (including day-of-month). Returns false if impossible.
inline bool civilToEpoch(int y, int mo, int d, int h, int mi, int s, int64_t &out)
{
    if (y < 1970 || y > 2100 || mo < 1 || mo > 12) return false;
    if (d < 1 || d > daysInMonth(y, mo)) return false;
    if (h < 0 || h > 23 || mi < 0 || mi > 59 || s < 0 || s > 59) return false;
    out = daysFromCivil(y, (unsigned)mo, (unsigned)d) * 86400LL + h * 3600LL + mi * 60LL + s;
    return true;
}

inline void splitEpoch(int64_t epoch, int &y, int &mo, int &d, int &h, int &mi, int &s)
{
    int64_t days = epoch >= 0 ? epoch / 86400 : -((-epoch + 86399) / 86400);
    int64_t rem = epoch - days * 86400;
    unsigned um, ud;
    civilFromDays(days, y, um, ud);
    mo = (int)um;
    d = (int)ud;
    h = (int)(rem / 3600);
    mi = (int)((rem % 3600) / 60);
    s = (int)(rem % 60);
}

// "2026-07-26T09:12:00Z"  (needs >= 21 bytes)
inline void epochToIso(int64_t epoch, char *out)
{
    int y, mo, d, h, mi, s;
    splitEpoch(epoch, y, mo, d, h, mi, s);
    snprintf(out, 21, "%04d-%02d-%02dT%02d:%02d:%02dZ", y, mo, d, h, mi, s);
}

// "2026-07-26 12:12:00" for epoch shifted by a fixed local offset (needs >= 20 bytes)
inline void epochToLocalStamp(int64_t epoch, long offsetSec, char *out)
{
    int y, mo, d, h, mi, s;
    splitEpoch(epoch + offsetSec, y, mo, d, h, mi, s);
    snprintf(out, 20, "%04d-%02d-%02d %02d:%02d:%02d", y, mo, d, h, mi, s);
}

// ---------------------------------------------------------------------
// CSV row parser. Row format written by logToCSV():
//   "2026-09-24 13:00:00","34.90*2*2","58.20*2*2","812",2.00,2
// `line` is modified in place (no heap use). The local timestamp is converted
// to a UTC ISO string using the fixed tzOffsetSec.
// Returns false for blank/garbled rows, rows logged before the clock was
// synced ("0000-..."), and physically impossible values.
// ---------------------------------------------------------------------
inline void stripQuotesAndTrim(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++)
        if (*r != '"') *w++ = *r;
    *w = 0;
    while (w > s && isspace((unsigned char)w[-1])) *--w = 0;
    char *st = s;
    while (*st && isspace((unsigned char)*st)) st++;
    if (st != s) memmove(s, st, strlen(st) + 1);
}

inline bool parseCsvRow(char *line, long tzOffsetSec, char *isoOut /*>=21*/,
                        float &tempC, float &humPct, int &co2ppm)
{
    char *f[6];
    int n = 0;
    char *p = line;
    while (n < 6)
    {
        f[n++] = p;
        char *comma = strchr(p, ',');
        if (!comma) break;
        *comma = 0;
        p = comma + 1;
    }
    if (n < 4) return false;
    for (int i = 0; i < 4; i++) stripQuotesAndTrim(f[i]);

    if (strlen(f[0]) != 19) return false;
    int y, mo, d, h, mi, s;
    if (sscanf(f[0], "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6) return false;

    int64_t epoch;
    if (!civilToEpoch(y, mo, d, h, mi, s, epoch)) return false;
    if (y < 2024) return false; // "0000-00-00" or a clock that never synced
    epochToIso(epoch - tzOffsetSec, isoOut);

    char *end;
    tempC = strtof(f[1], &end); // "34.90*2*2" -> 34.90 (legacy suffix ignored)
    if (end == f[1]) return false;
    humPct = strtof(f[2], &end);
    if (end == f[2]) return false;
    long c = strtol(f[3], &end, 10);
    if (end == f[3]) return false;
    co2ppm = (int)c;

    if (isnan(tempC) || isnan(humPct)) return false;
    if (tempC < -40 || tempC > 85) return false;
    if (humPct < 0 || humPct > 100) return false;
    if (co2ppm < 0 || co2ppm > 40000) return false;
    return true;
}

// ---------------------------------------------------------------------
// Minimal JSON string-value extractor. Handles whitespace around ':' and the
// escapes Laravel's json_encode produces (\/ and \uXXXX, e.g. \u0026 for '&'),
// which a naive substring copy would leave inside the presigned URL and break
// its signature. Skips matches that are values rather than keys.
// ---------------------------------------------------------------------
inline int hexVal(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

inline bool jsonGetString(const char *json, const char *key, char *out, size_t outSize)
{
    if (!json || !key || !out || outSize == 0) return false;
    char needle[48];
    if (snprintf(needle, sizeof(needle), "\"%s\"", key) >= (int)sizeof(needle)) return false;

    const char *p = json;
    while ((p = strstr(p, needle)) != nullptr)
    {
        p += strlen(needle);
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p != ':') continue; // the text matched a value, not a key
        p++;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p != '"') return false; // not a string value
        p++;

        size_t o = 0;
        while (*p)
        {
            char ch = *p++;
            if (ch == '"')
            {
                out[o] = 0;
                return true;
            }
            if (ch == '\\')
            {
                char e = *p++;
                if (!e) return false;
                switch (e)
                {
                case '/': ch = '/'; break;
                case '\\': ch = '\\'; break;
                case '"': ch = '"'; break;
                case 'n': ch = '\n'; break;
                case 't': ch = '\t'; break;
                case 'r': ch = '\r'; break;
                case 'u':
                {
                    int cp = 0;
                    for (int i = 0; i < 4; i++)
                    {
                        int hv = *p ? hexVal(*p) : -1;
                        if (hv < 0) return false;
                        cp = cp * 16 + hv;
                        p++;
                    }
                    if (cp == 0 || cp > 127) return false; // URLs/keys are ASCII
                    ch = (char)cp;
                    break;
                }
                default: ch = e; break;
                }
            }
            if (o + 1 >= outSize) return false; // would truncate: fail loudly
            out[o++] = ch;
        }
        return false; // unterminated
    }
    return false;
}

} // namespace hu
