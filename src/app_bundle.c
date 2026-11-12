/* The start of a Mac app built around bb-probe (scripts/package-app.sh: 13 Sentinels.app and
 * Bloodborne.app). The bundle's executable is bb-probe itself, not a script that execs it: macOS
 * then loses the game's process ID, and with it the Home button override and window checks of a
 * launcher such as Wayfarer. So the launch is prepared here, at the top of main(), when the
 * executable sits in an app's Contents/MacOS and is started without a game argument (the Finder,
 * `open`, a launcher): for 13 Sentinels what scripts/run-13s.sh sets up, in C; for Bloodborne
 * what the real run.sh sets up (it is 350 lines of settings, patches and memory sizing that are not
 * written twice): run.sh runs once with BB_LAUNCH_DUMP set and, where it would exec the game, the
 * probe writes the arguments, environment and directory it was given to that file and exits
 * (bb_app_launch_dump). This process then takes them over and carries on as the game. Which game
 * it is comes from the Info.plist key BBTitleID. See app_bundle.h. */
#include "app_bundle.h"

#ifdef __APPLE__
#include <errno.h>
#include <fcntl.h>
#include <crt_externs.h>
#include <glob.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <SDL3/SDL.h>

extern char **environ;

#define APP_LOGS_KEPT 20
#define APP_DUMP_MAGIC "BBLAUNCH1"

/* The two games an app can hold (the Info.plist's BBTitleID). */
typedef struct {
    const char *id;         /* BBTitleID, and the game's folder beside the app */
    const char *name;       /* the app's name, the data folder's name under Application Support */
    const char *data_env;   /* overrides the data folder (the checks use it) */
    int bloodborne;         /* 1: prepared by run.sh; 0: by this file */
} Title;
static const Title TITLE_13S = {"CUSA19610", "13 Sentinels", "S13_APP_DATA_DIR", 0};
static const Title TITLE_BB = {"CUSA03173", "Bloodborne", "BB_APP_DATA_DIR", 1};
static const Title *title = &TITLE_13S;
static int dry_run; /* BB_APP_DRY_RUN=1 (or S13_APP_DRY_RUN=1 for 13 Sentinels): nothing opens, nothing is asked */

static int make_dir(const char *path) {
    return mkdir(path, 0755) == 0 || errno == EEXIST ? 0 : -1;
}
/* make_dir with the missing parents too (mkdir -p). */
static int make_dirs(const char *path) {
    char partial[PATH_MAX];
    snprintf(partial, sizeof(partial), "%s", path);
    for (char *p = partial + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = 0;
        if (make_dir(partial)) return -1;
        *p = '/';
    }
    return make_dir(partial);
}

static void trim(char *s) {
    char *start = s;
    while (*start == ' ' || *start == '\t') ++start;
    if (start != s) memmove(s, start, strlen(start) + 1);
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
}

/* wayfarer.cfg: `key=value` lines, `#` comments. -1 (or "") is "not given". */
typedef struct {
    char renderer[16];  /* metal | moltenvk */
    int fullscreen;     /* 0 | 1 */
    char display[8];    /* a monitor's number, 1 on: BB_DISPLAY */
    int vsync;          /* 1: BB_PRESENT_MODE=Fifo, 0: Immediate */
    char fps_mode[8];   /* uncap | 90 | 60 | 30 (Bloodborne: BB_FPS) */
    int fps_limit;      /* 0..480: BB_FPS_LIMIT */
    int metal_hud;      /* 1: MTL_HUD_ENABLED=1 */
    int frame_log;      /* 1: the frame statistics, to logs/<stamp>.frames.csv */
} Options;

static int is_number(const char *value, int max, int *out) {
    if (!*value || strlen(value) > 4) return 0;
    int n = 0;
    for (const char *p = value; *p; ++p) {
        if (*p < '0' || *p > '9') return 0;
        n = n * 10 + (*p - '0');
    }
    if (n > max) return 0;
    *out = n;
    return 1;
}

