#ifndef SD_MANAGER_H
#define SD_MANAGER_H

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>

class SDManager
{
private:
    bool sdReady;
    String hiveNo;

public:
    SDManager(const String& hiveId);

    bool begin(
        uint8_t csPin,
        uint8_t sckPin,
        uint8_t misoPin,
        uint8_t mosiPin
    );

    bool isReady() const;

    void ensureCSVExists();

    void logToCSV(
        float temperature,
        float humidity,
        uint16_t co2
    );

    bool fileExists(const String& filename);

    bool deleteFile(const String& filename);

    File openFile(
        const String& filename,
        const char* mode
    );

    String getCSVFilename() const;
};

#endif