/* SPDX-License-Identifier: LicenseRef-CSSL-1.0
 * Host transport double, not DSP/GPU evidence. Deliberately owns no CUDA code. */
#include "nr_pdcch_nid_gpu.h"
#include <cstdlib>
#include <atomic>
#include <thread>
#include <chrono>
#include <sched.h>
static std::atomic<int> uploaded{0},runs{0},paused{0},errors{0};
struct Fake { nr_pdcch_nid_iq_t *memory;int rb,dur; };
static void *create(int rb,int dur,int,nr_pdcch_nid_iq_t **staging) {
 auto *f=new Fake{(nr_pdcch_nid_iq_t*)calloc((size_t)3*336*dur*12*rb,sizeof(nr_pdcch_nid_iq_t)),rb,dur};
 *staging=f->memory;return f;
}
static int upload(void *p,const nr_pdcch_nid_batch_t *b) {
 auto *f=(Fake*)p;
 while(paused.load())std::this_thread::sleep_for(std::chrono::milliseconds(1));
 cpu_set_t cpus;CPU_ZERO(&cpus);sched_getaffinity(0,sizeof(cpus),&cpus);
 if(sched_getscheduler(0)!=SCHED_OTHER || CPU_COUNT(&cpus)!=1)errors++;
 for(int k=0;k<(b->count?b->count:336);k++)for(int d=0;d<f->dur;d++)for(int i=0;i<b->cfg.nsc;i++) {
  auto y=b->iq[((size_t)k*f->dur+d)*12*f->rb+i];
  int v=b->sym[k]+d<b->cfg.sps?k+d+i:0;
  if(y.r!=v || y.i!=-v){errors++;return -1;}
 }
 uploaded++;return 0;
}
static int run(void *,nr_pdcch_nid_result_t *out) {
 runs++;*out={};out->count=1;
 out->records[0]={12000+uploaded.load(),3,1,12,2,0,0,0,1};return 0;
}
static void destroy(void *p) {auto *f=(Fake*)p;free(f->memory);delete f;}
extern "C" void nid_fake_pause(int on) {paused=on;}
extern "C" int nid_fake_uploaded() {return uploaded.load();}
extern "C" int nid_fake_errors() {return errors.load();}
extern "C" int nid_fake_runs() {return runs.load();}
extern "C" const nr_pdcch_nid_ops_t *nr_pdcch_nid_gpu_ops() {
 static const nr_pdcch_nid_ops_t ops={NR_PDCCH_NID_ABI,create,upload,run,destroy};return &ops;
}
