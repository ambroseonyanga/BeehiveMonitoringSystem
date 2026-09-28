# AWS uplink for the hive ESP32

Sends the hive's readings to the AdEMNEA platform on AWS. It replaces the old
`/upload` + `Authorization: Bearer` flow for sensor data.

| | Old | New |
|---|---|---|
| Server | `196.43.168.57:8005`, plain HTTP | `https://erwfjmnjog.execute-api.eu-north-1.amazonaws.com`, certificate verified |
| Auth | one shared bearer token | one key per board, sent as `X-Api-Key` |
| Payload | CSV / multipart file | one small JSON message per sensor type, plus a heartbeat |

## 1. Get a key for the board

1. In the dashboard, go to **IoT & Monitoring → Register Device** and register the board with a unique device code, e.g. `AEU-UG-012`. Set **device type** to `numeric_sensor`. Set **expected interval** (minutes) to a little more than the upload window, e.g. `2` for the default 30 s window.
2. Copy the key. **It is shown once only.**
3. Open the device and click **Assign**, then pick the hive. Data from a board with no hive is rejected.

## 2. Add the files to the sketch folder

Copy `aws_uplink.h` and `aws_root_ca.h` next to the `.ino`. Copy `aws_secrets.example.h` to `aws_secrets.h` and paste the key into it. `aws_secrets.h` is gitignored, so keep it out of any repository.

## 3. Edits to the current firmware

```cpp
// (a) with the other includes
#include "aws_uplink.h"

// (b) in BOTH normalLoop() and dutyCycleLoop(), right after logToCSV(...):
if (co2Ok)
{
    logToCSV(temp, humidity, co2);
    AwsUplink::queueReading(temp, humidity, co2);   // NEW
}

// (c) in BOTH loops, inside the upload window, after connectSTA() succeeds:
if (connectSTA())
{
    AwsUplink::flush([]() { server.handleClient(); });   // NEW: sends queued readings
    AwsUplink::sendHeartbeat(batteryVoltage);           // NEW: battery, signal, uptime
    uploadPendingFiles();                               // unchanged for now (audio/CSV)
    disconnectSTA();
}
```

Optional settings, defined **before** `#include "aws_uplink.h"`:

```cpp
#define AWS_CLIMATE_ZONE "brood_section"   // where the SCD41 sits: brood_section | honey_section | exterior
#define AWS_BATTERY_EMPTY_V 3.30f          // battery voltage at 0 %
#define AWS_BATTERY_FULL_V  4.20f          // battery voltage at 100 %
#define AWS_FIRMWARE_VERSION "hive-fw-1.0.0"
```

## How it behaves

- **Timestamps:** each reading keeps the UTC time it was **taken**, not the time it was sent. Readings taken before NTP sync are skipped, so they never get a 1970 timestamp.
- **Offline buffer:** up to 180 readings (3 h at one per minute) are held in RAM, and the buffer survives light sleep. When it's full, the oldest reading is dropped.
- **Retries:** on a network error or HTTP 429/5xx it stops, keeps the rest, and retries in the next window. Resending a partly-delivered reading is safe, because the server discards exact duplicates. HTTP 400/401/413 are dropped instead, since they would fail the same way on every retry.
- **Time limit:** one flush runs for at most 20 s and calls `server.handleClient()` between messages, so the local dashboard stays responsive.
- **Weight:** not sent, because this board has no load cell. The CSV's `2.0` is a placeholder.
- **Audio:** WAV files still go through `uploadPendingFiles()`. The AWS path for media (an upload link, direct upload to S3, then `/media/confirm`) needs the platform hosted and an S3 bucket first.

## Checking it

On the serial monitor you should see `[AWS] N readings sent, 0 pending`. In the dashboard the device turns **online**, and the hive's temperature, humidity and CO₂ appear after a page refresh. If data doesn't arrive, see the troubleshooting table in `infra/aws/README.md`, or check `iot_auth_logs` and `iot_ingestion_logs`.
