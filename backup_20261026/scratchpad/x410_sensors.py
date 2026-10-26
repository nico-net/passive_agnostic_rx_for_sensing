#!/usr/bin/env python3
"""X410 327B872 (UC_200) per-run sensor snapshot. Receiver MUST NOT be running (pgrep -x nr-uesoftmodem empty); UHD claims the device.
Usage:  python3 x410_sensors.py pre  <runtag>      # before lab_sens4_run.sh (also tunes 3450 MHz / gain 36 / 122.88 MS/s like the receiver)
        python3 x410_sensors.py post <runtag>      # right after the run (device is hot: temperature drift)
        python3 x410_sensors.py stress <N> <gap_s> # N open/read/close cycles: counts LMX2572 init failures and lock states per open
Appends one JSON line per snapshot to ~/NICOLA/captures/x410_sensors.jsonl. Read-only apart from the tune (no TX, no stream).
Exit cleanly (never SIGKILL): the object is released by `del usrp`."""
import sys, json, time, os
import uhd

ARGS = "type=x4xx,addr=192.168.20.2,mgmt_addr=192.168.20.2,master_clock_rate=245760000"
OUT = os.path.expanduser("~/NICOLA/captures/x410_sensors.jsonl")
NCH = 4

def val(s):
    try:
        return {"name": s.name, "value": s.value, "unit": getattr(s, "unit", ""), "bool": s.to_bool() if s.type == uhd.types.SensorValue.Type.BOOLEAN else None}
    except Exception:
        return {"name": s.name, "value": str(s)}

def snapshot(tag, tune=True):
    t0 = time.time()
    rec = {"tag": tag, "t": time.strftime("%Y-%m-%dT%H:%M:%S"), "open_ok": False}
    try:
        usrp = uhd.usrp.MultiUSRP(ARGS)
    except Exception as e:                       # the "Unable to communicate with LMX2572" path lands here
        rec["open_error"] = str(e)[:300]
        return rec
    rec["open_ok"] = True
    rec["open_s"] = round(time.time() - t0, 2)
    try:
        if tune:
            usrp.set_rx_rate(122.88e6, 0)
            for c in range(NCH):
                usrp.set_rx_freq(uhd.types.TuneRequest(3450e6), c)
                usrp.set_rx_gain(36.0, c)
                usrp.set_rx_antenna("RX1", c)
            time.sleep(0.5)
        rec["mboard"] = {n: val(usrp.get_mboard_sensor(n, 0)) for n in usrp.get_mboard_sensor_names(0)}
        rec["rx"] = {}
        for c in range(NCH):
            d = {}
            for n in usrp.get_rx_sensor_names(c):   # lo_locked (all LOs of the channel), per-LO <name>_locked, nco_locked, temperature*, rfdc_rate
                try:
                    d[n] = val(usrp.get_rx_sensor(n, c))
                except Exception as e:
                    d[n] = {"error": str(e)[:80]}
            rec["rx"][str(c)] = d
        rec["rx_freq_mhz"] = [usrp.get_rx_freq(c) / 1e6 for c in range(NCH)]
        rec["rx_gain_db"] = [usrp.get_rx_gain(c) for c in range(NCH)]
        rec["master_clock_mhz"] = usrp.get_master_clock_rate() / 1e6
        rec["clock_source"] = usrp.get_clock_source(0)
    except Exception as e:
        rec["read_error"] = str(e)[:300]
    del usrp                                     # releases the claim
    return rec

def emit(rec):
    with open(OUT, "a") as f:
        f.write(json.dumps(rec) + "\n")
    print(json.dumps(rec)[:2000])

if __name__ == "__main__":
    mode = sys.argv[1]
    if mode in ("pre", "post"):
        emit(snapshot(mode + ":" + (sys.argv[2] if len(sys.argv) > 2 else "")))
    elif mode == "stress":
        n, gap = int(sys.argv[2]), float(sys.argv[3])
        for i in range(n):
            emit(snapshot("stress:%d" % i))
            time.sleep(gap)
