#include "AudioRecorder.h"

AudioRecorder::AudioRecorder(
    int wsPin,
    int sckPin,
    int sdPin,
    int sampleRate)
{
    _wsPin = wsPin;
    _sckPin = sckPin;
    _sdPin = sdPin;

    _sampleRate = sampleRate;

    _initialized = false;
}

bool AudioRecorder::begin()
{
    Serial.println();
    Serial.println("[MICROPHONE]");

    i2s_driver_uninstall(I2S_NUM_0);

    i2s_config_t i2s_config =
    {
        .mode =
        (i2s_mode_t)
        (
            I2S_MODE_MASTER |
            I2S_MODE_RX
        ),

        .sample_rate = _sampleRate,

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
        .bck_io_num = _sckPin,
        .ws_io_num = _wsPin,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = _sdPin
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
        _initialized = false;
        return false;
    }

    err = i2s_set_pin(
        I2S_NUM_0,
        &pin_config
    );

    if (err != ESP_OK)
    {
        Serial.println("PIN FAILED");
        _initialized = false;
        return false;
    }

    i2s_zero_dma_buffer(I2S_NUM_0);

    _initialized = true;

    Serial.println("MIC READY");

    return true;
}

bool AudioRecorder::isReady()
{
    return _initialized;
}

long AudioRecorder::getPeak(
    int numSamples)
{
    if (!_initialized)
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

        if (
            err == ESP_OK &&
            bytesRead > 0
        )
        {
            int count =
            bytesRead /
            sizeof(int32_t);

            for (
                int i = 0;
                i < count &&
                samplesRead < numSamples;
                i++
            )
            {
                long level =
                abs(samples[i] >> 14);

                if (level > peak)
                    peak = level;

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

void AudioRecorder::writeWavHeader(
    File &file,
    int sampleRate,
    int totalSamples)
{
    int dataSize =
    totalSamples * 2;

    int fileSize =
    dataSize + 36;

    file.write(
        (const uint8_t*)"RIFF",
        4
    );

    file.write(
        (const uint8_t*)&fileSize,
        4
    );

    file.write(
        (const uint8_t*)"WAVE",
        4
    );

    file.write(
        (const uint8_t*)"fmt ",
        4
    );

    int subChunk1Size = 16;

    short audioFormat = 1;

    short numChannels = 1;

    int byteRate =
    sampleRate * 2;

    short blockAlign = 2;

    short bitsPerSample = 16;

    file.write(
        (const uint8_t*)&subChunk1Size,
        4
    );

    file.write(
        (const uint8_t*)&audioFormat,
        2
    );

    file.write(
        (const uint8_t*)&numChannels,
        2
    );

    file.write(
        (const uint8_t*)&sampleRate,
        4
    );

    file.write(
        (const uint8_t*)&byteRate,
        4
    );

    file.write(
        (const uint8_t*)&blockAlign,
        2
    );

    file.write(
        (const uint8_t*)&bitsPerSample,
        2
    );

    file.write(
        (const uint8_t*)"data",
        4
    );

    file.write(
        (const uint8_t*)&dataSize,
        4
    );
}

bool AudioRecorder::record(
    const String& filename,
    int durationSeconds)
{
    if (!_initialized)
        return false;

    File file =
    SD.open(
        filename.c_str(),
        FILE_WRITE
    );

    if (!file)
    {
        Serial.println(
            "FILE CREATE FAILED"
        );

        return false;
    }

    int totalSamples =
    _sampleRate *
    durationSeconds;

    writeWavHeader(
        file,
        _sampleRate,
        totalSamples
    );

    int32_t samples32[256];

    int16_t samples16[256];

    int samplesWritten = 0;

    while (
        samplesWritten <
        totalSamples
    )
    {
        size_t bytesRead;

        esp_err_t err =
        i2s_read(
            I2S_NUM_0,
            samples32,
            sizeof(samples32),
            &bytesRead,
            portMAX_DELAY
        );

        if (err != ESP_OK)
            continue;

        int count =
        bytesRead /
        sizeof(int32_t);

        for (
            int i = 0;
            i < count;
            i++
        )
        {
            int32_t sample =
            samples32[i] >> 14;

            if (sample > 32767)
                sample = 32767;

            if (sample < -32768)
                sample = -32768;

            samples16[i] =
            (int16_t)sample;
        }

        file.write(
            (uint8_t*)samples16,
            count * sizeof(int16_t)
        );

        samplesWritten += count;
    }

    file.close();

    Serial.println(
        "AUDIO SAVED"
    );

    return true;
}