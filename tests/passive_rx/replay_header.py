#!/usr/bin/env python3
"""P02 replay.bin header reader. stdlib only, read-only, never runs the radio.

Prints magic/version/slots/n_ul/n_dl/iq_bytes/job_bytes/fp_bytes plus the count of
DL records with tb_bytes>0 (success) vs ==0 (failure) recorded in
openair1/PHY/NR_UE_TRANSPORT/nr_passive_replay_capture.c's replay_header_t.

Layout notes (verified against the live build's own debug info, not guessed --
`gdb -batch -ex 'set max-value-size unlimited' -ex 'ptype /o replay_header_t'` on
CMakeFiles/PHY_NR_UE.dir/openair1/PHY/NR_UE_TRANSPORT/nr_passive_replay_capture.c.o,
2026-09-11):
  - Fixed 52-byte prefix (magic..n_dl) has no internal padding on x86-64 SysV.
  - `fp` (NR_DL_FRAME_PARMS) starts at an alignment-driven offset that is NOT
    hardcoded here -- it is derived from the file's own header_bytes/fp_bytes/
    job_bytes fields, which is exact regardless of that struct's alignment
    requirement in whatever build produced the file (measured 64 on this build's
    .o, a 12-byte hole after n_dl, but this script does not assume that number).
  - replay_slot_t = {long source; double fo;} -> 16 bytes (no padding, both
    8-byte, x86-64 SysV) (nr_passive_replay_capture.c:26).
  - replay_ul_t = {long source; uint64_t payload; uint16_t rnti; uint8_t length;}
    -> 24 bytes (19 bytes of members, padded to the 8-byte alignment of `long`/
    uint64_t) (nr_passive_replay_capture.c:27).
  - replay_dl_t = {nr_pdsch_passive_job_t job; uint64_t tb_hash; uint32_t
    tb_bytes;} (nr_passive_replay_capture.c:28-32) -- tb_bytes sits at
    job_bytes+8 (job_bytes is a sizeof() so it is already a multiple of the
    job struct's own alignment; tb_hash needs no extra padding before it).
  - REPLAY_FRAMES=16, REPLAY_SLOTS=16*160=2560, REPLAY_UL=2048, REPLAY_DL=512
    (nr_passive_replay_capture.c:19-22) -- compile-time array capacities, not
    stored in the file, so they are the one thing this script must hardcode.
  - magic = 0x314951525041534e (nr_passive_replay_capture.c:117).
"""
import struct
import sys

REPLAY_SLOTS = 16 * 160
REPLAY_UL = 2048
REPLAY_DL = 512
SLOT_SIZE = 16
UL_SIZE = 24
MAGIC = 0x314951525041534E

# magic, version, header_bytes, job_bytes, fp_bytes, iq_bytes, start, slots, n_ul, n_dl
PREFIX_FMT = "<QIIIIQqIII"
PREFIX_SIZE = struct.calcsize(PREFIX_FMT)  # 52


def read_header(path):
    with open(path, "rb") as f:
        prefix = f.read(PREFIX_SIZE)
        if len(prefix) != PREFIX_SIZE:
            raise ValueError(f"file too short for header prefix: {len(prefix)} < {PREFIX_SIZE}")
        (magic, version, header_bytes, job_bytes, fp_bytes, iq_bytes, start,
         slots, n_ul, n_dl) = struct.unpack(PREFIX_FMT, prefix)

        dl_size = job_bytes + 16  # tb_hash(8)+tb_bytes(4) padded to 8-byte alignment; see module docstring
        # Derive fp's offset from the file's OWN recorded sizes, not a hardcoded constant --
        # this is exact for whatever alignment padding that build's compiler inserted.
        fp_offset = header_bytes - fp_bytes - REPLAY_SLOTS * SLOT_SIZE - REPLAY_UL * UL_SIZE - REPLAY_DL * dl_size
        dl_offset = fp_offset + fp_bytes + REPLAY_SLOTS * SLOT_SIZE + REPLAY_UL * UL_SIZE

        info = {
            "magic": magic, "version": version, "header_bytes": header_bytes,
            "job_bytes": job_bytes, "fp_bytes": fp_bytes, "iq_bytes": iq_bytes,
            "start": start, "slots": slots, "n_ul": n_ul, "n_dl": n_dl,
            "fp_offset": fp_offset, "dl_offset": dl_offset, "dl_size": dl_size,
        }

        if magic != MAGIC:
            info["dl_success"] = info["dl_failure"] = None
            return info
        if fp_offset < PREFIX_SIZE or dl_offset <= 0:
            info["dl_success"] = info["dl_failure"] = None
            return info

        f.seek(dl_offset)
        n = min(n_dl, REPLAY_DL)
        success = failure = 0
        for i in range(n):
            rec = f.read(dl_size)
            if len(rec) != dl_size:
                break
            tb_bytes = struct.unpack_from("<I", rec, job_bytes + 8)[0]
            if tb_bytes > 0:
                success += 1
            else:
                failure += 1
        info["dl_success"] = success
        info["dl_failure"] = failure

        f.seek(0, 2)
        info["file_bytes"] = f.tell()
        info["expected_file_bytes"] = header_bytes + iq_bytes
    return info


def main(argv):
    if len(argv) != 1:
        print("usage: replay_header.py REPLAY.bin", file=sys.stderr)
        return 2
    info = read_header(argv[0])
    print(f"magic=0x{info['magic']:016x} (expected 0x{MAGIC:016x}, match={info['magic'] == MAGIC})")
    print(f"version={info['version']}")
    print(f"header_bytes={info['header_bytes']}")
    print(f"job_bytes={info['job_bytes']}")
    print(f"fp_bytes={info['fp_bytes']}")
    print(f"iq_bytes={info['iq_bytes']}")
    print(f"start={info['start']}")
    print(f"slots={info['slots']}")
    print(f"n_ul={info['n_ul']}")
    print(f"n_dl={info['n_dl']}")
    print(f"derived_fp_offset={info['fp_offset']}")
    print(f"derived_dl_offset={info['dl_offset']}")
    print(f"derived_dl_record_size={info['dl_size']}")
    print(f"dl_records_tb_bytes_gt0(success)={info['dl_success']}")
    print(f"dl_records_tb_bytes_eq0(failure)={info['dl_failure']}")
    if "file_bytes" in info:
        print(f"file_bytes={info['file_bytes']} expected={info['expected_file_bytes']} "
              f"match={info['file_bytes'] == info['expected_file_bytes']}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
