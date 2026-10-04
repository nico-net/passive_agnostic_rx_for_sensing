/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Standalone, bounded export writer. Producers only copy into preallocated slots. */
#include "nr_sense_api.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef NR_SENSE_HAVE_ZMQ
#include <zmq.h>
#endif

#define FRAME_HEADER 48u
#define RECORD_BYTES 4096u
#define MAX_ANT 8
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t mu_once = PTHREAD_ONCE_INIT;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static _Atomic bool active, closed_seen, pause_writer, fail_writes;
static _Atomic uint64_t after_close;
static bool opened, stopping;
static uint8_t *frames;
static uint32_t *frame_lengths, frame_cap, frame_re, frame_stride, frame_head, frame_count;
static char *records;
static uint32_t *record_lengths, record_cap, record_head, record_count;
static int cfr_fd = -1, record_fd = -1;
static pthread_t worker;
static uint64_t next_id;
static nr_sense_stats_t stats;
#ifdef NR_SENSE_HAVE_ZMQ
static void *zmq_context, *zmq_pub;
#endif

static void init_mu(void)
{
  pthread_mutexattr_t attr;
  if (pthread_mutexattr_init(&attr) == 0) {
    pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);
    pthread_mutex_init(&mu, &attr);
    pthread_mutexattr_destroy(&attr);
  }
}
static void put16(uint8_t *p, uint16_t x) { p[0]=x; p[1]=x>>8; }
static void put32(uint8_t *p, uint32_t x) { for(int i=0;i<4;i++) p[i]=x>>(8*i); }
static void put64(uint8_t *p, uint64_t x) { for(int i=0;i<8;i++) p[i]=x>>(8*i); }
static void putf(uint8_t *p, float x) { uint32_t bits; memcpy(&bits,&x,4); put32(p,bits); }

/* A single write per item. Short writes count as errors; there is no retry in the writer. */
static bool write_item(int fd, const void *buf, size_t len)
{
  if (fd < 0) return true;
  if (atomic_load_explicit(&fail_writes, memory_order_relaxed)) { errno=ENOSPC; return false; }
  return write(fd, buf, len) == (ssize_t)len;
}
static void *run_writer(void *arg)
{
  (void)arg;
  for (;;) {
    pthread_mutex_lock(&mu);
    while (((frame_count == 0 && record_count == 0) || atomic_load(&pause_writer)) && !stopping)
      pthread_cond_wait(&cv, &mu);
    if (stopping && frame_count == 0 && record_count == 0) { pthread_mutex_unlock(&mu); break; }
    const bool frame = frame_count != 0;
    const uint8_t *payload = frame ? frames + (size_t)frame_head * frame_stride
                                   : (uint8_t *)(records + (size_t)record_head * RECORD_BYTES);
    const uint32_t length = frame ? frame_lengths[frame_head] : record_lengths[record_head];
    pthread_mutex_unlock(&mu);
    bool ok;
    if (frame) {
      ok = write_item(cfr_fd, payload, length);
#ifdef NR_SENSE_HAVE_ZMQ
      if (zmq_pub && zmq_send(zmq_pub, "cfr", 3, ZMQ_SNDMORE | ZMQ_DONTWAIT) == 3)
        zmq_send(zmq_pub, payload, length, ZMQ_DONTWAIT);
#endif
    } else {
      /* Record slot reserves its last byte for the newline. */
      ok = write_item(record_fd, payload, length);
#ifdef NR_SENSE_HAVE_ZMQ
      if (zmq_pub && zmq_send(zmq_pub, "rec", 3, ZMQ_SNDMORE | ZMQ_DONTWAIT) == 3)
        zmq_send(zmq_pub, payload, length, ZMQ_DONTWAIT);
#endif
    }
    pthread_mutex_lock(&mu);
    if (!ok) stats.io_errors++;
    if (frame) { frame_head=(frame_head+1)%frame_cap; frame_count--; }
    else { record_head=(record_head+1)%record_cap; record_count--; }
    pthread_mutex_unlock(&mu);
  }
  return NULL;
}

