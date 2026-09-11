/* Immutable received-sample evidence, deliberately separate from discovery.
 * Opt-in bounded capture; no gNB input, RF replay, or background file I/O on
 * the receive thread. Binary format is local same-build ABI, not an interchange
 * format. A live CRC and identical decoded bytes are the replay acceptance test.
 */
#include "nr_passive_replay_capture.h"
#include "PHY/MODULATION/modulation_UE.h"
#include "common/utils/LOG/log.h"
#include <stdatomic.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <math.h>

#define REPLAY_FRAMES 16
#define REPLAY_SLOTS (REPLAY_FRAMES * 160)
#define REPLAY_UL 2048
#define REPLAY_DL 512
#define REPLAY_MAX_BYTES (512u * 1024u * 1024u)
enum { RP_DISABLED, RP_ARMED, RP_REQUESTED, RP_CAPTURING, RP_DRAINING, RP_WRITING, RP_DONE, RP_VOID };
typedef struct { long source; double fo; } replay_slot_t;
typedef struct { long source; uint64_t payload; uint16_t rnti; uint8_t length; } replay_ul_t;
typedef struct {
  nr_pdsch_passive_job_t job;
  uint64_t tb_hash;
  uint32_t tb_bytes;
} replay_dl_t;
typedef struct {
  uint64_t magic;
  uint32_t version, header_bytes, job_bytes, fp_bytes;
  uint64_t iq_bytes;
  long start;
  unsigned slots, n_ul, n_dl;
  NR_DL_FRAME_PARMS fp;
  replay_slot_t slot[REPLAY_SLOTS];
  replay_ul_t ul[REPLAY_UL];
  replay_dl_t dl[REPLAY_DL];
} replay_header_t;
#include "nr_passive_replay_probe.h"
#include "nr_passive_replay_ul_config.h"
#include "nr_passive_delay_contract.h"
static replay_header_t *header;
static unsigned char *iq;
static char *output;
static atomic_int state;
static atomic_bool ul_seen;
static pthread_mutex_t record_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t writer_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t writer_cv = PTHREAD_COND_INITIALIZER;
static pthread_t writer;
static long next_source;

