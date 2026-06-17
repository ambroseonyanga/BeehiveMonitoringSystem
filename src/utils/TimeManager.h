#pragma once

#include <Arduino.h>
#include <time.h>

class TimeManager
{
private:
    const char* _ntpServer;
    long _gmtOffset;
    int _daylightOffset;

public:
    TimeManager();
    TimeManager(
        const char* ntpServer,
        long gmtOffset,
        int daylightOffset
    );

    void begin();

    bool isTimeValid();

    String getTimestamp();

    String getDateTime();

    struct tm getTimeInfo();
};