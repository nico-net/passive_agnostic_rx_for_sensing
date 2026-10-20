# tests/passive_rx/test_dl_crc_split.py
import tempfile, unittest
import dl_crc_split as dcs

def ev(crc=1, mcs=10, tbl=0, nl=1, snr_l="20.0,-99.0,-99.0,-99.0", probe=0, rv=0, tot=400):
    return (f"[PHY]    SENSING: EVMGRANT rnti=0x1 1.2 crc={crc} mcs={mcs} tbl={tbl} Qm=2 Nl={nl} rb=0+5 nsym=12 "
            f"evm=1 snr=20.0 probe={probe} rv={rv} ndi=0 pid=0 tbs=96 snr_l={snr_l} l2=1 zabs=0 "
            f"e2e=100,1,1,1,1,1,{tot},1 stg=fep:1/1/1 cb=1\n")

def write(lines):
    f = tempfile.NamedTemporaryFile('w', suffix='.log', delete=False)
    f.writelines(lines); f.close(); return f.name

class DlCrcSplitTest(unittest.TestCase):
    def test_split_by_layers(self):
        p = write([ev(nl=1, snr_l="30,-99,-99,-99"), ev(nl=2, crc=0, snr_l="30,30,-99,-99"),
                   ev(nl=2, crc=1, snr_l="30,30,-99,-99")])
        s = dcs.split([p])
        self.assertEqual(s['Nl']['1'], [1, 1])
        self.assertEqual(s['Nl']['2'], [2, 1])
        self.assertEqual(s['cb_bucket']['1'], [3, 2])

if __name__ == '__main__':
    unittest.main()
