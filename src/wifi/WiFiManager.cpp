#include "WiFiManager.h"

WiFiManager::WiFiManager(
    const char* ssid,
    const char* password,
    const char* apName,
    const char* apPassword)
{
    _ssid = ssid;
    _password = password;

    _apName = apName;
    _apPassword = apPassword;
}

void WiFiManager::begin()
{
    Serial.println();
    Serial.println("[WIFI]");

    WiFi.mode(WIFI_AP_STA);

    WiFi.begin(_ssid, _password);

    Serial.print("Connecting");

    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print(".");
    }

    Serial.println();
    Serial.println("Connected to Router");

    WiFi.softAP(
        _apName,
        _apPassword
    );

    Serial.print("Router IP: ");
    Serial.println(WiFi.localIP());

    Serial.print("Dashboard IP: ");
    Serial.println(WiFi.softAPIP());
}

void WiFiManager::checkConnection()
{
    if (WiFi.status() == WL_CONNECTED)
        return;

    Serial.println();
    Serial.println("WiFi Lost!");

    begin();
}

bool WiFiManager::connected()
{
    return WiFi.status() == WL_CONNECTED;
}

IPAddress WiFiManager::localIP()
{
    return WiFi.localIP();
}

IPAddress WiFiManager::apIP()
{
    return WiFi.softAPIP();
}