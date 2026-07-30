
#include <Arduino.h>
#include "logging.h"
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