static void read_cfg(const char *path, Options *o) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = line, *value = eq + 1;
        trim(key);
        trim(value);
        int n = 0;
        if (!strcmp(key, "renderer")) {
            if (!strcmp(value, "metal") || !strcmp(value, "moltenvk")) snprintf(o->renderer, sizeof(o->renderer), "%s", value);
            else fprintf(stderr, "App: wayfarer.cfg: renderer=%s is not metal or moltenvk; ignored\n", value);
        } else if (!strcmp(key, "fullscreen")) {
            if (!strcmp(value, "0") || !strcmp(value, "1")) o->fullscreen = value[0] == '1';
            else fprintf(stderr, "App: wayfarer.cfg: fullscreen=%s is not 0 or 1; ignored\n", value);
        } else if (!strcmp(key, "display")) {
            /* 0 is "the primary display": BB_DISPLAY stays unset (it counts from 1). */
            if (is_number(value, 64, &n)) snprintf(o->display, sizeof(o->display), "%s", n ? value : "");
            else fprintf(stderr, "App: wayfarer.cfg: display=%s is not a monitor number; ignored\n", value);
        } else if (!strcmp(key, "vsync")) {
            if (!strcmp(value, "0") || !strcmp(value, "1")) o->vsync = value[0] == '1';
            else fprintf(stderr, "App: wayfarer.cfg: vsync=%s is not 0 or 1; ignored\n", value);
        } else if (!strcmp(key, "fps_mode")) {
            if (!strcmp(value, "uncap") || !strcmp(value, "90") || !strcmp(value, "60") || !strcmp(value, "30"))
                snprintf(o->fps_mode, sizeof(o->fps_mode), "%s", value);
            else fprintf(stderr, "App: wayfarer.cfg: fps_mode=%s is not uncap, 90, 60 or 30; ignored\n", value);
        } else if (!strcmp(key, "fps_limit")) {
            if (is_number(value, 480, &n)) o->fps_limit = n;
            else fprintf(stderr, "App: wayfarer.cfg: fps_limit=%s is not a number from 0 to 480; ignored\n", value);
        } else if (!strcmp(key, "metal_hud")) {
            if (!strcmp(value, "0") || !strcmp(value, "1")) o->metal_hud = value[0] == '1';
            else fprintf(stderr, "App: wayfarer.cfg: metal_hud=%s is not 0 or 1; ignored\n", value);
        } else if (!strcmp(key, "frame_log")) {
            if (!strcmp(value, "0") || !strcmp(value, "1")) o->frame_log = value[0] == '1';
            else fprintf(stderr, "App: wayfarer.cfg: frame_log=%s is not 0 or 1; ignored\n", value);
        } else {
            fprintf(stderr, "App: wayfarer.cfg: %s is not a known key; ignored\n", key);
        }
    }
    fclose(f);
}

static void set_default(const char *name, const char *value) {
    setenv(name, value, 0); /* what the environment already has wins, as in run-13s.sh's ${X:-...} */
}
static void set_always(const char *name, const char *value) {
    setenv(name, value, 1); /* run-13s.sh exports these outright */
}

static int newer_first(const struct dirent **a, const struct dirent **b) {
    return strcmp((*b)->d_name, (*a)->d_name); /* names are dates: YYYYMMDD-HHMMSS.log */
}
static int is_log(const struct dirent *entry) {
    const size_t n = strlen(entry->d_name);
    return n > 4 && !strcmp(entry->d_name + n - 4, ".log");
}
/* Keeps the newest APP_LOGS_KEPT - 1, before this start's log is made; a removed log takes the
 * frame statistics of its start (<stamp>.frames.csv) with it. */
static void prune_logs(const char *logs) {
    struct dirent **names = NULL;
    const int count = scandir(logs, &names, is_log, newer_first);
    for (int i = 0; i < count; ++i) {
        if (i >= APP_LOGS_KEPT - 1) {
            char path[PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s", logs, names[i]->d_name);
            unlink(path);
            const size_t n = strlen(path);
            snprintf(path + n - 4, sizeof(path) - n + 4, ".frames.csv");
            unlink(path);
        }
        free(names[i]);
    }
    free(names);
}

/* A dry run never opens a window (a check must not wait for someone to click it away). */
static void fail_box(const char *message) {
    fprintf(stderr, "App: %s\n", message);
    if (!dry_run) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title->name, message, NULL);
    exit(1);
}


