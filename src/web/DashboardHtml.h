#ifndef DASHBOARD_HTML_H
#define DASHBOARD_HTML_H

#include <Arduino.h>

const char INDEX_HTML[] PROGMEM = R"rawliteral(

<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>🐝 Smart Beehive Monitor</title>

<style>

*{
    margin:0;
    padding:0;
    box-sizing:border-box;
}

body{
    font-family:Arial,sans-serif;
    background:linear-gradient(135deg,#fef3c7,#fde68a);
    padding:20px;
}

.container{
    max-width:900px;
    margin:auto;
}

header{
    background:#f59e0b;
    color:white;
    padding:20px;
    border-radius:16px;
    text-align:center;
    margin-bottom:20px;
}

.grid{
    display:grid;
    grid-template-columns:repeat(auto-fit,minmax(180px,1fr));
    gap:15px;
}

.card{
    background:white;
    border-radius:16px;
    padding:20px;
    text-align:center;
}

.value{
    font-size:30px;
    font-weight:bold;
}

.label{
    color:#666;
}

.badge{
    padding:8px 14px;
    border-radius:20px;
    color:white;
    font-size:13px;
    font-weight:bold;
}

.ok{
    background:#10b981;
}

.err{
    background:#ef4444;
}

.status-bar{
    display:flex;
    gap:10px;
    flex-wrap:wrap;
    justify-content:center;
    margin-top:20px;
}

</style>

</head>

<body>

<div class="container">

<header>
<h1>🐝 Smart Beehive Monitor</h1>
<p>Live Hive Monitoring Dashboard</p>
</header>

<div class="grid">

<div class="card">
<div class="value" id="temp">--</div>
<div class="label">Temperature (°C)</div>
</div>

<div class="card">
<div class="value" id="hum">--</div>
<div class="label">Humidity (%)</div>
</div>

<div class="card">
<div class="value" id="co2">--</div>
<div class="label">CO₂ (ppm)</div>
</div>

<div class="card">
<div class="value" id="battery">--</div>
<div class="label">Battery (V)</div>
</div>

<div class="card">
<div class="value" id="audio">--</div>
<div class="label">Audio Peak</div>
</div>

</div>

<div class="status-bar" id="statusBar"></div>

<script>

async function updateData()
{
    const response = await fetch('/data');
    const data = await response.json();

    document.getElementById("temp").innerHTML=data.temp;
    document.getElementById("hum").innerHTML=data.humidity;
    document.getElementById("co2").innerHTML=data.co2;
    document.getElementById("battery").innerHTML=data.battV;
    document.getElementById("audio").innerHTML=data.audio;
}

updateData();
setInterval(updateData,2000);

</script>

</body>
</html>

)rawliteral";

#endif