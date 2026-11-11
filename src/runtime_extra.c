/* Imports 13 Sentinels: Aegis Rim needs that no other runtime file owns: libc.prx start-up hooks,
 * POSIX rwlock names, offline dialog and Http answers, console features with no counterpart.
 *
 * Copied from saulob's 13 Sentinels PC port (github.com/saulob/13-sentinels-aegis-rim-pc/src/runtime_extra.c and runtime_tsr.c,
 * GPL-2), adapted to this runtime (NID-keyed tables, no Windows paths, no thread-name-by-handle
 * helper of its own). saulob's libc start-up and service contracts in turn follow Catherine: Full
 * Body PC and MediEvil PC (runtime_extra.c).
 *
 * Only 13 Sentinels is served from this table (runtime_13s_enable, set from probe.c when the
 * executable is 13 Sentinels'); Bloodborne's imports resolve exactly as before. */
#include "runtime.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define E_SRCH 3
#define E_INVAL 22
#define HTTP_NETWORK ((int32_t)0x80431063)
#define DIALOG_UNAVAILABLE ((int32_t)0x80B80001)

static int enabled;
void runtime_13s_enable(void) { enabled=1; }
int runtime_13s_enabled(void) { return enabled; }

static ABI int32_t ok_noop(void) { return 0; }
static ABI void *null_pointer(void) { return NULL; }
static ABI int32_t not_found(void) { return ERR(E_SRCH); }

/* ---- libc.prx start-up (SDK 7.00 and 9.50): thread-exit and sanitizer hooks the runtime does not need ---- */
typedef struct { int64_t tv_sec, tv_nsec; } GuestTimespec;
static ABI int32_t kernel_nanosleep(const GuestTimespec *request, GuestTimespec *remain) {
    if (!request) return ERR(E_INVAL);
    struct timespec t={(time_t)request->tv_sec,(long)request->tv_nsec};
    while (nanosleep(&t,&t) && errno==EINTR) {}
    if (remain) remain->tv_sec=remain->tv_nsec=0;
    return 0;
}
static ABI int32_t thread_name(void *thread, char *name) {
    if (!name) return ERR(E_INVAL);
    const char *n=runtime_thread_name_of(thread);
    if (!n) return ERR(E_SRCH);
    snprintf(name,32,"%s",n);
    return 0;
}
static ABI int32_t kernel_chmod(const char *path, uint16_t mode) { (void)path; (void)mode; return 0; }
static ABI int32_t kernel_utimes(const char *path, const void *times) { (void)path; (void)times; return 0; }

/* ---- POSIX rwlocks: errno results, not SCE codes ---- */
typedef int32_t (ABI *RwOne)(void **);
static int32_t posix_code(int32_t r) { return r ? (int32_t)((uint32_t)r & 0xffff) : 0; }
#define POSIX_RW(name,nid) static ABI int32_t posix_rw_##name(void **rw) { \
    return posix_code(((RwOne)runtime_rwlock_resolve(nid))(rw)); }
POSIX_RW(rdlock,"Ox9i0c7L5w0#libkernel")
POSIX_RW(wrlock,"mqdNorrB+gI#libkernel")
POSIX_RW(unlock,"+L98PIbGttk#libkernel")

/* ---- Dialogs the offline console never shows: opening fails, status stays idle ---- */
static ABI int32_t dialog_unavailable(void) { return DIALOG_UNAVAILABLE; }
static ABI int32_t dialog_none(void) { return 0; }

/* ---- HTTP without a network: settings are accepted, nothing can be read back ---- */
static ABI int32_t http_network(void) { return HTTP_NETWORK; }

/* ---- runtime_tsr.c: imports needed by this title's boot path ---- */
typedef struct { uint64_t size; uint32_t flag, get_segment_info; uint64_t *mspace_id_mask, *mstate_table; } HeapTraceInfo;

/* libc.prx's initialiser clears *mspace_id_mask unconditionally, so both pointers must be valid. */
static ABI int32_t libc_heap_trace_info(HeapTraceInfo *info) {
    static uint64_t mask, table[64];
    if (!info) return -1;
    info->get_segment_info = 0;
    info->mspace_id_mask = &mask;
    info->mstate_table = table;
    return 0;
}

/* The game asks sceKernelIsNeoMode to pick 1080p (base PS4) or 4K (PS4 Pro). */
static int32_t neo_mode;
static ABI int32_t is_neo_mode(void) { return neo_mode; }
void runtime_tsr_set_neo_mode(int on) { neo_mode = on != 0; }