/* The Info.plist's BBTitleID, by a plain read of the XML the packaging writes (CFBundle would be a
 * new link for one string). 0: no key. */
static int plist_title_id(const char *contents, char *id, size_t size) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/Info.plist", contents);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    static char text[65536];
    const size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = 0;
    const char *key = strstr(text, "<key>BBTitleID</key>");
    if (!key) return 0;
    const char *open = strstr(key, "<string>");
    if (!open) return 0;
    open += strlen("<string>");
    const char *close = strstr(open, "</string>");
    if (!close || (size_t)(close - open) >= size) return 0;
    memcpy(id, open, (size_t)(close - open));
    id[close - open] = 0;
    return 1;
}

/* Reads a whole file, NUL-terminated after its last byte; NULL when it cannot. */
static char *read_all(const char *path, size_t *length) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t capacity = 1 << 16, used = 0;
    char *data = malloc(capacity + 1);
    for (size_t got; data && (got = fread(data + used, 1, capacity - used, f)) > 0;) {
        used += got;
        if (used == capacity) {
            capacity *= 2;
            data = realloc(data, capacity + 1);
        }
    }
    fclose(f);
    if (!data) return NULL;
    data[used] = 0;
    *length = used;
    return data;
}

/* What run.sh's last line would exec, written instead (the app's preparation of Bloodborne). The
 * file is a run of NUL-ended records, so no argument or value can be mistaken for another:
 *   BBLAUNCH1  A<argument>...  V<NAME=value>...  C<directory>  Z (the end: a file without it was cut short)
 * Called first in main(): only with BB_LAUNCH_DUMP set and `-` as the first argument (what
 * run.sh passes; its --print-scaled question is answered as always). Then it exits. */
void bb_app_launch_dump(int argc, char **argv) {
    const char *path = getenv("BB_LAUNCH_DUMP");
    if (!path || !*path || argc < 2 || strcmp(argv[1], "-")) return;
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "BB_LAUNCH_DUMP: cannot write %s: %s\n", path, strerror(errno));
        exit(1);
    }
    fputs(APP_DUMP_MAGIC, f);
    fputc(0, f);
    for (int i = 0; i < argc; ++i) {
        fputc('A', f);
        fputs(argv[i], f);
        fputc(0, f);
    }
    for (char **e = environ; e && *e; ++e) {
        fputc('V', f);
        fputs(*e, f);
        fputc(0, f);
    }
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) cwd[0] = 0;
    fputc('C', f);
    fputs(cwd, f);
    fputc(0, f);
    fputc('Z', f);
    fputc(0, f);
    if (fclose(f) != 0) {
        fprintf(stderr, "BB_LAUNCH_DUMP: cannot write %s: %s\n", path, strerror(errno));
        exit(1);
    }
    exit(0);
}

/* The dump as this process's own launch: the arguments (the program's name first), the whole
 * environment in place of the current one (minus the two variables of the preparation), the
 * directory. 0 when the file is not a complete dump; the environment is then still the old one. */
static int take_over_dump(char *data, size_t length, char ***out_args, int *out_count, char *cwd, size_t cwd_size) {
    const size_t magic = strlen(APP_DUMP_MAGIC) + 1;
    if (length < magic || memcmp(data, APP_DUMP_MAGIC, magic)) return 0;
    char **args = calloc(1024, sizeof(char *));
    char **env = calloc(16384, sizeof(char *));
    int count = 0, env_count = 0, complete = 0, have_cwd = 0;
    for (size_t at = magic; args && env && at < length;) {
        char *record = data + at;
        at += strlen(record) + 1;
        if (record[0] == 'A' && count < 1023) args[count++] = record + 1;
        else if (record[0] == 'V' && env_count < 16383) {
            /* The preparation's own two variables stay behind; every other entry goes over whole,
             * whatever its name's length. */
            if (strncmp(record + 1, "BB_LAUNCH_DUMP=", 15) && strncmp(record + 1, "BB_APP_PREPARING=", 17))
                env[env_count++] = record + 1;
        } else if (record[0] == 'C') {
            snprintf(cwd, cwd_size, "%s", record + 1);
            have_cwd = 1;
        } else if (!strcmp(record, "Z")) complete = 1;
    }
    if (!args || !env || !complete || !have_cwd || count < 1) {
        free(args);
        free(env);
        return 0;
    }
    /* Installed in one step, not entry by entry: an entry unsetenv can't remove (an empty or very
     * long name) would otherwise keep the whole old environment, and with it what run.sh unset.
     * The array and its strings (in data) stay for the life of the process; setenv later copies
     * the array into one of libc's own. */
    env[env_count] = NULL;
    *_NSGetEnviron() = env;
    *out_args = args;
    *out_count = count;
    return 1;
}

