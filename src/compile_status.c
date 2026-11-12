/* --compile-status <data dir> (bb-probe; also an app's executable): whether Bloodborne's one-time
 * shader compile step is due, as one line of JSON on stdout. Read-only: it builds the cache path
 * itself (<user dir>/cache, the user dir being $BB_USER_DIR or <data dir>/user, as run.sh
 * chooses) and creates nothing; the answer is the GPU library's own rule (bbgpu_compile_status),
 * the one the game's launch uses. Handled before any window, device or game exists. */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compile_status.h"
#include "gpu/bbgpu.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* A JSON string: quotes, backslashes and control characters escaped; bytes of UTF-8 kept. */
static void put_json_string(FILE *out, const char *s) {
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        switch (*p) {
        case '"': fputs("\\\"", out); break;
        case '\\': fputs("\\\\", out); break;
        case '\n': fputs("\\n", out); break;
        case '\r': fputs("\\r", out); break;
        case '\t': fputs("\\t", out); break;
        default:
            if (*p < 0x20 || *p == 0x7f) fprintf(out, "\\u%04x", *p);
            else fputc(*p, out);
        }
    }
    fputc('"', out);
}

int bb_compile_status(const char *data_dir, int applicable) {
    if (!applicable) {
        puts("{\"applicable\":false,\"reason\":\"title\"}");
        return 0;
    }
    const char *user = getenv("BB_USER_DIR");
    char cache[PATH_MAX];
    int n = (user && *user) ? snprintf(cache, sizeof(cache), "%s/cache", user)
                            : snprintf(cache, sizeof(cache), "%s/user/cache", data_dir);
    if (n < 0 || (size_t)n >= sizeof(cache)) {
        fprintf(stderr, "--compile-status: the path is too long\n");
        return 1;
    }
    BbCompileStatus s;
    bbgpu_compile_status(cache, &s);
    printf("{\"applicable\":true,\"due\":%s,\"reason\":", s.due ? "true" : "false");
    put_json_string(stdout, s.reason);
    printf(",\"built_states\":%u,\"seen_states\":%u,\"new_states_for_a_run\":%u,\"built_for\":",
           s.built_states, s.seen_states, s.new_states_for_a_run);
    put_json_string(stdout, s.built_for);
    fputs(",\"system\":", stdout);
    put_json_string(stdout, s.system);
    printf(",\"version\":%u,\"built_version\":%u}\n", s.version, s.built_version);
    return 0;
}
