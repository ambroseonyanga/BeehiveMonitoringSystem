#include "BatteryMonitor.h"

BatteryMonitor::BatteryMonitor()
    : batterySensor(0x40),
      lastBatteryVoltage(0.0)
{
}

bool BatteryMonitor::begin()
{
    if (!batterySensor.begin())
    {
        Serial.println("Battery Sensor: FAILED");
        return false;
    }

    batterySensor.setCalibration_32V_2A();

    Serial.println("Battery Sensor: OK");

    return true;
}

float BatteryMonitor::readVoltage()
{
    float voltage = batterySensor.getBusVoltage_V();

    if (isnan(voltage) || voltage < 0)
    {
        return lastBatteryVoltage;
    }

    // Exponential smoothing
    if (lastBatteryVoltage > 0)
    {
        voltage =
            (0.7f * voltage) +
            (0.3f * lastBatteryVoltage);
    }

    lastBatteryVoltage = voltage;

    return voltage;
}

float BatteryMonitor::getLastVoltage() const
{
    return lastBatteryVoltage;
}