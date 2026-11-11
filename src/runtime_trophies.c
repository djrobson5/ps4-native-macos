/* Local trophy events; persistence uses only IDs. The GPU overlay carries the public catalog,
 * never game trophy files or artwork.
 * Inspired by Supermedo/bloodborne_pc 79d44f0 (Ambrose): persist unlock events per user.
 * Our version includes title identity, edited-save gating and checked append operations. */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/bbgpu.h"
#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* The titles with a platinum the runtime awards itself: it is awarded, as trophy `platinum`, once
 * the trophies first..last are recorded. `ledger` titles also have their catalog in libbbgpu
 * (gpu/shim/trophy_catalog*.json), keep <user dir>/achievements.txt (Wayfarer reads it) and report
 * their earned set to sceNpTrophyGetTrophyUnlockState. Only CUSA03173's numbering is verified
 * (docs/TROPHIES.md); Bloodborne's Old Hunters six (34..39) are separate from its platinum. */
typedef struct { const char *title; int platinum,first,last; int ledger; } TrophyRules;
static const TrophyRules trophy_rules[]={
    {"CUSA03173",0,1,33,0},
    {"CUSA19610",0,1,55,1},
};
static pthread_mutex_t trophy_lock=PTHREAD_MUTEX_INITIALIZER;
static char trophy_title[17];
static const TrophyRules *rules; /* of trophy_title, or NULL */
static long long earned_at[128]; /* unix time of the earliest record of each trophy, 0: not earned */
#define EARNED(id) (earned_at[id]>0)
static int history_valid;
static void read_history(void) {
    memset(earned_at,0,sizeof(earned_at)); history_valid=1;
    char path[640];
    int length=snprintf(path,sizeof(path),"%s/trophies-v1.log",runtime_file_user_dir());
    if (length<0 || (size_t)length>=sizeof(path)) { history_valid=0; return; }
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);
    if (fd<0) { history_valid=errno==ENOENT; return; }
    struct stat st;
    if (fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size>32*1024*1024) { close(fd); history_valid=0; return; }
    FILE *f=fdopen(fd,"r"); if (!f) { close(fd); history_valid=0; return; }
    char line[128],title[17],extra,stamp_text[32],id_text[32];
    /* Only read the initial file size; a concurrent append cannot extend startup work. */
    size_t remaining=(size_t)st.st_size;
    while (remaining && fgets(line,remaining<sizeof(line)-1 ? (int)remaining+1 : (int)sizeof(line),f)) {
        size_t length=strlen(line);
        if (!length || length>remaining) { history_valid=0; break; }
        remaining-=length;
        if (!strchr(line,'\n')) {
            int c;
            while (remaining && (c=fgetc(f))!=EOF) { --remaining; if (c=='\n') break; }
            continue;
        }
        if (sscanf(line,"v1 %31s %16s %31s %c",stamp_text,title,id_text,&extra)!=3) continue;
        char *stamp_end,*id_end;
        errno=0; long long timestamp=strtoll(stamp_text,&stamp_end,10); int stamp_error=errno;
        errno=0; long long id=strtoll(id_text,&id_end,10);
        if (!stamp_error && !errno && !*stamp_end && !*id_end && timestamp>0 &&
            timestamp<=(long long)time(NULL)+300 && id>=0 && id<128 && !strcmp(title,trophy_title))
            if (!earned_at[id] || timestamp<earned_at[id]) earned_at[id]=timestamp;
    }
    if (ferror(f)) history_valid=0;
    fclose(f);
}
/* ---- the ledger: <user dir>/achievements.txt, the format Wayfarer's ReportedAchievements reads
 * ("key = value" lines, ";" comments on lines of their own):
 *   [unlocked] id = yyyy-MM-dd HH:mm (local time of the earliest record)
 *   [grades]   id = platinum | gold | silver | bronze
 *   [list]     id = name | description, every trophy in id order
 * Written whole to a temporary file and renamed, so a reader never sees half of one. */
