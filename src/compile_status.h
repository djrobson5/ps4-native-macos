#pragma once
/* bb-probe --compile-status <data dir> (compile_status.c): one line of JSON on stdout, whether
 * Bloodborne's shader compile step is due. `applicable` 0 (another title's app): the answer
 * {"applicable":false,"reason":"title"}. Returns the exit code. Creates and writes nothing. */
int bb_compile_status(const char *data_dir, int applicable);
