/*
 * miniOSv overrides for jemalloc's build configuration.
 *
 * Placed ahead of third_party/jemalloc/include on the include path, so this is
 * what jemalloc's sources see. It pulls in the real header and then adjusts the
 * few settings that do not hold on this kernel -- no patching of the vendored
 * tree.
 *
 * Most of the hard parts switch themselves off already: upstream guards
 * JEMALLOC_DSS (sbrk), JEMALLOC_HAVE_PRCTL and JEMALLOC_USE_SYSCALL behind
 * __GLIBC__, and miniOSv builds against llvm-libc. prctl in particular does not
 * exist here at all.
 */

#ifndef MINIOSV_JEMALLOC_INTERNAL_DEFS_H
#define MINIOSV_JEMALLOC_INTERNAL_DEFS_H

#include_next <jemalloc/internal/jemalloc_internal_defs.h>

/*
 * The one hard requirement. Upstream asks for global-dynamic TLS, but the
 * kernel is a static EXEC with no .dynamic and no __tls_get_addr, and is built
 * -ftls-model=local-exec throughout (Makefile). The per-variable attribute
 * would override that flag and fail to link.
 */
#undef JEMALLOC_TLS_MODEL
#define JEMALLOC_TLS_MODEL __attribute__((tls_model("local-exec")))

/*
 * __linux__ is defined, so jemalloc would probe /proc/sys/vm/overcommit_memory
 * at startup. There is no /proc: open() returns -1 and it silently concludes
 * the OS does not overcommit. Skip the pointless syscall.
 */
#undef JEMALLOC_PROC_SYS_VM_OVERCOMMIT_MEMORY

/*
 * No background purge thread for now. It needs pthread_create plus timed
 * condition waits, all of which miniOSv has, but it is one less moving part
 * while the port is being brought up. Re-enable by deleting this.
 */
#undef JEMALLOC_BACKGROUND_THREAD

/*
 * strerror_r comes in two incompatible flavours. miniOSv's libc declares the
 * GNU one -- char *strerror_r(int, char *, size_t) -- whenever _GNU_SOURCE is
 * set, which the kernel always sets (include/api/string.h:78-80). jemalloc
 * only expects that under __GLIBC__, so without this it takes the POSIX branch
 * and tries to return a char * from a function returning int.
 */
#ifndef JEMALLOC_STRERROR_R_RETURNS_CHAR_WITH_GNU_SOURCE
#define JEMALLOC_STRERROR_R_RETURNS_CHAR_WITH_GNU_SOURCE
#endif

#endif /* MINIOSV_JEMALLOC_INTERNAL_DEFS_H */
