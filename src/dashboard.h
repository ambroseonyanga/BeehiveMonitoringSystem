// ======================================================
// DASHBOARD HTML
// ======================================================

#ifndef DASHBOARD_H
#define DASHBOARD_H
#include <Arduino.h>

const char INDEX_HTML[] PROGMEM = R"rawliteral(

<!DOCTYPE html>

<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">

<title>Smart Beehive Monitor</title>

<style>

*{
    margin:0;
    padding:0;
    box-sizing:border-box;
}

body{
    font-family:Arial, sans-serif;
    background:#f3f4f6;
    padding:10px;
}

.container{
    max-width:900px;
    margin:auto;
}

.header{
    background:#f59e0b;
    color:white;
    text-align:center;
    padding:15px;
    border-radius:12px;
    margin-bottom:10px;
}

.header h1{
    font-size:26px;
}

.header p{
    font-size:14px;
    margin-top:5px;
}

.status-bar{
    display:flex;
    flex-wrap:wrap;
    justify-content:center;
    gap:6px;
    margin-bottom:10px;
}

.badge{
    padding:6px 10px;
    border-radius:20px;
    color:white;
    font-size:12px;
    font-weight:bold;
}

.ok{
    background:#10b981;
}

.err{
    background:#ef4444;
}

.grid{
    display:grid;
    grid-template-columns:repeat(3,1fr);
    gap:8px;
}

.card{
    background:white;
    border-radius:12px;
    padding:12px;
    text-align:center;
    box-shadow:0 2px 6px rgba(0,0,0,0.1);
}

.icon{
    font-size:24px;
}

.value{
    font-size:22px;
    font-weight:bold;
    margin-top:5px;
    color:#111827;
}

.label{
    font-size:12px;
    color:#6b7280;
    margin-top:3px;
}

.audio-card{
    grid-column:span 3;
}

.footer{
    background:white;
    margin-top:10px;
    border-radius:12px;
    padding:10px;
    text-align:center;
    font-size:14px;
    box-shadow:0 2px 6px rgba(0,0,0,0.1);
}

@media(max-width:600px)
{
    .grid{
        grid-template-columns:repeat(2,1fr);
    }

    .audio-card{
        grid-column:span 2;
    }

    .header h1{
        font-size:20px;
    }

    .value{
        font-size:18px;
    }
}

</style>

</head>

<body>

<div class="container">

<div class="header">
    <h1>🐝 Smart Beehive Monitor</h1>
    <p>Hive 12 Live Dashboard</p>
</div>

<div id="statusBar" class="status-bar"></div>

<div class="grid">

    <div class="card">
        <div class="icon">🌡️</div>
        <div id="temp" class="value">--</div>
        <div class="label">Temperature (°C)</div>
    </div>

    <div class="card">
        <div class="icon">💧</div>
        <div id="hum" class="value">--</div>
        <div class="label">Humidity (%)</div>
    </div>

    <div class="card">
        <div class="icon">💨</div>
        <div id="co2" class="value">--</div>
        <div class="label">CO₂ (ppm)</div>
    </div>

    <div class="card">
        <div class="icon">🔋</div>
        <div id="battery" class="value">--</div>
        <div class="label">Battery (V)</div>
    </div>

    <div class="card">
        <div class="icon">⚡</div>
        <div id="current" class="value">--</div>
        <div class="label">Current (A)</div>
    </div>

    <div class="card">
        <div class="icon">☀️</div>
        <div id="solar" class="value">--</div>
        <div class="label">Solar Power (W)</div>
    </div>

    <div class="card audio-card">
        <div class="icon">🎤</div>
        <div id="audio" class="value">--</div>
        <div class="label">Audio Peak</div>
    </div>

</div>
<!-- <div class="card" style="margin-top:10px">

<h3>Duty Cycle Settings</h3>

<label>Sensor Interval (sec)</label><br>
<input id="sensorInterval" type="number"><br><br>

<label>ThingSpeak Interval (sec)</label><br>
<input id="tsInterval" type="number"><br><br>

<label>Audio Record Interval (sec)</label><br>
<input id="recordInterval" type="number"><br><br>

