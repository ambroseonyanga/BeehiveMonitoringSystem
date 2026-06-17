#ifndef AUDIO_MONITOR_H
#define AUDIO_MONITOR_H

#include <Arduino.h>
#include <driver/i2s.h>

class AudioMonitor
{
private:
    bool mic_ok;

    int wsPin;
    int sckPin;
    int sdPin;
    int sampleRate;

public:
    AudioMonitor(
        int ws,
        int sck,
        int sd,
        int sampleRate
    );

    bool begin();

    bool isReady();

    long readPeak(
        int numSamples = 100
    );
};

#endif