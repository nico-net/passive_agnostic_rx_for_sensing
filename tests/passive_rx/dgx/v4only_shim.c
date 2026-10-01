/* LD_PRELOAD shim for hosts whose kernel has no IPv6 (e.g. some cloud containers).
 * The OAI rfsimulator server socket is AF_INET6 (dual-stack ::). This maps it to AF_INET/0.0.0.0 so the
 * unmodified nr-softmodem can serve on such hosts. Test tooling only; never used on the DGX (IPv6 present).
 * Known limits: bind() maps ANY v6 address to 0.0.0.0 (no per-address translation); connect() is NOT translated;
 * only the gNB server role is supported (the receiver is a client over IPv4 and runs without the shim).
 * build: gcc -shared -fPIC -O2 -o v4only_shim.so v4only_shim.c -ldl */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>

int socket(int domain, int type, int protocol) {
  static int (*real)(int, int, int);
  if (!real) real = dlsym(RTLD_NEXT, "socket");
  return real(domain == AF_INET6 ? AF_INET : domain, type, protocol);
}
int setsockopt(int fd, int level, int opt, const void *val, socklen_t len) {
  static int (*real)(int, int, int, const void *, socklen_t);
  if (!real) real = dlsym(RTLD_NEXT, "setsockopt");
  if (level == IPPROTO_IPV6) return 0;
  return real(fd, level, opt, val, len);
}
int bind(int fd, const struct sockaddr *a, socklen_t len) {
  static int (*real)(int, const struct sockaddr *, socklen_t);
  if (!real) real = dlsym(RTLD_NEXT, "bind");
  if (a && a->sa_family == AF_INET6) {
    const struct sockaddr_in6 *a6 = (const struct sockaddr_in6 *)a;
    struct sockaddr_in a4;
    memset(&a4, 0, sizeof a4);
    a4.sin_family = AF_INET;
    a4.sin_port = a6->sin6_port;
    return real(fd, (struct sockaddr *)&a4, sizeof a4);
  }
  return real(fd, a, len);
}
