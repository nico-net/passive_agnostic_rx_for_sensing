import sys,re,json
for d in sys.argv[1:]:
    print("==",d)
    prev=None
    for l in open(f"/home/nicola/NICOLA/ota_runs/{d}/run.log",errors="ignore"):
        i=l.find('ISAC_METRICS {')
        if i<0: continue
        try: j=json.loads(l[i+13:].strip().rstrip("\x1b[0m"))
        except Exception: continue
        k=(j["t_mono_ns"]/1e9,j["pdschq_queued"],j["pdschq_decoded"],j["pdschq_drop_stale"],j["pdschq_reasons"]["queue_expired"],j["pdschq_reasons"]["copy_expired"],j["pdschq_max_lag"],j["scanq_queued"],j["scanq_drop_stale"],j["pdcch_occasions"])
        if prev:
            dt=k[0]-prev[0]
            print("dt %5.1f q/s %6.0f dec/s %6.0f stale/s %6.0f  qexp %d cexp %d maxlag %d | scanq/s %5.0f scanstale/s %5.0f occ/s %5.0f"%(dt,(k[1]-prev[1])/dt,(k[2]-prev[2])/dt,(k[3]-prev[3])/dt,k[4]-prev[4],k[5]-prev[5],k[6],(k[7]-prev[7])/dt,(k[8]-prev[8])/dt,(k[9]-prev[9])/dt))
        prev=k