int nr_sense_open(const nr_sense_cfg_t *cfg)
{
  pthread_once(&mu_once, init_mu);
  pthread_mutex_lock(&mu);
  if (opened || !cfg || !cfg->cfr_ring_frames || !cfg->cfr_max_re || !cfg->rec_ring_slots ||
      (!cfg->cfr_path && !cfg->records_path && !cfg->endpoint) ||
      cfg->cfr_max_re > (SIZE_MAX - FRAME_HEADER - 3) / (3 + 8 * MAX_ANT) ||
      cfg->cfr_ring_frames > SIZE_MAX / sizeof(uint32_t) || cfg->rec_ring_slots > SIZE_MAX / RECORD_BYTES) {
    pthread_mutex_unlock(&mu); return -1;
  }
  const size_t stride=FRAME_HEADER + 3*(size_t)cfg->cfr_max_re + 3 + 8*MAX_ANT*(size_t)cfg->cfr_max_re;
  if (stride>UINT32_MAX || cfg->cfr_ring_frames > SIZE_MAX / stride) { pthread_mutex_unlock(&mu); return -1; }
  frame_stride=(uint32_t)stride;
  frames=malloc((size_t)cfg->cfr_ring_frames*frame_stride);
  frame_lengths=malloc((size_t)cfg->cfr_ring_frames*sizeof(*frame_lengths));
  records=malloc((size_t)cfg->rec_ring_slots*RECORD_BYTES);
  record_lengths=malloc((size_t)cfg->rec_ring_slots*sizeof(*record_lengths));
  if (cfg->cfr_path) cfr_fd=open(cfg->cfr_path,O_CREAT|O_WRONLY|O_TRUNC|O_CLOEXEC,0644);
  if (cfg->records_path) record_fd=open(cfg->records_path,O_CREAT|O_WRONLY|O_TRUNC|O_CLOEXEC,0644);
#ifdef NR_SENSE_HAVE_ZMQ
  if (cfg->endpoint && *cfg->endpoint) {
    zmq_context=zmq_ctx_new();
    if (zmq_context) zmq_pub=zmq_socket(zmq_context,ZMQ_PUB);
    if (zmq_pub) { int linger=0; zmq_setsockopt(zmq_pub,ZMQ_LINGER,&linger,sizeof(linger)); }
    if (!zmq_pub || zmq_bind(zmq_pub,cfg->endpoint) != 0) goto fail;
  }
#endif
  if (!frames || !frame_lengths || !records || !record_lengths ||
      (cfg->cfr_path && cfr_fd < 0) || (cfg->records_path && record_fd < 0)) goto fail;
  frame_cap=cfg->cfr_ring_frames; frame_re=cfg->cfr_max_re; record_cap=cfg->rec_ring_slots;
  frame_head=frame_count=record_head=record_count=0;
  next_id=0; memset(&stats,0,sizeof(stats)); stopping=false; opened=true;
  atomic_store(&after_close,0); atomic_store(&closed_seen,false);
  if (pthread_create(&worker,NULL,run_writer,NULL) != 0) { opened=false; goto fail; }
  atomic_store_explicit(&active,true,memory_order_release);
  pthread_mutex_unlock(&mu);
  return 0;
fail:
#ifdef NR_SENSE_HAVE_ZMQ
  if (zmq_pub) zmq_close(zmq_pub);
  if (zmq_context) zmq_ctx_term(zmq_context);
  zmq_pub=zmq_context=NULL;
#endif
  if (cfr_fd>=0) close(cfr_fd);
  if (record_fd>=0) close(record_fd);
  cfr_fd=record_fd=-1;
  free(frames); free(frame_lengths); free(records); free(record_lengths);
  frames=NULL; frame_lengths=NULL; records=NULL; record_lengths=NULL;
  pthread_mutex_unlock(&mu);
  return -1;
}

void nr_sense_close(void)
{
  pthread_mutex_lock(&mu);
  if (!opened) { pthread_mutex_unlock(&mu); return; }
  opened=false; atomic_store_explicit(&active,false,memory_order_release);
  atomic_store(&closed_seen,true); stopping=true; pthread_cond_signal(&cv);
  pthread_mutex_unlock(&mu);
  pthread_join(worker,NULL);
  pthread_mutex_lock(&mu);
  if (cfr_fd>=0) close(cfr_fd);
  if (record_fd>=0) close(record_fd);
  cfr_fd=record_fd=-1;
#ifdef NR_SENSE_HAVE_ZMQ
  if (zmq_pub) zmq_close(zmq_pub);
  if (zmq_context) zmq_ctx_term(zmq_context);
  zmq_pub=zmq_context=NULL;
#endif
  free(frames); free(frame_lengths); free(records); free(record_lengths);
  frames=NULL; frame_lengths=NULL; records=NULL; record_lengths=NULL;
  pthread_mutex_unlock(&mu);
}
bool nr_sense_enabled(void) { return atomic_load_explicit(&active,memory_order_relaxed); }

