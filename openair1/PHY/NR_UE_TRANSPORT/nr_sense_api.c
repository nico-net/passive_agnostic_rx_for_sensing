/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Standalone, bounded export writer. Producers only copy into preallocated slots. */
#include "common/utils/nr_core_plan.h"
#include "nr_sense_api.h"
#include "nr_sense_records.h"
/* The standalone core test links without the receiver record module. The receiver
 * links that module through its emitters and supplies the formatter. */
__attribute__((weak)) int nr_sense_rs_to_json(nr_sense_src_t, const nr_sense_rs_meta_t *, uint64_t, char *, size_t);
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>
#include "common/utils/LOG/log.h"
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
static _Atomic uint32_t write_chunk, writer_limit, written_items;
static _Atomic uint64_t after_close;
static bool opened, stopping, no_sink;
static uint8_t *frames;
static uint32_t *frame_lengths, frame_cap, frame_re, frame_ant, frame_stride, frame_head, frame_count;
static char *records;
static uint32_t *record_lengths, record_cap, record_head, record_count;
typedef struct {
  nr_sense_rs_meta_t meta;
  uint64_t frame_id;
  nr_sense_src_t src;
  char csirs_resource[256], srs_attribution[256];
} rs_job_t;
static rs_job_t *rs_jobs;
static uint8_t *record_binary;
static int cfr_fd = -1, record_fd = -1;
static pthread_t worker;
static uint64_t next_id;
static nr_sense_stats_t stats;
static _Atomic uint64_t cost_ns[4][NR_SENSE_SRC_COUNT], cost_count[4][NR_SENSE_SRC_COUNT];
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
static bool write_item(int fd, const void *buf, size_t len);

void nr_sense_cost_add(nr_sense_cost_t kind, nr_sense_src_t src, uint64_t ns)
{
  if (kind < NR_SENSE_COST_PROD || kind > NR_SENSE_COST_WRITE || src < 0 || src >= NR_SENSE_SRC_COUNT) return;
  atomic_fetch_add_explicit(&cost_ns[kind][src], ns, memory_order_relaxed);
  atomic_fetch_add_explicit(&cost_count[kind][src], 1, memory_order_relaxed);
}

/* A single writev for several adjacent ring slots; retry short writes without copying. */
static bool write_batch(int fd, struct iovec *iov, int count)
{
  if (fd < 0) return true;
  if (atomic_load_explicit(&fail_writes, memory_order_relaxed)) { errno=ENOSPC; return false; }
  if (atomic_load_explicit(&write_chunk, memory_order_relaxed)) {
    for (int i=0; i<count; i++)
      if (!write_item(fd, iov[i].iov_base, iov[i].iov_len)) return false;
    return true;
  }
  int first=0;
  while (first<count) {
    ssize_t n=writev(fd,iov+first,count-first);
    if (n<0 && errno==EINTR) continue;
    if (n<=0) return false;
    size_t consumed=(size_t)n;
    while (first<count && consumed>=iov[first].iov_len) {
      consumed-=iov[first].iov_len; first++;
    }
    if (consumed) {
      iov[first].iov_base=(uint8_t *)iov[first].iov_base+consumed;
      iov[first].iov_len-=consumed;
    }
  }
  return true;
}

