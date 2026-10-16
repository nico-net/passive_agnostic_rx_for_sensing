# tests/passive_rx/test_sysan_nsys.py
import sqlite3, tempfile, unittest
import sysan_nsys as sn

SCHEMA = """
CREATE TABLE StringIds(id INTEGER, value TEXT);
CREATE TABLE ThreadNames(globalTid INTEGER, nameId INTEGER, priority INTEGER);
CREATE TABLE CUPTI_ACTIVITY_KIND_RUNTIME(start INTEGER, end INTEGER, globalTid INTEGER, correlationId INTEGER, nameId INTEGER);
CREATE TABLE CUPTI_ACTIVITY_KIND_KERNEL(start INTEGER, end INTEGER, correlationId INTEGER, shortName INTEGER, streamId INTEGER, globalPid INTEGER);
INSERT INTO StringIds VALUES (1,'passivePdsch0'),(2,'k_ldpc'),(3,'cudaLaunchKernel'),(4,'k_big'),(5,'nidWorker'),(6,'cudaFree');
INSERT INTO ThreadNames VALUES (10,1,0),(11,5,0);
"""
MS = 1000000

def db(rows):
    p = tempfile.NamedTemporaryFile(suffix='.sqlite', delete=False).name
    c = sqlite3.connect(p); c.executescript(SCHEMA + rows); c.commit(); c.close(); return p

class StallTest(unittest.TestCase):
    def test_finds_stall_and_blocker(self):
        p = db(f"""INSERT INTO CUPTI_ACTIVITY_KIND_RUNTIME VALUES (0,1000,11,1,3),({10*MS},{10*MS+10000},10,2,3);
                  INSERT INTO CUPTI_ACTIVITY_KIND_KERNEL VALUES (0,{200*MS},1,4,7,1),({200*MS},{200*MS+100000},2,2,8,1);""")
        s = sn.stalls(p, gap_ms=50.0)
        self.assertEqual(len(s), 1)
        self.assertGreater(s[0]['dur_ms'], 150)
        self.assertIn('k_big', s[0]['running_kernels'])

    def test_same_stream_wait_is_not_a_stall(self):
        # the consumer's own long kernel on stream 8 delays its next kernel on stream 8: not a stall
        p = db(f"""INSERT INTO CUPTI_ACTIVITY_KIND_RUNTIME VALUES (0,1000,10,1,3),({10*MS},{10*MS+10000},10,2,3);
                  INSERT INTO CUPTI_ACTIVITY_KIND_KERNEL VALUES (0,{200*MS},1,2,8,1),({200*MS},{200*MS+100000},2,2,8,1);""")
        self.assertEqual(sn.stalls(p, gap_ms=50.0), [])

    def test_api_blocker_without_kernel(self):
        # nothing runs on the GPU, but another thread sits in cudaFree for 300 ms while the consumer kernel waits
        p = db(f"""INSERT INTO CUPTI_ACTIVITY_KIND_RUNTIME VALUES (0,{300*MS},11,9,6),({10*MS},{10*MS+10000},10,2,3);
                  INSERT INTO CUPTI_ACTIVITY_KIND_KERNEL VALUES ({300*MS},{300*MS+100000},2,2,8,1);""")
        s = sn.stalls(p, gap_ms=50.0)
        self.assertEqual(len(s), 1)
        self.assertEqual(s[0]['running_kernels'], [])
        self.assertTrue(any(api == 'cudaFree' for _, api, _ in s[0]['blocking_api']))

if __name__ == '__main__':
    unittest.main()
