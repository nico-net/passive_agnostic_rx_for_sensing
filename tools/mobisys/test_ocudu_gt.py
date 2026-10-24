"""Unit tests for ocudu_gt.py (OCUDU gNB ground truth -> groundtruth.py CSV). SYNTHETIC fixtures built here, in the
line formats of OCUDU lib/scheduler/logging/scheduler_result_logger.cpp and lib/pcap/mac_pcap_impl.cpp.
Run: cd tools/mobisys && python3 -m unittest test_ocudu_gt -v"""
import contextlib
import gzip
import io
import os
import shutil
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import groundtruth  # noqa: E402
import ocudu_gt  # noqa: E402

INFO_LOG = """\
2026-10-24T17:35:38.019233 [SCHED   ] [I] [     0.0] cell=0: Cell scheduling was activated.
2026-10-24T17:35:49.601036 [SCHED   ] [I] [   760.7] Slot decisions pci=2 t=8us (1 PDSCH, 0 PUSCHs, 0 PUCCHs): RAR: ra-rnti=0x10b rb=[0..5) tbs=8
2026-10-24T17:35:49.606523 [SCHED   ] [I] [  760.18] Slot decisions pci=2 t=8us (0 PDSCHs, 1 PUSCH, 0 PUCCHs): UL: ue=8192 rnti=0x4601 h_id=0 ss_id=1 rb=[8..11) newtx=true rv=0 tbs=11 msg3_delay=11
2026-10-24T17:35:49.613008 [MAC     ] [I] [  761.11] DL PDU: ue=0 rnti=0x4601 size=317: SDU: lcid=0
2026-10-24T17:39:31.736048 [SCHED   ] [I] [  445.17] Slot decisions pci=2 t=38us (2 PDSCHs, 0 PUSCHs, 0 PUCCHs): DL: ue=1 c-rnti=0x4602 h_id=9 ss_id=2 rb=[0..6) k1=12 cw[0]: newtx=true rv=0 tbs=1121 ri=4 dl_bo=0, DL: ue=0 c-rnti=0x4603 h_id=1 ss_id=2 rb=[6..8) k1=12 cw[0]: newtx=false rv=2 tbs=421
2026-10-24T17:39:31.740000 [SCHED   ] [I] [    0.8] Slot decisions pci=2 t=4us (0 PDSCHs, 1 PUSCH, 0 PUCCHs): UL: ue=1 rnti=0x4602 h_id=2 ss_id=2 rb=[3..9) newtx=true rv=0 tbs=512 k2=12
2026-10-24T17:39:31.750000 [SCHED   ] [I] [    1.0] Slot decisions pci=7 t=4us (1 PDSCH, 0 PUSCHs, 0 PUCCHs): DL: ue=0 c-rnti=0x5000 h_id=0 ss_id=2 rb=[0..6) k1=4 cw[0]: newtx=true rv=0 tbs=99 ri=2 dl_bo=0
"""

DEBUG_LOG = """\
2026-10-14T08:14:04.443126 [SCHED   ] [D] [   970.9] Processed slot events pci=1:
- UE creation: ue=0 rnti=0x4601
2026-10-14T08:14:04.443129 [SCHED   ] [D] [   970.9] Slot decisions pci=1 t=49us (1 PDSCH, 0 PUSCHs, 0 PUCCHs, 0 attempted PDCCHs, 0 attempted UCIs):
- DL PDCCH: rnti=0x4601 type=tc-rnti cs_id=0 ss_id=1 format=1_0 cce=0 al=4 dci: h_id=0 ndi=1 rv=0 mcs=0 res_ind=0 tdra_idx=0
- UE PDSCH: ue=0 c-rnti=0x4601 h_id=0 rb=[0..3) symb=[2..14) cw[0]: tbs=9 mcs=0 rv=0 nrtx=0 k1=6 dl_bo=0 olla=0 grants: lcid=62: size=6
2026-10-14T08:14:04.443144 [DU-F1   ] [I] Tx PDU du=0 tid=1 ue=0 du_ue=0: InitialULRRCMessageTransfer
2026-10-14T08:14:04.476057 [SCHED   ] [D] [  973.15] Slot decisions pci=1 t=6us (1 PDSCH, 1 PUSCH, 0 PUCCHs, 0 attempted PDCCHs, 0 attempted UCIs):
- DL PDCCH: rnti=0x4601 type=c-rnti cs_id=1 ss_id=2 format=1_1 cce=8 al=2 dci: h_id=3 ndi=0 rv=1 mcs=27 res_ind=0 tdra_idx=0 tpc=1 dai=0 ant=0
- UL PDCCH: rnti=0x4601 type=c-rnti cs_id=0 ss_id=1 format=0_0 cce=0 al=4 dci: h_id=1 ndi=1 rv=0 mcs=9 tpc=1
- UE PDSCH: ue=0 c-rnti=0x4601 h_id=3 rb=[0..20) symb=[2..14) cw[0]: tbs=4000 mcs=27 rv=1 nrtx=1 k1=7 ri=2 prg=0
- UE PUSCH: ue=0 c-rnti=0x4601 h_id=0 rb=[8..32) symb=[0..14) tbs=528 rv=0 nrtx=0 nof_layers=1 cg=No olla=0 k2=17 uci: harq_bits=1 csi-1_bits=0 csi-2_present=No
2026-10-14T08:14:04.476060 [SCHED   ] [D] [  973.16] Processed slot events pci=1:
- SR: ue=0 rnti=0x4601
"""