static bool write_item(int fd, const void *buf, size_t len)
{
  if (fd < 0) return true;
  if (atomic_load_explicit(&fail_writes, memory_order_relaxed)) { errno=ENOSPC; return false; }
  const uint8_t *p=buf;
  while (len) {
    size_t requested=len;
    uint32_t chunk=atomic_load_explicit(&write_chunk,memory_order_relaxed);
    if (chunk && requested>chunk) requested=chunk;
    ssize_t n=write(fd,p,requested);
    if (n<0 && errno==EINTR) continue;
    if (n<=0) return false;
    p+=n; len-=(size_t)n;
  }
  return true;
}
static void *run_writer(void *arg)
{
  (void)arg;
  bool last_frame=false;
  for (;;) {
    pthread_mutex_lock(&mu);
    while (((frame_count == 0 && record_count == 0) || atomic_load(&pause_writer) ||
            (atomic_load(&writer_limit) && atomic_load(&written_items)>=atomic_load(&writer_limit))) && !stopping)
      pthread_cond_wait(&cv, &mu);
    if (stopping && frame_count == 0 && record_count == 0) { pthread_mutex_unlock(&mu); break; }
    const bool frame = frame_count != 0 && (record_count == 0 || !last_frame);
    uint32_t count=frame ? frame_count : record_count;
    /* Bound each run when both streams are ready; the test writer limit observes
     * one item from each stream, while normal operation amortises syscalls. */
    if (frame_count && record_count && count>(atomic_load(&writer_limit) ? 1u : 8u))
      count=atomic_load(&writer_limit) ? 1u : 8u;
    if (count>16) count=16;
    const uint32_t limit=atomic_load(&writer_limit);
    if (limit && !stopping && count>limit-atomic_load(&written_items)) count=limit-atomic_load(&written_items);
    pthread_mutex_unlock(&mu);
    struct iovec vec[16];
    uint32_t format_drops=0;
    for (uint32_t i=0;i<count;i++) {
      uint32_t at=frame ? (frame_head+i)%frame_cap : (record_head+i)%record_cap;
      if (!frame && record_binary[at]) {
        const uint64_t t_rec=nr_sense_now_ns();
        rs_job_t *job=&rs_jobs[at];
        char *line=records+(size_t)at*RECORD_BYTES;
        int length=nr_sense_rs_to_json(job->src,&job->meta,job->frame_id,line,RECORD_BYTES-1);
        if (length>0) { line[length]='\n'; record_lengths[at]=(uint32_t)length+1; }
        else { record_lengths[at]=0; format_drops++; }
        nr_sense_cost_add(NR_SENSE_COST_REC,job->src,nr_sense_now_ns()-t_rec);
        record_binary[at]=0;
      }
      vec[i].iov_base=frame ? (void *)(frames+(size_t)at*frame_stride)
                            : (void *)(records+(size_t)at*RECORD_BYTES);
      vec[i].iov_len=frame ? frame_lengths[at] : record_lengths[at];
    }
    const int fd=frame ? cfr_fd : record_fd;
    const uint64_t t_write=fd>=0 ? nr_sense_now_ns() : 0;
    bool ok=write_batch(fd,vec,(int)count);
    const uint64_t write_ns=t_write ? nr_sense_now_ns()-t_write : 0;
#ifdef NR_SENSE_HAVE_ZMQ
    bool zmq_failed=false;
    if (zmq_pub) for (uint32_t i=0;i<count;i++) {
      const void *payload=frame ? (const void *)(frames+(size_t)((frame_head+i)%frame_cap)*frame_stride)
                                : (const void *)(records+(size_t)((record_head+i)%record_cap)*RECORD_BYTES);
      const size_t length=frame ? frame_lengths[(frame_head+i)%frame_cap] : record_lengths[(record_head+i)%record_cap];
      zmq_failed |= zmq_send(zmq_pub, frame ? "cfr" : "rec", 3, ZMQ_SNDMORE | ZMQ_DONTWAIT) != 3 ||
                    zmq_send(zmq_pub, payload, length, ZMQ_DONTWAIT) != (int)length;
    }
#endif
    if (t_write) nr_sense_cost_add(NR_SENSE_COST_WRITE,NR_SENSE_SRC_SSB,write_ns);
    pthread_mutex_lock(&mu);
    stats.rec_dropped+=format_drops;
    if (!ok) stats.io_errors++;
#ifdef NR_SENSE_HAVE_ZMQ
    if (zmq_failed) stats.zmq_drops++;
#endif
    last_frame=frame;
    atomic_fetch_add(&written_items,count);
    if (frame) { frame_head=(frame_head+count)%frame_cap; frame_count-=count; }
    else { record_head=(record_head+count)%record_cap; record_count-=count; }
    pthread_mutex_unlock(&mu);
  }
  return NULL;
}

