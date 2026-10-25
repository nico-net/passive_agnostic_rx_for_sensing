/* C side of test_nr_cell: NR_CELL_STATIC_INIT needs C (GNU range designators). */
#include "nr_cell.h"

NR_CELL_STATIC_INIT(int, t_counter, 3)
#define t_counter NR_CELL_REF(t_counter)
typedef struct { int a, b; } pair_t;
NR_CELL_STATIC_INIT(pair_t, t_pair, {7, 9})
#define t_pair NR_CELL_REF(t_pair)
NR_CELL_STATIC(long, t_zero)
#define t_zero NR_CELL_REF(t_zero)
typedef struct { char blob[4096]; int n; } big_t;
NR_CELL_STATIC_HEAP(big_t, t_big)
#define t_big NR_CELL_HEAP_REF(big_t, t_big)

int t_counter_get(void) { return t_counter; }
void t_counter_add(int v) { t_counter += v; }
int *t_counter_addr(void) { return &t_counter; }
int t_pair_a(void) { return t_pair.a; }
int t_pair_b(void) { return t_pair.b; }
long t_zero_get(void) { return t_zero; }
int t_big_n(void) { return t_big.n; }
void t_big_set(int n) { t_big.n = n; t_big.blob[4095] = 1; }
void t_big_prealloc(int c) { (void)NR_CELL_HEAP_PREALLOC(big_t, t_big, c); }
