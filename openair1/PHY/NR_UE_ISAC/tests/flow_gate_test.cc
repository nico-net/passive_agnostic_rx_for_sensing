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
  std::puts("flow_gate_test: PASS");
  return 0;
}
