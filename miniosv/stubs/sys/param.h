/*
 * miniOSv: minimal <sys/param.h>.
 *
 * jemalloc includes this unconditionally on non-Windows targets
 * (third_party/jemalloc/include/jemalloc/internal/jemalloc_internal_decls.h:20)
 * out of habit -- it is a BSD grab-bag header. llvm-libc does not ship one.
 * Everything jemalloc actually uses from it is below; the header exists so the
 * include resolves.
 *
 * Only on the include path for DuckDB translation units, so the kernel's own
 * view of the libc surface is unchanged.
 */

#ifndef MINIOSV_SYS_PARAM_H
#define MINIOSV_SYS_PARAM_H

#include <limits.h>

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

#ifndef howmany
#define howmany(x, y) (((x) + ((y) - 1)) / (y))
#endif
#ifndef roundup
#define roundup(x, y) (howmany(x, y) * (y))
#endif
#ifndef rounddown
#define rounddown(x, y) (((x) / (y)) * (y))
#endif

#ifndef NBBY
#define NBBY CHAR_BIT
#endif

#endif /* MINIOSV_SYS_PARAM_H */
