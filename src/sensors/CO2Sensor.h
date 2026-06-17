#ifndef CO2_SENSOR_H
#define CO2_SENSOR_H

#include <Arduino.h>
#include <Wire.h>
#include <SensirionI2cScd4x.h>

class CO2Sensor
{
private:
    SensirionI2cScd4x scd4x;

    uint16_t lastCO2;
    float lastTemp;
    float lastHumidity;

public:
    CO2Sensor();

    bool begin(TwoWire &wire = Wire);

    bool read(uint16_t &co2,
              float &temperature,
              float &humidity);

    uint16_t getLastCO2() const;

    float getLastTemperature() const;

    float getLastHumidity() const;
};

#endif