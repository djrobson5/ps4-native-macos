/* The start of a Mac app built around bb-probe (scripts/package-13s-app.sh: 13 Sentinels.app).
 * The bundle's executable is bb-probe itself, not a script that execs it: macOS then loses the
 * game's process ID, and with it the Home button override and window checks of a launcher such as
 * Wayfarer. So what scripts/run-13s.sh sets up is done here, at the top of main(), when the
 * executable sits in an app's Contents/MacOS and is started without a game argument (the Finder,
 * `open`, a launcher). See app_bundle.h. */
#include "app_bundle.h"

#ifdef __APPLE__
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <SDL3/SDL.h>

#define APP_GAME_FOLDER "CUSA19610"
#define APP_DATA_NAME "13 Sentinels"
#define APP_LOGS_KEPT 20

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

/* wayfarer.cfg: `key=value` lines, `#` comments. Only renderer and fullscreen are read. */
static void read_cfg(const char *path, char *renderer, size_t renderer_size, int *fullscreen) {
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
        if (!strcmp(key, "renderer")) {
            if (!strcmp(value, "metal") || !strcmp(value, "moltenvk")) snprintf(renderer, renderer_size, "%s", value);
            else fprintf(stderr, "App: wayfarer.cfg: renderer=%s is not metal or moltenvk; ignored\n", value);
        } else if (!strcmp(key, "fullscreen")) {
            if (!strcmp(value, "0") || !strcmp(value, "1")) *fullscreen = value[0] == '1';
            else fprintf(stderr, "App: wayfarer.cfg: fullscreen=%s is not 0 or 1; ignored\n", value);
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
/* Keeps the newest APP_LOGS_KEPT - 1, before this start's log is made. */
static void prune_logs(const char *logs) {
    struct dirent **names = NULL;
    const int count = scandir(logs, &names, is_log, newer_first);
    for (int i = 0; i < count; ++i) {
        if (i >= APP_LOGS_KEPT - 1) {
            char path[PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s", logs, names[i]->d_name);
            unlink(path);
        }
        free(names[i]);
    }
    free(names);
}

static void fail_box(const char *message) {
    fprintf(stderr, "App: %s\n", message);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "13 Sentinels", message, NULL);
    exit(1);
}

void bb_app_bundle_bootstrap(int *argc, char ***argv) {
    /* Only an app's executable (…/Name.app/Contents/MacOS/…) without a game argument: `bb-probe
     * - --app0 …` or a boot file as the first argument is a developer's run and is left alone. */
    char raw[PATH_MAX], exe[PATH_MAX];
    uint32_t size = sizeof(raw);
    if (_NSGetExecutablePath(raw, &size) != 0 || !realpath(raw, exe)) return;
    char *macos = strstr(exe, ".app/Contents/MacOS/");
    if (!macos) return;
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

    /* The game's folder beside the app (eboot.bin, sce_module/, sce_sys/). */
    static char game[PATH_MAX];
    snprintf(game, sizeof(game), "%s/" APP_GAME_FOLDER, app_dir);
    char eboot[PATH_MAX];
    snprintf(eboot, sizeof(eboot), "%s/eboot.bin", game);
    if (access(eboot, R_OK) != 0) {
        char message[PATH_MAX + 128];
        snprintf(message, sizeof(message), "The game's folder is missing: %s (it holds eboot.bin).", game);
        fail_box(message);
    }

    /* The data folder: S13_APP_DATA_DIR (checks), else ~/Library/Application Support/13 Sentinels. */
    static char data[PATH_MAX];
    const char *override = getenv("S13_APP_DATA_DIR");
    const char *home = getenv("HOME");
    char cwd[PATH_MAX];
    if (override && *override && override[0] != '/' && getcwd(cwd, sizeof(cwd)))
        snprintf(data, sizeof(data), "%s/%s", cwd, override); /* absolute: the bootstrap changes to it below */
    else if (override && *override) snprintf(data, sizeof(data), "%s", override);
    else if (home && *home) snprintf(data, sizeof(data), "%s/Library/Application Support/" APP_DATA_NAME, home);
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

    /* The player's choices: wayfarer.cfg, then S13_RENDERER as run-13s.sh reads it. */
    char renderer[16] = "metal";
    int fullscreen = 1;
    read_cfg(cfg, renderer, sizeof(renderer), &fullscreen);
    const char *renderer_env = getenv("S13_RENDERER");
    if (renderer_env && (!strcmp(renderer_env, "metal") || !strcmp(renderer_env, "moltenvk")))
        snprintf(renderer, sizeof(renderer), "%s", renderer_env);

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
    setenv("BB_APP_BUNDLE", "1", 1); /* the menu shows no Restart button (runtime_restart) */

    /* The arguments run-13s.sh passes; flags given to the app (--headless, --timeout N) follow
     * and win, since bb-probe takes the last of each. */
    static char *args[64];
    int n = 0;
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

    const char *dry = getenv("S13_APP_DRY_RUN");
    if (dry && !strcmp(dry, "1")) {
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

    /* The output goes to logs/<date>.log (run-13s.sh tees it there); S13_APP_LOG=- keeps it on
     * the terminal. */
    const char *log_choice = getenv("S13_APP_LOG");
    if (!(log_choice && !strcmp(log_choice, "-"))) {
        prune_logs(logs);
        char stamp[32], log[PATH_MAX];
        const time_t now = time(NULL);
        struct tm local;
        localtime_r(&now, &local);
        strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
        snprintf(log, sizeof(log), "%s/%s.log", logs, stamp);
        const int fd = open(log, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
    }
    printf("App: %s; data %s; renderer %s; fullscreen %s\n", app, data, renderer, getenv("BB_FULLSCREEN"));

    /* Relative paths (bbport.ini's default and the like) land in the data folder, never in the
     * app or wherever the Finder started it. */
    if (chdir(data) != 0) fprintf(stderr, "App: cannot change to %s: %s\n", data, strerror(errno));

    *argc = n;
    *argv = args;
}
#endif