def mac_nr_framed(direction, rnti_type, rnti, sfn, sf, payload=b"\x3f" * 5, ueid=0, harq=0, slot=None):
    b = bytes([2, direction, rnti_type]) + b"\x02" + struct.pack(">H", rnti) + b"\x03" + struct.pack(">H", ueid)
    b += b"\x06" + bytes([harq]) + b"\x05\x00"
    b += (b"\x07" + struct.pack(">HH", sfn, slot)) if slot is not None else (b"\x04" + struct.pack(">H", (sfn << 4) | sf))
    return b + b"\x01" + payload


def exported_pdu(body, proto=b"mac-nr-framed"):
    name = proto.ljust(16, b"\0")
    return struct.pack(">HH", 12, len(name)) + name + struct.pack(">HH", 0, 0) + body


def pcap_bytes(recs, dlt=252):
    out = struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 0xFFFFFFFF, dlt)
    for i, r in enumerate(recs):
        out += struct.pack("<IIII", 1792867132, i * 500, len(r), len(r)) + r
    return out


class T(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.d)

    def w(self, name, data, gz=False):
        p = os.path.join(self.d, name)
        if gz:
            with gzip.open(p, "wt") as f:
                f.write(data)
        else:
            with open(p, "wb" if isinstance(data, bytes) else "w") as f:
                f.write(data)
        return p

    def test_info_log(self):
        rows = ocudu_gt.read_log(self.w("i.log.gz", INFO_LOG, gz=True), pci=2)
        self.assertEqual([(r["frame"], r["slot"], r["rnti"], r["dir"], r["kind"]) for r in rows],
                         [(760, 18, 0x4601, "ul", "msg3"), (445, 17, 0x4602, "dl", "pdsch"), (445, 17, 0x4603, "dl", "pdsch"),
                          (0, 8, 0x4602, "ul", "pusch")])
        self.assertEqual((rows[1]["tbs"], rows[1]["layers"], rows[1]["newtx"], rows[1]["mcs"]), (1121, 4, 1, None))
        self.assertEqual((rows[2]["newtx"], rows[2]["rv"], rows[2]["layers"]), (0, 2, None))
        self.assertEqual((rows[3]["dci_frame"], rows[3]["dci_slot"]), (1023, 16))  # PUSCH 0.8 - k2=12 wraps the SFN
        self.assertEqual(len(ocudu_gt.read_log(self.w("i2.log", INFO_LOG))), 5)  # no pci filter: cell 7 too
        common = ocudu_gt.read_log(self.w("i3.log", INFO_LOG), pci=2, include_common=True)
        self.assertEqual([(r["kind"], r["rnti"], r["slot"]) for r in common][0], ("rar", 0x10B, 7))

    def test_debug_log(self):
        rows = ocudu_gt.read_log(self.w("d.log", DEBUG_LOG))
        self.assertEqual([(r["frame"], r["slot"], r["dir"]) for r in rows], [(970, 9, "dl"), (973, 15, "dl"), (973, 15, "ul")])
        a, b, u = rows
        self.assertEqual((a["mcs"], a["dci_format"], a["cce"], a["al"], a["newtx"], a["symb"]), (0, "1_0", 0, 4, 1, "[2..14)"))
        self.assertEqual((b["mcs"], b["rv"], b["nrtx"], b["newtx"], b["layers"], b["dci_format"], b["cce"], b["al"]),
                         (27, 1, 1, 0, 2, "1_1", 8, 2))
        self.assertEqual((u["layers"], u["k2"], u["dci_frame"], u["dci_slot"], u["tbs"]), (1, 17, 972, 18, 528))

    def test_csv_feeds_groundtruth(self):
        rows = ocudu_gt.read_log(self.w("i.log", INFO_LOG), pci=2)
        p = os.path.join(self.d, "s.csv")
        ocudu_gt.write_csv(rows, p)
        g = groundtruth.read_gnb_csv(p)
        self.assertEqual([(x["frame"], x["slot"], x["rnti"], x["dir"]) for x in g],
                         [(r["frame"], r["slot"], r["rnti"], r["dir"]) for r in rows])
        dci = [{"dir": "dl", "frame": 445, "slot": 17, "rnti": 0x4602, "ext_slot": 445 * 20 + 17 + 20480}]
        grants = [{"dir": "dl", "ext_slot": dci[0]["ext_slot"], "rnti": 0x4602, "outcome": "crc_ok"}]
        res, cls = groundtruth.match(dci, grants, g)
        self.assertEqual((res["offset_slots"], res["tp_dci"], res["gnb_grants"]), (20480, 1, 2))
        self.assertEqual(sorted(c["class"] for c in cls), ["correct_tb", "missed_tb"])

    def test_pcap(self):
        recs = [exported_pdu(mac_nr_framed(1, 4, 0xFFFF, 992, 0)),           # SIB1 (SI-RNTI): skipped by default
                exported_pdu(mac_nr_framed(1, 3, 0x4601, 184, 3, harq=5)),  # DL C-RNTI, subframe 3
                exported_pdu(mac_nr_framed(0, 3, 0x4601, 184, 9)),          # UL
                exported_pdu(mac_nr_framed(1, 3, 0x4602, 185, 0, slot=1)),  # FRAME_SLOT tag: exact
                exported_pdu(b"\x00\x01", proto=b"other")]
        p = self.w("m.pcap", pcap_bytes(recs) + b"\x01\x02\x03")  # truncated tail tolerated
        st = {}
        rows = ocudu_gt.read_pcap(p, 2, stats=st)
        self.assertEqual([(r["frame"], r["slot"], r["rnti"], r["dir"], r["slot_exact"]) for r in rows],
                         [(184, 6, 0x4601, "dl", 0), (184, 18, 0x4601, "ul", 0), (185, 1, 0x4602, "dl", 1)])
        self.assertEqual((rows[0]["h_id"], rows[0]["tbs"], st["records"], st["unparsed"], st["skipped_rnti_type"]), (5, 5, 5, 1, 1))
        udp = b"\x00" * 28 + b"mac-nr" + mac_nr_framed(1, 3, 0x4603, 10, 2)  # UDP framing (mac_type udp)
        rows = ocudu_gt.read_pcap(self.w("u.pcap", pcap_bytes([udp], dlt=228)), 2)
        self.assertEqual([(r["frame"], r["slot"], r["rnti"]) for r in rows], [(10, 4, 0x4603)])
        out = os.path.join(self.d, "m.csv")
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(ocudu_gt.main(["--pcap", p, "--out", out]), 2)  # subframe-only refused by default
        self.assertFalse(os.path.exists(out))
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(ocudu_gt.main(["--pcap", p, "--out", out, "--subframe-ok"]), 0)
        self.assertEqual(len(groundtruth.read_gnb_csv(out)), 3)

    def test_no_sched_records(self):
        p = self.w("x.log", "2026-10-24T17:35:34.891899 [GNB     ] [I] Built in Release mode\n")
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(ocudu_gt.main(["--log", p, "--out", os.path.join(self.d, "x.csv")]), 2)


if __name__ == "__main__":
    unittest.main()
