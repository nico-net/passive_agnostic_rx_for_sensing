/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_gpu_platform_internal.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* Values from CUDA's stable cudaDeviceAttr ABI (driver_types.h). */
enum { ATTR_INTEGRATED=18, ATTR_MAP_HOST=19, ATTR_UVA=41, ATTR_MAJOR=75, ATTR_MINOR=76,
       ATTR_HOST_ATOMIC=86, ATTR_PAGEABLE=88, ATTR_MANAGED=89, ATTR_HOST_TABLES=100 };
enum { GPU_ERROR=1, GPU_NO_MEMORY=2, GPU_NOT_READY=600 };
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static nr_gpu_runtime_t rt;
static nr_gpu_caps_t caps;
static int probed, sealed, managed, warned_late;
static uint64_t allocations, denied;
static void (*logger)(int, const char *);
typedef struct allocation {
  void *host, *device;
  size_t bytes;
  nr_gpu_storage_t storage;
  int registered;
  struct allocation *next;
} allocation;
static allocation *live;

static void report(int warning, const char *fmt, ...)
{
  char text[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(text, sizeof(text), fmt, ap);
  va_end(ap);
  if (logger) logger(warning, text);
  else fprintf(stderr, "%s%s\n", warning ? "WARNING: " : "", text);
}
void nr_gpu_set_logger(void (*fn)(int, const char *))
{
  pthread_mutex_lock(&mu);
  logger = fn;
  pthread_mutex_unlock(&mu);
}
nr_gpu_model_t nr_gpu_choose_model(const nr_gpu_caps_t *c, const char *override)
{
  if (!c || !c->available) return NR_GPU_NONE;
  if (!override || !strcmp(override, "auto"))
    return c->integrated || (c->pageable_memory_access && c->host_page_tables) ? NR_GPU_SHARED : NR_GPU_DISCRETE;
  if (!strcmp(override, "shared")) return NR_GPU_SHARED;
  if (!strcmp(override, "discrete")) return NR_GPU_DISCRETE;
  return NR_GPU_NONE;
}
nr_gpu_storage_t nr_gpu_storage(nr_gpu_model_t model, nr_gpu_kind_t kind, int use_managed)
{
  if (model != NR_GPU_SHARED) return NR_GPU_ALLOC_UNAVAILABLE;
  switch (kind) {
    case NR_GPU_DEVICE: return NR_GPU_ALLOC_DEVICE;
    case NR_GPU_STAGING: return NR_GPU_ALLOC_MAPPED;
    case NR_GPU_SHARED_RW: return use_managed ? NR_GPU_ALLOC_MANAGED : NR_GPU_ALLOC_MAPPED;
  }
  return NR_GPU_ALLOC_UNAVAILABLE;
}
int nr_gpu_arch_compatible(int major, int minor, const char *archs)
{
  if (!archs || major <= 0 || minor < 0) return 0;
  const int cc = major * 10 + minor;
  while (*archs) {
    char *end;
    long arch = strtol(archs, &end, 10);
    if (end == archs || arch <= 0) return 0;
    int real = 1, virt = 1;
    if (!strncmp(end, "-real", 5)) { virt = 0; end += 5; }
    else if (!strncmp(end, "-virtual", 8)) { real = 0; end += 8; }
    if (*end && *end != ',' && *end != ';') return 0;
    /* cubin compatibility is within a major; generic PTX is forward compatible. */
    if ((real && arch / 10 == major && arch <= cc) || (virt && arch <= cc)) return 1;
    archs = *end ? end + 1 : end;
  }
  return 0;
}
static int load_runtime(void)
{
  const char *names[] = {"libcudart.so", "libcudart.so.13", "libcudart.so.12", "libcudart.so.11.0"};
  void *h = NULL;
  for (unsigned i=0; i<sizeof(names)/sizeof(names[0]) && !h; ++i) h=dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
  if (!h) return 0;
#define LOAD(field, symbol) do { *(void **)(&rt.field)=dlsym(h, symbol); if (!rt.field) return 0; } while (0)
  LOAD(get_count,"cudaGetDeviceCount"); LOAD(get_device,"cudaGetDevice");
  LOAD(attribute,"cudaDeviceGetAttribute"); LOAD(mem_info,"cudaMemGetInfo");
  LOAD(malloc_device,"cudaMalloc"); LOAD(malloc_host,"cudaHostAlloc"); LOAD(malloc_managed,"cudaMallocManaged");
  LOAD(free_device,"cudaFree"); LOAD(free_host,"cudaFreeHost"); LOAD(host_device_ptr,"cudaHostGetDevicePointer");
  LOAD(register_host,"cudaHostRegister"); LOAD(unregister_host,"cudaHostUnregister");
  LOAD(copy_async,"cudaMemcpyAsync"); LOAD(stream_sync,"cudaStreamSynchronize"); LOAD(device_sync,"cudaDeviceSynchronize");
  LOAD(set_flags,"cudaSetDeviceFlags"); LOAD(get_last_error,"cudaGetLastError");
#undef LOAD
  return 1; /* runtime remains resident for module lifetime */
}
static int attribute(int attr)
{
  int value=0;
  if (rt.attribute(&value, attr, caps.device)) {
    rt.get_last_error(); /* optional attributes on older drivers must not poison warm-up */
    return 0;
  }
  return value;
}
static void probe_locked(void)
{
  if (probed) return;
  probed=1;
  int count=0;
  if (!rt.get_count && !load_runtime()) goto finish;
  if (rt.get_count(&count) || !count || rt.get_device(&caps.device)) goto finish;
  /* Schedule flags before memory info creates a context; tolerate an existing context. */
  if (rt.set_flags(4 | 8)) rt.get_last_error(); /* blocking sync | map host; context may already exist */
  caps.available=1;
  caps.major=attribute(ATTR_MAJOR); caps.minor=attribute(ATTR_MINOR);
  caps.integrated=attribute(ATTR_INTEGRATED); caps.can_map_host_memory=attribute(ATTR_MAP_HOST);
  caps.unified_addressing=attribute(ATTR_UVA); caps.pageable_memory_access=attribute(ATTR_PAGEABLE);
  caps.host_page_tables=attribute(ATTR_HOST_TABLES); caps.concurrent_managed_access=attribute(ATTR_MANAGED);
  caps.host_native_atomic_supported=attribute(ATTR_HOST_ATOMIC);
  if (rt.mem_info(&caps.free_memory,&caps.total_memory)) caps.available=0;
  strcpy(caps.name,"unknown");
  /* Only the name uses the driver's scalar ABI; runtime device ordinal is the same
   * CUDA_VISIBLE_DEVICES ordinal. Avoid version-dependent cudaDeviceProp structs. */
#ifndef NR_GPU_PLATFORM_TEST
  void *driver=dlopen("libcuda.so.1",RTLD_NOW | RTLD_LOCAL);
  if (driver) {
    int (*get_name)(char *,int,int)=NULL;
    *(void **)(&get_name)=dlsym(driver,"cuDeviceGetName");
    if (get_name) get_name(caps.name,sizeof(caps.name),caps.device);
    dlclose(driver);
  }
#endif
finish:
  caps.model=nr_gpu_choose_model(&caps,getenv("ISAC_GPU_MEMORY_MODEL"));
  const char *opt=getenv("ISAC_GPU_SHARED_MANAGED");
  managed=opt && !strcmp(opt,"1");
  report(0,"SENSING: GPU_PLATFORM device=%d name=\"%s\" cc=%d.%d integrated=%d pageableMemoryAccess=%d pageableMemoryAccessUsesHostPageTables=%d canMapHostMemory=%d concurrentManagedAccess=%d hostNativeAtomicSupported=%d total=%zu free=%zu model=%s managed=%d",
         caps.device,caps.name,caps.major,caps.minor,caps.integrated,caps.pageable_memory_access,caps.host_page_tables,
         caps.can_map_host_memory,caps.concurrent_managed_access,caps.host_native_atomic_supported,caps.total_memory,caps.free_memory,
         caps.model==NR_GPU_SHARED?"shared":caps.model==NR_GPU_DISCRETE?"discrete":"unavailable",managed);
  if (caps.model==NR_GPU_DISCRETE)
    report(1,"SENSING: GPU_PLATFORM DISCRETE backend is not implemented (phase 2); GPU paths use CPU fallback");
  else if (caps.model!=NR_GPU_SHARED || !caps.can_map_host_memory || !caps.unified_addressing)
    report(1,"SENSING: GPU_PLATFORM unavailable/invalid model or mapped-memory capability; CPU fallback");
}
const nr_gpu_caps_t *nr_gpu_platform_probe(void)
{
  pthread_mutex_lock(&mu); probe_locked(); pthread_mutex_unlock(&mu); return &caps;
}
int nr_gpu_platform_ready(void)
{
  const nr_gpu_caps_t *c=nr_gpu_platform_probe();
  return c->available && c->model==NR_GPU_SHARED && c->can_map_host_memory && c->unified_addressing;
}
int nr_gpu_module_check(const char *module, const char *archs)
{
  if (!nr_gpu_platform_ready()) return 0;
  if (nr_gpu_arch_compatible(caps.major,caps.minor,archs)) return 1;
  report(1,"SENSING: GPU_PLATFORM module=%s built=%s device=sm_%d%d incompatible; CPU fallback",module,archs,caps.major,caps.minor);
  return 0;
}
void nr_gpu_rx_started(void)
{
  pthread_mutex_lock(&mu); __atomic_store_n(&sealed,1,__ATOMIC_RELEASE); pthread_mutex_unlock(&mu);
}
int nr_gpu_allocations_allowed(void) { return !__atomic_load_n(&sealed,__ATOMIC_ACQUIRE); }
static int allowed_locked(void)
{
  if (!sealed) return 1;
  ++denied;
  if (!warned_late++) report(1,"SENSING: GPU_PLATFORM allocation/registration/free after RX start denied; CPU fallback");
  return 0;
}
int nr_gpu_allocation_guard(void)
{
  pthread_mutex_lock(&mu);
  int ok=allowed_locked();
  pthread_mutex_unlock(&mu);
  return ok;
}
void nr_gpu_memory_stats(uint64_t *a,uint64_t *d)
{
  pthread_mutex_lock(&mu); *a=allocations; *d=denied; pthread_mutex_unlock(&mu);
}
static allocation *lookup(void *ptr)
{
  for (allocation *a=live;a;a=a->next) if (a->host==ptr) return a;
  return NULL;
}
void *nr_gpu_alloc(nr_gpu_kind_t kind,size_t bytes)
{
  pthread_mutex_lock(&mu);
  void *p=NULL;
  if (!allowed_locked() || !bytes) goto done;
  probe_locked();
  if (!caps.available || !caps.can_map_host_memory || !caps.unified_addressing) goto done;
  nr_gpu_storage_t storage=nr_gpu_storage(caps.model,kind,managed);
  if (!storage || (storage==NR_GPU_ALLOC_MANAGED && !caps.concurrent_managed_access)) goto done;
  allocation *a=calloc(1,sizeof(*a));
  if (!a) goto done;
  int rc=storage==NR_GPU_ALLOC_DEVICE ? rt.malloc_device(&p,bytes) :
         storage==NR_GPU_ALLOC_MANAGED ? rt.malloc_managed(&p,bytes,1) : rt.malloc_host(&p,bytes,3);
  if (rc) { free(a); p=NULL; goto done; }
  a->device=p;
  if (storage==NR_GPU_ALLOC_MAPPED && rt.host_device_ptr(&a->device,p,0)) {
    rt.free_host(p); free(a); p=NULL; goto done;
  }
  a->host=p; a->bytes=bytes; a->storage=storage; a->next=live; live=a; ++allocations;
done:
  pthread_mutex_unlock(&mu); return p;
}
void *nr_gpu_device_ptr(void *ptr)
{
  pthread_mutex_lock(&mu);
  allocation *a=lookup(ptr);
  void *p=a ? a->device : NULL;
  pthread_mutex_unlock(&mu); return p;
}
static int release(void *ptr,int registered)
{
  if (!ptr) return 0;
  pthread_mutex_lock(&mu);
  int rc=GPU_ERROR;
  if (!allowed_locked()) { rc=GPU_NOT_READY; goto done; }
  allocation **link=&live;
  while (*link && (*link)->host!=ptr) link=&(*link)->next;
  if (!*link || (*link)->registered!=registered) goto done;
  allocation *a=*link;
  rc=registered ? rt.unregister_host(ptr) : a->storage==NR_GPU_ALLOC_MAPPED ? rt.free_host(ptr) : rt.free_device(ptr);
  if (!rc) { *link=a->next; free(a); }
done:
  pthread_mutex_unlock(&mu); return rc;
}
int nr_gpu_free(void *ptr) { return release(ptr,0); }
int nr_gpu_unregister_host(void *ptr) { return release(ptr,1); }
int nr_gpu_register_host(void *ptr,size_t bytes)
{
  pthread_mutex_lock(&mu);
  int rc=GPU_ERROR;
  if (!allowed_locked()) { rc=GPU_NOT_READY; goto done; }
  probe_locked();
  if (!ptr || !bytes || caps.model!=NR_GPU_SHARED || !caps.can_map_host_memory || !caps.unified_addressing) goto done;
  allocation *a=lookup(ptr);
  if (a) { rc=a->registered && a->bytes==bytes ? 0 : GPU_ERROR; goto done; }
  a=calloc(1,sizeof(*a));
  if (!a) { rc=GPU_NO_MEMORY; goto done; }
  rc=rt.register_host(ptr,bytes,3); /* portable | mapped */
  if (!rc) {
    rc=rt.host_device_ptr(&a->device,ptr,0);
    if (rc) rt.unregister_host(ptr);
  }
  if (rc) { free(a); goto done; }
  a->host=ptr; a->bytes=bytes; a->registered=1; a->next=live; live=a; ++allocations;
done:
  pthread_mutex_unlock(&mu); return rc;
}
int nr_gpu_copy_async(void *dst,const void *src,size_t bytes,void *stream)
{
  if (!nr_gpu_platform_ready()) return GPU_NOT_READY;
  return rt.copy_async(dst,src,bytes,4,stream); /* cudaMemcpyDefault */
}
int nr_gpu_sync(void *stream)
{
  if (!nr_gpu_platform_ready()) return GPU_NOT_READY;
  return rt.stream_sync(stream);
}
int nr_gpu_sync_all(void)
{
  if (!nr_gpu_platform_ready()) return GPU_NOT_READY;
  return rt.device_sync();
}
#ifdef NR_GPU_PLATFORM_TEST
void nr_gpu_test_reset(const nr_gpu_runtime_t *runtime)
{
  /* Test-only; caller has joined all users and released allocations first. */
  while (live) { allocation *a=live; live=a->next; free(a); }
  rt=*runtime; memset(&caps,0,sizeof(caps));
  probed=sealed=managed=warned_late=0; allocations=denied=0;
}
#endif
