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

/* report: formats a message (printf style) and sends it to the registered logger, or to stderr with a WARNING prefix when none is set.
 *   Callers: probe_locked(), nr_gpu_module_check(), allowed_locked() and the platform log lines.
 */
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
/* nr_gpu_set_logger: installs the logging callback used by the platform for its status and warning messages (NULL restores stderr).
 *   Takes the platform mutex. Called by executables/nr-uesoftmodem.c at start-up.
 */
void nr_gpu_set_logger(void (*fn)(int, const char *))
{
  pthread_mutex_lock(&mu);
  logger = fn;
  pthread_mutex_unlock(&mu);
}
/* nr_gpu_choose_model: selects the memory model from the probed capabilities and an override string ("auto", "shared" or "discrete"; unknown values give NONE).
 *   Auto chooses SHARED for integrated devices or devices with pageable memory access and host page tables, else DISCRETE. Returns NR_GPU_NONE when the device is unavailable.
 */
nr_gpu_model_t nr_gpu_choose_model(const nr_gpu_caps_t *c, const char *override)
{
  if (!c || !c->available) return NR_GPU_NONE;
  if (!override || !strcmp(override, "auto"))
    return c->integrated || (c->pageable_memory_access && c->host_page_tables) ? NR_GPU_SHARED : NR_GPU_DISCRETE;
  if (!strcmp(override, "shared")) return NR_GPU_SHARED;
  if (!strcmp(override, "discrete")) return NR_GPU_DISCRETE;
  return NR_GPU_NONE;
}
/* nr_gpu_storage: maps a buffer kind (device, staging or shared read-write) to the allocation type for the model: device memory, mapped host memory, or managed memory when use_managed is set.
 *   Only the SHARED model allocates; any other model returns NR_GPU_ALLOC_UNAVAILABLE. Used by nr_gpu_alloc().
 */
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
/* nr_gpu_arch_compatible: true when a CUDA module built for the architecture list archs (for example "80,89-virtual") can run on the device with compute capability major.minor.
 *   A real (cubin) entry must match the major version and not exceed the device; a virtual (PTX) entry is forward compatible. Pure function. Caller: nr_gpu_module_check().
 */
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
/* load_runtime: loads libcudart (several SONAME fallbacks) and resolves every runtime entry point used by the platform into rt; returns 0 when any symbol is missing.
 *   The library stays resident for the life of the module. Called once from probe_locked().
 */
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
/* attribute: reads one device attribute (cudaDeviceGetAttribute) and returns it, or 0 when the attribute is missing on this driver (the error is cleared). */
static int attribute(int attr)
{
  int value=0;
  if (rt.attribute(&value, attr, caps.device)) {
    rt.get_last_error(); /* optional attributes on older drivers must not poison warm-up */
    return 0;
  }
  return value;
}
/* probe_locked: probes the GPU once (caller holds mu): loads the runtime, selects device 0, reads its capabilities, chooses the memory model (ISAC_GPU_MEMORY_MODEL) and the managed-memory option (ISAC_GPU_SHARED_MANAGED).
 *   Logs the device summary, a warning for DISCRETE (not implemented) and for an unusable shared model. Sets caps.available = 0 when the probe fails.
 */
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
/* nr_gpu_platform_probe: returns the GPU capabilities (probing on first use, under mu). Callers: nr_gpu_platform_ready(), nr_gpu_module_check() and the GPU module start-up code. */
const nr_gpu_caps_t *nr_gpu_platform_probe(void)
{
  pthread_mutex_lock(&mu); probe_locked(); pthread_mutex_unlock(&mu); return &caps;
}
/* nr_gpu_platform_ready: true when a device is available with the SHARED model, mapped host memory and unified addressing: the condition for any GPU path to run.
 *   Callers: nr_gpu_module_check(), nr_gpu_copy_async(), nr_gpu_sync(), nr_gpu_sync_all().
 */
int nr_gpu_platform_ready(void)
{
  const nr_gpu_caps_t *c=nr_gpu_platform_probe();
  return c->available && c->model==NR_GPU_SHARED && c->can_map_host_memory && c->unified_addressing;
}
/* nr_gpu_module_check: checks that a GPU module built for `archs` can run on the device; logs a warning and returns 0 otherwise (the caller then uses the CPU path).
 *   Returns 0 when the platform itself is not ready. Callers: the GPU module wrappers (nr_gpu_module.c) and the tests.
 */
int nr_gpu_module_check(const char *module, const char *archs)
{
  if (!nr_gpu_platform_ready()) return 0;
  if (nr_gpu_arch_compatible(caps.major,caps.minor,archs)) return 1;
  report(1,"SENSING: GPU_PLATFORM module=%s built=%s device=sm_%d%d incompatible; CPU fallback",module,archs,caps.major,caps.minor);
  return 0;
}
/* nr_gpu_rx_started: seals the allocation gate: after this, allocations, registrations and frees are denied (counted and warned once).
 *   Called once when the RX stream starts, so that no GPU memory is allocated during reception.
 */
