# Gate arithmetic. debt is a LEAKY sum of (wall - owed) over ~16 occasions, so shedding can never
# latch: stop adding lateness and it decays back under the threshold.
SLOT=500_000; THRESH=2_000_000
def run(cost_ns, occ_every=4, n=20000, catchup=True):
    debt=0; prev_ns=0; prev_slot=-1; slot=0; now=0; shed=0; work=0; backlog=0
    for _ in range(n):
        slot += occ_every; owed = occ_every*SLOT
        # RF buffer: work longer than the slot period builds backlog; short work drains it.
        if catchup:
            gap = work if work>owed else max(0, owed-backlog)
            backlog = max(0, backlog + work - owed)
        else:
            gap = max(owed, work)
        now += gap
        if prev_slot >= 0:
            debt = max(0, min(debt*15//16 + (now-prev_ns) - (slot-prev_slot)*SLOT, 1_000_000_000))
        prev_ns, prev_slot = now, slot
        if debt > THRESH: shed += 1; work = 0
        else: work = cost_ns
    return shed/n, debt
for name,c in (("light",100_000),("heavy",3_200_000)):
    f,d = run(c); print("%-6s shed_frac=%.3f debt_us=%d" % (name,f,d/1000))
assert run(100_000)[0] == 0.0, "must not shed when keeping up"
f,_ = run(3_200_000); assert 0.2 < f < 0.95, f          # sheds, but not everything
f,_ = run(3_200_000, catchup=False); assert 0.2 < f < 0.98, f  # no-catchup RF model too
print("OK")
