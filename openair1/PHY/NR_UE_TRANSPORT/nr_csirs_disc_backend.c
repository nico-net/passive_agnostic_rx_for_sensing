/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_csirs_disc.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static pthread_once_t once=PTHREAD_ONCE_INIT;
static const nr_csirs_disc_gpu_ops_t *ops;
/* Keep module loaded for the lifetime of any managed allocations/worker. */
static void load_module(void)
{
  void *handle=dlopen("libcsirs_disc_gpu.so",RTLD_NOW|RTLD_LOCAL);
  if(!handle) {
    char path[4096];ssize_t n=readlink("/proc/self/exe",path,sizeof(path)-1);
    if(n>0) {
      path[n]=0;char *slash=strrchr(path,'/');
      if(slash && (size_t)(slash-path)+sizeof("/libcsirs_disc_gpu.so")<=sizeof(path)) {
        strcpy(slash,"/libcsirs_disc_gpu.so");handle=dlopen(path,RTLD_NOW|RTLD_LOCAL);
      }
    }
  }
  if(!handle) return;
  const nr_csirs_disc_gpu_ops_t *(*get_ops)(void)=dlsym(handle,"nr_csirs_disc_gpu_ops");
  if(get_ops) ops=get_ops();
  if(!ops) { dlclose(handle); return; }
  /* Install the module's managed allocator into the U1 ring hooks (core stays CUDA-free). */
  void *(*a)(size_t)=(void *(*)(size_t))dlsym(handle,"nr_csirs_disc_gpu_alloc_unified");
  void (*f)(void *)=(void (*)(void *))dlsym(handle,"nr_csirs_disc_gpu_free_unified");
  if(a && f) { nr_csirs_disc_alloc_unified=a; nr_csirs_disc_free_unified=f; }
}
const nr_csirs_disc_gpu_ops_t *nr_csirs_disc_gpu_load(void)
{
  const char *enabled=getenv("ISAC_CSIRS_DISC"), *backend=getenv("ISAC_CSIRS_DISC_BACKEND");
  if(!enabled || strcmp(enabled,"1") || (backend && !strcmp(backend,"cpu"))) return NULL;
  if(backend && strcmp(backend,"auto") && strcmp(backend,"gpu")) return NULL;
  pthread_once(&once,load_module);
  return ops;
}