/* nr_sense_open: opens the sensing output channel once: validates the configuration (ring sizes, antenna and RE limits and the size arithmetic), then either sets up the in-memory sink (no file or endpoint) or allocates the CFR frame ring, the record ring and the RS job ring for the file or endpoint sink.
 *   Returns 0, or -1 when the configuration is invalid or an allocation fails, and when a channel is already open. Callers: nr_isac.cc (sensing start-up) and the SRS tests.
 */
int nr_sense_open(const nr_sense_cfg_t *cfg)
{
  pthread_once(&mu_once, init_mu);
  pthread_mutex_lock(&mu);
  const uint32_t max_ant = cfg && cfg->cfr_max_ant ? cfg->cfr_max_ant : MAX_ANT;
  if (opened || !cfg || !cfg->cfr_ring_frames || !cfg->cfr_max_re || !cfg->rec_ring_slots ||
      max_ant > MAX_ANT ||
      cfg->cfr_max_re > (SIZE_MAX - FRAME_HEADER - 3) / (3 + 8 * max_ant) ||
      cfg->cfr_ring_frames > SIZE_MAX / sizeof(uint32_t) ||
      cfg->rec_ring_slots > SIZE_MAX / RECORD_BYTES || cfg->rec_ring_slots > SIZE_MAX / sizeof(rs_job_t)) {
    pthread_mutex_unlock(&mu); return -1;
  }
  no_sink=!cfg->cfr_path && !cfg->records_path && !cfg->endpoint;
  frame_ant=max_ant;
  if (no_sink) {
    frame_cap=cfg->cfr_ring_frames; frame_re=cfg->cfr_max_re; record_cap=cfg->rec_ring_slots;
    next_id=0; memset(&stats,0,sizeof(stats));
    for (int k=0;k<4;k++) for (int s=0;s<NR_SENSE_SRC_COUNT;s++) { atomic_store(&cost_ns[k][s],0); atomic_store(&cost_count[k][s],0); }
    stopping=false; opened=true;
    atomic_store(&after_close,0); atomic_store(&closed_seen,false);
    atomic_store_explicit(&active,true,memory_order_release);
    pthread_mutex_unlock(&mu); return 0;
  }
  const size_t stride=FRAME_HEADER + 3*(size_t)cfg->cfr_max_re + 3 + 8*max_ant*(size_t)cfg->cfr_max_re;
  if (stride>UINT32_MAX || cfg->cfr_ring_frames > SIZE_MAX / stride) { pthread_mutex_unlock(&mu); return -1; }
  frame_stride=(uint32_t)stride;
  frames=malloc((size_t)cfg->cfr_ring_frames*frame_stride);
  frame_lengths=malloc((size_t)cfg->cfr_ring_frames*sizeof(*frame_lengths));
  records=malloc((size_t)cfg->rec_ring_slots*RECORD_BYTES);
  record_lengths=malloc((size_t)cfg->rec_ring_slots*sizeof(*record_lengths));
  rs_jobs=calloc(cfg->rec_ring_slots,sizeof(*rs_jobs));
  record_binary=calloc(cfg->rec_ring_slots,sizeof(*record_binary));
  if (cfg->cfr_path) cfr_fd=open(cfg->cfr_path,O_CREAT|O_WRONLY|O_TRUNC|O_CLOEXEC,0644);
  if (cfg->records_path) record_fd=open(cfg->records_path,O_CREAT|O_WRONLY|O_TRUNC|O_CLOEXEC,0644);
#ifndef NR_SENSE_HAVE_ZMQ
  static bool warned_no_zmq;
  if (cfg->endpoint && *cfg->endpoint && !warned_no_zmq && g_log) {
    warned_no_zmq=true;
    LOG_W(PHY, "Sensing endpoint ignored: built without ZMQ support\n");
  }
#endif
#ifdef NR_SENSE_HAVE_ZMQ
  if (cfg->endpoint && *cfg->endpoint) {
    zmq_context=zmq_ctx_new();
    if (zmq_context) zmq_pub=zmq_socket(zmq_context,ZMQ_PUB);
    if (zmq_pub) { int linger=0; zmq_setsockopt(zmq_pub,ZMQ_LINGER,&linger,sizeof(linger)); }
    if (!zmq_pub || zmq_bind(zmq_pub,cfg->endpoint) != 0) goto fail;
  }
#endif
  if (!frames || !frame_lengths || !records || !record_lengths || !rs_jobs || !record_binary ||
      (cfg->cfr_path && cfr_fd < 0) || (cfg->records_path && record_fd < 0)) goto fail;
  frame_cap=cfg->cfr_ring_frames; frame_re=cfg->cfr_max_re; record_cap=cfg->rec_ring_slots;
  frame_head=frame_count=record_head=record_count=0;
  next_id=0; memset(&stats,0,sizeof(stats));
  for (int k=0;k<4;k++) for (int s=0;s<NR_SENSE_SRC_COUNT;s++) { atomic_store(&cost_ns[k][s],0); atomic_store(&cost_count[k][s],0); }
  stopping=false; opened=true; atomic_store(&written_items,0);
  atomic_store(&after_close,0); atomic_store(&closed_seen,false);
  if (nr_core_plan_create(&worker,NULL,run_writer,NULL, -1) != 0) { opened=false; goto fail; }
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
  free(frames); free(frame_lengths); free(records); free(record_lengths); free(rs_jobs); free(record_binary);
  frames=NULL; frame_lengths=NULL; records=NULL; record_lengths=NULL; rs_jobs=NULL; record_binary=NULL;
  pthread_mutex_unlock(&mu);
  return -1;
}

