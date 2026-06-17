#ifndef BATTERY_MONITOR_H
#define BATTERY_MONITOR_H

#include <Arduino.h>
#include <Adafruit_INA219.h>

class BatteryMonitor
{
private:
    Adafruit_INA219 batterySensor;
    float lastBatteryVoltage;

public:
    BatteryMonitor();

    bool begin();

    float readVoltage();

    float getLastVoltage() const;
};

#endif