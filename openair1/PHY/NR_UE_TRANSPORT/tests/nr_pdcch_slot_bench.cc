/* CPU reference dump validation and whole-slot CORESET GPU benchmark. */
#define NR_PDCCH_GPU_RECORD_FORMAT_ONLY
#include "nr_pdcch_gpu_record.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <vector>

struct Sample { nr_pdcch_gpu_record_t h; std::vector<int16_t> iq, cpu; };
static double elapsed(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b)
{ return std::chrono::duration<double, std::micro>(b-a).count(); }
static double pct(std::vector<double> v, double q)
{ if (v.empty()) return NAN; std::sort(v.begin(),v.end()); return v[(size_t)std::ceil(q*v.size())-1]; }

int main(int argc, char **argv)
{
  if (argc < 2) { fprintf(stderr,"usage: nr_pdcch_slot_bench dump.bin [--cpu-only|--gpu] [repetitions]\n"); return 2; }
  if (argc>2 && strcmp(argv[2],"--gpu") && strcmp(argv[2],"--cpu-only")) return 2;
  const bool gpu = argc > 2 && !strcmp(argv[2],"--gpu");
  const int reps = argc > 3 ? std::max(1,atoi(argv[3])) : 1;
  FILE *f = fopen(argv[1],"rb");
  if (!f) { perror(argv[1]); return 2; }
  std::vector<Sample> rows;
  bool malformed = false;
  for (;;) {
    int c = fgetc(f);
    if (c == EOF) break;
    ungetc(c,f);
    Sample s;
    if (fread(&s.h,sizeof(s.h),1,f)!=1) { malformed=true; break; }
    const auto &h=s.h;
    if (h.magic!=NR_PDCCH_GPU_RECORD_MAGIC || h.version!=1 || h.cfg.nant<1 || h.cfg.nant>4
        || h.cfg.ofdm_symbol_size<128 || h.cfg.ofdm_symbol_size>8192 || h.job.duration<1 || h.job.duration>3
        || h.job.n_rb<1 || h.job.n_rb>273 || h.llr_count!=18u*h.job.n_rb*h.job.duration) { malformed=true; break; }
    s.iq.resize((size_t)2*h.cfg.nant*h.job.duration*h.cfg.ofdm_symbol_size);
    s.cpu.resize(h.llr_count);
    if (h.bytes != sizeof(h)+(s.iq.size()+s.cpu.size())*sizeof(int16_t)
        || fread(s.iq.data(),sizeof(int16_t),s.iq.size(),f)!=s.iq.size()
        || fread(s.cpu.data(),sizeof(int16_t),s.cpu.size(),f)!=s.cpu.size()) { malformed=true; break; }
    rows.push_back(std::move(s));
  }
  fclose(f);
  if (malformed || rows.empty()) { fprintf(stderr,"empty or malformed dump\n"); return 2; }
  std::stable_sort(rows.begin(),rows.end(),[](const Sample &a,const Sample &b){return a.h.absolute_slot<b.h.absolute_slot;});
  std::vector<uint16_t> ids;
  for (const auto &r:rows) ids.push_back(r.h.job.dmrs_scrambling_id);
  std::sort(ids.begin(),ids.end());
  ids.erase(std::unique(ids.begin(),ids.end()),ids.end());
  size_t slots=0,max_passes=0,over64=0;
  for (size_t i=0;i<rows.size();) {
    const size_t begin=i;
    const auto n=rows[i].h.absolute_slot;
    ++slots;
    do {++i;} while(i<rows.size() && rows[i].h.absolute_slot==n);
    max_passes=std::max(max_passes,i-begin);
    over64+=i-begin>64;
  }
  printf("recorded PDCCH CPU reference: passes=%zu slots=%zu max_passes_per_slot=%zu over_gpu_batch_limit=%zu nIDs=",
         rows.size(),slots,max_passes,over64);
  for (auto id:ids) printf("%u,",id);
  printf("\n");
  if (!gpu) return 0;
  void *lib=dlopen("./libpdcch_gpu.so",RTLD_NOW|RTLD_LOCAL);
  if (!lib) { fprintf(stderr,"dlopen libpdcch_gpu.so: %s\n",dlerror()); return 2; }
  auto get=(const nr_gpu_pdcch_fep_api_t *(*)())dlsym(lib,"nr_gpu_pdcch_fep_api");
  const auto *api=get?get():nullptr;
  if (!api || api->init(&rows[0].h.cfg)) { fprintf(stderr,"PDCCH GPU init failed\n"); return 2; }
  std::vector<double> timings;
  size_t words=0, exact=0, sign=0, unsupported=0, batched=0;
  size_t cpu_zero=0, gpu_zero=0, both_nonzero=0, nonzero_sign=0;
  double xy=0,x2=0,y2=0;
  for (int rep=0;rep<reps;++rep) for (size_t i=0;i<rows.size();) {
    const size_t begin=i;
    const auto &first=rows[i];
    do { ++i; } while(i<rows.size() && rows[i].h.absolute_slot==first.h.absolute_slot
        && rows[i].h.job.start_symbol==first.h.job.start_symbol
        && rows[i].h.job.duration==first.h.job.duration && rows[i].iq==first.iq);
    const int N=first.h.cfg.ofdm_symbol_size, dur=first.h.job.duration;
    const int16_t *rx[4]={};
    for (int a=0;a<first.h.cfg.nant;++a) rx[a]=first.iq.data()+(size_t)2*a*dur*N;
    std::vector<nr_gpu_pdcch_job_t> jobs;
    size_t cap=0;
    for (size_t k=begin;k<i;++k) {jobs.push_back(rows[k].h.job); cap+=rows[k].cpu.size();}
    std::vector<int16_t> out(cap);
    const auto t0=std::chrono::steady_clock::now();
    const int up=api->upload_slot(rx,dur);
    const int64_t got=up?-1:api->pdcch_llr(jobs.data(),jobs.size(),out.data(),out.size());
    const auto t1=std::chrono::steady_clock::now();
    if (got<0) { fprintf(stderr,"PDCCH GPU error slot=%u\n",first.h.absolute_slot); return 1; }
    timings.push_back(elapsed(t0,t1));
    if (jobs.size()>1) ++batched;
    if (rep==0) for (size_t k=begin;k<i;++k) {
      const auto &j=jobs[k-begin];
      if (j.n_llr!=rows[k].h.llr_count) {++unsupported;continue;}
      for (uint32_t b=0;b<j.n_llr;++b) {
        const int16_t x=out[j.llr_offset+b], y=rows[k].cpu[b];
        exact+=x==y; sign+=(x<0)==(y<0); ++words;
        cpu_zero+=y==0; gpu_zero+=x==0;
        if (x && y) { ++both_nonzero; nonzero_sign+=(x<0)==(y<0); }
        xy+=(double)x*y; x2+=(double)x*x; y2+=(double)y*y;
      }
    }
  }
  const double corr=x2>0 && y2>0?xy/std::sqrt(x2*y2):0;
  printf("PDCCH GPU scan window us p50=%.1f p99=%.1f, batched_windows=%zu; CPU LLR exact=%zu/%zu sign=%zu/%zu corr=%.6f unsupported=%zu\n",
         pct(timings,.5),pct(timings,.99),batched,exact,words,sign,words,corr,unsupported);
  printf("PDCCH zero/sign split: cpu_zero=%zu/%zu gpu_zero=%zu/%zu nonzero_sign=%zu/%zu\n",
         cpu_zero, words, gpu_zero, words, nonzero_sign, both_nonzero);
  return unsupported || !words || sign!=words || corr<.99995 ? 1 : 0;
}