<label>Upload Interval (sec)</label><br>
<input id="uploadInterval" type="number"><br><br>

<button onclick="saveSettings()">Save</button>

</div> -->


<div style="text-align:center;margin-top:15px">

<a href="/settingsPage">
<button
style="
padding:10px 20px;
background:#f59e0b;
color:white;
border:none;
border-radius:8px;">
⚙ Settings
</button>
</a>

</div>

<div class="card" style="margin-top:10px">
    <h3>⚠ Error Log</h3>

    <div id="errorLog"
         style="
         text-align:left;
         max-height:200px;
         overflow-y:auto;
         margin-top:10px;
         font-size:13px;">
    Loading...
    </div>
</div>

<div class="footer">
    Last Update:
    <span id="lastUpdated">--</span>
</div>


</div>

<script>
async function updateErrors()
{
    try
    {
        const response =
            await fetch('/errors');

        const errors =
            await response.json();

        const div =
            document.getElementById("errorLog");

        if(errors.length === 0)
        {
            div.innerHTML =
                "<span style='color:green'>No errors</span>";
            return;
        }

div.innerHTML =
    errors.reverse()
          .map(e =>
          {
              let color = "#ef4444";

              if(e.includes("[WARN]"))
                  color = "#f59e0b";

              if(e.includes("[INFO]"))
                  color = "#10b981";

              return `
                  <div style="
                      padding:4px;
                      border-bottom:1px solid #ddd;
                      color:${color};">
                      ${e}
                  </div>`;
          })
          .join("");
    }
    catch(e)
    {
        console.log(e);
    }
}

async function loadSettings()
{
    const r = await fetch('/settings');
    const s = await r.json();

    document.getElementById("sensorInterval").value =
        s.sensor;

    document.getElementById("tsInterval").value =
        s.ts;

    document.getElementById("recordInterval").value =
        s.record;

    document.getElementById("uploadInterval").value =
        s.upload;
}

loadSettings();

async function updateData()
{
    try
    {
        const response = await fetch('/data');
        const data = await response.json();

        document.getElementById("temp").innerHTML =
            Number(data.temp).toFixed(1);

        document.getElementById("hum").innerHTML =
            Number(data.humidity).toFixed(1);

        document.getElementById("co2").innerHTML =
            data.co2;

        document.getElementById("battery").innerHTML =
            Number(data.battV).toFixed(2);

        document.getElementById("current").innerHTML =
            Number(data.battA).toFixed(3);

        document.getElementById("solar").innerHTML =
            Number(data.solW).toFixed(2);

        document.getElementById("audio").innerHTML =
            data.audio;

        const statusBar =
            document.getElementById("statusBar");

        statusBar.innerHTML = "";

        const sensors = [
            ["CO₂", data.co2_ok],
            ["Battery", data.batt_ok],
            ["Solar", data.solar_ok],
            ["Mic", data.mic_ok],
            ["SD", data.sd_ok],
            ["Cloud", data.cloud_ok]
        ];

        sensors.forEach(sensor =>
        {
            const badge =
                document.createElement("span");

            badge.className =
                "badge " +
                (sensor[1] ? "ok" : "err");

            badge.innerHTML =
                (sensor[1] ? "✓ " : "✗ ") +
                sensor[0];

            statusBar.appendChild(badge);
        });

        document.getElementById("lastUpdated")
            .innerHTML =
            new Date().toLocaleTimeString();
    }
    catch(err)
    {
        console.log(err);
    }
}

updateData();
setInterval(updateData, 2000);
updateErrors();
setInterval(updateErrors,5000);

async function saveSettings()
{
    const sensor =
        document.getElementById("sensorInterval").value;

    const ts =
        document.getElementById("tsInterval").value;

    const record =
        document.getElementById("recordInterval").value;

    const upload =
        document.getElementById("uploadInterval").value;

    const response =
        await fetch(
            `/setDuty?sensor=${sensor}&ts=${ts}&record=${record}&upload=${upload}`
        );

    alert(await response.text());
}
</script>

</body>
</html>
)rawliteral";

#endif