void nr_gpu_rx_started(void)
{
  pthread_mutex_lock(&mu); __atomic_store_n(&sealed,1,__ATOMIC_RELEASE); pthread_mutex_unlock(&mu);
}
/* nr_gpu_allocations_allowed: returns 1 while the allocation gate is open (before nr_gpu_rx_started()), else 0. Lock-free acquire read. */
int nr_gpu_allocations_allowed(void) { return !__atomic_load_n(&sealed,__ATOMIC_ACQUIRE); }
/* allowed_locked: checks the allocation gate with mu held: returns 1 before the seal; after it, counts the denial, warns once, and returns 0. */
static int allowed_locked(void)
{
  if (!sealed) return 1;
  ++denied;
  if (!warned_late++) report(1,"SENSING: GPU_PLATFORM allocation/registration/free after RX start denied; CPU fallback");
  return 0;
}
/* nr_gpu_allocation_guard: takes mu and returns the allocation gate state (1 allowed, 0 sealed). Used by the GPU modules to guard their own allocations.
 *   Callers: GPU module sources and the hygiene test.
 */
int nr_gpu_allocation_guard(void)
{
  pthread_mutex_lock(&mu);
  int ok=allowed_locked();
  pthread_mutex_unlock(&mu);
  return ok;
}
/* nr_gpu_memory_stats: returns the number of successful allocations (*a) and of denied operations (*d) since start-up. */
void nr_gpu_memory_stats(uint64_t *a,uint64_t *d)
{
  pthread_mutex_lock(&mu); *a=allocations; *d=denied; pthread_mutex_unlock(&mu);
}
/* lookup: finds the live allocation whose host address is ptr (linear walk of the list; caller holds mu). Returns NULL when absent. */
static allocation *lookup(void *ptr)
{
  for (allocation *a=live;a;a=a->next) if (a->host==ptr) return a;
  return NULL;
}
/* nr_gpu_alloc: allocates a buffer of the given kind and size for the platform's model: device memory, mapped pinned host memory (with its device pointer) or managed memory.
 *   Returns the host pointer (use nr_gpu_device_ptr() for the device side), or NULL when denied, unavailable or out of memory. Tracked in the live list for nr_gpu_free().
 */
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
/* nr_gpu_device_ptr: returns the device pointer of a buffer from nr_gpu_alloc() or nr_gpu_register_host(), or NULL when ptr is not tracked. */
void *nr_gpu_device_ptr(void *ptr)
{
  pthread_mutex_lock(&mu);
  allocation *a=lookup(ptr);
  void *p=a ? a->device : NULL;
  pthread_mutex_unlock(&mu); return p;
}
/* release: frees a tracked allocation (host-registered or allocated) after checking the gate, the kind and the pointer; removes it from the live list only on success.
 *   Returns the CUDA status, or GPU_NOT_READY/GPU_ERROR. Used by nr_gpu_free() (registered = 0) and nr_gpu_unregister_host() (registered = 1).
 */
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
/* nr_gpu_free: frees a buffer from nr_gpu_alloc(). Returns 0 on success, or an error code. Callers: GPU modules and tests. */
int nr_gpu_free(void *ptr) { return release(ptr,0); }
/* nr_gpu_unregister_host: unregisters a host buffer registered with nr_gpu_register_host(). Returns 0 on success, or an error code. */
int nr_gpu_unregister_host(void *ptr) { return release(ptr,1); }
/* nr_gpu_register_host: registers an existing host buffer as mapped and portable, and records its device pointer; a second registration of the same buffer returns 0 when the size matches.
 *   Denied after the seal. Returns 0, or an error code (GPU_NO_MEMORY, GPU_ERROR). Callers: GPU modules and tests.
 */
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
/* nr_gpu_copy_async: asynchronous copy (cudaMemcpyDefault) of bytes on stream; returns GPU_NOT_READY when the platform is not ready. */
int nr_gpu_copy_async(void *dst,const void *src,size_t bytes,void *stream)
{
  if (!nr_gpu_platform_ready()) return GPU_NOT_READY;
  return rt.copy_async(dst,src,bytes,4,stream); /* cudaMemcpyDefault */
}
/* nr_gpu_sync: waits for a stream to finish (cudaStreamSynchronize); returns GPU_NOT_READY when the platform is not ready. */
int nr_gpu_sync(void *stream)
{
  if (!nr_gpu_platform_ready()) return GPU_NOT_READY;
  return rt.stream_sync(stream);
}
/* nr_gpu_sync_all: waits for all device work (cudaDeviceSynchronize); returns GPU_NOT_READY when the platform is not ready. */
int nr_gpu_sync_all(void)
{
  if (!nr_gpu_platform_ready()) return GPU_NOT_READY;
  return rt.device_sync();
}
#ifdef NR_GPU_PLATFORM_TEST
/* nr_gpu_test_reset: test-only (NR_GPU_PLATFORM_TEST): frees the tracked allocations, installs the given runtime table and clears the probe, seal and counters.
 *   The caller must have joined all GPU users first. Caller: openair1/PHY/TOOLS/tests/nr_gpu_platform_test.cc.
 */
void nr_gpu_test_reset(const nr_gpu_runtime_t *runtime)
{
  /* Test-only; caller has joined all users and released allocations first. */
  while (live) { allocation *a=live; live=a->next; free(a); }
  rt=*runtime; memset(&caps,0,sizeof(caps));
  probed=sealed=managed=warned_late=0; allocations=denied=0;
}
#endif
