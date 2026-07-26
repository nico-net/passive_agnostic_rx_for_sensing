#!/usr/bin/env python3
"""World-frame scorer for fused multi-static tracks (isac-track output).
Credits a confirmed track update to a real target if within TOL_M of that target's true (x,y) path.
Time-alignment-free (searches the whole path), mirroring score_run.py's philosophy.
Usage: score_world_tracks.py <tracks.jsonl> [tol_m=15]
Target world paths are the mot2 scene endpoints; edit if the scene changes."""
import sys, json, numpy as np, collections
tracks=sys.argv[1]; TOL=float(sys.argv[2]) if len(sys.argv)>2 else 15.0
A=[(35,55),(115,235)]; B=[(70,120),(10,280)]
def path(p0,p1,n=2000):
    t=np.linspace(0,1,n); return np.stack([p0[0]+(p1[0]-p0[0])*t,p0[1]+(p1[1]-p0[1])*t],1)
PA,PB=path(*A),path(*B)
def md(x,y):
    da=np.min(np.hypot(PA[:,0]-x,PA[:,1]-y)); db=np.min(np.hypot(PB[:,0]-x,PB[:,1]-y))
    return (da,'A') if da<db else (db,'B')
conf=[]; ids=set()
for line in open(tracks):
    t=json.loads(line)
    if t['status']!='confirmed': continue
    d,w=md(t['x'],t['y']); conf.append((d,w)); ids.add(t['id'])
n=len(conf); hit=sum(1 for d,_ in conf if d<=TOL)
print(f"confirmed updates: {n}  within {TOL}m: {hit} ({100*hit//max(n,1)}% precision)  distinct ids: {len(ids)}")
if hit:
    ds=np.array([d for d,_ in conf]); cov=collections.Counter(w for d,w in conf if d<=TOL)
    print(f"median world err: {np.median(ds):.1f} m (matched {np.median(ds[ds<=TOL]):.1f} m)  per-target: A={cov['A']} B={cov['B']}")
