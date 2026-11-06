"""NRCF v1 frame reader + record linking. Layout (nr_sense_api.c push_cfr): 48 B header '<4sHHQBBHIQffII'
(magic, ver, hlen, frame_id, src, n_ant, pad, keep, abs_slot, slot_frac, noise_var, epoch, identity_gen), k u16[keep], l u8[keep],
pad to 4, then n_ant x (keep x complex64 interleaved re,im)."""
import struct, json, numpy as np
H = struct.Struct('<4sHHQBBHIQffII')
def frames(path):
    """yield dict(fid, src, n_ant, slot, nv, k, l, h[n_ant, keep] complex64) sequentially (mmap)."""
    mm = np.memmap(path, dtype=np.uint8, mode='r'); n = len(mm); off = 0
    while off + 48 <= n:
        magic, ver, hl, fid, src, na, _, keep, slot, fr, nv, ep, ig = H.unpack(bytes(mm[off:off+48]))
        if magic != b'NRCF': break
        o = off + hl; k = np.frombuffer(mm[o:o+2*keep], np.uint16); l = np.frombuffer(mm[o+2*keep:o+3*keep], np.uint8)
        o = off + ((hl + 3*keep + 3) & ~3); end = o + 8*na*keep
        if end > n: break
        h = np.frombuffer(mm[o:end], np.complex64).reshape(na, keep)
        yield dict(fid=fid, src=src, n_ant=na, slot=slot, nv=nv, k=k, l=l, h=h); off = end
def records(path, types=('pdsch_dmrs', 'csirs')):
    out = {}
    for ln in open(path):
        if '"type":"rs"' in ln and any('"source":"%s"' % t in ln for t in types):
            r = json.loads(ln)
            if r.get('cfr_frame_id'): out[r['cfr_frame_id']] = r
    return out
