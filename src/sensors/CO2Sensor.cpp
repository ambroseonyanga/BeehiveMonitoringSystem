#include "CO2Sensor.h"

CO2Sensor::CO2Sensor()
{
    lastCO2 = 400;
    lastTemp = 25.0f;
    lastHumidity = 50.0f;
}

bool CO2Sensor::begin(TwoWire &wire)
{
    scd4x.begin(wire, 0x62);

    delay(2000);

    uint64_t serialNumber;

    if (scd4x.getSerialNumber(serialNumber) != 0)
    {
        Serial.println("CO2 Sensor: FAILED");
        return false;
    }

    if (scd4x.startPeriodicMeasurement() != 0)
    {
        Serial.println("CO2 Sensor: FAILED");
        return false;
    }

    Serial.println("CO2 Sensor: OK");

    delay(500);

    return true;
}

bool CO2Sensor::read(uint16_t &co2,
                     float &temperature,
                     float &humidity)
{
    uint16_t error;

    uint16_t rawCO2 = 0;
    float rawTemp = 0;
    float rawHumidity = 0;

    error =
        scd4x.readMeasurement(
            rawCO2,
            rawTemp,
            rawHumidity
        );

    if (error != 0 || rawCO2 == 0)
    {
        co2 = lastCO2;
        temperature = lastTemp;
        humidity = lastHumidity;

        return false;
    }

    co2 =
        (uint16_t)(
            0.7f * rawCO2 +
            0.3f * lastCO2
        );

    temperature =
        0.7f * rawTemp +
        0.3f * lastTemp;

    humidity =
        0.7f * rawHumidity +
        0.3f * lastHumidity;

    lastCO2 = co2;
    lastTemp = temperature;
    lastHumidity = humidity;

    return true;
}

uint16_t CO2Sensor::getLastCO2() const
{
    return lastCO2;
}

float CO2Sensor::getLastTemperature() const
{
    return lastTemp;
}

float CO2Sensor::getLastHumidity() const
{
    return lastHumidity;
}