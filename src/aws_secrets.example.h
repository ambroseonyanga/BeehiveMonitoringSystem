#pragma once

// Copy this file to aws_secrets.h (which is gitignored) and fill in the key.
// Each board gets its OWN key: register the ESP32 in the AdEMNEA dashboard
// (IoT & Monitoring → Register Device), copy the key shown once, and assign
// the device to its hive. Never reuse one key on several boards.

#define AWS_API_BASE   "https://erwfjmnjog.execute-api.eu-north-1.amazonaws.com"
// #define AWS_DEVICE_KEY "paste-the-64-character-device-key-here"
