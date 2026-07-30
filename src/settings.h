// ======================================================
// SETTINGS HTML
// ======================================================

#ifndef SETTINGS_H
#define SETTINGS_H
#include <Arduino.h>

const char SETTINGS_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>

<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">

<title>Settings</title>

<style>

*{
    margin:0;
    padding:0;
    box-sizing:border-box;
}

body{
    font-family:Arial,sans-serif;
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
    margin-bottom:15px;
}

.header h1{
    font-size:26px;
}

.header p{
    margin-top:5px;
    font-size:14px;
}

.grid{
    display:grid;
    grid-template-columns:repeat(2,1fr);
    gap:12px;
}

.card{
    background:white;
    border-radius:12px;
    padding:18px;
    box-shadow:0 2px 6px rgba(0,0,0,.1);
}

.settingTitle{
    font-size:15px;
    color:#374151;
    margin-bottom:8px;
    font-weight:bold;
}

.settingIcon{
    font-size:22px;
    margin-right:8px;
}

input[type=number]{
    width:100%;
    padding:10px;
    border:1px solid #d1d5db;
    border-radius:8px;
    font-size:16px;
    outline:none;
}

input[type=number]:focus{
    border-color:#f59e0b;
}

.switchCard{
    grid-column:span 2;
}

.switchRow{
    display:flex;
    justify-content:space-between;
    align-items:center;
}

.switch{
    position:relative;
    display:inline-block;
    width:60px;
    height:34px;
}

.switch input{
    display:none;
}

.slider{
    position:absolute;
    cursor:pointer;
    inset:0;
    background:#ccc;
    transition:.3s;
    border-radius:34px;
}

.slider:before{
    position:absolute;
    content:"";
    height:26px;
    width:26px;
    left:4px;
    bottom:4px;
    background:white;
    transition:.3s;
    border-radius:50%;
}

input:checked + .slider{
    background:#10b981;
}

input:checked + .slider:before{
    transform:translateX(26px);
}

.buttons{
    margin-top:20px;
    display:flex;
    gap:10px;
    justify-content:center;
}

button{
    border:none;
    padding:12px 25px;
    border-radius:8px;
    cursor:pointer;
    font-size:15px;
}

.saveBtn{
    background:#10b981;
    color:white;
}

.backBtn{
    background:#f59e0b;
    color:white;
}

button:hover{
    opacity:.9;
}

.footer{
    text-align:center;
    margin-top:20px;
    color:#6b7280;
    font-size:13px;
}

@media(max-width:700px){

.grid{
    grid-template-columns:1fr;
}

.switchCard{
    grid-column:span 1;
}

.header h1{
    font-size:22px;
}

}

</style>

</head>

<body>

<div class="container">

<div class="header">
<h1>⚙ System Settings</h1>
<p>Configure the Smart Beehive Monitor</p>
</div>

<div class="grid">

<div class="card">
<div class="settingTitle">
📡 Sensor Interval (seconds)
</div>

<input id="sensorInterval"
type="number"
min="5">
</div>

<div class="card">
<div class="settingTitle">
☁ ThingSpeak Interval (seconds)
</div>

<input id="tsInterval"
type="number"
min="15">
</div>

<div class="card">
<div class="settingTitle">
🎤 Audio Recording Interval (seconds)
</div>

<input id="recordInterval"
type="number"
min="10">
</div>

<div class="card">
<div class="settingTitle">
📤 Upload Interval (seconds)
</div>

<input id="uploadInterval"
type="number"
min="10">
</div>

<div class="card switchCard">

<div class="switchRow">

<div>

<div style="font-weight:bold;font-size:17px;">
🔋 Full Duty Cycling
</div>

<div style="font-size:13px;color:#6b7280;margin-top:5px;">
Enable low-power operation using sleep mode.
</div>

</div>

<label class="switch">

<input
type="checkbox"
id="fullDuty">

<span class="slider"></span>

</label>

</div>

</div>

</div>

<div class="buttons">

<button
class="saveBtn"
onclick="saveSettings()">

💾 Save Settings

</button>

<a href="/">

<button
class="backBtn">

🏠 Dashboard

</button>

</a>

</div>

<div class="footer">

Smart Beehive Monitoring System

</div>

</div>

<script>

async function loadSettings()
{
    const r = await fetch('/settings');
    const s = await r.json();

    sensorInterval.value=s.sensor;
    tsInterval.value=s.ts;
    recordInterval.value=s.record;
    uploadInterval.value=s.upload;

    document.getElementById("fullDuty").checked=s.fullDuty;
}

loadSettings();

async function saveSettings()
{
    const sensor=sensorInterval.value;
    const ts=tsInterval.value;
    const record=recordInterval.value;
    const upload=uploadInterval.value;

    const fullDuty=
        document.getElementById("fullDuty").checked?1:0;

    const response=
        await fetch(
`/setDuty?sensor=${sensor}&ts=${ts}&record=${record}&upload=${upload}&fullDuty=${fullDuty}`);

    alert(await response.text());
}

</script>

</body>

</html>

)rawliteral";

#endif