/* The last lines of the log (at most 8, 1500 bytes), for the error box. */
static void log_tail(const char *log, char *out, size_t size) {
    out[0] = 0;
    size_t length = 0;
    char *text = read_all(log, &length);
    if (!text) return;
    while (length && text[length - 1] == '\n') text[--length] = 0;
    size_t from = length > 1500 ? length - 1500 : 0;
    int lines = 0;
    for (size_t i = length; i > from; --i)
        if (text[i - 1] == '\n' && ++lines == 8) {
            from = i;
            break;
        }
    snprintf(out, size, "%s", text + from);
    free(text);
}

static int cmp_strings(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}
static int dry_run_variable(const char *entry) {
    static const char *prefixes[] = {"BB_", "MTL_", "VK_", "MVK_"};
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i)
        if (!strncmp(entry, prefixes[i], strlen(prefixes[i]))) return 1;
    return !strncmp(entry, "PYTHON=", 7);
}
/* The dry run of the new kind (BB_APP_DRY_RUN=1): what the launch is, by the variables that matter,
 * sorted so two runs can be compared. */
static void print_dry_run(const char *app, const char *data, char **args, int count) {
    printf("App: %s\nApp: data %s\nApp: title %s\nApp: arguments:", app, data, title->id);
    for (int i = 0; i < count; ++i) printf(" [%s]", args[i]);
    printf("\n");
    const char *shown[16384];
    int n = 0;
    for (char **e = environ; e && *e && n < 16384; ++e)
        if (dry_run_variable(*e)) shown[n++] = *e;
    qsort(shown, (size_t)n, sizeof(shown[0]), cmp_strings);
    for (int i = 0; i < n; ++i) printf("App: %s\n", shown[i]);
}

/* Bloodborne: the real run.sh prepares the launch (bash and Python come with the bundle). Returns
 * the arguments and directory it ended on. */