uint64_t nr_sense_push_cfr(nr_sense_src_t src, int n_ant, const float *iq, int ant_stride, int n_re,
                           const uint16_t *k_abs, const uint8_t *l_sym, uint64_t abs_slot, float slot_frac,
                           float noise_var, uint32_t epoch, uint32_t identity_gen, bool *truncated)
{
  if (truncated) *truncated=false;
  if (!nr_sense_enabled()) { if (atomic_load(&closed_seen)) atomic_fetch_add(&after_close,1); return 0; }
  if (src<0 || src>=NR_SENSE_SRC_COUNT || n_ant<1 || n_ant>MAX_ANT || n_re<1 || ant_stride<n_re || !iq || !k_abs || !l_sym) return 0;
  pthread_mutex_lock(&mu);
  if (!opened) { pthread_mutex_unlock(&mu); atomic_fetch_add(&after_close,1); return 0; }
  if (frame_count==frame_cap) { stats.cfr_dropped[src]++; pthread_mutex_unlock(&mu); return 0; }
  const bool cut=(uint32_t)n_re>frame_re;
  const uint32_t keep=cut ? frame_re : (uint32_t)n_re;
  const uint64_t id=++next_id;
  const uint32_t at=(frame_head+frame_count)%frame_cap;
  uint8_t *p=frames+(size_t)at*frame_stride;
  memcpy(p,"NRCF",4); put16(p+4,NR_SENSE_CFR_VERSION); put16(p+6,FRAME_HEADER); put64(p+8,id);
  p[16]=(uint8_t)src; p[17]=(uint8_t)n_ant; put16(p+18,0); put32(p+20,keep); put64(p+24,abs_slot);
  putf(p+32,slot_frac); putf(p+36,noise_var); put32(p+40,epoch); put32(p+44,identity_gen);
  size_t off=FRAME_HEADER;
  for(uint32_t i=0;i<keep;i++) { put16(p+off,k_abs[i]); off+=2; }
  memcpy(p+off,l_sym,keep); off+=keep;
  while(off%4) p[off++]=0;
  for(int ant=0;ant<n_ant;ant++) for(uint32_t i=0;i<keep;i++) {
    const float *pair=iq+2*((size_t)ant*ant_stride+i);
    putf(p+off,pair[0]); putf(p+off+4,pair[1]); off+=8;
  }
  frame_lengths[at]=(uint32_t)off; frame_count++; stats.cfr_pushed++;
  if (cut) stats.truncated++;
  if (truncated) *truncated=cut;
  pthread_cond_signal(&cv); pthread_mutex_unlock(&mu);
  return id;
}
bool nr_sense_push_record(const char *json_line)
{
  if (!nr_sense_enabled()) { if (atomic_load(&closed_seen)) atomic_fetch_add(&after_close,1); return false; }
  if (!json_line) return false;
  const size_t len=strnlen(json_line,RECORD_BYTES);
  pthread_mutex_lock(&mu);
  if (!opened) { pthread_mutex_unlock(&mu); atomic_fetch_add(&after_close,1); return false; }
  if (len>=RECORD_BYTES-1 || record_count==record_cap) { stats.rec_dropped++; pthread_mutex_unlock(&mu); return false; }
  const uint32_t at=(record_head+record_count)%record_cap;
  char *p=records+(size_t)at*RECORD_BYTES;
  memcpy(p,json_line,len); p[len]='\n'; record_lengths[at]=(uint32_t)len+1;
  record_count++; stats.rec_pushed++; pthread_cond_signal(&cv); pthread_mutex_unlock(&mu);
  return true;
}
void nr_sense_stats(nr_sense_stats_t *out)
{
  if (!out) return;
  pthread_mutex_lock(&mu); *out=stats; pthread_mutex_unlock(&mu);
  out->after_close=atomic_load(&after_close);
}
void nr_sense_test_pause_writer(bool pause)
{
  atomic_store(&pause_writer,pause);
  pthread_mutex_lock(&mu); pthread_cond_signal(&cv); pthread_mutex_unlock(&mu);
}
void nr_sense_test_fail_writes(bool fail) { atomic_store(&fail_writes,fail); }
#ifdef NR_SENSE_HAVE_ZMQ
void *nr_sense_test_zmq_context(void) { return zmq_context; }
#endif
