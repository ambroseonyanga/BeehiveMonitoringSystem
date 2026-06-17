#include "ThingSpeakManager.h"

ThingSpeakManager::ThingSpeakManager(
    unsigned long channel,
    const char* apiKey)
{
    channelNumber = channel;
    writeAPIKey = apiKey;
}

void ThingSpeakManager::begin(
    WiFiClient& client)
{
    ThingSpeak.begin(client);
}

bool ThingSpeakManager::sendData(
    float temp,
    float humidity,
    uint16_t co2,
    float batteryVoltage,
    long audioPeak)
{
    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println(
            "⚠️ WiFi not connected, cannot send to ThingSpeak");
        return false;
    }

    Serial.println();
    Serial.println("[THINGSPEAK]");

    ThingSpeak.setField(1, temp);
    ThingSpeak.setField(2, humidity);
    ThingSpeak.setField(3, co2);
    ThingSpeak.setField(4, batteryVoltage);
    ThingSpeak.setField(5, audioPeak);

    int httpCode =
        ThingSpeak.writeFields(
            channelNumber,
            writeAPIKey);

    if (httpCode == 200)
    {
        Serial.println(
            "✅ Data sent to ThingSpeak successfully!");

        Serial.printf(
            "   Field 1 (Temp): %.1f°C\n",
            temp);

        Serial.printf(
            "   Field 2 (Humidity): %.1f%%\n",
            humidity);

        Serial.printf(
            "   Field 3 (CO2): %u ppm\n",
            co2);

        Serial.printf(
            "   Field 4 (Battery): %.2f V\n",
            batteryVoltage);

        Serial.printf(
            "   Field 5 (Audio Peak): %ld\n",
            audioPeak);

        return true;
    }

    Serial.printf(
        "❌ ThingSpeak error: HTTP code %d\n",
        httpCode);

    return false;
}