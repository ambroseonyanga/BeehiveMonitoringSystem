#include "TimeManager.h"

TimeManager::TimeManager()
{
    _ntpServer = "pool.ntp.org";
    _gmtOffset = 3 * 3600;
    _daylightOffset = 0;
}

TimeManager::TimeManager(
    const char* ntpServer,
    long gmtOffset,
    int daylightOffset)
{
    _ntpServer = ntpServer;
    _gmtOffset = gmtOffset;
    _daylightOffset = daylightOffset;
}

void TimeManager::begin()
{
    Serial.println();
    Serial.println("[TIME]");

    configTime(
        _gmtOffset,
        _daylightOffset,
        _ntpServer
    );

    struct tm timeinfo;

    while (!getLocalTime(&timeinfo))
    {
        Serial.println("Waiting for NTP time...");
        delay(1000);
    }

    Serial.println("Time Synced");
}

bool TimeManager::isTimeValid()
{
    struct tm timeinfo;
    return getLocalTime(&timeinfo);
}

String TimeManager::getTimestamp()
{
    struct tm timeinfo;

    if (!getLocalTime(&timeinfo))
    {
        return "NO_TIME";
    }

    char buffer[40];

    sprintf(
        buffer,
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

String TimeManager::getDateTime()
{
    struct tm timeinfo;

    if (!getLocalTime(&timeinfo))
    {
        return "0000-00-00 00:00:00";
    }

    char buffer[30];

    sprintf(
        buffer,
        "%04d-%02d-%02d %02d:%02d:%02d",
        timeinfo.tm_year + 1900,
        timeinfo.tm_mon + 1,
        timeinfo.tm_mday,
        timeinfo.tm_hour,
        timeinfo.tm_min,
        timeinfo.tm_sec
    );

    return String(buffer);
}

struct tm TimeManager::getTimeInfo()
{
    struct tm timeinfo;

    if (!getLocalTime(&timeinfo))
    {
        memset(&timeinfo, 0, sizeof(timeinfo));
    }

    return timeinfo;
}