static int catalog_count(void) {
    int n=0;
    while (n<128 && bbgpu_trophy_catalog_text(trophy_title,n,NULL,NULL,NULL)) ++n;
    return n;
}
static void write_ledger(void) {
    if (!rules || !rules->ledger || !history_valid) return;
    int count=catalog_count();
    if (!count) return;
    char path[640],temp[648];
    int length=snprintf(path,sizeof(path),"%s/achievements.txt",runtime_file_user_dir());
    if (length<0 || (size_t)length>=sizeof(path) || snprintf(temp,sizeof(temp),"%s.tmp",path)<0) return;
    unlink(temp); /* a leftover (or a link) is removed, never followed */
    int fd=open(temp,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0644);
    FILE *f=fd>=0 ? fdopen(fd,"w") : NULL;
    if (!f) { if (fd>=0) close(fd); printf("Runtime: trophy ledger not written (cannot create %s)\n",temp); return; }
    fprintf(f,"; Written by the runtime (src/runtime_trophies.c); replaced whole at start and at each unlock.\n[unlocked]\n");
    for (int id=0;id<count;++id) {
        if (!EARNED(id)) continue;
        time_t at=(time_t)earned_at[id]; struct tm local; char text[32]="";
        if (localtime_r(&at,&local)) strftime(text,sizeof(text),"%Y-%m-%d %H:%M",&local);
        fprintf(f,"%d = %s\n",id,text);
    }
    fputs("[grades]\n",f);
    for (int id=0;id<count;++id) {
        const char *grade="";
        bbgpu_trophy_catalog_text(trophy_title,id,NULL,NULL,&grade);
        fprintf(f,"%d = %s\n",id,grade);
    }
    fputs("[list]\n",f);
    for (int id=0;id<count;++id) {
        const char *name="",*description="";
        bbgpu_trophy_catalog_text(trophy_title,id,&name,&description,NULL);
        fprintf(f,"%d = %s | %s\n",id,name,description); /* the generator keeps '|' and newlines out */
    }
    int ok=!ferror(f) && fflush(f)==0 && fsync(fd)==0;
    ok=fclose(f)==0 && ok;
    if (!ok || rename(temp,path)) { unlink(temp); printf("Runtime: trophy ledger not written (%s)\n",path); }
}
/* sceNpTrophyGetTrophyUnlockState for a title with its own catalog: the PS4's SceNpTrophyFlagArray
 * (four uint32s, bit id&31 of word id>>5, as shadPS4's ORBIS_NP_TROPHY_FLAG_SET) and the number of
 * trophies in the set. Returns 0 (and touches nothing) for any other title. */
int runtime_trophy_state(uint32_t flags[4],uint32_t *count) {
    pthread_mutex_lock(&trophy_lock);
    int reports=rules && rules->ledger && history_valid;
    if (reports) {
        memset(flags,0,16);
        for (int id=0;id<128;++id) if (EARNED(id)) flags[id>>5]|=1u<<(id&31);
        *count=(uint32_t)catalog_count(); /* shadPS4 counts the set's trophies, not the earned ones */
    }
    pthread_mutex_unlock(&trophy_lock);
    return reports;
}
void runtime_trophy_configure(const char *title) {
    pthread_mutex_lock(&trophy_lock);
    size_t n=title ? strlen(title) : 0;
    int valid=n>0 && n<sizeof(trophy_title);
    for (size_t i=0;i<n && valid;++i)
        valid=(title[i]>='A' && title[i]<='Z') || (title[i]>='0' && title[i]<='9');
    snprintf(trophy_title,sizeof(trophy_title),"%s",valid ? title : "");
    rules=NULL;
    for (size_t i=0;i<sizeof(trophy_rules)/sizeof(trophy_rules[0]);++i)
        if (!strcmp(trophy_title,trophy_rules[i].title)) rules=&trophy_rules[i];
    read_history();
    write_ledger(); /* all of the catalog shows before the first unlock */
    pthread_mutex_unlock(&trophy_lock);
}
static int eligible(void) {
    const char *v=getenv("BB_TROPHIES_ELIGIBLE");
    if (v && strcmp(v,"1")) return 0;
    char path[640];
    struct stat st;
    snprintf(path,sizeof(path),"%s/savedata",runtime_file_user_dir());
    if (lstat(path,&st)==0) {
        if (!S_ISDIR(st.st_mode)) return 0;
    } else if (errno!=ENOENT) return 0;
    snprintf(path,sizeof(path),"%s/savedata/.bb-edited",runtime_file_user_dir());
    if (lstat(path,&st)==0) return 0; /* Any marker, even malformed or a link, disables unlocks. */
    return errno==ENOENT;
}
/* Public base/DLC grouping cross-checked against Supermedo/bloodborne_pc 79d44f0
 * (Ambrose). Only CUSA03173's ID mapping has been verified for this runtime. */
