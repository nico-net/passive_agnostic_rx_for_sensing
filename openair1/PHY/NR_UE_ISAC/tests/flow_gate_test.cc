// openair1/PHY/NR_UE_ISAC/tests/flow_gate_test.cc
#include "flow_gate.h"
#include <cstdio>
#include <stdexcept>
static void require(bool c, const char* m) { if (!c) throw std::runtime_error(m); }
int main() {
  nr_isac::FlowGate g(2.0);
  require(!g.open(), "starts closed");
  require(g.note(0x4601, false, 10.0) == 0, "DL alone does not open");
  require(!g.admit(0x4601, 10.0), "DL-only RNTI not admitted");
  require(g.note(0x4601, true, 10.5) == 1, "DL+UL opens");
  require(g.admit(0x4601, 10.6), "flow admitted");
  require(!g.admit(0x4602, 10.6), "unknown RNTI not admitted");
  require(g.note(0x4602, false, 11.0) == 0, "second RNTI DL-only: gate already open");
  require(!g.admit(0x4602, 11.0), "second RNTI is not a flow yet");
  require(g.poll(11.9) == 0, "DL 1.9 s old: still open");
  require(g.poll(12.1) == -1, "DL 2.1 s old: closes");
  require(!g.open() && !g.admit(0x4601, 12.1), "closed admits nothing");
  require(g.poll(13.0) == 0, "close reported once");
  require(g.note(0x4601, false, 20.0) == 0 && g.note(0x4601, true, 20.1) == 1, "re-opens");

  // Fix round 1 (P20, Critical): a fast DL-only stream must never let note() itself close the
  // gate. Seed a flow, then note() DL only every 1 ms for 4 s (UL never returns) while polling
  // every 100 ms: exactly one poll() must report the close, no note() may ever report non-zero
  // again, and admit() must read false afterward.
  nr_isac::FlowGate g2(2.0);
  require(g2.note(0x9001, false, 0.0) == 0, "seed DL");
  require(g2.note(0x9001, true, 0.0) == 1, "seed UL: opens");
  int closes = 0;
  for (int ms = 1; ms <= 4000; ++ms) {
    const double t = ms * 0.001;
    require(g2.note(0x9001, false, t) == 0, "note() never itself reports a close");
    if (ms % 100 == 0 && g2.poll(t) == -1) ++closes;
  }
  require(closes == 1, "exactly one poll() closed the gate");
  require(!g2.admit(0x9001, 4.0), "admit false after close, UL never returned");

  std::puts("flow_gate_test: PASS");
  return 0;
}
