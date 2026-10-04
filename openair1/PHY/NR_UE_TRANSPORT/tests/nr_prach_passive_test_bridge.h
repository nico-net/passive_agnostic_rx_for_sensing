#ifndef NR_PRACH_PASSIVE_TEST_BRIDGE_H
#define NR_PRACH_PASSIVE_TEST_BRIDGE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { int format, start_symbol, num_occasions, duration; } test_prach_occasion_t;
void test_prach_init(void);
int test_prach_occasion(int index, int frame, int slot, int mu, test_prach_occasion_t *out);
int test_prach_rar_relation(int frame, int slot, int symbol, int fdm, int preamble, uint16_t rnti, uint8_t rapid);
int test_prach_rar_age(uint64_t detection_abs, uint64_t rar_abs, int slots_per_frame);
int test_prach_ra_slot(int slot, int mu, int format);
typedef struct { uint8_t rapid; uint16_t ta; } test_prach_rar_entry_t;
int test_prach_rar_entries(const uint8_t *pdu, uint32_t len, test_prach_rar_entry_t *out, int cap);
typedef struct { int preamble, delay, energy; } test_prach_detection_t;
test_prach_detection_t test_prach_synth(int root, int shift);
void test_prach_json(int relation, char *line, uint32_t capacity);
#ifdef __cplusplus
}
#endif
#endif