static int persist_unlock(int id) {
    if (EARNED(id)) return 1;
    char path[640],line[96];
    int length=snprintf(path,sizeof(path),"%s/trophies-v1.log",runtime_file_user_dir());
    time_t timestamp=time(NULL);
    int saved=0;
    if (length>0 && (size_t)length<sizeof(path) && trophy_title[0] && timestamp>0) {
        int n=snprintf(line,sizeof(line),"v1 %lld %s %d\n",(long long)timestamp,trophy_title,id);
        int fd=open(path,O_RDWR|O_CREAT|O_APPEND|O_NOFOLLOW|O_NONBLOCK,0600);
        struct stat st;
        if (fd>=0 && fstat(fd,&st)==0 && S_ISREG(st.st_mode)) {
            /* A failed append may leave a partial tail. Invalidate it without truncating
             * history or completing a possibly truncated trophy ID. */
            char last='\n';
            int tail_ok=st.st_size==0 || pread(fd,&last,1,st.st_size-1)==1;
            if (tail_ok && last!='\n') {
                ssize_t repair;
                do { repair=write(fd," invalid\n",9); } while (repair<0 && errno==EINTR);
                tail_ok=repair==9;
            }
            ssize_t written;
            written=-1;
            if (tail_ok) do { written=write(fd,line,(size_t)n); } while (written<0 && errno==EINTR);
            saved=written==n && fsync(fd)==0;
        }
        if (fd>=0) close(fd);
    }
    if (saved) {
        earned_at[id]=(long long)timestamp;
        if (history_valid) bbgpu_trophy_notification(trophy_title,id);
    }
    printf("Runtime: trophy %d %s\n",id,saved ? "unlocked (recorded locally)" : "not persisted (event write failed)");
    return saved;
}
int32_t runtime_trophy_unlock(int32_t id,int32_t *platinum) {
    if (platinum) *platinum=-1;
    if (id<0 || id>=128) return (int32_t)0x80551604;
    pthread_mutex_lock(&trophy_lock);
    if (!eligible()) {
        printf("Runtime: trophy %d suppressed (save ineligible)\n",id);
        pthread_mutex_unlock(&trophy_lock);
        return 0; /* Preserve the local service's success contract without granting an unlock. */
    }
    int recorded=!EARNED(id);
    int saved=persist_unlock(id);
    recorded=recorded && saved;
    if (saved && history_valid && rules && !EARNED(rules->platinum)) {
        int complete=1;
        for (int other=rules->first;other<=rules->last;++other) complete=complete && EARNED(other);
        if (complete && persist_unlock(rules->platinum)) {
            recorded=1;
            if (platinum) *platinum=rules->platinum;
        }
    }
    if (recorded) write_ledger();
    pthread_mutex_unlock(&trophy_lock);
    return 0;
}

#else
void runtime_trophy_configure(const char *title) { (void)title; }
int32_t runtime_trophy_unlock(int32_t id,int32_t *platinum) { (void)id; if (platinum) *platinum=-1; return 0; }
int runtime_trophy_state(uint32_t flags[4],uint32_t *count) { (void)flags; (void)count; return 0; }
#endif
