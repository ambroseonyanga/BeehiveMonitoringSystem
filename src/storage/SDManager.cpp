#include "SDManager.h"
#include <time.h>

SDManager::SDManager(const String& hiveId)
{
    hiveNo = hiveId;
    sdReady = false;
}

bool SDManager::begin(
    uint8_t csPin,
    uint8_t sckPin,
    uint8_t misoPin,
    uint8_t mosiPin
)
{
    Serial.println();
    Serial.println("[SD CARD]");

    SPI.begin(
        sckPin,
        misoPin,
        mosiPin,
        csPin
    );

    if (!SD.begin(csPin, SPI, 4000000))
    {
        Serial.println("FAILED");
        sdReady = false;
        return false;
    }

    sdReady = true;

    Serial.println("READY");

    uint64_t cardSize =
        SD.cardSize() / (1024 * 1024);

    Serial.printf(
        "Card Size: %llu MB\n",
        cardSize
    );

    ensureCSVExists();

    return true;
}

bool SDManager::isReady() const
{
    return sdReady;
}

String SDManager::getCSVFilename() const
{
    return "/" + hiveNo + ".csv";
}

void SDManager::ensureCSVExists()
{
    if (!sdReady)
        return;

    String filename = getCSVFilename();

    if (!SD.exists(filename))
    {
        Serial.println(
            filename + " not found. Creating..."
        );

        File file =
            SD.open(
                filename,
                FILE_WRITE
            );

        if (!file)
        {
            Serial.println(
                "Failed to create CSV"
            );
            return;
        }

        file.close();

        Serial.println(
            "CSV created successfully"
        );
    }
}

void SDManager::logToCSV(
    float temperature,
    float humidity,
    uint16_t co2
)
{
    if (!sdReady)
        return;

    ensureCSVExists();

    File file =
        SD.open(
            getCSVFilename(),
            FILE_APPEND
        );

    if (!file)
    {
        Serial.println(
            "Failed to open CSV"
        );
        return;
    }

    char timestamp[25];

    struct tm timeinfo;

    if (getLocalTime(&timeinfo))
    {
        sprintf(
            timestamp,
            "%04d-%02d-%02d %02d:%02d:%02d",
            timeinfo.tm_year + 1900,
            timeinfo.tm_mon + 1,
            timeinfo.tm_mday,
            timeinfo.tm_hour,
            timeinfo.tm_min,
            timeinfo.tm_sec
        );
    }
    else
    {
        strcpy(
            timestamp,
            "0000-00-00 00:00:00"
        );
    }

    file.printf(
        "\"%s\",%.2f,%.2f,%u\n",
        timestamp,
        temperature,
        humidity,
        co2
    );

    file.close();
}

bool SDManager::fileExists(
    const String& filename
)
{
    if (!sdReady)
        return false;

    return SD.exists(filename);
}

bool SDManager::deleteFile(
    const String& filename
)
{
    if (!sdReady)
        return false;

    return SD.remove(filename);
}

File SDManager::openFile(
    const String& filename,
    const char* mode
)
{
    if (!sdReady)
        return File();

    if (strcmp(mode, "r") == 0)
    {
        return SD.open(
            filename,
            FILE_READ
        );
    }

    return SD.open(
        filename,
        FILE_WRITE
    );
}