static const RuntimeExport exports[]={
    {"_nanosleep",kernel_nanosleep}, {"scePthreadGetname",thread_name},
    {"sceKernelChmod",kernel_chmod}, {"sceKernelUtimes",kernel_utimes},
    {"_sceKernelRtldThreadAtexitIncrement",ok_noop}, {"_sceKernelRtldThreadAtexitDecrement",ok_noop},
    {"_sceKernelSetThreadAtexitCount",ok_noop}, {"_sceKernelSetThreadAtexitReport",ok_noop},
    {"_sceKernelSetThreadDtors",ok_noop},
    {"sceKernelIsAddressSanitizerEnabled",ok_noop},
    {"sceKernelGetSanitizerMallocReplaceExternal",null_pointer},
    {"sceKernelGetSanitizerNewReplaceExternal",null_pointer},
    {"sceKernelInternalMemoryGetModuleSegmentInfo",not_found},
    {"sceSysmoduleGetModuleInfoForUnwind",not_found},
    {"pthread_rwlock_rdlock",posix_rw_rdlock}, {"pthread_rwlock_wrlock",posix_rw_wrlock},
    {"pthread_rwlock_unlock",posix_rw_unlock},
    {"sceInvitationDialogInitialize",ok_noop}, {"sceInvitationDialogTerminate",ok_noop},
    {"sceInvitationDialogOpenA",dialog_unavailable}, {"sceInvitationDialogUpdateStatus",dialog_none},
    {"sceInvitationDialogGetResultA",dialog_unavailable},
    {"sceGameCustomDataDialogInitialize",ok_noop}, {"sceGameCustomDataDialogTerminate",ok_noop},
    {"sceGameCustomDataDialogOpenA",dialog_unavailable}, {"sceGameCustomDataDialogUpdateStatus",dialog_none},
    {"sceGameCustomDataDialogGetResultA",dialog_unavailable},
    {"sceSigninDialogOpen",dialog_unavailable}, {"sceSigninDialogUpdateStatus",dialog_none},
    {"sceSigninDialogGetResult",dialog_unavailable},
    {"sceNpProfileDialogOpenA",dialog_unavailable}, {"sceNpCommerceDialogGetResult",dialog_unavailable},
    {"sceNpCommerceShowPsStoreIcon",ok_noop}, {"sceNpCommerceHidePsStoreIcon",ok_noop},
    {"sceNpTrophyShowTrophyList",ok_noop}, {"sceNpTrophyCaptureScreenshot",ok_noop},
    {"sceHttpSetAuthEnabled",ok_noop}, {"sceHttpSetAuthInfoCallback",ok_noop}, {"sceHttpSetCookieEnabled",ok_noop},
    {"sceHttpSetRecvTimeOut",ok_noop}, {"sceHttpSetResolveRetry",ok_noop}, {"sceHttpSetResolveTimeOut",ok_noop},
    {"sceHttpSetSendTimeOut",ok_noop}, {"sceHttpsSetSslCallback",ok_noop},
    {"sceHttpGetAllResponseHeaders",http_network}, {"sceHttpGetMemoryPoolStats",http_network},
    {"sceNpRegisterStateCallbackA",ok_noop}, {"sceNpUnregisterStateCallbackA",ok_noop},
    /* Console features with no PC counterpart: accepted and ignored. */
    {"sceGameLiveStreamingEnableLiveStreaming",ok_noop}, {"sceVideoRecordingSetInfo",ok_noop},
    {"sceShareUtilityAdvanceInitialize",ok_noop}, {"sceShareUtilityAdvanceTerminate",ok_noop},
    {"sceShareUtilityAdvanceSetUploadContentData",ok_noop},
    /* runtime_tsr.c */
    {"sceKernelIsNeoMode",is_neo_mode},
    {"sceGameLiveStreamingInitialize",ok_noop}, {"sceGameLiveStreamingTerminate",ok_noop},
    {"sceCoredumpRegisterCoredumpHandler",ok_noop}, {"sceCoredumpWriteUserData",ok_noop},
    {"sceLibcHeapGetTraceInfo",libc_heap_trace_info},
};
uintptr_t runtime_extra_resolve(const char *name) { return enabled ? RUNTIME_LOOKUP(exports,name) : 0; }
