#pragma once

// Copy to secrets.h (gitignored) and fill in. Never commit secrets.h.

// WiFi
const char *ssid = "your-wifi-ssid";
const char *password = "your-wifi-password";

// ThingSpeak
const char *writeAPIKey = "your-thingspeak-write-key";

// AdEMNEA on AWS — one key per board: register the board in the dashboard
// (IoT & Monitoring → Register Device), copy the key shown once, and assign
// the device to its hive.
const char *AWS_API_BASE = "https://erwfjmnjog.execute-api.eu-north-1.amazonaws.com";
const char *AWS_DEVICE_KEY = "paste-the-64-character-device-key-here";

// Legacy servers (used only while SEND_CSV_TO_LEGACY_SERVERS is 1, and for audio)
const char *BEARER_TOKEN = "legacy-upload-bearer-token";
const char *SWARMING_API_KEY = "legacy-swarming-api-key";
