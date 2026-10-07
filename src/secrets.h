#pragma once
// Copy this file to secrets.h (next to main.cpp) and fill it in.
// Add secrets.h to .gitignore. Never commit real keys.

#define WIFI_SSID            "IoT-ra"
#define WIFI_PASS            "P9$y#F5x!b&"

#define THINGSPEAK_WRITE_KEY "HEHEZST627ICEU7E"

// Device API key from the AdEMNEA dashboard (shown once).
// Authenticates /ingest, /heartbeat, /media/confirm and /api/iot/media/upload-url.
#define AWS_API_KEY_VALUE "ZubW1ho4kGIF0uuuraTUbOmXe6xCppUtg7T3twDCYTUBtXf3nSI8UqOyJf5wd24g"

#define ADEMNEA_API_KEY  "ZubW1ho4kGIF0uuuraTUbOmXe6xCppUtg7T3twDCYTUBtXf3nSI8UqOyJf5wd24g"

// Informational only: the server resolves the device from the key.
#define DEVICE_ID_VALUE      "AEU-UG-005"


// AdEMNEA app server (NOT the AWS gateway). 127.0.0.1 only works for a
// laptop test; the ESP32 needs the real reachable host.
#define ADEMNEA_BASE_URL     "https://smarthive.iot-ra.net"
