// miniOSv shim for <libgen.h>.
//
// llvm-libc does not ship <libgen.h>. DuckDB uses it in exactly one spot —
// stripping the directory prefix from argv[0] to get a friendly process name
// for logging (src/main/database.cpp region). Provide the standard POSIX
// signatures with minimal, path-aware inline implementations that don't
// modify their argument (POSIX permits either behaviour; not mutating is
// friendlier when the caller passes a std::string's underlying data).
//
// If more of the POSIX libgen surface is ever needed, add it here rather
// than pulling in a full library port.

#ifndef _LIBGEN_H
#define _LIBGEN_H

#ifdef __cplusplus
extern "C" {
#endif

static inline char *basename(char *path)
{
    if (!path || !*path) {
        static char dot[] = ".";
        return dot;
    }
    char *last = path;
    for (char *c = path; *c; ++c) {
        if (*c == '/' && *(c + 1) != '\0') {
            last = c + 1;
        }
    }
    return last;
}

static inline char *dirname(char *path)
{
    static char dot[] = ".";
    if (!path || !*path) {
        return dot;
    }
    char *last_slash = nullptr;
    for (char *c = path; *c; ++c) {
        if (*c == '/') {
            last_slash = c;
        }
    }
    if (!last_slash) {
        return dot;
    }
    if (last_slash == path) {
        // Root "/…": dirname is "/".
        static char root[] = "/";
        return root;
    }
    *last_slash = '\0';
    return path;
}

#ifdef __cplusplus
}
#endif

#endif /* _LIBGEN_H */
