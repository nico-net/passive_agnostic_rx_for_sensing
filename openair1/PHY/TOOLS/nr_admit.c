/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_admit.h"
#include <stdio.h>

/* Admission control is off in the one configuration: everything runs, nothing is counted. */
void nr_admit_register_backlog(nr_admit_backlog_fn f) { (void)f; }
int nr_admit_on(void) { return 0; }
uint64_t nr_admit_now_ns(void) { return 0; }
int nr_admit_try(nr_admit_class_t c, uint64_t now, uint64_t deadline, int token)
{
  (void)c; (void)now; (void)deadline; (void)token;
  return 1;
}
void nr_admit_done(nr_admit_class_t c, uint64_t busy_ns) { (void)c; (void)busy_ns; }
void nr_admit_stats(char *buf, int n) { if (n > 0) buf[0] = 0; }
