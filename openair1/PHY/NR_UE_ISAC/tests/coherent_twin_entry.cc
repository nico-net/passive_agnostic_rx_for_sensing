// openair1/PHY/NR_UE_ISAC/tests/coherent_twin_entry.cc
// C entry point of the "twin" oracle library (coherent_core.cc rebuilt with -ffp-contract=off, see
// CMakeLists.txt), dlopen'ed by tests/coherent_cuda_parity_test.cc.
#include "coherent_core.h"

extern "C" void nr_isac_coherent_twin_detect(const std::vector<float>* E, const nr_isac::coherent::RdResult* R, const nr_isac::coherent::Grid* g,
                                              const nr_isac::coherent::Geometry* geo, const nr_isac::coherent::DetectParams* p,
                                              std::vector<nr_isac::coherent::Detection>* out)
{
  *out = nr_isac::coherent::detect(*E, *R, *g, *geo, *p);
}
