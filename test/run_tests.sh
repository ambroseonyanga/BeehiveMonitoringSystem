#!/bin/sh
# Host tests: pure-logic unit tests + firmware delivery simulation. Needs g++ and python3.
set -e
cd "$(dirname "$0")"
echo "### unit tests (hive_util.h)"
g++ -std=c++17 -Wall -Wextra -I.. test_util.cpp -o /tmp/test_util && /tmp/test_util
echo; echo "### firmware simulation (main.cpp against fake SD + fake gateway)"
g++ -std=gnu++17 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-unused-function \
    -Istubs -I.. host_sim.cpp -o /tmp/host_sim && /tmp/host_sim
echo; echo "### validating every payload the firmware produced"
python3 - <<'PY'
import json, re
env = {"device_id","sensor_type","recorded_at","reading"}
hb  = {"battery_level","signal_strength","uptime","firmware_version","cpu_usage","storage_usage","reboot_count","sensor_read_success_rate","error_codes"}
med = {"device_id","media_type","s3_object_key","captured_at","file_size_bytes"}
iso = re.compile(r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$")
n = {"ingest":0,"heartbeat":0,"media":0}
for line in open("/tmp/bodies.jsonl"):
    if not line.strip(): continue
    d = json.loads(line)                       # raises if the JSON is malformed
    k = set(d)
    if k == env:
        assert iso.match(d["recorded_at"]), d
        t = d["sensor_type"]; r = d["reading"]
        if t in ("temperature","humidity"): assert set(r) <= {"exterior","brood_section","honey_section"} and r, d
        elif t == "co2": assert set(r) == {"co2_level"}, d
        else: raise AssertionError(d)
        n["ingest"] += 1
    elif k == hb: assert isinstance(d["error_codes"], list); n["heartbeat"] += 1
    elif k == med: assert iso.match(d["captured_at"]) and d["media_type"] in ("photo","audio","video"); n["media"] += 1
    elif {"media_type","content_type"} == k: pass
    else: raise AssertionError("unexpected payload shape: %s" % d)
print("all payloads are valid JSON and match the documented schemas:", n)
PY
