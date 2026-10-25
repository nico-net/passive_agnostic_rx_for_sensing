import os, sys, tempfile, unittest
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import parse, ul_groundtruth as u


class T(unittest.TestCase):
    def test_match_with_offset_and_fp(self):
        un = parse.SfnUnwrap(20)
        g = [{"frame": 10, "slot": s, "rnti": 0x4601} for s in (2, 4, 12)] + [{"frame": 10, "slot": 8, "rnti": 0x4602}]
        rx = []
        for x in g[:3]:  # same SFN, receiver hyperframe one wrap later (+20480 slots); misses 0x4602, one false accept
            rx.append({"dir": "ul", "frame": x["frame"], "slot": x["slot"], "rnti": x["rnti"]})
        rx.append({"dir": "ul", "frame": 10, "slot": 6, "rnti": 0x4601})
        rx.append({"dir": "dl", "frame": 10, "slot": 3, "rnti": 0x4601})
        for r in rx:
            r["ext_slot"] = 20480 + r["frame"] * 20 + r["slot"]
        res = u.match_ul(rx, g)
        self.assertEqual(res["offset_slots"], 20480)
        self.assertEqual((res["tp"], res["fp"], res["fn"]), (3, 1, 1))
        self.assertEqual(res["per_rnti"]["0x4601"]["recall"], 1.0)
        self.assertEqual(res["per_rnti"]["0x4602"]["recall"], 0.0)

    def test_csv_dci_slot_and_crc_log(self):
        d = tempfile.mkdtemp()
        p = os.path.join(d, "s.csv")
        with open(p, "w") as f:
            f.write("frame,slot,rnti,dir,dci_frame,dci_slot,mcs\n5,8,0x4601,ul,5,4,9\n5,9,0x4601,ul,,,\n5,2,0x4601,dl,,,\n")
        rows = u.read_gnb_ul(p)
        self.assertEqual([(r["frame"], r["slot"]) for r in rows], [(5, 4)])
        self.assertEqual(len(u.read_gnb_ul(p, pusch_slot=True)), 2)
        lg = os.path.join(d, "rx.log")
        with open(lg, "w") as f:
            f.write("SENSING: UL_PUSCH_CRC rnti=0x4601 ok=1 mcs=9 prb=0+106 probe=0 snr_db=20.0\n"
                    "SENSING: UL_PUSCH_CRC rnti=0x4601 ok=0 mcs=9 prb=0+106 probe=0 snr_db=20.0\n"
                    "SENSING: UL_PUSCH_CRC rnti=0x4601 ok=1 mcs=9 prb=0+6 probe=1 snr_db=20.0\n")
        c = u.rx_ul_crc(lg)["0x4601"]
        self.assertEqual((c["pusch_real"], c["crc_ok"], c["probe"], c["crc_pct"]), (2, 1, 1, 50.0))


if __name__ == "__main__":
    unittest.main()
