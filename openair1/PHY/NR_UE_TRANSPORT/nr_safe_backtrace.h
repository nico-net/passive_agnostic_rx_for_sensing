/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* backtrace() for asynchronous signal handlers (SLOWPASS_BT) that survives a bad unwind.
 * Why: functions with scalable (SVE) stack slots (GCC -mcpu=native on Grace, e.g. nr_polar_dci_decode_fssc) describe their CFA with a
 * VG-based DWARF expression; unwinding through such a frame from a signal handler reads stack data as the return address and libgcc
 * faults on it (SIGSEGV at a non-canonical, int16-pattern address), killing the process. Reproduced by test_nr_polar_fssc.
 * Use: the signal handler calls nr_safe_backtrace(); the SIGSEGV/SIGBUS handler of the SAME translation unit calls
 * nr_safe_backtrace_on_fault() first, which jumps back out of the failed unwind (the TLS anchor is per translation unit). */
#ifndef NR_SAFE_BACKTRACE_H
#define NR_SAFE_BACKTRACE_H
#include <execinfo.h>
#include <setjmp.h>
#include <stddef.h>

static __thread sigjmp_buf *nr_sbt_jmp;

/* frames written to fr[], or -1 when the unwind faulted (nothing usable in fr[]) */
static inline int nr_safe_backtrace(void **fr, int max)
{
  sigjmp_buf jb;
  volatile int n = -1;
  if (sigsetjmp(jb, 1) == 0) {
    nr_sbt_jmp = &jb;
    n = backtrace(fr, max);
  }
  nr_sbt_jmp = NULL;
  return n;
}

/* returns only when the fault did not happen inside nr_safe_backtrace() on this thread */
static inline void nr_safe_backtrace_on_fault(void)
{
  sigjmp_buf *j = nr_sbt_jmp;
  if (j) {
    nr_sbt_jmp = NULL;
    siglongjmp(*j, 1);
  }
}
#endif