static uint64_t hash_tb(const uint8_t *p, size_t n)
{
  uint64_t h = UINT64_C(14695981039346656037);
  for (size_t i=0; i<n; ++i) h = (h ^ p[i]) * UINT64_C(1099511628211);
  return h;
}
static bool write_all(int fd, const void *data, size_t bytes)
{
  const unsigned char *p=data;
  while (bytes) {
    ssize_t n=write(fd,p,bytes);
    if (n<=0) return false;
    p+=n; bytes-=n;
  }
  return true;
}
static void *write_capture(void *unused)
{
  (void)unused;
  for (;;) {
    pthread_mutex_lock(&writer_lock);
    while (atomic_load(&state)!=RP_WRITING) pthread_cond_wait(&writer_cv,&writer_lock);
    pthread_mutex_unlock(&writer_lock);
    pthread_mutex_lock(&record_lock);
    unsigned controls=0;
    for(unsigned i=0;i<header->n_dl;++i) controls+=header->dl[i].tb_bytes>0;
    bool valid=header->n_ul>0 && controls>0;
    if (!valid) {
      unsigned n_ul=header->n_ul,n_dl=header->n_dl;
      header->n_ul=header->n_dl=0;
      atomic_store(&state,RP_ARMED);
      pthread_mutex_unlock(&record_lock);
      LOG_W(PHY,"REPLAY window VOID: no coincident UL/DL control (%u/%u); re-armed\n",n_ul,n_dl);
      continue;
    }
    pthread_mutex_unlock(&record_lock);
    int fd=open(output,O_WRONLY|O_CREAT|O_EXCL,0600);
    bool ok=fd>=0 && write_all(fd,header,sizeof(*header)) && write_all(fd,iq,header->iq_bytes);
    if (fd>=0) { if (fsync(fd)) ok=false; if (close(fd)) ok=false; }
    LOG_I(PHY,"REPLAY %s: %s slots=%u UL=%u DL-controls=%u IQ=%lu bytes\n",
          ok?"READY":"VOID",output,header->slots,header->n_ul,header->n_dl,(unsigned long)header->iq_bytes);
    atomic_store(&state,ok?RP_DONE:RP_VOID);
    return NULL;
  }
}
void nr_passive_replay_init(PHY_VARS_NR_UE *ue)
{
  const char *path=getenv("ISAC_PASSIVE_REPLAY_CAPTURE");
  if (!path || !*path) return;
  const NR_DL_FRAME_PARMS *fp=&ue->frame_parms;
  size_t bytes=(size_t)REPLAY_FRAMES*fp->samples_per_frame*fp->nb_antennas_rx*sizeof(c16_t);
  LOG_I(PHY,"REPLAY geometry nb_ant=%u samples_per_frame=%u slots_per_frame=%u bytes=%zu\n",
        fp->nb_antennas_rx, fp->samples_per_frame, fp->slots_per_frame, bytes);
  if (!fp->nb_antennas_rx || fp->nb_antennas_rx>4 || !bytes || bytes>REPLAY_MAX_BYTES
      || !fp->slots_per_frame || fp->slots_per_frame>160) {
    LOG_E(PHY,"REPLAY VOID: geometry exceeds bounded recorder\n"); return;
  }
  header=calloc(1,sizeof(*header)); iq=malloc(bytes); output=strdup(path);
  if (!header || !iq || !output) {
    LOG_E(PHY,"REPLAY VOID: allocation failed\n"); atomic_store(&state,RP_VOID); return;
  }
  memset(iq,0,bytes); /* Prefault before RF start, never page in 157 MB at capture time. */
  header->magic=UINT64_C(0x314951525041534e);
  header->version=getenv("ISAC_PASSIVE_REPLAY_FAILURES")?2:1;
  header->header_bytes=sizeof(*header);
  header->job_bytes=sizeof(nr_pdsch_passive_job_t); header->fp_bytes=sizeof(*fp);
  header->iq_bytes=bytes; header->slots=REPLAY_FRAMES*fp->slots_per_frame;
  memcpy(&header->fp,fp,sizeof(*fp));
  atomic_store(&state,RP_ARMED);
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setinheritsched(&attr,PTHREAD_EXPLICIT_SCHED);
  pthread_attr_setschedpolicy(&attr,SCHED_OTHER);
  struct sched_param sp={.sched_priority=0};
  pthread_attr_setschedparam(&attr,&sp);
  int err=pthread_create(&writer,&attr,write_capture,NULL);
  pthread_attr_destroy(&attr);
  if (err) { atomic_store(&state,RP_VOID); LOG_E(PHY,"REPLAY VOID: writer creation failed %d\n",err); }
  else pthread_detach(writer);
}
void nr_passive_replay_slot(PHY_VARS_NR_UE *ue, const UE_nr_rxtx_proc_t *proc)
{
  int s=atomic_load_explicit(&state,memory_order_acquire);
  if (s!=RP_REQUESTED && s!=RP_CAPTURING && s!=RP_DRAINING) return;
  const NR_DL_FRAME_PARMS *fp=&ue->frame_parms;
  long source=((long)proc->hfn_rx*1024+proc->frame_rx)*fp->slots_per_frame+proc->nr_slot_rx;
  if (s==RP_REQUESTED) {
    if (proc->nr_slot_rx || !ue->is_synchronized) return;
    /* Rare fixed-size metadata snapshot. IQ allocation/prefault was before RF. */
    memcpy(&header->fp,fp,sizeof(*fp));
    header->start=source; next_source=source;
    atomic_store_explicit(&state,RP_CAPTURING,memory_order_release); s=RP_CAPTURING;
  }
  if (!ue->is_synchronized || source!=next_source) {
    LOG_E(PHY,"REPLAY VOID: non-contiguous source expected=%ld got=%ld\n",next_source,source);
    atomic_store(&state,RP_VOID); return;
  }
  ++next_source;
  long index=source-header->start;
  if (s==RP_DRAINING) {
    if (index>=header->slots+fp->slots_per_frame) {
      pthread_mutex_lock(&record_lock);
      atomic_store(&state,RP_WRITING);
      pthread_mutex_unlock(&record_lock);
      pthread_mutex_lock(&writer_lock);
      pthread_cond_signal(&writer_cv);
      pthread_mutex_unlock(&writer_lock);
    }
    return;
  }
  unsigned offset=get_samples_slot_timestamp(fp,proc->nr_slot_rx);
  unsigned count=get_samples_per_slot(proc->nr_slot_rx,fp);
  if (offset+count>fp->samples_per_frame || index<0 || index>=header->slots) {
    LOG_E(PHY,"REPLAY VOID: sample extent\n"); atomic_store(&state,RP_VOID); return;
  }
  header->slot[index]=(replay_slot_t){source,ue->cont_fo_comp?ue->dl_Doppler_shift+ue->freq_offset:0};
  size_t frame=index/fp->slots_per_frame;
  for (unsigned a=0;a<fp->nb_antennas_rx;++a) {
    size_t dst=((frame*fp->nb_antennas_rx+a)*fp->samples_per_frame+offset)*sizeof(c16_t);
    memcpy(iq+dst,ue->common_vars.rxdata[a]+offset,count*sizeof(c16_t));
  }
  if (index+1==header->slots) atomic_store(&state,RP_DRAINING);
}
static bool recordable(long source)
{
  int s=atomic_load(&state);
  if (s!=RP_CAPTURING && s!=RP_DRAINING) return false;
  long relative=source-header->start;
  /* One full preceding frame for negative UL TA; three following frames for k2. */
  return relative>=header->fp.slots_per_frame && relative<(REPLAY_FRAMES-3)*header->fp.slots_per_frame;
}
void nr_passive_replay_ul(long source, uint16_t rnti, unsigned length, uint64_t payload)
{
  if (atomic_load(&state)==RP_DISABLED) return;
  atomic_store(&ul_seen,true);
  pthread_mutex_lock(&record_lock);
  if (recordable(source)) {
    if (header->n_ul==REPLAY_UL) atomic_store(&state,RP_VOID);
    else header->ul[header->n_ul++]=(replay_ul_t){source,payload,rnti,length};
  }
  pthread_mutex_unlock(&record_lock);
}
void nr_passive_replay_dl(const nr_pdsch_passive_job_t *job,
                          const nr_pdsch_passive_decode_result_t *result)
{
  if (atomic_load(&state)==RP_DISABLED) return;
  const bool success=result->status==NR_PDSCH_PASSIVE_DECODE_CRC_OK && result->tb;
  const bool failures=header->version>=2;
  /* Manual mode (TESTING MODE RULE): the layout is fixed, not swept, so generation==0 means
   * "no sweep in progress, already settled" -- treat it exactly like a settled ticket. */
  if(!success && !(failures && (job->sweep_ticket.generation==0 || job->sweep_ticket.settled) &&
                  result->status==NR_PDSCH_PASSIVE_DECODE_CRC_FAIL))
    return;
  if (success && (job->sweep_ticket.generation==0 || !failures || job->sweep_ticket.settled)
      && atomic_load(&ul_seen)) {
    int expected=RP_ARMED;
    if (atomic_compare_exchange_strong(&state,&expected,RP_REQUESTED))
      LOG_I(PHY,"REPLAY ARMED at absolute_slot=%ld\n",job->absolute_slot);
  }
  pthread_mutex_lock(&record_lock);
  if (recordable(job->absolute_slot) && header->n_dl<REPLAY_DL) {
    unsigned failed_records=0;
    for(unsigned i=0;i<header->n_dl;++i) failed_records+=header->dl[i].tb_bytes==0;
    if(!success && failed_records>=64) { pthread_mutex_unlock(&record_lock); return; }
    replay_dl_t *r=&header->dl[header->n_dl++];
    r->job=*job;
    r->tb_bytes=success?(result->cw.TBS+7)/8:0;
    r->tb_hash=success?hash_tb(result->tb,r->tb_bytes):0;
  }
  pthread_mutex_unlock(&record_lock);
}