static void prepare_with_run_sh(const char *contents, const char *exe, const char *argv0, const char *game, const char *data,
        const char *user, const char *config, char *const *given, int given_count, const char *log,
        char ***out_args, int *out_count, char *cwd, size_t cwd_size) {
    char bash[PATH_MAX], script[PATH_MAX], python[PATH_MAX], out_dir[PATH_MAX], dump[PATH_MAX];
    snprintf(bash, sizeof(bash), "%s/MacOS/bash", contents);
    snprintf(script, sizeof(script), "%s/Resources/game/run.sh", contents);
    snprintf(python, sizeof(python), "%s/Resources/python/bin/python3", contents);
    if (access(bash, X_OK) != 0 || access(script, R_OK) != 0 || access(python, X_OK) != 0)
        fail_box("The app is incomplete: its bash, python or run.sh is missing. Install it again.");
    snprintf(out_dir, sizeof(out_dir), "%s/out", data);
    snprintf(dump, sizeof(dump), "%s/launch-%d.dump", out_dir, (int)getpid());
    if (make_dir(out_dir)) {
        char message[PATH_MAX + 64];
        snprintf(message, sizeof(message), "Cannot make %s: %s", out_dir, strerror(errno));
        fail_box(message);
    }
    /* Dumps left by a start that was force-quit while run.sh ran (run.sh then carries on and
     * writes one nobody reads). */
    char stale[PATH_MAX + 32];
    snprintf(stale, sizeof(stale), "%s/launch-*.dump", out_dir);
    glob_t found;
    if (glob(stale, 0, NULL, &found) == 0) {
        for (size_t i = 0; i < found.gl_pathc; ++i) unlink(found.gl_pathv[i]);
        globfree(&found);
    }

    /* What run.sh is told: the bundle's own parts, never the environment's. The dumped result
     * replaces this environment, so setting it here is all there is to pass. */
    set_always("BB_PREBUILT", "1");
    set_always("PYTHON", python);
    set_always("BB_GAME_DIR", game);
    set_always("BB_DATA_DIR", data);
    set_always("BB_CONFIG", config);
    set_always("BB_USER_DIR", user);
    /* run.sh's mod view runs the game in the background and removes the view when it ends, which a
     * handover can't keep: mods are off in the app, and the log says so. */
    set_always("BB_MODS_ENABLED", "0");
    fprintf(stderr, "App: mods are off in the app (run.sh's mod view can't be handed over)\n");
    /* run.sh's own log (its tee and fd 3) doesn't survive the handover: the app keeps the log. */
    unsetenv("BB_SAVE_LOG");
    set_always("BB_PROBE", exe);
    set_always("BB_PROBE_PROGRAM", exe); /* run.sh's own --print-scaled question */
    set_always("BB_LAUNCH_DUMP", dump);
    set_always("BB_APP_PREPARING", "1");
    set_always("PYTHONDONTWRITEBYTECODE", "1"); /* the signed bundle gets no __pycache__ (an empty value would) */

    char *argv_run[64];
    int n = 0;
    argv_run[n++] = bash;
    argv_run[n++] = script;
    for (int i = 0; i < given_count && n < 62; ++i) argv_run[n++] = given[i];
    argv_run[n] = NULL;

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0); /* a launcher's channel on stdin is the game's */
    if (dry_run) posix_spawn_file_actions_adddup2(&actions, STDERR_FILENO, STDOUT_FILENO); /* stdout stays the dry run's */
    pid_t pid;
    int status = -1;
    signal(SIGCHLD, SIG_DFL); /* an inherited SIG_IGN would make waitpid lose run.sh's status */
    const int spawned = posix_spawn(&pid, bash, &actions, NULL, argv_run, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (spawned != 0) {
        char message[PATH_MAX + 64];
        snprintf(message, sizeof(message), "Cannot start run.sh: %s", strerror(spawned));
        fail_box(message);
    }
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    size_t length = 0;
    char *data_text = (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? read_all(dump, &length) : NULL;
    unlink(dump);
    if (!data_text || !take_over_dump(data_text, length, out_args, out_count, cwd, cwd_size)) {
        char tail[1600], message[2048];
        if (log) log_tail(log, tail, sizeof(tail));
        else tail[0] = 0;
        if (WIFSIGNALED(status)) snprintf(message, sizeof(message), "run.sh stopped with signal %d.", WTERMSIG(status));
        else if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
            snprintf(message, sizeof(message), "run.sh failed (exit code %d).", WEXITSTATUS(status));
        else snprintf(message, sizeof(message), "run.sh did not hand over a launch.");
        if (tail[0]) {
            const size_t used = strlen(message);
            snprintf(message + used, sizeof(message) - used, "\n\n%s", tail);
        } else if (!log) {
            const size_t used = strlen(message);
            snprintf(message + used, sizeof(message) - used, " Its output is above.");
        }
        fail_box(message);
    }
    /* The dumped argument 0 is the program as run.sh called it: this process's own name stands in.
     * The arguments point into data_text, which stays for the life of the process. */
    (*out_args)[0] = (char *)argv0;
}

void bb_app_bundle_bootstrap(int *argc, char ***argv) {
    /* Only an app's executable (…/Name.app/Contents/MacOS/…) without a game argument: `bb-probe
     * - --app0 …` or a boot file as the first argument is a developer's run and is left alone. A
     * program run.sh started for its own questions is not a start either (BB_APP_PREPARING). */
    char raw[PATH_MAX], exe[PATH_MAX];
    uint32_t size = sizeof(raw);
    if (_NSGetExecutablePath(raw, &size) != 0 || !realpath(raw, exe)) return;
    char *macos = strstr(exe, ".app/Contents/MacOS/");
    if (!macos) return;
    if (getenv("BB_APP_PREPARING")) return;
    /* AppKit's own arguments (-psn_…, -ApplePersistenceIgnoreState YES, -NSDocumentRevisionsDebugMode YES):
     * one dash and a name, perhaps a value. They are left out of bb-probe's arguments. */
    static char *given[64];
    int given_count = 0;
    for (int i = 1; i < *argc; ++i) {
        const char *arg = (*argv)[i];
        if (arg[0] == '-' && arg[1] && arg[1] != '-') {
            if (i + 1 < *argc && (*argv)[i + 1][0] != '-') ++i;
            continue;
        }
        if (given_count == 0 && strncmp(arg, "--", 2) != 0) return; /* `-` or a boot file: a developer's run */
        if (given_count < 48) given[given_count++] = (*argv)[i];
    }

    char app[PATH_MAX], contents[PATH_MAX], app_dir[PATH_MAX];
    snprintf(app, sizeof(app), "%.*s.app", (int)(macos - exe), exe);
    snprintf(contents, sizeof(contents), "%s/Contents", app);
    snprintf(app_dir, sizeof(app_dir), "%s", app);
    char *slash = strrchr(app_dir, '/');
    if (slash) *slash = 0;

    /* Which game this app holds: the Info.plist's BBTitleID; an app without it is 13 Sentinels. */
    char id[32];
    if (plist_title_id(contents, id, sizeof(id))) {
        if (!strcmp(id, TITLE_BB.id)) title = &TITLE_BB;
        else if (strcmp(id, TITLE_13S.id) != 0) {
            fprintf(stderr, "App: the Info.plist's BBTitleID is %s: not a title this program knows\n", id);
            exit(1);
        }
    }
    const char *dry_choice = getenv("BB_APP_DRY_RUN");
    const char *dry_legacy = title->bloodborne ? NULL : getenv("S13_APP_DRY_RUN"); /* 13 Sentinels' older switch: its older output */
    const int legacy_dry_run = dry_legacy && !strcmp(dry_legacy, "1");
    dry_run = legacy_dry_run || (dry_choice && !strcmp(dry_choice, "1"));

    /* The game's folder beside the app (eboot.bin, sce_module/, sce_sys/). */
    static char game[PATH_MAX];
    snprintf(game, sizeof(game), "%s/%s", app_dir, title->id);
    char eboot[PATH_MAX];
    snprintf(eboot, sizeof(eboot), "%s/eboot.bin", game);
    if (access(eboot, R_OK) != 0) {
        char message[PATH_MAX + 128];
        snprintf(message, sizeof(message), "The game's folder is missing: %s (it holds eboot.bin).", game);
        fail_box(message);
    }

    /* The data folder: <title>'s override (checks), else ~/Library/Application Support/<name>. */
    static char data[PATH_MAX];
    const char *override = getenv(title->data_env);
    const char *home = getenv("HOME");
    char cwd[PATH_MAX];
    if (override && *override && override[0] != '/' && getcwd(cwd, sizeof(cwd)))
        snprintf(data, sizeof(data), "%s/%s", cwd, override); /* absolute: the bootstrap changes directory below */
    else if (override && *override) snprintf(data, sizeof(data), "%s", override);
    else if (home && *home) snprintf(data, sizeof(data), "%s/Library/Application Support/%s", home, title->name);
    else fail_box("HOME is not set, so there is no data folder.");
    static char user[PATH_MAX];
    char logs[PATH_MAX], config[PATH_MAX], cfg[PATH_MAX];
    snprintf(user, sizeof(user), "%s/user", data);
    snprintf(logs, sizeof(logs), "%s/logs", data);
    snprintf(config, sizeof(config), "%s/bbport.ini", data);
    snprintf(cfg, sizeof(cfg), "%s/wayfarer.cfg", data);
    if (make_dirs(data) || make_dir(user) || make_dir(logs)) {
        char message[PATH_MAX + 64];
        snprintf(message, sizeof(message), "Cannot make the data folder %s: %s", data, strerror(errno));
        fail_box(message);
    }

    /* This start's stamp: its log's name, and its frame statistics' (frame_log=1). */
    char stamp[32];
    {
        const time_t now = time(NULL);
        struct tm local;
        localtime_r(&now, &local);
        strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
    }

    /* The player's choices: wayfarer.cfg, then S13_RENDERER as run-13s.sh reads it. 13 Sentinels
     * starts on Metal in fullscreen unless told otherwise; Bloodborne leaves what the cfg does not
     * say to run.sh and bbport.ini. */
    Options options = {"", -1, "", -1, "", -1, -1, -1};
    if (!title->bloodborne) {
        snprintf(options.renderer, sizeof(options.renderer), "metal");
        options.fullscreen = 1;
    }
    read_cfg(cfg, &options);
    if (!title->bloodborne) {
        const char *renderer_env = getenv("S13_RENDERER");
        if (renderer_env && (!strcmp(renderer_env, "metal") || !strcmp(renderer_env, "moltenvk")))
            snprintf(options.renderer, sizeof(options.renderer), "%s", renderer_env);
    }
    const char *renderer = options.renderer;
    const int fullscreen = options.fullscreen;

    if (!title->bloodborne) {
        /* What scripts/run-13s.sh exports, keep the two in step: set_always where it exports outright,
         * set_default where it keeps a value the environment has. */
        set_always("BB_GUEST_IN_PLACE", "0");
        if (!strcmp(renderer, "metal")) {
            set_always("BB_METAL_FRAME", "1");
            set_always("BB_METAL_NATIVE_BOOT", "1");
            set_always("BB_GFX_BACKEND", "metal");
            set_default("BB_QUADS_AS_TRIANGLES", "1");
            set_default("BB_FANS_AS_TRIANGLES", "1");
        } else {
            char icd[PATH_MAX], loader[PATH_MAX];
            snprintf(icd, sizeof(icd), "%s/Resources/game/share/vulkan/icd.d/MoltenVK_icd.json", contents);
            snprintf(loader, sizeof(loader), "%s/Frameworks/libvulkan.1.dylib", contents);
            set_always("BB_METAL_FRAME", "0");
            set_always("BB_METAL_NATIVE_BOOT", "0");
            set_always("BB_GFX_BACKEND", "vulkan");
            set_default("VK_DRIVER_FILES", icd);
            set_default("BB_VULKAN_LOADER", loader);
            set_default("MVK_CONFIG_LOG_LEVEL", "1");
        }
        set_always("BB_DATA_DIR", data);
        set_always("BB_GPU_USER_DIR", user);
        set_always("BB_CONFIG", config);
        set_default("BB_PREUPLOAD", "1");
        set_always("BB_UFFD", "0");
        set_default("BB_COPY_GPU_BUFFERS", "1");
        set_default("BB_GPU_WRITE_TWINS", "1");
        set_default("BB_GPU_WRITE_TWINS_MAX", "65536");
        set_default("BB_VBLANK_HZ", "60");
        set_default("BB_FULLSCREEN", fullscreen ? "1" : "0");
    } else {
        /* run.sh's own defaults stand (Metal, windowed per bbport.ini): only what the cfg says. */
        if (*renderer) set_default("BB_METAL_FRAME", !strcmp(renderer, "metal") ? "1" : "0");
        if (fullscreen >= 0) set_default("BB_FULLSCREEN", fullscreen ? "1" : "0");
    }
    /* The options both games have; a variable the environment already has wins, and nothing is set
     * for a key that is absent (or 0 where 0 is the default). */
    if (*options.display) set_default("BB_DISPLAY", options.display);
    if (options.vsync >= 0) set_default("BB_PRESENT_MODE", options.vsync ? "Fifo" : "Immediate");
    if (*options.fps_mode && title->bloodborne) set_default("BB_FPS", options.fps_mode); /* 13 Sentinels has no frame-rate patch */
    if (options.fps_limit > 0) {
        char limit[16];
        snprintf(limit, sizeof(limit), "%d", options.fps_limit);
        set_default("BB_FPS_LIMIT", limit);
    }
    if (options.metal_hud == 1) set_default("MTL_HUD_ENABLED", "1");
    if (options.frame_log == 1) {
        char frames[PATH_MAX];
        snprintf(frames, sizeof(frames), "%s/%s.frames.csv", logs, stamp);
        set_default("BB_FRAME_STATS", "1");
        set_default("BB_FRAME_LOG", frames);
    }

    /* The output goes to logs/<stamp>.log (run-13s.sh tees it there); BB_APP_LOG=- (13 Sentinels:
     * S13_APP_LOG=- too) keeps it on the terminal. A dry run prints. For Bloodborne the log is
     * made before run.sh runs, so its output is in it. */
    const char *log_choice = getenv("BB_APP_LOG");
    const char *log_choice_13s = title->bloodborne ? NULL : getenv("S13_APP_LOG");
    const int use_log = !dry_run && !(log_choice && !strcmp(log_choice, "-")) &&
                        !(log_choice_13s && !strcmp(log_choice_13s, "-"));
    char log[PATH_MAX] = "";
    if (use_log) snprintf(log, sizeof(log), "%s/%s.log", logs, stamp);
    int log_fd = -1;
    if (use_log) {
        prune_logs(logs);
        log_fd = open(log, O_WRONLY | O_CREAT | O_APPEND, 0644);
    }

    static char *args_13s[64];
    char **args = args_13s;
    int n = 0;
    if (!title->bloodborne) {
        /* The arguments run-13s.sh passes; flags given to the app (--headless, --timeout N) follow
         * and win, since bb-probe takes the last of each. */
        args[n++] = (*argv)[0];
        args[n++] = "-";
        args[n++] = "--app0";
        args[n++] = game;
        args[n++] = "--user";
        args[n++] = user;
        args[n++] = "--timeout";
        args[n++] = "0";
        for (int i = 0; i < given_count; ++i) args[n++] = given[i];
        args[n] = NULL;
        setenv("BB_APP_BUNDLE", "1", 1); /* the menu shows no Restart button (runtime_restart) */
        if (legacy_dry_run) {
            printf("App: %s\nApp: data %s\nApp: renderer %s, fullscreen %d\nApp: arguments:", app, data, renderer, fullscreen);
            for (int i = 0; i < n; ++i) printf(" [%s]", args[i]);
            printf("\n");
            static const char *names[] = {"BB_GUEST_IN_PLACE", "BB_METAL_FRAME", "BB_METAL_NATIVE_BOOT", "BB_GFX_BACKEND",
                "BB_QUADS_AS_TRIANGLES", "BB_FANS_AS_TRIANGLES", "VK_DRIVER_FILES", "BB_VULKAN_LOADER",
                "MVK_CONFIG_LOG_LEVEL", "BB_DATA_DIR", "BB_GPU_USER_DIR", "BB_CONFIG", "BB_PREUPLOAD", "BB_UFFD",
                "BB_COPY_GPU_BUFFERS", "BB_GPU_WRITE_TWINS", "BB_GPU_WRITE_TWINS_MAX", "BB_VBLANK_HZ", "BB_FULLSCREEN",
                "BB_APP_BUNDLE"};
            for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
                if (getenv(names[i])) printf("App: %s=%s\n", names[i], getenv(names[i]));
            exit(0);
        }
        if (dry_run) {
            print_dry_run(app, data, args, n);
            exit(0);
        }
    }
    if (log_fd >= 0) {
        dup2(log_fd, STDOUT_FILENO);
        dup2(log_fd, STDERR_FILENO);
        close(log_fd);
    }

    char run_cwd[PATH_MAX];
    snprintf(run_cwd, sizeof(run_cwd), "%s", data);
    if (title->bloodborne) {
        /* The real run.sh decides everything else (sizes, patches, GPU choices); what it would have
         * exec'd is taken over. */
        prepare_with_run_sh(contents, exe, (*argv)[0], game, data, user, config, given, given_count,
                            use_log ? log : NULL, &args, &n, run_cwd, sizeof(run_cwd));
        setenv("BB_APP_BUNDLE", "1", 1);
        if (dry_run) {
            print_dry_run(app, data, args, n);
            exit(0);
        }
    }
    if (title->bloodborne)
        printf("App: %s; data %s; title %s\n", app, data, title->id);
    else
        printf("App: %s; data %s; renderer %s; fullscreen %s\n", app, data, renderer, getenv("BB_FULLSCREEN"));

    /* Relative paths (bbport.ini's default and the like) land in the data folder, never in the
     * app or wherever the Finder started it; Bloodborne's are run.sh's: it changed to its own folder. */
    if (chdir(run_cwd) != 0) fprintf(stderr, "App: cannot change to %s: %s\n", run_cwd, strerror(errno));

    *argc = n;
    *argv = args;
}
#endif
