#include <Arduino.h>
#include <time.h>
#include "logging.h"

// ======================================================
// ERROR LOGGING  (ring buffer shown on the dashboard)
// ======================================================
#define MAX_ERRORS    20
#define MAX_ERROR_LEN 160 // longer messages are truncated: bounded RAM

static String errorLogs[MAX_ERRORS];
static int    errorIndex = 0;

static const time_t MIN_VALID_EPOCH = 1735689600; // 2025-01-01

// Prints the entry to Serial itself: callers must not print the message again.
void addError(String msg)
{
    char ts[12];

    // time() instead of getLocalTime(): the latter blocks up to 5 s per call while
    // the clock is unset, and errors are most frequent exactly then.
    const time_t now = time(nullptr);
    if (now >= MIN_VALID_EPOCH)
    {
        struct tm timeinfo;
        localtime_r(&now, &timeinfo);
        snprintf(ts, sizeof(ts), "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    }
    else
    {
        strcpy(ts, "--:--:--");
    }

    if (msg.length() > MAX_ERROR_LEN) msg.remove(MAX_ERROR_LEN);

    String entry;
    entry.reserve(msg.length() + 14);
    entry = "[";
    entry += ts;
    entry += "] ";
    entry += msg;

    errorLogs[errorIndex] = entry;
    errorIndex = (errorIndex + 1) % MAX_ERRORS;

    Serial.println(entry);
}

// Appends s as the body of a JSON string (quotes, backslashes and control
// characters escaped: an unescaped quote used to break the whole /errors reply).
static void appendJsonEscaped(String &out, const String &s)
{
    for (unsigned int i = 0; i < s.length(); i++)
    {
        const char c = s[i];
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n')        out += "\\n";
        else if (c == '\r')        out += "\\r";
        else if (c == '\t')        out += "\\t";
        else if ((unsigned char)c < 0x20) out += ' ';
        else                       out += c;
    }
}

// =====================================================
// API ENDPOINT FOR ERRORS (oldest first)
// =====================================================
void handleErrors()
{
    String json;
    json.reserve(1024);
    json = "[";

    bool first = true;
    for (int i = 0; i < MAX_ERRORS; i++)
    {
        const int idx = (errorIndex + i) % MAX_ERRORS;
        if (errorLogs[idx].length() == 0) continue;

        if (!first) json += ",";
        json += "\"";
        appendJsonEscaped(json, errorLogs[idx]);
        json += "\"";
        first = false;
    }
    json += "]";

    server.send(200, "application/json", json);
}