void nr_sense_close(void)
{
  pthread_mutex_lock(&mu);
  if (!opened) { pthread_mutex_unlock(&mu); return; }
  opened=false; atomic_store_explicit(&active,false,memory_order_release);
  atomic_store(&closed_seen,true); stopping=true; pthread_cond_signal(&cv);
  if (no_sink) { no_sink=false; pthread_mutex_unlock(&mu); return; }
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
  free(frames); free(frame_lengths); free(records); free(record_lengths); free(rs_jobs); free(record_binary);
  frames=NULL; frame_lengths=NULL; records=NULL; record_lengths=NULL; rs_jobs=NULL; record_binary=NULL;
  pthread_mutex_unlock(&mu);
}
bool nr_sense_enabled(void) { return atomic_load_explicit(&active,memory_order_acquire); }

void nr_sense_reject_cfr(void)
{
  if (!nr_sense_enabled()) return;
  pthread_mutex_lock(&mu); stats.cfr_rejected++; pthread_mutex_unlock(&mu);
}

static uint64_t push_cfr(nr_sense_src_t src, int n_ant, const float *iq, int ant_stride, int n_re,
                         const void *k_abs, const void *l_sym, bool wide, uint64_t abs_slot, float slot_frac,
                         float noise_var, uint32_t epoch, uint32_t identity_gen, bool *truncated)
{
  if (truncated) *truncated=false;
  if (!nr_sense_enabled()) { if (atomic_load(&closed_seen)) atomic_fetch_add(&after_close,1); return 0; }
  if (src<0 || src>=NR_SENSE_SRC_COUNT || n_ant<1 || n_ant>frame_ant || n_re<1 || ant_stride<n_re || !iq || !k_abs || !l_sym) {
    nr_sense_reject_cfr(); return 0;
  }
  const bool cut=(uint32_t)n_re>frame_re;
  const uint32_t keep=cut ? frame_re : (uint32_t)n_re;
  pthread_mutex_lock(&mu);
  if (!opened) { pthread_mutex_unlock(&mu); atomic_fetch_add(&after_close,1); return 0; }
  if (no_sink) { stats.cfr_no_sink++; pthread_mutex_unlock(&mu); return 0; }
  if (frame_count==frame_cap) {
    if (wide) for(uint32_t i=0;i<keep;i++)
      if (((const uint32_t *)k_abs)[i]>UINT16_MAX || ((const uint32_t *)l_sym)[i]>UINT8_MAX) {
        stats.cfr_rejected++; pthread_mutex_unlock(&mu); return 0;
      }
    next_id++; stats.cfr_dropped[src]++; pthread_mutex_unlock(&mu); return 0;
  }
  const uint32_t at=(frame_head+frame_count)%frame_cap;
  const uint64_t t_pack=nr_sense_now_ns();
  uint8_t *p=frames+(size_t)at*frame_stride;
  memcpy(p,"NRCF",4); put16(p+4,NR_SENSE_CFR_VERSION); put16(p+6,FRAME_HEADER);
  p[16]=(uint8_t)src; p[17]=(uint8_t)n_ant; put16(p+18,0); put32(p+20,keep); put64(p+24,abs_slot);
  putf(p+32,slot_frac); putf(p+36,noise_var); put32(p+40,epoch); put32(p+44,identity_gen);
  size_t off=FRAME_HEADER;
  if (wide) { for(uint32_t i=0;i<keep;i++) {
    const uint32_t k=((const uint32_t *)k_abs)[i], l=((const uint32_t *)l_sym)[i];
    if (k>UINT16_MAX || l>UINT8_MAX) { stats.cfr_rejected++; pthread_mutex_unlock(&mu); return 0; }
    put16(p+off,k); off+=2;
    p[FRAME_HEADER+2*(size_t)keep+i]=(uint8_t)l;
  } }
  else {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    memcpy(p+off,k_abs,2*(size_t)keep); off+=2*(size_t)keep;
#else
    for(uint32_t i=0;i<keep;i++) { put16(p+off,((const uint16_t *)k_abs)[i]); off+=2; }
#endif
  }
  if (wide) off+=keep;
  else { memcpy(p+off,l_sym,keep); off+=keep; }
  while(off%4) p[off++]=0;
  for(int ant=0;ant<n_ant;ant++) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    memcpy(p+off,iq+2*(size_t)ant*ant_stride,8*(size_t)keep);
    off+=8*(size_t)keep;
#else
    for(uint32_t i=0;i<2*keep;i++) { putf(p+off,iq[2*(size_t)ant*ant_stride+i]); off+=4; }
#endif
  }
  const uint64_t id=++next_id;
  put64(p+8,id);
  nr_sense_cost_add(NR_SENSE_COST_PACK,src,nr_sense_now_ns()-t_pack);
  frame_lengths[at]=(uint32_t)off; frame_count++; stats.cfr_pushed++;
  if (cut) stats.truncated++;
  if (truncated) *truncated=cut;
  pthread_cond_signal(&cv); pthread_mutex_unlock(&mu);
  return id;
}
uint64_t nr_sense_push_cfr(nr_sense_src_t src, int n_ant, const float *iq, int ant_stride, int n_re,
                           const uint16_t *k_abs, const uint8_t *l_sym, uint64_t abs_slot, float slot_frac,
                           float noise_var, uint32_t epoch, uint32_t identity_gen, bool *truncated)
{
  return push_cfr(src,n_ant,iq,ant_stride,n_re,k_abs,l_sym,false,abs_slot,slot_frac,noise_var,epoch,identity_gen,truncated);
}
uint64_t nr_sense_push_cfr_u32(nr_sense_src_t src, int n_ant, const float *iq, int ant_stride, int n_re,
                               const uint32_t *k_abs, const uint32_t *l_sym, uint64_t abs_slot, float slot_frac,
                               float noise_var, uint32_t epoch, uint32_t identity_gen, bool *truncated)
{
  return push_cfr(src,n_ant,iq,ant_stride,n_re,k_abs,l_sym,true,abs_slot,slot_frac,noise_var,epoch,identity_gen,truncated);
}
bool nr_sense_push_record(const char *json_line)
{
  if (!nr_sense_enabled()) { if (atomic_load(&closed_seen)) atomic_fetch_add(&after_close,1); return false; }
  if (!json_line) { pthread_mutex_lock(&mu); stats.rec_dropped++; pthread_mutex_unlock(&mu); return false; }
  const size_t len=strnlen(json_line,RECORD_BYTES);
  pthread_mutex_lock(&mu);
  if (!opened) { pthread_mutex_unlock(&mu); atomic_fetch_add(&after_close,1); return false; }
  if (no_sink) { stats.rec_dropped++; pthread_mutex_unlock(&mu); return false; }
  if (len>=RECORD_BYTES-1 || memchr(json_line,'\n',len) || record_count==record_cap) { stats.rec_dropped++; pthread_mutex_unlock(&mu); return false; }
  const uint32_t at=(record_head+record_count)%record_cap;
  char *p=records+(size_t)at*RECORD_BYTES;
  memcpy(p,json_line,len); p[len]='\n'; record_lengths[at]=(uint32_t)len+1;
  record_binary[at]=0;
  record_count++; stats.rec_pushed++; pthread_cond_signal(&cv); pthread_mutex_unlock(&mu);
  return true;
}
int nr_sense_push_rs_binary(nr_sense_src_t src, const nr_sense_rs_meta_t *meta, uint64_t frame_id)
{
  if (!nr_sense_enabled() || !nr_sense_rs_to_json || !meta || src<0 || src>=NR_SENSE_SRC_COUNT) return -1;
  const size_t cslen=meta->csirs_resource ? strnlen(meta->csirs_resource,256) : 0;
  const size_t atlen=meta->srs_attribution ? strnlen(meta->srs_attribution,256) : 0;
  if (cslen>=256 || atlen>=256) return -1;
  pthread_mutex_lock(&mu);
  if (!opened) { pthread_mutex_unlock(&mu); return -1; }
  if (no_sink || record_count==record_cap) { stats.rec_dropped++; pthread_mutex_unlock(&mu); return 0; }
  const uint32_t at=(record_head+record_count)%record_cap;
  rs_job_t *job=&rs_jobs[at];
  job->meta=*meta; job->src=src; job->frame_id=frame_id;
  if (meta->csirs_resource) {
    memcpy(job->csirs_resource,meta->csirs_resource,cslen+1);
    job->meta.csirs_resource=job->csirs_resource;
  }
  if (meta->srs_attribution) {
    memcpy(job->srs_attribution,meta->srs_attribution,atlen+1);
    job->meta.srs_attribution=job->srs_attribution;
  }
  record_binary[at]=1;
  record_lengths[at]=0;
  record_count++; stats.rec_pushed++; pthread_cond_signal(&cv); pthread_mutex_unlock(&mu);
  return 1;
}
void nr_sense_stats(nr_sense_stats_t *out)
{
  if (!out) return;
  pthread_mutex_lock(&mu); *out=stats; pthread_mutex_unlock(&mu);
  out->after_close=atomic_load(&after_close);
  for (int s=0;s<NR_SENSE_SRC_COUNT;s++) {
    out->prod_ns[s]=atomic_load(&cost_ns[NR_SENSE_COST_PROD][s]);
    out->pack_ns[s]=atomic_load(&cost_ns[NR_SENSE_COST_PACK][s]);
    out->prod_count[s]=atomic_load(&cost_count[NR_SENSE_COST_PROD][s]);
    out->pack_count[s]=atomic_load(&cost_count[NR_SENSE_COST_PACK][s]);
    out->rec_ns+=atomic_load(&cost_ns[NR_SENSE_COST_REC][s]);
    out->write_ns+=atomic_load(&cost_ns[NR_SENSE_COST_WRITE][s]);
    out->rec_count+=atomic_load(&cost_count[NR_SENSE_COST_REC][s]);
    out->write_count+=atomic_load(&cost_count[NR_SENSE_COST_WRITE][s]);
  }
}
void nr_sense_test_pause_writer(bool pause)
{
  atomic_store(&pause_writer,pause);
  pthread_mutex_lock(&mu); pthread_cond_signal(&cv); pthread_mutex_unlock(&mu);
}
void nr_sense_test_fail_writes(bool fail) { atomic_store(&fail_writes,fail); }
void nr_sense_test_write_chunk(uint32_t bytes) { atomic_store(&write_chunk,bytes); }
void nr_sense_test_writer_limit(uint32_t items)
{
  atomic_store(&writer_limit,items);
  pthread_mutex_lock(&mu); pthread_cond_signal(&cv); pthread_mutex_unlock(&mu);
}
uint32_t nr_sense_test_written_items(void) { return atomic_load(&written_items); }
#ifdef NR_SENSE_HAVE_ZMQ
void *nr_sense_test_zmq_context(void) { return zmq_context; }
#endif