int nr_passive_replay_read(PHY_VARS_NR_UE *ue, const char *path)
{
  /* Replay bypasses UE_thread(), which normally initializes the CFO LUT.
   * A nonzero-CFO FEP otherwise multiplies samples by the zero-initialized LUT. */
  InitSinLUT();
  FILE *f=fopen(path,"rb");
  if (!strcmp(path,"@delay-contract")) return replay_delay_contract();
  replay_header_t *h=malloc(sizeof(*h));
  if (!f || !h) { fprintf(stderr,"REPLAY VOID: cannot open input\n"); return 2; }
  bool ok=fread(h,sizeof(*h),1,f)==1;
  const NR_DL_FRAME_PARMS *fp=&h->fp, *allocated=&ue->frame_parms;
  ok=ok && h->magic==UINT64_C(0x314951525041534e) &&
     (h->version==1 || h->version==2) &&
     h->header_bytes==sizeof(*h) && h->job_bytes==sizeof(nr_pdsch_passive_job_t) &&
     h->fp_bytes==sizeof(*fp) && h->n_dl>0 && h->n_dl<=REPLAY_DL &&
     h->n_ul>0 && h->n_ul<=REPLAY_UL && fp->nb_antennas_rx>0 && fp->nb_antennas_rx<=4 &&
     fp->slots_per_frame>0 && fp->slots_per_frame<=160 && h->slots==REPLAY_FRAMES*fp->slots_per_frame &&
     fp->samples_per_frame==allocated->samples_per_frame &&
     fp->ofdm_symbol_size==allocated->ofdm_symbol_size &&
     fp->samples_per_slot_wCP==allocated->samples_per_slot_wCP &&
     fp->nb_antennas_rx==allocated->nb_antennas_rx &&
     h->iq_bytes==(uint64_t)REPLAY_FRAMES*fp->samples_per_frame*fp->nb_antennas_rx*sizeof(c16_t) &&
     h->iq_bytes<=REPLAY_MAX_BYTES;
  if (!ok) { fprintf(stderr,"REPLAY VOID: incompatible header/geometry\n"); fclose(f); free(h); return 2; }
  unsigned char *samples=malloc(h->iq_bytes);
  ok=samples && fread(samples,h->iq_bytes,1,f)==1 && fgetc(f)==EOF;
  fclose(f);
  if (!ok) { fprintf(stderr,"REPLAY VOID: incomplete/oversized IQ file\n"); free(samples); free(h); return 2; }
  for (unsigned i=0;i<h->slots;++i)
    if (h->slot[i].source!=h->start+i || !isfinite(h->slot[i].fo)) ok=false;
  if (!ok) { fprintf(stderr,"REPLAY VOID: discontinuous metadata\n"); free(samples); free(h); return 2; }
  memcpy(&ue->frame_parms,fp,sizeof(*fp));
  ue->is_synchronized=true; ue->cont_fo_comp=true;
  size_t freq_bytes=(size_t)fp->nb_antennas_rx*fp->samples_per_slot_wCP*sizeof(c16_t);
  c16_t (*rxF)[fp->samples_per_slot_wCP]=aligned_alloc(32,(freq_bytes+31)&~(size_t)31);
  if (!rxF) { free(samples); free(h); return 2; }
  unsigned matches=0,failed=0;
  for (unsigned i=0;i<h->n_dl;++i) {
    replay_dl_t *r=&h->dl[i]; long index=r->job.absolute_slot-h->start;
    if (index<fp->slots_per_frame || index>=h->slots ||
        r->job.nr_slot_rx!=index%fp->slots_per_frame ||
        r->job.frame_rx!=(r->job.absolute_slot/fp->slots_per_frame)%1024 ||
        (!r->tb_bytes && h->version<2) || r->tb_bytes>1024*1024) { ++failed; continue; }
    size_t frame=index/fp->slots_per_frame;
    unsigned end=get_samples_slot_timestamp(fp,r->job.nr_slot_rx)+get_samples_per_slot(r->job.nr_slot_rx,fp);
    for (unsigned a=0;a<fp->nb_antennas_rx;++a) {
      size_t prior=((frame-1)*fp->nb_antennas_rx+a)*fp->samples_per_frame*sizeof(c16_t);
      size_t current=(frame*fp->nb_antennas_rx+a)*fp->samples_per_frame*sizeof(c16_t);
      memcpy(ue->common_vars.rxdata[a],samples+prior,fp->samples_per_frame*sizeof(c16_t));
      memcpy(ue->common_vars.rxdata[a],samples+current,end*sizeof(c16_t));
    }
    UE_nr_rxtx_proc_t proc={0};
    proc.frame_rx=r->job.frame_rx; proc.nr_slot_rx=r->job.nr_slot_rx; proc.gNB_id=r->job.gNB_id;
    r->job.grant.check_sample_lifetime=false;
    nr_slot_fep_fo_override_hz=r->job.fo_hz;
    nr_pdsch_passive_decode_result_t result;
    nr_pdsch_passive_decode_status_t status=nr_pdsch_passive_decode(ue,&proc,&r->job.dlsch_pdu,
        &r->job.freq_alloc,&r->job.grant,rxF,&result);
    if(!r->tb_bytes) {
      printf("DL-REPLAY-FAILURE rnti=%04x source=%ld mcs=%u rb=%u sym=%u+%u dmrs=%x "
             "status=%d tbs=%u\n",r->job.rnti,r->job.absolute_slot,r->job.grant.mcs,
             r->job.freq_alloc.num_rbs,r->job.dlsch_pdu.start_symbol,
             r->job.dlsch_pdu.number_symbols,r->job.dlsch_pdu.dlDmrsSymbPos,
             status,result.cw.TBS);
      continue;
    }
    bool match=status==NR_PDSCH_PASSIVE_DECODE_CRC_OK && (result.cw.TBS+7)/8==r->tb_bytes &&
               hash_tb(result.tb,r->tb_bytes)==r->tb_hash;
    matches+=match; failed+=!match;
    printf("REPLAY-CONTROL rnti=%04x source=%ld status=%d identical=%d\n",r->job.rnti,r->job.absolute_slot,status,match);
  }
  nr_slot_fep_fo_override_hz=NAN;
  if(!matches) ++failed; /* A failure-only recording is not a replay control. */
  for (unsigned i=0;i<h->n_ul;++i)
    printf("REPLAY-RAW-UL source=%ld rnti=%04x bits=%u payload=%016lx\n",
           h->ul[i].source,h->ul[i].rnti,h->ul[i].length,(unsigned long)h->ul[i].payload);
  if (h->n_ul<8) printf("UL-SEARCH UNRESOLVED: fewer than eight raw observations; no UL convergence claim\n");
  printf("REPLAY %s: identical DL controls=%u failed=%u raw UL=%u; no radio opened\n",
         failed?"VOID":"PASS",matches,failed,h->n_ul);
  if (!failed && getenv("ISAC_PASSIVE_REPLAY_UL_PROBE")) replay_probe_ul(ue,h,samples);
  if (!failed && getenv("ISAC_PASSIVE_REPLAY_UL_CONFIG")) replay_ul_config(ue,h,samples);
  free(rxF); free(samples); free(h);
  return failed?2:0;
}
