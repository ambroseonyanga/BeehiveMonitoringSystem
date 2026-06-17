#pragma once

#include <Arduino.h>
#include <driver/i2s.h>
#include <SD.h>

class AudioRecorder
{
private:
    int _wsPin;
    int _sckPin;
    int _sdPin;

    int _sampleRate;

    bool _initialized;

    void writeWavHeader(
        File &file,
        int sampleRate,
        int totalSamples
    );

public:
    AudioRecorder(
        int wsPin,
        int sckPin,
        int sdPin,
        int sampleRate
    );

    bool begin();

    bool isReady();

    long getPeak(
        int numSamples = 100
    );

    bool record(
        const String& filename,
        int durationSeconds
    );
};