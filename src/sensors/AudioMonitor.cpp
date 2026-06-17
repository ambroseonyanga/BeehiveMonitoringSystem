#include "AudioMonitor.h"

AudioMonitor::AudioMonitor(
    int ws,
    int sck,
    int sd,
    int sampleRateValue)
{
    wsPin = ws;
    sckPin = sck;
    sdPin = sd;
    sampleRate = sampleRateValue;

    mic_ok = false;
}

bool AudioMonitor::begin()
{
    Serial.println();
    Serial.println("[MICROPHONE]");

    i2s_driver_uninstall(I2S_NUM_0);

    i2s_config_t i2s_config =
    {
        .mode =
            (i2s_mode_t)(
                I2S_MODE_MASTER |
                I2S_MODE_RX
            ),

        .sample_rate = sampleRate,

        .bits_per_sample =
            I2S_BITS_PER_SAMPLE_32BIT,

        .channel_format =
            I2S_CHANNEL_FMT_ONLY_RIGHT,

        .communication_format =
            I2S_COMM_FORMAT_STAND_I2S,

        .intr_alloc_flags =
            ESP_INTR_FLAG_LEVEL1,

        .dma_buf_count = 8,
        .dma_buf_len = 64,

        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0
    };

    i2s_pin_config_t pin_config =
    {
        .bck_io_num = sckPin,
        .ws_io_num = wsPin,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = sdPin
    };

    esp_err_t err;

    err = i2s_driver_install(
        I2S_NUM_0,
        &i2s_config,
        0,
        NULL
    );

    if (err != ESP_OK)
    {
        Serial.println("MIC FAILED");
        mic_ok = false;
        return false;
    }

    err = i2s_set_pin(
        I2S_NUM_0,
        &pin_config
    );

    if (err != ESP_OK)
    {
        Serial.println("PIN FAILED");
        mic_ok = false;
        return false;
    }

    i2s_zero_dma_buffer(I2S_NUM_0);

    mic_ok = true;

    Serial.println("MIC READY");

    return true;
}

bool AudioMonitor::isReady()
{
    return mic_ok;
}

long AudioMonitor::readPeak(
    int numSamples)
{
    if (!mic_ok)
        return 0;

    int32_t samples[64];
    size_t bytesRead = 0;

    long peak = 0;
    int samplesRead = 0;

    while (samplesRead < numSamples)
    {
        esp_err_t err =
            i2s_read(
                I2S_NUM_0,
                samples,
                sizeof(samples),
                &bytesRead,
                pdMS_TO_TICKS(100)
            );

        if (err == ESP_OK &&
            bytesRead > 0)
        {
            int count =
                bytesRead /
                sizeof(int32_t);

            for (
                int i = 0;
                i < count &&
                samplesRead < numSamples;
                i++)
            {
                long level =
                    abs(samples[i] >> 14);

                if (level > peak)
                {
                    peak = level;
                }

                samplesRead++;
            }
        }
        else
        {
            break;
        }
    }

    return peak;
}