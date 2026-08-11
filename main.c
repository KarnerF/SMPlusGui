#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <sys/time.h>
#include <sys/sysctl.h>
#include <sys/syscall.h>
#include <sys/statvfs.h>
#include <sys/mount.h>
#include <time.h>
#include <dirent.h>
#include <ctype.h>
#include "mongoose.h"
#include "app_installer.h"
#include "lang.h"

/* PS5 system language API — same as SM uses */
#define SCE_SYSTEM_SERVICE_PARAM_ID_LANG 1
int sceSystemServiceParamGetInt(int paramId, int *value);

/* Maps PS5 lang integers → our 2-letter codes (DE/FR/ES only; others → EN) */
static const char* ps5_lang_to_ui(int id) {
    /* 0=ja,1=en-US,2=fr,3=es,4=de,5=it,6=nl,7=pt-PT,8=ru,9=ko,
       10=zh-TW,11=zh-CN,12=fi,13=sv,14=da,15=no,16=pl,17=pt-BR,
       18=en-GB,19=tr,20=es-MX,21=ar,22=el,23=cs,24=hu,25=ro,
       26=uk,27=id,28=th,29=vi */
    if(id==4) return "de";
    if(id==2) return "fr";
    if(id==3||id==20) return "es";
    return "en";
}

#define SCE_NOTIFY_UID 0xFE
int sceNotificationSend(int uid, int logged, const char *payload);

static void _notify_send(const char *title, const char *sub) {
    char id[24], buf[2048];
    snprintf(id, sizeof(id), "%lu", (unsigned long)time(NULL));
    if (sub && sub[0]) {
        snprintf(buf, sizeof(buf),
            "{\"rawData\":{\"viewTemplateType\":\"InteractiveToastTemplateB\","
            "\"channelType\":\"Downloads\",\"useCaseId\":\"IDC\","
            "\"toastOverwriteType\":\"No\",\"isImmediate\":true,\"priority\":100,"
            "\"viewData\":{\"icon\":{\"type\":\"Url\",\"parameters\":{\"url\":\"/user/app/SMPL00001/sce_sys/icon0.png\"}},"
            "\"message\":{\"body\":\"%s\"},\"subMessage\":{\"body\":\"%s\"}}},"
            "\"createdDateTime\":\"2025-01-01T00:00:00.000Z\","
            "\"localNotificationId\":\"%s\"}",
            title, sub, id);
    } else {
        snprintf(buf, sizeof(buf),
            "{\"rawData\":{\"viewTemplateType\":\"InteractiveToastTemplateB\","
            "\"channelType\":\"Downloads\",\"useCaseId\":\"IDC\","
            "\"toastOverwriteType\":\"No\",\"isImmediate\":true,\"priority\":100,"
            "\"viewData\":{\"icon\":{\"type\":\"Predefined\",\"parameters\":{\"icon\":\"download\"}},"
            "\"message\":{\"body\":\"%s\"}}},"
            "\"createdDateTime\":\"2025-01-01T00:00:00.000Z\","
            "\"localNotificationId\":\"%s\"}",
            title, id);
    }
    sceNotificationSend(SCE_NOTIFY_UID, 1, buf);
}

static void notify(const char *msg) { _notify_send(msg, NULL); }

/* SMPlusGui own preferences (not SM config) */
#define PREFS_PATH "/data/SMPlusGui/prefs.ini"
typedef struct { int auto_start; char preferred_elf[512]; } SMPrefs;

static void read_prefs(SMPrefs *p) {
    memset(p, 0, sizeof(*p));
    FILE *f = fopen(PREFS_PATH, "r"); if(!f) return;
    char line[600];
    while(fgets(line, sizeof(line), f)) {
        line[strcspn(line,"\r\n")]=0;
        if(strncmp(line,"auto_start=",11)==0) p->auto_start=atoi(line+11);
        else if(strncmp(line,"preferred_elf=",14)==0) strncpy(p->preferred_elf,line+14,511);
    }
    fclose(f);
}
static void write_prefs(const SMPrefs *p) {
    mkdir("/data/SMPlusGui",0755);
    FILE *f = fopen(PREFS_PATH,"w"); if(!f) return;
    fprintf(f,"auto_start=%d\n",p->auto_start);
    fprintf(f,"preferred_elf=%s\n",p->preferred_elf);
    fclose(f);
}

static void get_local_ip(char *buf, size_t buflen) {
    struct ifaddrs *ifap, *ifa;
    buf[0] = 0;
    if (getifaddrs(&ifap) != 0) return;
    for (ifa = ifap; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;
        if (ifa->ifa_flags & IFF_LOOPBACK) continue;
        struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
        inet_ntop(AF_INET, &sin->sin_addr, buf, buflen);
        break;
    }
    freeifaddrs(ifap);
}

/* ShadowMount Version auslesen - API + Log-Fallback */
static void get_sm_version(char *buf, size_t buflen) {
    strncpy(buf, "nicht aktiv", buflen-1);
    buf[buflen-1] = 0;

    /* Erst versuchen aus debug.log zu lesen */
    FILE *lf = fopen("/data/shadowmount/debug.log", "r");
    if (lf) {
        char line[256];
        for (int i = 0; i < 20; i++) {
            if (!fgets(line, sizeof(line), lf)) break;
            /* Suche nach "ShadowMount" + Versionsnummer */
            char *p = strstr(line, "ShadowMount");
            if (!p) p = strstr(line, "shadowmount");
            if (p) {
                /* Suche nach Zahl nach Leerzeichen oder 'v' */
                char *v = strpbrk(p, "0123456789");
                if (v && (v[-1]==' '||v[-1]=='v'||v[-1]=='\t')) {
                    size_t len=0;
                    while(v[len]&&!strchr(" \t\r\n\"",v[len])&&len<buflen-1) len++;
                    if(len>0&&len<32){
                        strncpy(buf,v,len); buf[len]=0;
                        fclose(lf); return;
                    }
                }
            }
        }
        fclose(lf);
    }

    /* API anfragen */
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return;

    struct timeval tv = {1, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(10101);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(sock); return;
    }
    strncpy(buf, "aktiv", buflen-1);

    /* Verschiedene Endpoints probieren */
    const char *reqs[] = {
        "POST /api/v1/version HTTP/1.0\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n{}",
        "GET /version HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n",
        "GET /api/version HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n",
        "GET /status HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n",
        "GET /api/status HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n",
        "GET / HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n",
        NULL
    };

    char resp[4096];
    for (int i = 0; reqs[i]; i++) {
        memset(resp, 0, sizeof(resp));
        send(sock, reqs[i], strlen(reqs[i]), 0);
        ssize_t n = recv(sock, resp, sizeof(resp)-1, 0);
        if (n <= 0) continue;
        resp[n] = 0;

        /* JSON Keys suchen */
        const char *keys[] = {
            "\"shadowmount_version\":\"", "\"shadowmount_version\": \"",
            "\"version\":\"", "\"version\": \"",
            "\"sm_version\":\"", "\"sm_version\": \"",
            "\"app_version\":\"", "\"app_version\": \"",
            NULL
        };
        for (int k = 0; keys[k]; k++) {
            char *p = strstr(resp, keys[k]);
            if (!p) continue;
            p += strlen(keys[k]);
            size_t len = 0;
            while(p[len] && p[len]!='"' && p[len]!=',' &&
                  p[len]!='}' && p[len]!='\n' && len<buflen-1) len++;
            if (len > 0 && len < 32) {
                strncpy(buf, p, len); buf[len]=0;
                close(sock); return;
            }
        }

        /* Freie Versionsnummer-Suche im Body */
        char *body = strstr(resp, "\r\n\r\n");
        if (body) {
            body += 4;
            char *v = strpbrk(body, "0123456789");
            while (v) {
                if (v>body && (v[-1]==' '||v[-1]=='"'||v[-1]==':'||v[-1]=='v')) {
                    size_t len=0;
                    while(v[len]&&!strchr(" \t\r\n\",}",v[len])&&len<buflen-1) len++;
                    if(len>=3&&len<32) {
                        strncpy(buf,v,len); buf[len]=0;
                        close(sock); return;
                    }
                }
                v = strpbrk(v+1, "0123456789");
            }
        }
    }
    close(sock);
}

/* Generic SM API proxy - returns malloc'd body or NULL */
static char* sm_api_req(const char *method, const char *path, const char *body) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return NULL;
    struct timeval tv = {3, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(10101);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) { close(sock); return NULL; }
    char req[512];
    int blen = body ? (int)strlen(body) : 0;
    if (blen > 0)
        snprintf(req, sizeof(req), "%s %s HTTP/1.0\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n", method, path, blen);
    else
        snprintf(req, sizeof(req), "%s %s HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n", method, path);
    send(sock, req, strlen(req), 0);
    if (blen > 0) send(sock, body, blen, 0);
    size_t bufsz = 131072;
    char *buf = malloc(bufsz);
    if (!buf) { close(sock); return NULL; }
    size_t total = 0; ssize_t rn;
    while (total < bufsz-1 && (rn = recv(sock, buf+total, bufsz-1-total, 0)) > 0) total += rn;
    close(sock);
    buf[total] = 0;
    char *start = strstr(buf, "\r\n\r\n");
    if (!start) { free(buf); return NULL; }
    char *result = strdup(start + 4);
    free(buf);
    return result;
}

#define KI_PID_OFFSET    72
#define KI_TDNAME_OFFSET 447
static pid_t find_pid(const char *name) {
    int mib[4]={1,14,8,0}; pid_t mypid=getpid(),found=-1; size_t sz=0;
    if(sysctl(mib,4,NULL,&sz,NULL,0)!=0) return -1;
    uint8_t *buf=malloc(sz); if(!buf) return -1;
    if(sysctl(mib,4,buf,&sz,NULL,0)==0)
        for(uint8_t *p=buf;p<buf+sz;){
            int ks=*(int*)p; pid_t kp=*(pid_t*)&p[KI_PID_OFFSET];
            char *kn=(char*)&p[KI_TDNAME_OFFSET]; p+=ks;
            if(!strcmp(name,kn)&&kp!=mypid) found=kp;
        }
    free(buf); return found;
}
static void terminate_existing_instances(const char *name) {
    pid_t pid; int tries=0;
    while(tries++<8 && (pid=find_pid(name))>0){kill(pid,SIGKILL);sleep(1);}
}

#define CONFIG_PATH "/data/shadowmount/config.ini"
#define BAK_DIR     "/data/SMPlusGui/backups"
#define ICO(n) "<svg class='ico'><use href='#i-" n "'/></svg>"
#define HTTP_PORT   "7070"
#define MAX_PATHS   20
#define PATH_LEN    256
#define MAX_IMG   50
#define MAX_LIST  30
#define IMG_LEN   128
#define ID_LEN    32
#define MANUAL_LST "/data/shadowmount/manual.lst"
#define MAX_MANUAL  50

typedef struct {
    /* Allgemein */
    int  debug;
    int  quiet_mode;
    char language[32];
    /* API */
    char api_bind_address[64];
    int  api_port;
    /* Mounting */
    int  mount_read_only;
    int  force_mount;
    int  persistent_image_mounts;
    int  app_install_all;
    /* Scan */
    int  scan_depth;
    int  recursive_scan;
    int  scan_interval_seconds;
    int  stability_wait_seconds;
    /* Auto-Remove */
    int  auto_remove_missing_games;
    int  auto_remove_games_with_dlc;
    int  auto_remove_missing_delay_seconds;
    /* kstuff */
    int  kstuff_game_auto_toggle;
    int  kstuff_crash_detection;
    int  kstuff_pause_delay_image_seconds;
    int  kstuff_pause_delay_direct_seconds;
    /* Fakelib */
    int  backport_fakelib;
    int  global_fakelib;
    char global_fakelib_path[PATH_LEN];
    char global_fakelib_priority[16];
    /* Backend */
    char exfat_backend[8];
    char ufs_backend[8];
    int  lvd_exfat_sector_size;
    int  lvd_ufs_sector_size;
    int  lvd_pfs_sector_size;
    int  md_exfat_sector_size;
    int  md_ufs_sector_size;
    /* Scanpaths */
    char scanpaths[MAX_PATHS][PATH_LEN];
    int  path_count;
    /* Per-Image Overrides */
    char image_ro[MAX_IMG][IMG_LEN];
    int  image_ro_count;
    char image_rw[MAX_IMG][IMG_LEN];
    int  image_rw_count;
    char image_sector[MAX_IMG][IMG_LEN]; /* "datei.exfat:65536" */
    int  image_sector_count;
    /* kstuff Ausnahmen */
    char kstuff_no_pause[MAX_LIST][ID_LEN];
    int  kstuff_no_pause_count;
    char kstuff_delay[MAX_LIST][ID_LEN+8]; /* "TITLEID:SEKUNDEN" */
    int  kstuff_delay_count;
    /* Fakelib Ausnahmen */
    char global_fakelib_exclude[MAX_LIST][PATH_LEN];
    int  global_fakelib_exclude_count;
    /* 1.7alpha5 */
    char fan_target_temperature[8];
} ShadowConfig;

static void set_defaults(ShadowConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->debug=0; cfg->quiet_mode=0;
    strncpy(cfg->language,"auto",31);
    strncpy(cfg->api_bind_address,"127.0.0.1",63);
    cfg->api_port=10101;
    cfg->mount_read_only=1; cfg->force_mount=0;
    cfg->persistent_image_mounts=0; cfg->app_install_all=0;
    cfg->scan_depth=1; cfg->recursive_scan=0; cfg->scan_interval_seconds=15; cfg->stability_wait_seconds=10;
    cfg->auto_remove_missing_games=0; cfg->auto_remove_games_with_dlc=0;
    cfg->auto_remove_missing_delay_seconds=300;
    cfg->kstuff_game_auto_toggle=1; cfg->kstuff_crash_detection=1;
    cfg->kstuff_pause_delay_image_seconds=25; cfg->kstuff_pause_delay_direct_seconds=15;
    cfg->backport_fakelib=1; cfg->global_fakelib=1;
    strncpy(cfg->global_fakelib_path,"/data/shadowmount/fakelib",PATH_LEN-1);
    strncpy(cfg->global_fakelib_priority,"game",15);
    strncpy(cfg->exfat_backend,"lvd",7); strncpy(cfg->ufs_backend,"lvd",7);
    cfg->lvd_exfat_sector_size=512; cfg->lvd_ufs_sector_size=4096;
    cfg->lvd_pfs_sector_size=32768; cfg->md_exfat_sector_size=512;
    cfg->md_ufs_sector_size=512;
    cfg->image_ro_count=0; cfg->image_rw_count=0; cfg->image_sector_count=0;
    cfg->kstuff_no_pause_count=0; cfg->kstuff_delay_count=0;
    cfg->global_fakelib_exclude_count=0;
    strncpy(cfg->fan_target_temperature,"auto",7);
}

static int parse_bool(const char *v) {
    return !strcmp(v,"true")||!strcmp(v,"1")||!strcmp(v,"yes")||!strcmp(v,"on")||!strcmp(v,"ro");
}

static void load_config(ShadowConfig *cfg) {
    set_defaults(cfg);
    FILE *f=fopen(CONFIG_PATH,"r"); if(!f) return;
    char line[512];
    while(fgets(line,sizeof(line),f)){
        line[strcspn(line,"\r\n")]=0;
        char *s=line; while(*s==' '||*s=='\t') s++;
        if(*s==';'||*s=='#'||*s=='['||!*s) continue;
        char *eq=strchr(s,'='); if(!eq) continue;
        *eq=0; char *key=s,*val=eq+1;
        int kl=strlen(key); while(kl>0&&(key[kl-1]==' '||key[kl-1]=='\t')) key[--kl]=0;
        while(*val==' '||*val=='\t') val++;
        #define B(k,f) if(!strcmp(key,k)){cfg->f=parse_bool(val);}
        #define I(k,f) else if(!strcmp(key,k)){cfg->f=atoi(val);}
        #define S(k,f,n) else if(!strcmp(key,k)){strncpy(cfg->f,val,n-1);}
        B("debug",debug)
        B("quiet_mode",quiet_mode)
        S("language",language,32)
        else if(!strcmp(key,"api_bind_address")){strncpy(cfg->api_bind_address,val,63);}
        I("api_port",api_port)
        B("mount_read_only",mount_read_only)
        B("force_mount",force_mount)
        B("persistent_image_mounts",persistent_image_mounts)
        B("app_install_all",app_install_all)
        I("scan_depth",scan_depth)
        B("recursive_scan",recursive_scan)
        I("scan_interval_seconds",scan_interval_seconds)
        I("stability_wait_seconds",stability_wait_seconds)
        B("auto_remove_missing_games",auto_remove_missing_games)
        B("auto_remove_games_with_dlc",auto_remove_games_with_dlc)
        I("auto_remove_missing_delay_seconds",auto_remove_missing_delay_seconds)
        B("kstuff_game_auto_toggle",kstuff_game_auto_toggle)
        B("kstuff_crash_detection",kstuff_crash_detection)
        S("fan_target_temperature",fan_target_temperature,8)
        I("kstuff_pause_delay_image_s",kstuff_pause_delay_image_seconds)
        I("kstuff_pause_delay_image_seconds",kstuff_pause_delay_image_seconds)
        I("kstuff_pause_delay_direct_s",kstuff_pause_delay_direct_seconds)
        I("kstuff_pause_delay_direct_seconds",kstuff_pause_delay_direct_seconds)
        B("backport_fakelib",backport_fakelib)
        B("global_fakelib",global_fakelib)
        S("global_fakelib_path",global_fakelib_path,PATH_LEN)
        S("global_fakelib_priority",global_fakelib_priority,16)
        S("exfat_backend",exfat_backend,8)
        S("ufs_backend",ufs_backend,8)
        I("lvd_exfat_sector_size",lvd_exfat_sector_size)
        I("lvd_ufs_sector_size",lvd_ufs_sector_size)
        I("lvd_pfs_sector_size",lvd_pfs_sector_size)
        I("md_exfat_sector_size",md_exfat_sector_size)
        I("md_ufs_sector_size",md_ufs_sector_size)
        else if(!strcmp(key,"scanpath")&&cfg->path_count<MAX_PATHS)
            strncpy(cfg->scanpaths[cfg->path_count++],val,PATH_LEN-1);
        else if(!strcmp(key,"image_ro")&&cfg->image_ro_count<MAX_IMG)
            strncpy(cfg->image_ro[cfg->image_ro_count++],val,IMG_LEN-1);
        else if(!strcmp(key,"image_rw")&&cfg->image_rw_count<MAX_IMG)
            strncpy(cfg->image_rw[cfg->image_rw_count++],val,IMG_LEN-1);
        else if(!strcmp(key,"image_sector")&&cfg->image_sector_count<MAX_IMG)
            strncpy(cfg->image_sector[cfg->image_sector_count++],val,IMG_LEN-1);
        else if(!strcmp(key,"kstuff_no_pause")&&cfg->kstuff_no_pause_count<MAX_LIST)
            strncpy(cfg->kstuff_no_pause[cfg->kstuff_no_pause_count++],val,ID_LEN-1);
        else if(!strcmp(key,"kstuff_delay")&&cfg->kstuff_delay_count<MAX_LIST)
            strncpy(cfg->kstuff_delay[cfg->kstuff_delay_count++],val,ID_LEN+7);
        else if(!strcmp(key,"global_fakelib_exclude")&&cfg->global_fakelib_exclude_count<MAX_LIST)
            strncpy(cfg->global_fakelib_exclude[cfg->global_fakelib_exclude_count++],val,PATH_LEN-1);
        #undef B
        #undef I
        #undef S
    }
    fclose(f);
    /* legacy: recursive_scan=1 forces scan_depth=2 */
    if(cfg->recursive_scan) cfg->scan_depth=2;
}

static int save_config(ShadowConfig *cfg) {
    char lines[2048][512]; int lc=0;
    FILE *f=fopen(CONFIG_PATH,"r");
    if(f){while(lc<2048&&fgets(lines[lc],512,f))lc++;fclose(f);}
    FILE *out=fopen(CONFIG_PATH,"w"); if(!out) return 0;

    int wd=0,wq=0,wlang=0,wapi=0,wport=0;
    int wro=0,wfm=0,wpim=0,waia=0;
    int wsd=0,wrs=0,wsi=0,wss=0;
    int warm=0,wargd=0,warmd=0;
    int wkgt=0,wkcd=0,wkpi=0,wkpd=0;
    int wbf=0,wgf=0,wgfp=0,wgfpr=0;
    int web=0,wub=0,wles=0,wlus=0,wlps=0,wmes=0,wmus=0;
    int wscan=0,wfan=0;

    #define MATCH(key) (strncmp(pc, key "=", strlen(key)+1)==0)
    for(int i=0;i<lc;i++){
        char s[512]; strncpy(s,lines[i],511);
        char *p=s; while(*p==' '||*p=='\t') p++;
        /* also match commented-out keys: # key=value → written at correct position */
        char *pc=(*p=='#')?(p+1):p;
        while(*pc==' '||*pc=='\t') pc++;
        if(MATCH("debug")&&!wd){fprintf(out,"debug=%d\n",cfg->debug);wd=1;}
        else if(MATCH("quiet_mode")&&!wq){fprintf(out,"quiet_mode=%d\n",cfg->quiet_mode);wq=1;}
        else if(MATCH("language")&&!wlang){fprintf(out,"language=%s\n",cfg->language);wlang=1;}
        else if(MATCH("api_bind_address")&&!wapi){fprintf(out,"api_bind_address=%s\n",cfg->api_bind_address);wapi=1;}
        else if(MATCH("api")&&!wapi){fprintf(out,"api_bind_address=%s\n",cfg->api_bind_address);wapi=1;} /* migrate combined key */
        else if(MATCH("api_port")&&!wport){fprintf(out,"api_port=%d\n",cfg->api_port);wport=1;}
        else if(MATCH("mount_read_only")&&!wro){fprintf(out,"mount_read_only=%d\n",cfg->mount_read_only);wro=1;}
        else if(MATCH("force_mount")&&!wfm){fprintf(out,"force_mount=%d\n",cfg->force_mount);wfm=1;}
        else if(MATCH("persistent_image_mounts")&&!wpim){fprintf(out,"persistent_image_mounts=%d\n",cfg->persistent_image_mounts);wpim=1;}
        else if(MATCH("app_install_all")&&!waia){fprintf(out,"app_install_all=%d\n",cfg->app_install_all);waia=1;}
        else if(MATCH("scan_depth")&&!wsd){fprintf(out,"scan_depth=%d\n",cfg->scan_depth);wsd=1;}
        else if(MATCH("recursive_scan")&&!wrs){fprintf(out,"recursive_scan=%d\n",cfg->recursive_scan);wrs=1;}
        else if(MATCH("scan_interval_seconds")&&!wsi){fprintf(out,"scan_interval_seconds=%d\n",cfg->scan_interval_seconds);wsi=1;}
        else if(MATCH("scan_interval_s")&&!wsi){fprintf(out,"scan_interval_seconds=%d\n",cfg->scan_interval_seconds);wsi=1;} /* migrate */
        else if(MATCH("stability_wait_seconds")&&!wss){fprintf(out,"stability_wait_seconds=%d\n",cfg->stability_wait_seconds);wss=1;}
        else if(MATCH("stability_wait_s")&&!wss){fprintf(out,"stability_wait_seconds=%d\n",cfg->stability_wait_seconds);wss=1;} /* migrate */
        else if(MATCH("auto_remove_missing_games")&&!warm){fprintf(out,"auto_remove_missing_games=%d\n",cfg->auto_remove_missing_games);warm=1;}
        else if(MATCH("auto_remove_games_with_dlc")&&!wargd){fprintf(out,"auto_remove_games_with_dlc=%d\n",cfg->auto_remove_games_with_dlc);wargd=1;}
        else if(MATCH("auto_remove_missing_delay_seconds")&&!warmd){fprintf(out,"auto_remove_missing_delay_seconds=%d\n",cfg->auto_remove_missing_delay_seconds);warmd=1;}
        else if(MATCH("auto_remove_missing_delay_s")&&!warmd){fprintf(out,"auto_remove_missing_delay_seconds=%d\n",cfg->auto_remove_missing_delay_seconds);warmd=1;} /* migrate */
        else if(MATCH("kstuff_game_auto_toggle")&&!wkgt){fprintf(out,"kstuff_game_auto_toggle=%d\n",cfg->kstuff_game_auto_toggle);wkgt=1;}
        else if(MATCH("kstuff_crash_detection")&&!wkcd){fprintf(out,"kstuff_crash_detection=%d\n",cfg->kstuff_crash_detection);wkcd=1;}
        else if((MATCH("kstuff_pause_delay_image_s")||MATCH("kstuff_pause_delay_image_seconds"))&&!wkpi){fprintf(out,"kstuff_pause_delay_image_seconds=%d\n",cfg->kstuff_pause_delay_image_seconds);wkpi=1;}
        else if((MATCH("kstuff_pause_delay_direct_s")||MATCH("kstuff_pause_delay_direct_seconds"))&&!wkpd){fprintf(out,"kstuff_pause_delay_direct_seconds=%d\n",cfg->kstuff_pause_delay_direct_seconds);wkpd=1;}
        else if(MATCH("fan_target_temperature")&&!wfan){fprintf(out,"fan_target_temperature=%s\n",cfg->fan_target_temperature);wfan=1;}
        else if(MATCH("backport_fakelib")&&!wbf){fprintf(out,"backport_fakelib=%d\n",cfg->backport_fakelib);wbf=1;}
        else if(MATCH("global_fakelib")&&!wgf){fprintf(out,"global_fakelib=%d\n",cfg->global_fakelib);wgf=1;}
        else if(MATCH("global_fakelib_path")&&!wgfp){fprintf(out,"global_fakelib_path=%s\n",cfg->global_fakelib_path);wgfp=1;}
        else if(MATCH("global_fakelib_priority")&&!wgfpr){fprintf(out,"global_fakelib_priority=%s\n",cfg->global_fakelib_priority);wgfpr=1;}
        else if(MATCH("exfat_backend")&&!web){fprintf(out,"exfat_backend=%s\n",cfg->exfat_backend);web=1;}
        else if(MATCH("ufs_backend")&&!wub){fprintf(out,"ufs_backend=%s\n",cfg->ufs_backend);wub=1;}
        else if(MATCH("lvd_exfat_sector_size")&&!wles){fprintf(out,"lvd_exfat_sector_size=%d\n",cfg->lvd_exfat_sector_size);wles=1;}
        else if(MATCH("lvd_ufs_sector_size")&&!wlus){fprintf(out,"lvd_ufs_sector_size=%d\n",cfg->lvd_ufs_sector_size);wlus=1;}
        else if(MATCH("lvd_pfs_sector_size")&&!wlps){fprintf(out,"lvd_pfs_sector_size=%d\n",cfg->lvd_pfs_sector_size);wlps=1;}
        else if(MATCH("md_exfat_sector_size")&&!wmes){fprintf(out,"md_exfat_sector_size=%d\n",cfg->md_exfat_sector_size);wmes=1;}
        else if(MATCH("md_ufs_sector_size")&&!wmus){fprintf(out,"md_ufs_sector_size=%d\n",cfg->md_ufs_sector_size);wmus=1;}
        else if(MATCH("scanpath")){
            if(!wscan){
                for(int j=0;j<cfg->path_count;j++) if(cfg->scanpaths[j][0]) fprintf(out,"scanpath=%s\n",cfg->scanpaths[j]);
                wscan=1;
            }
        }
        else if(MATCH("image_ro")||MATCH("image_rw")||MATCH("image_sector")){
            /* Per-Image Overrides werden gesammelt und später geschrieben */
        }
        else if(MATCH("kstuff_no_pause")||MATCH("kstuff_delay")){
            /* kstuff Ausnahmen werden gesammelt und später geschrieben */
        }
        else if(MATCH("global_fakelib_exclude")){
            /* Fakelib Ausnahmen werden gesammelt und später geschrieben */
        }
        else {
            /* skip duplicate active key lines — only write if not a known key */
            if(*p=='#'||!strchr(p,'=')) fputs(lines[i],out);
            else if(!(
                MATCH("debug")||MATCH("quiet_mode")||MATCH("language")||
                MATCH("api_bind_address")||MATCH("api_port")||MATCH("api")||
                MATCH("mount_read_only")||MATCH("force_mount")||
                MATCH("persistent_image_mounts")||MATCH("app_install_all")||
                MATCH("scan_depth")||MATCH("recursive_scan")||
                MATCH("scan_interval_seconds")||MATCH("scan_interval_s")||
                MATCH("stability_wait_seconds")||MATCH("stability_wait_s")||
                MATCH("auto_remove_missing_games")||MATCH("auto_remove_games_with_dlc")||
                MATCH("auto_remove_missing_delay_seconds")||MATCH("auto_remove_missing_delay_s")||
                MATCH("kstuff_game_auto_toggle")||MATCH("kstuff_crash_detection")||
                MATCH("kstuff_pause_delay_image_s")||MATCH("kstuff_pause_delay_image_seconds")||
                MATCH("kstuff_pause_delay_direct_s")||MATCH("kstuff_pause_delay_direct_seconds")||
                MATCH("fan_target_temperature")||
                MATCH("backport_fakelib")||MATCH("global_fakelib")||
                MATCH("global_fakelib_path")||MATCH("global_fakelib_priority")||
                MATCH("exfat_backend")||MATCH("ufs_backend")||
                MATCH("lvd_exfat_sector_size")||MATCH("lvd_ufs_sector_size")||
                MATCH("lvd_pfs_sector_size")||MATCH("md_exfat_sector_size")||
                MATCH("md_ufs_sector_size")
            )) fputs(lines[i],out);
        }
    }
    #undef MATCH
    if(!wd)    fprintf(out,"debug=%d\n",cfg->debug);
    if(!wq)    fprintf(out,"quiet_mode=%d\n",cfg->quiet_mode);
    if(!wlang) fprintf(out,"language=%s\n",cfg->language);
    if(!wapi)  fprintf(out,"api_bind_address=%s\n",cfg->api_bind_address);
    if(!wport) fprintf(out,"api_port=%d\n",cfg->api_port);
    if(!wro)   fprintf(out,"mount_read_only=%d\n",cfg->mount_read_only);
    if(!wfm)   fprintf(out,"force_mount=%d\n",cfg->force_mount);
    if(!wpim)  fprintf(out,"persistent_image_mounts=%d\n",cfg->persistent_image_mounts);
    if(!waia)  fprintf(out,"app_install_all=%d\n",cfg->app_install_all);
    if(!wsd)   fprintf(out,"scan_depth=%d\n",cfg->scan_depth);
    if(!wrs)   fprintf(out,"recursive_scan=%d\n",cfg->recursive_scan);
    if(!wsi)   fprintf(out,"scan_interval_seconds=%d\n",cfg->scan_interval_seconds);
    if(!wss)   fprintf(out,"stability_wait_seconds=%d\n",cfg->stability_wait_seconds);
    if(!warm)  fprintf(out,"auto_remove_missing_games=%d\n",cfg->auto_remove_missing_games);
    if(!wargd) fprintf(out,"auto_remove_games_with_dlc=%d\n",cfg->auto_remove_games_with_dlc);
    if(!warmd) fprintf(out,"auto_remove_missing_delay_seconds=%d\n",cfg->auto_remove_missing_delay_seconds);
    if(!wkgt)  fprintf(out,"kstuff_game_auto_toggle=%d\n",cfg->kstuff_game_auto_toggle);
    if(!wkcd)  fprintf(out,"kstuff_crash_detection=%d\n",cfg->kstuff_crash_detection);
    if(!wkpi)  fprintf(out,"kstuff_pause_delay_image_seconds=%d\n",cfg->kstuff_pause_delay_image_seconds);
    if(!wkpd)  fprintf(out,"kstuff_pause_delay_direct_seconds=%d\n",cfg->kstuff_pause_delay_direct_seconds);
    if(!wfan)  fprintf(out,"fan_target_temperature=%s\n",cfg->fan_target_temperature);
    if(!wbf)   fprintf(out,"backport_fakelib=%d\n",cfg->backport_fakelib);
    if(!wgf)   fprintf(out,"global_fakelib=%d\n",cfg->global_fakelib);
    if(!wgfp)  fprintf(out,"global_fakelib_path=%s\n",cfg->global_fakelib_path);
    if(!wgfpr) fprintf(out,"global_fakelib_priority=%s\n",cfg->global_fakelib_priority);
    if(!web)   fprintf(out,"exfat_backend=%s\n",cfg->exfat_backend);
    if(!wub)   fprintf(out,"ufs_backend=%s\n",cfg->ufs_backend);
    if(!wles)  fprintf(out,"lvd_exfat_sector_size=%d\n",cfg->lvd_exfat_sector_size);
    if(!wlus)  fprintf(out,"lvd_ufs_sector_size=%d\n",cfg->lvd_ufs_sector_size);
    if(!wlps)  fprintf(out,"lvd_pfs_sector_size=%d\n",cfg->lvd_pfs_sector_size);
    if(!wmes)  fprintf(out,"md_exfat_sector_size=%d\n",cfg->md_exfat_sector_size);
    if(!wmus)  fprintf(out,"md_ufs_sector_size=%d\n",cfg->md_ufs_sector_size);
    if(!wscan) for(int j=0;j<cfg->path_count;j++) if(cfg->scanpaths[j][0]) fprintf(out,"scanpath=%s\n",cfg->scanpaths[j]);
    for(int j=0;j<cfg->image_ro_count;j++) if(cfg->image_ro[j][0]) fprintf(out,"image_ro=%s\n",cfg->image_ro[j]);
    for(int j=0;j<cfg->image_rw_count;j++) if(cfg->image_rw[j][0]) fprintf(out,"image_rw=%s\n",cfg->image_rw[j]);
    for(int j=0;j<cfg->image_sector_count;j++) if(cfg->image_sector[j][0]) fprintf(out,"image_sector=%s\n",cfg->image_sector[j]);
    for(int j=0;j<cfg->kstuff_no_pause_count;j++) if(cfg->kstuff_no_pause[j][0]) fprintf(out,"kstuff_no_pause=%s\n",cfg->kstuff_no_pause[j]);
    for(int j=0;j<cfg->kstuff_delay_count;j++) if(cfg->kstuff_delay[j][0]) fprintf(out,"kstuff_delay=%s\n",cfg->kstuff_delay[j]);
    for(int j=0;j<cfg->global_fakelib_exclude_count;j++) if(cfg->global_fakelib_exclude[j][0]) fprintf(out,"global_fakelib_exclude=%s\n",cfg->global_fakelib_exclude[j]);
    fclose(out); return 1;
}

static size_t next_kv_param(struct mg_str body, size_t ofs, struct mg_str *key, struct mg_str *val) {
    if(ofs>=body.len) return 0;
    size_t eq=ofs,amp;
    while(eq<body.len&&body.buf[eq]!='=') eq++;
    if(eq>=body.len) return 0;
    amp=eq+1; while(amp<body.len&&body.buf[amp]!='&') amp++;
    key->buf=body.buf+ofs; key->len=eq-ofs;
    val->buf=body.buf+eq+1; val->len=amp-(eq+1);
    return amp<body.len?amp+1:body.len;
}

/* ShadowMount laedt die Config automatisch via Datei-Watcher neu */

#define LANG_FILE    "/data/shadowmount/gui_lang"
#define PAYLOAD_NAME "SMPlusGui_" SMPLUS_VERSION ".elf"

static void read_lang(char *buf, size_t len) {
    strncpy(buf, "de", len-1);
    FILE *f = fopen(LANG_FILE, "r");
    if (!f) return;
    if (fgets(buf, len, f)) {
        buf[strcspn(buf, "\r\n")] = 0;
        if (strcmp(buf,"de")!=0 && strcmp(buf,"en")!=0 &&
            strcmp(buf,"fr")!=0 && strcmp(buf,"es")!=0 && strcmp(buf,"auto")!=0)
            strncpy(buf,"de",len-1);
    }
    fclose(f);
}

static void write_lang(const char *lang) {
    FILE *f = fopen(LANG_FILE, "w");
    if (!f) return;
    fputs(lang, f); fclose(f);
}



/* Prüft ob SM-Version >= geforderte Version ist.
 * Beispiele: "1.6beta16", "1.7alpha3", "1.6test13"
 * Reihenfolge: alpha < beta < test innerhalb gleicher major.minor */
static int sm_version_at_least(const char *ver, int maj, int min,
                                const char *suf, int num) {
    if(!ver||!*ver) return 0;
    int vmaj=0,vmin=0,vnum=0;
    char vsuf[8]={0};
    /* Major.Minor parsen */
    if(sscanf(ver,"%d.%d",&vmaj,&vmin)<2) return 0;
    /* Suffix und Nummer finden */
    const char *p=ver;
    while(*p&&*p!='.') p++; if(*p) p++;
    while(*p&&*p>='0'&&*p<='9') p++;
    /* p zeigt jetzt auf suffix wie "beta16" */
    int si=0;
    while(*p&&(*p<'0'||*p>'9')&&si<7) vsuf[si++]=*p++;
    vnum=atoi(p);
    /* Major.Minor vergleichen */
    if(vmaj>maj) return 1;
    if(vmaj<maj) return 0;
    if(vmin>min) return 1;
    if(vmin<min) return 0;
    /* Gleiche Major.Minor: Suffix Priorität (alpha<beta<test) */
    int vp=0,mp=0;
    if(!strcmp(vsuf,"alpha")) vp=1; else if(!strcmp(vsuf,"beta")) vp=2; else vp=3;
    if(!strcmp(suf, "alpha")) mp=1; else if(!strcmp(suf, "beta")) mp=2; else mp=3;
    if(vp>mp) return 1;
    if(vp<mp) return 0;
    return vnum>=num;
}

#define SM_MAX_ELFS 20
#define SM_EPATH    256

/* Scan known dirs for SM ELF files, returns count */
static int sm_find_elfs(char paths[SM_MAX_ELFS][SM_EPATH]) {
    int count=0;

    /* Helper: true if filename ends with .elf */
    #define IS_ELF(s) (strlen(s)>4&&strcmp((s)+strlen(s)-4,".elf")==0)

    /* pldmgr payloads: direct ELFs + scan subdirs with "shadow" in dir name */
    {
        DIR *d=opendir("/data/pldmgr/payloads");
        if(d){
            struct dirent *e;
            while((e=readdir(d))&&count<SM_MAX_ELFS){
                if(e->d_name[0]=='.') continue;
                char nl[64]={0}; int j=0;
                while(e->d_name[j]&&j<63){nl[j]=(char)tolower((unsigned char)e->d_name[j]);j++;}
                char fp[SM_EPATH];
                snprintf(fp,SM_EPATH,"/data/pldmgr/payloads/%s",e->d_name);
                /* Direct .elf file ending with .elf and shadowmount in name */
                if(IS_ELF(nl)&&strstr(nl,"shadowmount")){
                    snprintf(paths[count++],SM_EPATH,"%s",fp);
                    continue;
                }
                /* Subdirectory with "shadow" in dir name: grab all *.elf inside */
                if(strstr(nl,"shadow")){
                    DIR *sub=opendir(fp);
                    if(sub){
                        struct dirent *se;
                        while((se=readdir(sub))&&count<SM_MAX_ELFS){
                            char snl[64]={0}; int k=0;
                            while(se->d_name[k]&&k<63){snl[k]=(char)tolower((unsigned char)se->d_name[k]);k++;}
                            if(IS_ELF(snl)&&strstr(snl,"shadowmount"))
                                snprintf(paths[count++],SM_EPATH,"%s/%s",fp,se->d_name);
                        }
                        closedir(sub);
                    }
                }
            }
            closedir(d);
        }
    }

    /* /data/shadowmount: direct scan, only *.elf ending files */
    {
        DIR *d=opendir("/data/shadowmount");
        if(d){
            struct dirent *e;
            while((e=readdir(d))&&count<SM_MAX_ELFS){
                if(e->d_name[0]=='.') continue;
                char nl[64]={0}; int j=0;
                while(e->d_name[j]&&j<63){nl[j]=(char)tolower((unsigned char)e->d_name[j]);j++;}
                if(IS_ELF(nl)&&strstr(nl,"shadowmount"))
                    snprintf(paths[count++],SM_EPATH,"/data/shadowmount/%s",e->d_name);
            }
            closedir(d);
        }
    }

    /* USB roots /mnt/usb0 - /mnt/usb7: direct scan, only *.elf ending files */
    for(int ui=0;ui<8&&count<SM_MAX_ELFS;ui++){
        char usb[32]; snprintf(usb,32,"/mnt/usb%d",ui);
        DIR *d=opendir(usb); if(!d) continue;
        struct dirent *e;
        while((e=readdir(d))&&count<SM_MAX_ELFS){
            if(e->d_name[0]=='.') continue;
            char nl[64]={0}; int j=0;
            while(e->d_name[j]&&j<63){nl[j]=(char)tolower((unsigned char)e->d_name[j]);j++;}
            if(IS_ELF(nl)&&strstr(nl,"shadowmount"))
                snprintf(paths[count++],SM_EPATH,"%s/%s",usb,e->d_name);
        }
        closedir(d);
    }

    #undef IS_ELF
    return count;
}

/* Returns 1 if buf looks like a valid ShadowMount INI config */
static int is_valid_sm_cfg(const char *buf, size_t len){
    if(!buf||len<10||len>65536) return 0;
    for(size_t i=0;i<len;i++) if(buf[i]=='\0') return 0; /* reject binary */
    if(!memchr(buf,'=',len)) return 0;
    static const char *keys[]={"scan_depth=","quiet_mode=","kstuff_game_auto_toggle=",
        "fan_target_temperature=","exfat_backend=","scan_interval_seconds=",
        "auto_remove_missing_games=","fakelib",NULL};
    int found=0;
    for(int ki=0;keys[ki]&&found<2;ki++){
        size_t klen=strlen(keys[ki]);
        for(size_t j=0;j+klen<=len;j++)
            if(memcmp(buf+j,keys[ki],klen)==0){found++;break;}
    }
    return found>=2;
}

/* Returns 0 on success, -1 elf not found, -2 elfldr not reachable, -3 send error */
/* pending ELF — set by /api/sm/start, sent on next poll to avoid blocking browser */
static char g_pending_elf[SM_EPATH] = {0};

static int send_elf_to_elfldr(const char *path) {
    FILE *elf = fopen(path,"rb"); if(!elf) return -1;
    int ports[]={9021,9020,0}; int sock=-1;
    for(int pi=0;ports[pi];pi++){
        int s=socket(AF_INET,SOCK_STREAM,0); if(s<0) continue;
        struct timeval tv={5,0};
        setsockopt(s,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv));
        setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
        struct sockaddr_in sa; memset(&sa,0,sizeof(sa));
        sa.sin_family=AF_INET; sa.sin_port=htons((uint16_t)ports[pi]);
        sa.sin_addr.s_addr=inet_addr("127.0.0.1");
        if(connect(s,(struct sockaddr*)&sa,sizeof(sa))==0){sock=s;break;}
        close(s);
    }
    if(sock<0){fclose(elf);return -2;}
    char buf[8192]; int err=0; size_t n;
    while((n=fread(buf,1,sizeof(buf),elf))>0){
        size_t off=0;
        while(off<n){ssize_t s=send(sock,buf+off,n-off,0);if(s<=0){err=1;break;}off+=(size_t)s;}
        if(err) break;
    }
    close(sock); fclose(elf);
    return err ? -3 : 0;
}

static void fn(struct mg_connection *c, int ev, void *ev_data) {
    if(ev!=MG_EV_HTTP_MSG) return;
    struct mg_http_message *hm=(struct mg_http_message*)ev_data;

    /* Sprache setzen */
    if(mg_match(hm->uri,mg_str("/setlang"),NULL)){
        char lv[8]={0};
        mg_http_get_var(&hm->query,"l",lv,sizeof(lv));
        if(!lv[0]) mg_http_get_var(&hm->body,"l",lv,sizeof(lv));
        if(strcmp(lv,"en")==0||strcmp(lv,"de")==0||strcmp(lv,"fr")==0||strcmp(lv,"es")==0||strcmp(lv,"auto")==0) write_lang(lv);
        mg_http_reply(c,302,"Location: /\r\n",""); return;
    }

    if(mg_match(hm->uri,mg_str("/"),NULL)){
        char lang[8]; read_lang(lang,sizeof(lang));
        int is_auto=(lang[0]==0||strcmp(lang,"auto")==0);
        if(is_auto){
            /* Read PS5 system language directly via SCE API — same as SM does */
            int lid=1; /* default en-US */
            sceSystemServiceParamGetInt(SCE_SYSTEM_SERVICE_PARAM_ID_LANG,&lid);
            strncpy(lang,ps5_lang_to_ui(lid),7);
        }
        if(strcmp(lang,"de")==0) ui_lang=LANG_DE;
        else if(strcmp(lang,"fr")==0) ui_lang=LANG_FR;
        else if(strcmp(lang,"es")==0) ui_lang=LANG_ES;
        else ui_lang=LANG_EN;

        ShadowConfig cfg; load_config(&cfg);
        /* Read manual.lst entries */
        char manual_entries[MAX_MANUAL][PATH_LEN];
        int  manual_count=0;
        { FILE *mf=fopen(MANUAL_LST,"r"); if(mf){ char ml[PATH_LEN]; while(fgets(ml,sizeof(ml),mf)&&manual_count<MAX_MANUAL){ ml[strcspn(ml,"\r\n")]=0; char *ms=ml; while(*ms==' '||*ms=='\t')ms++; if(*ms=='#'||!*ms)continue; strncpy(manual_entries[manual_count++],ms,PATH_LEN-1); } fclose(mf); } }
        char sm_ver[64]; get_sm_version(sm_ver,sizeof(sm_ver));

        size_t hsz=262144;
        char *html=malloc(hsz);
        if(!html){mg_http_reply(c,500,"","OOM");return;}
        int pos=0;

        #define H(...)  pos+=snprintf(html+pos,hsz-pos,__VA_ARGS__)
        #define SW(id,nm,lstr,chk) H( \
            "<div class='row'><label for='%s'>%s</label>" \
            "<input type='checkbox' id='%s' name='%s' value='1' %s>" \
            "<label class='switch' for='%s'></label></div>", \
            id,lstr,id,nm,(chk)?"checked":"",id)
        #define NF(nm,lstr,mn,mx,val) H( \
            "<div class='numfield'><label>%s <span style='font-size:.7rem;color:var(--dim);font-weight:normal;'>(%s–%s)</span></label>" \
            "<input type='number' name='%s' min='%s' max='%s' value='%d'></div>", \
            lstr,mn,mx,nm,mn,mx,val)
        #define TF(nm,lstr,val,ph) H( \
            "<div class='numfield'><label>%s</label>" \
            "<input type='text' name='%s' value='%s' placeholder='%s'></div>", \
            lstr,nm,val,ph)
        #define SEC(dl,el) H("<details class='section'>" \
            "<summary><span class='arr'>&#9654;</span>%s</summary>",T(dl,el))

        /* CSS */
        H("<!DOCTYPE html><html><head><meta charset='UTF-8'>"
          "<meta name='viewport' content='width=device-width,initial-scale=1'>"
          "<title>SMPlusGui by Karner</title>"
          "<link rel='icon' type='image/svg+xml' href=\"data:image/svg+xml,"
          "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'>"
          "<rect width='32' height='32' rx='6' fill='%%230a0c11'/>"
          "<text x='16' y='22' text-anchor='middle' font-family='monospace' "
          "font-size='14' font-weight='bold' fill='%%2310b981'>SM</text>"
          "</svg>\">"
          "<style>"
          ":root{--bg:#07090f;--surface:#0d111a;--surface2:#131927;--surface3:#1a2135;""--border:#1e2a42;--text:#e2e8f0;--dim:#64748b;--muted:#334155;""--accent:#10b981;--accent-dim:rgba(16,185,129,.12);""--accent-glow:rgba(16,185,129,.25);""--green:#34d399;--orange:#f59e0b;--red:#f87171;""--mono:ui-monospace,'Cascadia Code',Menlo,Consolas,monospace;""--sans:'Inter',-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;}""*{box-sizing:border-box;}""html{height:100%%;}""body{margin:0;width:100%%;height:100%%;overflow:hidden;background:var(--bg);""background-image:""radial-gradient(ellipse at 20%% 0%%,rgba(16,185,129,.07) 0%%,transparent 50%%),""radial-gradient(ellipse at 80%% 100%%,rgba(96,165,250,.04) 0%%,transparent 50%%);""color:var(--text);font-family:var(--sans);font-size:16px;line-height:1.6;""display:flex;justify-content:center;}""@media(min-width:1280px){body{font-size:20px;}}"".card{width:100%%;max-width:1600px;display:flex;flex-direction:column;height:100%%;padding:12px 0 0 16px;}"".g2{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-bottom:12px;}"".g2.top{align-items:start;}""@media(max-width:720px){.g2{grid-template-columns:1fr;}}"".badge{display:inline-flex;align-items:center;gap:8px;font-family:var(--mono);""font-size:.72rem;letter-spacing:.14em;text-transform:uppercase;color:var(--accent);margin-bottom:10px;}"".badge-dot{width:7px;height:7px;border-radius:50%%;background:var(--accent);""box-shadow:0 0 6px var(--accent),0 0 14px var(--accent-glow);""animation:pulse 2.5s ease-in-out infinite;}""@keyframes pulse{0%%,100%%{opacity:1;box-shadow:0 0 6px var(--accent),0 0 14px var(--accent-glow);}""50%%{opacity:.5;box-shadow:0 0 3px var(--accent);}}"".topbar{display:flex;align-items:center;justify-content:space-between;flex-wrap:wrap;gap:10px;margin-bottom:16px;}""h1{font-size:1.8rem;font-weight:800;margin:0;letter-spacing:-.02em;""background:linear-gradient(135deg,#e2e8f0 0%%,#94a3b8 100%%);""-webkit-background-clip:text;-webkit-text-fill-color:transparent;background-clip:text;}"".sub{color:var(--dim);font-size:1rem;margin:0;}"".lang{display:flex;gap:4px;margin-top:6px;}"".lang a{font-family:var(--mono);font-size:.8rem;padding:5px 13px;""border-radius:6px;border:1px solid var(--border);color:var(--dim);text-decoration:none;""transition:all .15s;cursor:pointer;}"".lang a:hover{border-color:var(--accent);color:var(--accent);}"".lang a.active{border-color:var(--accent);color:var(--accent);""background:var(--accent-dim);font-weight:700;}"".statusbar{display:flex;gap:8px;flex-wrap:wrap;margin-bottom:32px;}"".chip{display:flex;align-items:center;gap:8px;font-family:var(--mono);""font-size:.78rem;color:var(--dim);background:var(--surface);""border:1px solid var(--border);border-radius:8px;padding:7px 14px;}"".chip b{color:var(--text);}"".chip-live{width:7px;height:7px;border-radius:50%%;background:var(--green);""box-shadow:0 0 6px var(--green);animation:pulse 2s infinite;}"".chip-dead{width:7px;height:7px;border-radius:50%%;background:var(--red);opacity:.7;}"".section{background:var(--surface);border:1px solid var(--border);""border-radius:12px;padding:18px 22px;margin-bottom:12px;""box-shadow:0 2px 12px rgba(0,0,0,.3);}"".g2 .section,.g2.top .section,.g2.top details.section{margin-bottom:0;}"".section h2,.section>summary{font-size:.95rem;text-transform:uppercase;""letter-spacing:.06em;color:var(--text);margin:0 0 16px;font-family:var(--mono);""font-weight:700;cursor:pointer;list-style:none;display:flex;align-items:center;gap:8px;}"".section>summary{margin-bottom:0;}"".section>summary::-webkit-details-marker{display:none;}""details.section{margin-bottom:16px;}""details.section[open]>summary{margin-bottom:20px;}""details.section[open]>summary .arr{transform:rotate(90deg);}"".arr{display:inline-block;transition:transform .2s;}"".row{display:flex;align-items:center;justify-content:space-between;""padding:9px 0;border-bottom:1px solid var(--border);gap:16px;}"".row:last-child{border-bottom:none;}"".row>label:first-child{flex:1;font-size:.85rem;font-weight:500;color:var(--text);}""input[type=checkbox]{display:none;}"".switch{width:58px;height:30px;border-radius:15px;background:var(--surface3);""border:1px solid var(--border);position:relative;cursor:pointer;""flex-shrink:0;transition:background .2s,border-color .2s;}"".switch::after{content:'';position:absolute;top:5px;left:5px;width:18px;height:18px;""border-radius:50%%;background:var(--dim);transition:transform .2s,background .2s,box-shadow .2s;}""input[type=checkbox]:checked+.switch{background:var(--accent-dim);border-color:var(--accent);}""input[type=checkbox]:checked+.switch::after{transform:translateX(28px);""background:var(--accent);box-shadow:0 0 8px var(--accent-glow);}"".numfield{display:flex;align-items:center;justify-content:space-between;""padding:9px 0;border-bottom:1px solid var(--border);font-size:.85rem;gap:16px;}"".numfield:last-child{border-bottom:none;}"".numfield label{flex:1;font-weight:500;color:var(--text);}""input[type=number],input[type=text],select{background:var(--surface2);""border:1px solid var(--border);border-radius:8px;color:var(--text);""padding:9px 13px;font-family:var(--mono);font-size:.9rem;""transition:border-color .15s,box-shadow .15s;}""select{cursor:pointer;width:auto;min-width:160px;}""input[type=number]{width:120px;min-width:120px;text-align:center;}""input[type=text]{width:220px;min-width:140px;max-width:100%%;}"".numfield input[type=text]{flex:0 1 280px;}""input:focus,select:focus{outline:none;border-color:var(--accent);box-shadow:0 0 0 3px var(--accent-dim);}""input::placeholder{color:var(--muted);font-style:italic;}""input[type=number]::-webkit-inner-spin-button,input[type=number]::-webkit-outer-spin-button{-webkit-appearance:none;margin:0;}""input[type=number]{-moz-appearance:textfield;}"".path-row{display:flex;gap:8px;margin-bottom:8px;align-items:center;}"".path-row input[type=text]{flex:1;width:auto;min-width:0;font-size:.88rem;}"".rm{width:36px;height:36px;flex-shrink:0;border-radius:8px;""border:1px solid var(--border);background:var(--surface2);""color:var(--dim);cursor:pointer;font-size:1rem;transition:all .15s;}"".rm:hover{border-color:var(--red);color:var(--red);}"".sublist{margin-bottom:16px;}"".sublist-title{font-size:.9rem;text-transform:uppercase;letter-spacing:.06em;""color:var(--text);font-family:var(--mono);margin-bottom:8px;""display:flex;align-items:center;gap:8px;}"".sublist-title::after{content:'';flex:1;height:1px;background:var(--border);}"".empty-hint{color:var(--muted);font-size:.82rem;font-family:var(--mono);""padding:8px 0;font-style:italic;}"".hint{font-size:.82rem;color:var(--text);font-family:var(--mono);""margin:0 0 12px;padding:8px 12px;background:var(--surface3);""border:1px solid var(--border);""border-left:3px solid var(--accent);border-radius:0 8px 8px 0;}"".hint b{color:var(--accent);font-weight:700;}"".hint code{color:var(--text);background:var(--surface2);padding:1px 5px;border-radius:4px;}"".vlock{opacity:.35;pointer-events:none;filter:grayscale(1);}"".vbadge{font-size:.65rem;font-family:var(--mono);color:#f59e0b;""background:rgba(245,158,11,.1);border:1px solid rgba(245,158,11,.3);""border-radius:4px;padding:2px 6px;margin-left:6px;font-weight:600;white-space:nowrap;}"".addbtn{width:100%%;padding:10px;border-radius:8px;""border:1px dashed var(--border);background:transparent;""color:var(--dim);cursor:pointer;font-size:.85rem;margin-top:6px;""transition:all .15s;font-family:var(--mono);}"".addbtn:hover{border-color:var(--accent);color:var(--accent);""background:var(--accent-dim);}"".btn-area{margin-top:8px;padding:8px 0;display:flex;flex-direction:row;flex-wrap:wrap;gap:8px;justify-content:center;border-top:1px solid var(--border);flex-shrink:0;}"".submit{flex:1;min-width:110px;max-width:220px;padding:9px 18px;border-radius:10px;border:none;""background:linear-gradient(135deg,var(--accent) 0%%,#047857 100%%);""color:#021a18;font-weight:700;font-size:.9rem;cursor:pointer;""transition:opacity .15s,transform .1s;""box-shadow:0 4px 20px var(--accent-glow);}"".submit:hover{opacity:.92;transform:translateY(-1px);}"".submit:active{transform:translateY(0);}"".reset{flex:1;min-width:100px;max-width:200px;padding:7px 14px;border-radius:10px;""border:1px solid var(--border);background:transparent;""color:var(--dim);font-size:.85rem;font-weight:500;cursor:pointer;""transition:all .15s;}"".reset:hover{border-color:var(--red);color:var(--red);background:rgba(248,113,113,.06);}""  .sm-ctrl{padding:8px 20px;border-radius:8px;font-family:var(--mono);font-size:.82rem;cursor:pointer;border:1px solid;font-weight:600;transition:all .15s;}"".sm-ctrl.stop{border-color:rgba(248,113,113,.4);background:rgba(248,113,113,.08);color:#f87171;}"".sm-ctrl.stop:hover{background:rgba(248,113,113,.2);}"".sm-ctrl.start{border-color:rgba(16,185,129,.4);background:rgba(16,185,129,.08);color:#10b981;}"".sm-ctrl.start:hover{background:rgba(16,185,129,.2);}""  .layout{display:flex;gap:12px;flex:1;min-height:0;}""  .sidebar{width:175px;flex-shrink:0;background:var(--surface);border:1px solid var(--border);border-radius:12px;padding:6px;overflow-y:auto;transition:width .2s,padding .2s,border .2s;}.sidebar::-webkit-scrollbar{width:4px;}.sidebar::-webkit-scrollbar-thumb{background:rgba(255,255,255,.1);border-radius:2px;}.sidebar::-webkit-scrollbar-thumb:hover{background:var(--accent);}""  .nav-item{display:block;width:100%%;padding:10px 12px;border-radius:8px;border:none;background:transparent;""color:var(--dim);cursor:pointer;font-family:var(--mono);font-size:.78rem;font-weight:600;""text-transform:uppercase;letter-spacing:.05em;transition:all .15s;margin-bottom:2px;text-align:left;}""  .nav-item:hover{background:var(--surface2);color:var(--text);}""  .nav-item.active{background:var(--accent-dim);color:var(--accent);border-left:3px solid var(--accent);padding-left:9px;}""  .nav-item.dim{opacity:.38;pointer-events:none;}""  .nav-sep{height:1px;background:var(--border);margin:4px 6px;}""  .content{flex:1;min-width:0;min-height:0;overflow-y:auto;padding-bottom:80px;}.content::-webkit-scrollbar{width:4px;}"".panel{display:none;}"".panel.active{display:block;}""@media(max-width:860px){.layout{flex-direction:column;}.sidebar{position:sticky;top:0;z-index:10;width:100%%;display:flex;overflow-x:auto;gap:4px;padding:6px;border-radius:0;border-bottom:1px solid var(--border);border-left:none;border-right:none;border-top:none;}.nav-item{flex-shrink:0;white-space:nowrap;width:auto;padding:7px 10px;margin-bottom:0;border-left:none!important;padding-left:10px!important;}.nav-sep{display:none;}}"  "@media(max-width:600px){h1{font-size:1.3rem;}""input[type=number]{width:90px;min-width:60px;}""input[type=text]{width:100%%;min-width:0;}""  .numfield{flex-wrap:wrap;}""  .numfield input[type=text]{flex:1;width:100%%;}""  .chip{padding:5px 8px;font-size:.68rem;}""  .topbar{gap:6px;}""  .btn-area{flex-direction:column;}""  .submit,.reset{max-width:none;}}"".footer{text-align:center;color:var(--dim);font-family:var(--mono);""font-size:.75rem;margin-top:36px;opacity:.45;}"".log-out{background:var(--bg);border:1px solid var(--border);border-radius:8px;padding:12px;font-family:var(--mono);font-size:.73rem;line-height:1.5;color:#94a3b8;overflow-x:auto;overflow-y:auto;max-height:500px;white-space:pre;margin:0;width:0;min-width:100%%;box-sizing:border-box;}"
"@media(max-width:720px){.log-out{white-space:pre-wrap;overflow-x:hidden;}}"
          ".bak-row{display:flex;gap:4px;align-items:center;padding:5px 0;border-bottom:1px solid var(--border);min-width:0;}"
          ".bak-nm{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;color:var(--text);font-size:.78rem;}"
          "@media(max-width:600px){"
          ".bak-row{flex-wrap:wrap;gap:2px;}"
          ".bak-nm{width:100%%;flex:none;font-size:.77rem;padding-bottom:2px;}"
          ".bak-row button{padding:3px 5px !important;font-size:.7rem !important;}}"
          "::-webkit-scrollbar{width:8px;height:8px;}::-webkit-scrollbar-track{background:transparent;}::-webkit-scrollbar-thumb{background:#444;border-radius:10px;}::-webkit-scrollbar-thumb:hover{background:var(--accent);}"
          ".ico{width:14px;height:14px;vertical-align:-.15em;pointer-events:none}"
          "#raw-editor::selection{background:#10b981;color:#021a18;}"
          "#raw-msg{position:fixed;bottom:20px;right:20px;z-index:9999;"
          "padding:8px 14px;border-radius:8px;border:1px solid;"
          "font-family:var(--mono);font-size:.78rem;display:none;"
          "background:var(--surface2);box-shadow:0 4px 20px rgba(0,0,0,.4);}"
          "</style></head><body><div class='card'>");

        /* Lucide SVG icon sprite — defined once, reused via <use href> */
        H("<svg style='display:none'>"
          "<symbol id='i-undo' viewBox='0 0 24 24'><g stroke='currentColor' fill='none' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'>"
          "<path d='M3 12a9 9 0 1 0 9-9 9.75 9.75 0 0 0-6.74 2.74L3 8'/><path d='M3 3v5h5'/></g></symbol>"
          "<symbol id='i-dl' viewBox='0 0 24 24'><g stroke='currentColor' fill='none' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'>"
          "<path d='M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4'/><polyline points='7 10 12 15 17 10'/><line x1='12' x2='12' y1='15' y2='3'/></g></symbol>"
          "<symbol id='i-ul' viewBox='0 0 24 24'><g stroke='currentColor' fill='none' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'>"
          "<path d='M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4'/><polyline points='17 8 12 3 7 8'/><line x1='12' x2='12' y1='3' y2='15'/></g></symbol>"
          "<symbol id='i-fout' viewBox='0 0 24 24'><g stroke='currentColor' fill='none' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'>"
          "<path d='M2 7.5V5c0-1.1.9-2 2-2h3.93a2 2 0 0 1 1.66.9l.82 1.2a2 2 0 0 0 1.66.9H20a2 2 0 0 1 2 2v10a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2v-2.5'/>"
          "<path d='M2 13h10'/><path d='m9 16 3-3-3-3'/></g></symbol>"
          "<symbol id='i-fin' viewBox='0 0 24 24'><g stroke='currentColor' fill='none' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'>"
          "<path d='M2 9V5c0-1.1.9-2 2-2h3.93a2 2 0 0 1 1.66.9l.82 1.2a2 2 0 0 0 1.66.9H20a2 2 0 0 1 2 2v10a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2v-4'/>"
          "<path d='M2 13h10'/><path d='m5 10-3 3 3 3'/></g></symbol>"
          "<symbol id='i-del' viewBox='0 0 24 24'><g stroke='currentColor' fill='none' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'>"
          "<path d='M3 6h18'/><path d='M19 6v14c0 1-1 2-2 2H7c-1 0-2-1-2-2V6'/>"
          "<path d='M8 6V4c0-1 1-2 2-2h4c1 0 2 1 2 2v2'/>"
          "<line x1='10' x2='10' y1='11' y2='17'/><line x1='14' x2='14' y1='11' y2='17'/></g></symbol>"
          "<symbol id='i-new' viewBox='0 0 24 24'><g stroke='currentColor' fill='none' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'>"
          "<path d='M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z'/>"
          "<line x1='12' x2='12' y1='11' y2='17'/><line x1='9' x2='15' y1='14' y2='14'/></g></symbol>"
          "<symbol id='i-ref' viewBox='0 0 24 24'><g stroke='currentColor' fill='none' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'>"
          "<path d='M3 12a9 9 0 0 1 9-9 9.75 9.75 0 0 1 6.74 2.74L21 8'/><path d='M21 3v5h-5'/>"
          "<path d='M21 12a9 9 0 0 1-9 9 9.75 9.75 0 0 1-6.74-2.74L3 16'/><path d='M8 16H3v5'/></g></symbol>"
          "<symbol id='i-cut' viewBox='0 0 24 24'><g stroke='currentColor' fill='none' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'>"
          "<circle cx='6' cy='6' r='3'/><circle cx='6' cy='18' r='3'/>"
          "<line x1='20' x2='8.12' y1='4' y2='15.88'/><line x1='14.47' x2='20' y1='14.48' y2='20'/>"
          "<line x1='8.12' x2='12' y1='8.12' y2='12'/></g></symbol>"
          "<symbol id='i-search' viewBox='0 0 24 24'><g stroke='currentColor' fill='none' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'>"
          "<circle cx='11' cy='11' r='8'/><path d='m21 21-4.3-4.3'/></g></symbol>"
          "<symbol id='i-pen' viewBox='0 0 24 24'><g stroke='currentColor' fill='none' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'>"
          "<path d='M17 3a2.83 2.83 0 1 1 4 4L7.5 20.5 2 22l1.5-5.5Z'/><path d='m15 5 4 4'/></g></symbol>"
          "<symbol id='i-menu' viewBox='0 0 24 24'><g stroke='currentColor' fill='none' stroke-width='2' stroke-linecap='round'>"
          "<line x1='3' x2='21' y1='6' y2='6'/><line x1='3' x2='21' y1='12' y2='12'/><line x1='3' x2='21' y1='18' y2='18'/></g></symbol>"
          "</svg>");

        /* Topbar + Status in einer Zeile */
        /* ShadowMount Prozess-Check via find_pid */
        const char *sm_proc_names[] = {
            "shadowmount.elf","ShadowMount.elf",
            "shadowmountplus.elf","ShadowMountPlus.elf",
            "shadowmount","ShadowMount",
            NULL
        };
        int sm_running = 0;
        for(int pi=0; sm_proc_names[pi]; pi++){
            if(find_pid(sm_proc_names[pi])>0){ sm_running=1; break; }
        }
        /* Fallback: API erreichbar? */
        if(!sm_running){
            int ts=socket(AF_INET,SOCK_STREAM,0);
            if(ts>=0){
                struct timeval tv={1,0};
                setsockopt(ts,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
                setsockopt(ts,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv));
                struct sockaddr_in ta;
                memset(&ta,0,sizeof(ta));
                ta.sin_family=AF_INET;
                ta.sin_port=htons(10101);
                ta.sin_addr.s_addr=inet_addr("127.0.0.1");
                if(connect(ts,(struct sockaddr*)&ta,sizeof(ta))==0) sm_running=1;
                close(ts);
            }
        }
        H("<div class='topbar'>"
          "<div style='display:flex;align-items:center;gap:12px;flex-wrap:wrap;flex:1;'>"
          "<button type='button' onclick='toggleSidebar()' style='background:transparent;border:1px solid var(--border);border-radius:6px;color:var(--dim);padding:4px 8px;cursor:pointer;font-size:1rem;flex-shrink:0;'>" ICO("menu") "</button>"
          "<h1 style='margin:0;flex-shrink:0;'>SMPlusGui <span style='font-size:.55em;opacity:.75;font-weight:500;'>v" SMPLUS_VERSION "</span></h1>"
          "<div class='chip'>PORT <b>" HTTP_PORT "</b></div>"
          "<div class='chip' id='sm-status-chip'>"
          "<span style='width:8px;height:8px;border-radius:50%%;display:inline-block;"
          "background:%s;box-shadow:0 0 6px %s;%s'></span>"
          "ShadowMount <b>%s</b></div>",
          sm_running ? "#10b981" : "#f87171",
          sm_running ? "#10b981" : "#f87171",
          sm_running ? "animation:pulse 2s infinite;" : "",
          sm_running ? L(LS_RUNNING) : L(LS_NOT_RUNNING));
        if(sm_running && strcmp(sm_ver,"nicht aktiv")!=0)
            H("<div class='chip' id='sm-ver-chip'>v <b>%s</b></div>", sm_ver);
        else
            H("<div class='chip' id='sm-ver-chip' style='display:none;'>v</div>");
        H("<button id='sm-ctrl-btn' class='sm-ctrl %s' data-action='%s' onclick='smCtrl()'>%s</button>",
          sm_running ? "stop" : "start",
          sm_running ? "stop" : "start",
          sm_running ? "Stop" : "Start");
        H("</div>"); /* close left flex */
        H("<div class='lang'>"
          "<a href='/setlang?l=auto' class='%s'>AUTO</a>"
          "<a href='/setlang?l=de' class='%s'>DE</a>"
          "<a href='/setlang?l=en' class='%s'>EN</a>"
          "<a href='/setlang?l=fr' class='%s'>FR</a>"
          "<a href='/setlang?l=es' class='%s'>ES</a>"
          "</div>"
          "</div>",
          is_auto?"active":"",
          !is_auto&&strcmp(lang,"de")==0?"active":"",
          !is_auto&&strcmp(lang,"en")==0?"active":"",
          !is_auto&&strcmp(lang,"fr")==0?"active":"",
          !is_auto&&strcmp(lang,"es")==0?"active":"");


        /* Warnung bei veralteter Version (< 1.6beta16) */
        if(sm_running && strcmp(sm_ver,"nicht aktiv")!=0 && strcmp(sm_ver,"aktiv")!=0){
            if(!sm_version_at_least(sm_ver,1,6,"beta",16)){
                H("<div style='margin-bottom:16px;padding:12px 16px;"
                  "background:rgba(248,113,113,.08);border:1px solid rgba(248,113,113,.3);"
                  "border-radius:10px;font-size:.85rem;font-family:var(--mono);color:#f87171;'>"
                  "&#9888; %s <b>%s</b> &mdash; %s</div>",
                  L(LS_WARNING_SM),
                  sm_ver,
                  L(LS_MIN_VER));
            }
        }
        /* Pre-compute version checks before sidebar */
        int has_lang      = !sm_running || sm_version_at_least(sm_ver,1,7,"alpha",4);
        int has_pim       = !sm_running || sm_version_at_least(sm_ver,1,7,"alpha",4);
        int has_autoremove= !sm_running || sm_version_at_least(sm_ver,1,7,"alpha",3);
        int has_api       = !sm_running || sm_version_at_least(sm_ver,1,7,"alpha",3);
        int has_fan       = !sm_running || sm_version_at_least(sm_ver,1,7,"alpha",5);

        H("<form action='/save' method='POST' onsubmit='try{doSave();}catch(e){}return false;' style='flex:1;min-height:0;display:flex;flex-direction:column;'><div class='layout'>");
        H("<nav class='sidebar'>");
        H("<button type='button' class='nav-item active' data-p='mnt' onclick='showP(this)'>Mounting</button>");
        H("<button type='button' class='nav-item' data-p='scn' onclick='showP(this)'>Scan</button>");
        H("<button type='button' class='nav-item' data-p='auto' onclick='showP(this)'>Autostart</button>");
        H("<button type='button' class='nav-item' data-p='arm' onclick='showP(this)'>Auto-Remove</button>");
        H("<div class='nav-sep'></div>");
        H("<button type='button' class='nav-item' data-p='kst' onclick='showP(this)'>kstuff</button>");
        H("<button type='button' class='nav-item' data-p='fkl' onclick='showP(this)'>Fakelib</button>");
        H("<button type='button' class='nav-item' data-p='bkd' onclick='showP(this)'>Backend</button>");
        H("<button type='button' class='nav-item' data-p='api' onclick='showP(this)'>API</button>");
        H("<button type='button' class='nav-item' data-p='sys' onclick='showP(this)'>Config</button>");
        H("<div class='nav-sep'></div>");
        H("<button type='button' class='nav-item' data-p='ovi' onclick='showP(this)'>%s</button>",L(LS_IMG_OVERRIDES));
        H("<button type='button' class='nav-item' data-p='man' onclick='showP(this)'>%s</button>",L(LS_MANUAL));
        H("<button type='button' class='nav-item' data-p='gen' onclick='showP(this)'>%s</button>",L(LS_NOTIFS));
        H("<button type='button' class='nav-item' data-p='log' onclick='showP(this)'>Log</button>");
        /* raw panel accessible via button in backup section, no sidebar tab */
        H("<button type='button' data-p='raw' onclick='showP(this)' style='display:none'></button>");
        H("</nav><div class='content'>");

        /* Panel: General / Notifications */
        H("<div id='panel-gen' class='panel'><div class='section'>");
        H("<div class='sublist-title'>%s</div>",L(LS_NOTIFS));
        H("<div id='lang-row' class='numfield' style='%s'>",has_lang?"":"opacity:.35;pointer-events:none;");
        H("<label>%s <span class='vbadge' id='lang-badge' style='%s'>ab 1.7alpha4</span></label>",
          L(LS_NOTIF_LANG),has_lang?"display:none;":"");
        { struct{const char*c;const char*n;}ll[]={
            {"auto","auto"},
            {"ar-SA","العربية (ar-SA)"},{"cs-CZ","Čeština (cs-CZ)"},
            {"da-DK","Dansk (da-DK)"},{"de-DE","Deutsch (de-DE)"},{"el-GR","Ελληνικά (el-GR)"},
            {"en-GB","English UK (en-GB)"},{"en-US","English US (en-US)"},
            {"es-ES","Español España (es-ES)"},{"es-MX","Español México (es-MX)"},
            {"fi-FI","Suomi (fi-FI)"},
            {"fr-CA","Français Canada (fr-CA)"},{"fr-FR","Français France (fr-FR)"},
            {"hu-HU","Magyar (hu-HU)"},{"id-ID","Indonesia (id-ID)"},
            {"it-IT","Italiano (it-IT)"},{"ja-JP","日本語 (ja-JP)"},{"ko-KR","한국어 (ko-KR)"},
            {"nl-NL","Nederlands (nl-NL)"},{"no-NO","Norsk (no-NO)"},{"pl-PL","Polski (pl-PL)"},
            {"pt-BR","Português Brasil (pt-BR)"},{"pt-PT","Português Portugal (pt-PT)"},
            {"ro-RO","Română (ro-RO)"},{"ru-RU","Русский (ru-RU)"},{"sv-SE","Svenska (sv-SE)"},
            {"th-TH","ภาษาไทย (th-TH)"},{"tr-TR","Türkçe (tr-TR)"},
            {"uk-UA","Українська (uk-UA)"},{"vi-VN","Tiếng Việt (vi-VN)"},
            {"zh-CN","中文简体 (zh-CN)"},{"zh-TW","中文繁體 (zh-TW)"},
            {NULL,NULL}};
          H("<select name='language'>");
          for(int li=0;ll[li].c;li++)
              H("<option value='%s'%s>%s</option>",ll[li].c,
                strcmp(cfg.language,ll[li].c)==0?" selected":"",ll[li].n);
          H("</select></div>"); }
        H("</div></div>");

        /* Panel: Mounting */
        /* ── Autostart panel ── */
        { SMPrefs prefs; read_prefs(&prefs);
          const char *elfname="ELF w\u00e4hlen";
          if(prefs.preferred_elf[0]){const char *sl=strrchr(prefs.preferred_elf,'/');elfname=sl?sl+1:prefs.preferred_elf;}
          H("<div id='panel-auto' class='panel'><div class='section'>");
          H("<div class='sublist-title'>Autostart</div>");
          H("<div class='row'><label>%s</label>"
            "<input type='checkbox' id='as-chk' style='display:none;'%s>"
            "<label class='switch' for='as-chk' onclick='onAS()'></label></div>",
            L(LS_AUTOSTART), prefs.auto_start?" checked":"");
          H("<div class='numfield' style='margin-top:12px;'><label>%s</label>"
            "<button type='button' id='as-elf-btn' onclick='pickASElf()' "
            "style='background:transparent;border:1px solid var(--border);border-radius:6px;"
            "color:var(--dim);padding:5px 12px;cursor:pointer;font-family:var(--mono);font-size:.8rem;'>%s &#9660;</button></div>"
            "<div class='numfield' style='margin-top:8px;'><label>Custom path</label>"
            "<input type='text' id='as-custom-path' placeholder='/data/pldmgr/payloads/shadowmount/shadowmount.elf'"
            " style='width:100%%;background:var(--surface2);border:1px solid var(--border);border-radius:6px;"
            "color:var(--text);padding:5px 10px;font-family:var(--mono);font-size:.75rem;'"
            " value='%s' onchange='setASCustomPath(this.value)'></div>",
            L(LS_PREF_ELF), elfname, prefs.preferred_elf);
          H("</div></div>"); } /* close autostart section + panel-auto */

        /* Panel: Auto-Remove */
        H("<div id='panel-arm' class='panel'><div id='ar-wrap' style='opacity:%s;pointer-events:%s'><div class='section'>",has_autoremove?"1":"0.35",has_autoremove?"auto":"none");
        H("<div class='sublist-title'>Auto-Remove</div>");
        H("<div class='row'><label for='arm'>%s <span class='ar-b vbadge' style='%s'>ab 1.7alpha3</span></label>"
          "<input type='checkbox' id='arm' name='auto_remove_missing_games' value='1' %s>"
          "<label class='switch' for='arm'></label></div>",
          L(LS_RM_MISSING),has_autoremove?"display:none;":"",cfg.auto_remove_missing_games?"checked":"");
        H("<div class='row'><label for='argd'>%s <span class='ar-b vbadge' style='%s'>ab 1.7alpha3</span></label>"
          "<input type='checkbox' id='argd' name='auto_remove_games_with_dlc' value='1' %s>"
          "<label class='switch' for='argd'></label></div>",
          L(LS_RM_DLC),has_autoremove?"display:none;":"",cfg.auto_remove_games_with_dlc?"checked":"");
        H("<div class='numfield'><label>%s <span style='font-size:.7rem;color:var(--dim);font-weight:normal;'>(1\u201386400)</span> <span class='ar-b vbadge' style='%s'>ab 1.7alpha3</span></label>"
          "<input type='number' name='auto_remove_missing_delay_seconds' min='1' max='86400' value='%d'></div>",
          L(LS_DELAY_SEC),has_autoremove?"display:none;":"",cfg.auto_remove_missing_delay_seconds);
        H("</div></div></div>"); /* close section + ar-wrap + panel-arm */

        /* Panel: Mounting */
        H("<div id='panel-mnt' class='panel active'><div class='section'>");
        H("<div class='sublist-title'>Mounting</div>");
        SW("ro","mount_read_only",L(LS_READ_ONLY),cfg.mount_read_only);
        SW("fm","force_mount",L(LS_MOUNT_DAMAGED),cfg.force_mount);
        H("<p class='hint' style='margin:-4px 0 10px;'>&#9888; %s</p>",L(LS_FORCE_HINT));
        {
            H("<div id='pim-row' class='row' style='%s'>",has_pim?"":"opacity:.35;pointer-events:none;");
            H("<label for='pim'>%s <span class='vbadge' id='pim-badge' style='%s'>ab 1.7alpha4</span></label>",
              L(LS_KEEP_IMGS),has_pim?"display:none":"");
            H("<input type='checkbox' id='pim' name='persistent_image_mounts' value='1' %s>"
              "<label class='switch' for='pim'></label></div>",cfg.persistent_image_mounts?"checked":"");
        }
        SW("aia","app_install_all",L(LS_BATCH_REG),cfg.app_install_all);
        H("</div></div>");

        /* Panel: Scan */
        H("<div id='panel-scn' class='panel'><div class='section'>");
        H("<div class='sublist-title'>Scan</div>");
        H("<div class='numfield'><label>%s</label>"
          "<select name='scan_depth'>"
          "<option value='1'%s>1 &mdash; %s</option>"
          "<option value='2'%s>2 &mdash; %s</option>"
          "</select></div>",
          L(LS_SCAN_DEPTH),
          cfg.scan_depth==1?" selected":"",
          L(LS_DEPTH_1),
          cfg.scan_depth==2?" selected":"",
          L(LS_DEPTH_2));
        NF("scan_interval_seconds",L(LS_SCAN_INTV),"1","3600",cfg.scan_interval_seconds);
        NF("stability_wait_seconds",L(LS_STAB_WAIT),"0","3600",cfg.stability_wait_seconds);
        H("<div class='sublist-title' style='margin-top:16px;'>%s</div>",L(LS_SCAN_PATHS));
        H("<p class='hint' style='margin-bottom:8px;'>&#9432; %s</p>",L(LS_SCANPATH_HINT));
        H("<div id='paths-list'>");
        for(int i=0;i<cfg.path_count;i++)
            H("<div class='path-row'><input type='text' name='paths[]' value='%s' placeholder='/data/homebrew'><button type='button' class='rm' onclick='this.parentElement.remove()'>&times;</button></div>",cfg.scanpaths[i]);
        H("</div><button type='button' class='addbtn' onclick='addPath()'>+ %s</button>",L(LS_ADD_PATH));
        H("</div></div>");

        /* Panel: kstuff */
        H("<div id='panel-kst' class='panel'><div class='section'>");
        H("<div class='sublist-title'>kstuff</div>");
        SW("kcd","kstuff_crash_detection",L(LS_CRASH_TUNE),cfg.kstuff_crash_detection);
        H("<p class='hint' style='margin:-4px 0 10px;'>&#9432; %s</p>",L(LS_KSTUFF_TOG));
        NF("kstuff_pause_delay_image_seconds",L(LS_PAUSE_IMG),"1","3600",cfg.kstuff_pause_delay_image_seconds);
        NF("kstuff_pause_delay_direct_seconds",L(LS_PAUSE_DIR),"1","3600",cfg.kstuff_pause_delay_direct_seconds);
        H("<p class='hint' style='margin:-4px 0 10px;'>&#9432; %s</p>",L(LS_KSTUFF_DLY));
        H("<div class='sublist-title' style='margin-top:16px;'>kstuff_no_pause</div>");
        H("<p class='hint'><b>&#128196; Beispiel:</b> &nbsp;<code>PPSA12345</code> &nbsp;&rarr;&nbsp; %s</p>",L(LS_KSTUFF_NOPAUSE));
        H("<div id='kstuff-np-list'>");
        for(int i=0;i<cfg.kstuff_no_pause_count;i++)
            H("<div class='path-row'><input type='text' name='kstuff_no_pause[]' value='%s' placeholder='PPSA12345'><button type='button' class='rm' onclick='this.parentElement.remove()'>&times;</button></div>",cfg.kstuff_no_pause[i]);
        H("</div><button type='button' class='addbtn' onclick='addRow(\"kstuff-np-list\",\"kstuff_no_pause[]\",\"PPSA12345\")'>+ kstuff_no_pause</button>");
        H("<div class='sublist-title' style='margin-top:12px;'>kstuff_delay</div>");
        H("<p class='hint'><b>&#128196; Beispiel:</b> &nbsp;<code>PPSA12345:30</code> &nbsp;(%s)</p>",L(LS_KSTUFF_FMT));
        H("<div id='kstuff-dl-list'>");
        for(int i=0;i<cfg.kstuff_delay_count;i++)
            H("<div class='path-row'><input type='text' name='kstuff_delay[]' value='%s' placeholder='PPSA12345:30'><button type='button' class='rm' onclick='this.parentElement.remove()'>&times;</button></div>",cfg.kstuff_delay[i]);
        H("</div><button type='button' class='addbtn' onclick='addRow(\"kstuff-dl-list\",\"kstuff_delay[]\",\"PPSA12345:30\")'>+ kstuff_delay</button>");
        H("</div></div>"); /* close kstuff section + panel-kst */

        /* Panel: Fakelib */
        H("<div id='panel-fkl' class='panel'><div class='section'>");
        H("<div class='sublist-title'>Fakelib</div>");
        SW("gf","global_fakelib",L(LS_EN_GLOBAL_FL),cfg.global_fakelib);
        H("<p class='hint' style='margin:-4px 0 10px;'>&#9432; %s</p>",L(LS_FAKELIB_HINT));
        TF("global_fakelib_path",L(LS_FL_PATH),cfg.global_fakelib_path,"/data/shadowmount/fakelib");
        H("<div class='numfield'><label>%s</label>"
          "<select name='global_fakelib_priority'>"
          "<option value='game'%s>game &mdash; %s</option>"
          "<option value='global'%s>global &mdash; %s</option>"
          "</select></div>",
          L(LS_PRIORITY),
          strcmp(cfg.global_fakelib_priority,"game")==0?" selected":"",
          L(LS_PRIO_GAME),
          strcmp(cfg.global_fakelib_priority,"global")==0?" selected":"",
          L(LS_PRIO_GLOBAL));
        H("<div class='sublist-title' style='margin-top:16px;'>global_fakelib_exclude</div>");
        H("<p class='hint'><b>&#128196; Beispiel:</b> &nbsp;<code>PPSA12345</code> &nbsp;&rarr;&nbsp; %s</p>",L(LS_FAKELIB_EXCL));
        H("<div id='fakelib-ex-list'>");
        for(int i=0;i<cfg.global_fakelib_exclude_count;i++)
            H("<div class='path-row'><input type='text' name='global_fakelib_exclude[]' value='%s' placeholder='PPSA12345'><button type='button' class='rm' onclick='this.parentElement.remove()'>&times;</button></div>",cfg.global_fakelib_exclude[i]);
        H("</div><button type='button' class='addbtn' onclick='addRow(\"fakelib-ex-list\",\"global_fakelib_exclude[]\",\"PPSA12345\")'>+ global_fakelib_exclude</button>");
        H("</div></div>"); /* close fakelib section + panel-fkl */

        /* Panel: Backend */
        H("<div id='panel-bkd' class='panel'><div class='section'>");
        H("<div class='sublist-title'>Backend</div>");
        H("<p class='hint' style='margin-bottom:12px;'>&#9432; %s</p>",L(LS_SECTOR_HINT));
        H("<div class='numfield'><label>%s</label>"
          "<select name='exfat_backend'>"
          "<option value='lvd'%s>lvd &mdash; /dev/lvdctl</option>"
          "<option value='md'%s>md &mdash; /dev/mdctl</option>"
          "</select></div>",
          L(LS_EXFAT_BE),
          strcmp(cfg.exfat_backend,"lvd")==0?" selected":"",
          strcmp(cfg.exfat_backend,"md")==0?" selected":"");
        H("<div class='numfield'><label>%s</label>"
          "<select name='ufs_backend'>"
          "<option value='lvd'%s>lvd &mdash; /dev/lvdctl</option>"
          "<option value='md'%s>md &mdash; /dev/mdctl</option>"
          "</select></div>",
          L(LS_UFS_BE),
          strcmp(cfg.ufs_backend,"lvd")==0?" selected":"",
          strcmp(cfg.ufs_backend,"md")==0?" selected":"");
        NF("lvd_exfat_sector_size",L(LS_LVD_EXFAT_S),"512","65536",cfg.lvd_exfat_sector_size);
        NF("lvd_ufs_sector_size",L(LS_LVD_UFS_S),"512","65536",cfg.lvd_ufs_sector_size);
        NF("lvd_pfs_sector_size",L(LS_LVD_PFS_S),"512","65536",cfg.lvd_pfs_sector_size);
        NF("md_exfat_sector_size",L(LS_MD_EXFAT_S),"512","65536",cfg.md_exfat_sector_size);
        NF("md_ufs_sector_size",L(LS_MD_UFS_S),"512","65536",cfg.md_ufs_sector_size);
        /* fan_target_temperature belongs here as SM hardware setting */
        H("<div class='sublist-title' style='margin-top:16px;'>fan_target_temperature</div>");
        H("<p class='hint' style='margin-bottom:8px;'>&#9432; %s</p>",L(LS_FAN_HINT));
        H("<div id='fan-row' style='%s'>",has_fan?"":"opacity:.35;pointer-events:none");
        H("<div class='numfield'><label>fan_target_temperature"
          "<span class='vbadge' style='%s'>ab 1.7alpha5</span></label>",
          has_fan?"display:none;":"");
        H("<select name='fan_target_temperature'><option value='auto'%s>auto</option>",
          strcmp(cfg.fan_target_temperature,"auto")==0?" selected":"");
        { int cur=atoi(cfg.fan_target_temperature);
          for(int t=50;t<=91;t++)
              H("<option value='%d'%s>%d&deg;C</option>",t,cur==t?" selected":"",t); }
        H("</select></div></div>");
        H("</div></div>"); /* close backend section + panel-bkd */

        /* Panel: API */
        H("<div id='panel-api' class='panel'><div id='api-wrap' style='opacity:%s;pointer-events:%s'><div class='section'>",
          has_api?"1":"0.35",has_api?"auto":"none");
        H("<div class='sublist-title'>API</div>");
        H("<div class='numfield'><label>%s <span class='vbadge' style='%s'>ab 1.7alpha3</span></label>"
          "<select name='api_bind_address'>"
          "<option value='127.0.0.1'%s>127.0.0.1 &mdash; %s</option>"
          "<option value='0.0.0.0'%s>0.0.0.0 &mdash; %s</option>"
          "</select></div>",
          L(LS_BIND_ADDR),has_api?"display:none;":"",
          strcmp(cfg.api_bind_address,"127.0.0.1")==0?" selected":"",
          L(LS_LOCAL_ONLY),
          strcmp(cfg.api_bind_address,"0.0.0.0")==0?" selected":"",
          L(LS_ALL_IFACES));
        H("<div class='numfield'><label>%s <span class='vbadge' style='%s'>ab 1.7alpha3</span></label>"
          "<input type='number' name='api_port' min='1' max='65535' value='%d'></div>",
          L(LS_PORT),has_api?"display:none;":"",cfg.api_port);
        H("</div></div></div>"); /* close section + api-wrap + panel-api */

        /* Panel: Config/Backup */
        { const char *bdir=BAK_DIR;
          mkdir("/data/SMPlusGui",0755); mkdir(bdir,0755);
          /* collect and sort local backups */
          char lnames[100][64]; int ln=0;
          { DIR *dp=opendir(bdir);
            if(dp){ struct dirent *de;
              while((de=readdir(dp))&&ln<100){
                char *nm=de->d_name; size_t nl=strlen(nm);
                if(nm[0]=='.'||nl<8||strcmp(nm+nl-4,".ini")!=0||strncmp(nm,"cfg_",4)!=0) continue;
                strncpy(lnames[ln++],nm,63);
              }
              closedir(dp);
            }
          }
          for(int i=0;i<ln-1;i++) for(int j=i+1;j<ln;j++)
            if(strcmp(lnames[i],lnames[j])<0){char t[64];strncpy(t,lnames[i],63);strncpy(lnames[i],lnames[j],63);strncpy(lnames[j],t,63);}
          /* find USB and collect USB backups */
          char usb_path[32]={0}; int usb_ok=0;
          for(int i=0;i<=7&&!usb_ok;i++){
            char p[24]; snprintf(p,sizeof(p),"/mnt/usb%d",i);
            DIR *d=opendir(p); if(d){
              struct dirent *de2; int n=0;
              while((de2=readdir(d))&&n<4) n++;
              closedir(d); if(n>2){strncpy(usb_path,p,31);usb_ok=1;}
            }
          }
          char usb_dir[64]={0};
          char unames[100][64]; int un=0;
          if(usb_ok){
            snprintf(usb_dir,sizeof(usb_dir),"%s/SMPlusGui",usb_path);
            DIR *udp=opendir(usb_dir);
            if(udp){ struct dirent *ude;
              while((ude=readdir(udp))&&un<100){
                char *nm=ude->d_name; size_t nl=strlen(nm);
                if(nm[0]=='.'||nl<8||strcmp(nm+nl-4,".ini")!=0||strncmp(nm,"cfg_",4)!=0) continue;
                strncpy(unames[un++],nm,63);
              }
              closedir(udp);
            }
            for(int i=0;i<un-1;i++) for(int j=i+1;j<un;j++)
              if(strcmp(unames[i],unames[j])<0){char t[64];strncpy(t,unames[i],63);strncpy(unames[i],unames[j],63);strncpy(unames[j],t,63);}
          }

          #define _BW6 "background:transparent;border:1px solid var(--dim);border-radius:6px;color:var(--dim);padding:6px 12px;cursor:pointer;font-size:.8rem;"
          #define _BW4 "background:transparent;border:1px solid var(--dim);border-radius:4px;color:var(--dim);padding:4px 9px;cursor:pointer;font-size:.75rem;"
          #define _BA4 "background:transparent;border:1px solid var(--accent);border-radius:4px;color:var(--accent);padding:4px 9px;cursor:pointer;font-size:.75rem;"
          #define _BR4 "background:transparent;border:1px solid #f87171;border-radius:4px;color:#f87171;padding:4px 9px;cursor:pointer;font-size:.75rem;"
          #define _BGN "background:rgba(16,185,129,.08);border:1px solid rgba(16,185,129,.4);border-radius:6px;color:#10b981;padding:6px 12px;cursor:pointer;font-size:.8rem;"
          #define _BRD "background:rgba(248,113,113,.08);border:1px solid rgba(248,113,113,.4);border-radius:6px;color:#f87171;padding:6px 12px;cursor:pointer;font-size:.8rem;"
          H("<div id='panel-sys' class='panel'><div class='section'>");

          /* ═══ Aktuelle Config ═══ */
          H("<div class='sublist-title'>%s</div>",L(LS_CURR_CONFIG));
          H("<p class='hint' style='margin-bottom:8px;'>&#9432; <code>%s</code></p>",CONFIG_PATH);
          H("<div style='display:flex;gap:6px;flex-wrap:wrap;margin-bottom:20px;'>");
          H("<button type='button' onclick='cfgDownBak(\"\")' style='" _BW6 "'>" ICO("dl") " %s</button>",L(LS_DL_CURRENT));
          H("<button type='button' onclick='cfgUsbExport(\"\")' style='" _BW6 "'>" ICO("fout") " %s</button>",L(LS_USB_EXPORT));
          H("<button type='button' onclick='cfgUsbImport()' style='" _BW6 "'>" ICO("fin") " %s</button>",L(LS_USB_IMPORT));
          H("<button type='button' onclick='cfgImportDirect()' style='" _BW6 "'>" ICO("ul") " %s</button>",L(LS_IMPORT_PC));
          H("<button type='button' onclick='openRaw()' style='background:transparent;border:1px solid var(--dim);border-radius:6px;color:var(--dim);padding:6px 12px;cursor:pointer;font-size:.8rem;' title='config.ini direkt bearbeiten'>" ICO("pen") " Raw</button>");
          H("</div>");

          /* ═══ Interne Backups — komplett eigenständig ═══ */
          H("<div class='sublist-title'>%s</div>",L(LS_INTERN_BAKS));
          H("<p class='hint' style='margin-bottom:8px;'>&#9432; <code>%s</code></p>",bdir);
          H("<div style='display:flex;gap:6px;align-items:center;flex-wrap:wrap;margin-bottom:8px;'>");
          H("<span style='color:var(--dim);font-size:.8rem;white-space:nowrap;'>Name:</span>");
          H("<input id='bak-name-intern' type='text' placeholder='%s'"
            " style='flex:1;min-width:110px;background:var(--surface2);border:1px solid var(--accent);"
            "border-radius:8px;color:var(--text);padding:6px 10px;font-size:.82rem;'>",L(LS_BAK_NAME_PH));
          H("<button type='button' style='" _BGN "' onclick='cfgBackup()'>" ICO("new") " %s</button>",L(LS_BAK_CREATE));
          H("<button type='button' onclick='cfgUpload()' style='" _BW6 "'>" ICO("ul") " %s</button>",L(LS_BAK_IMPORT_PC));
          H("</div>");
          if(ln==0)
            H("<p class='hint'>%s</p>",L(LS_BAK_NO_BAKS));
          else{
            H("<div style='font-family:var(--mono);font-size:.78rem;'>");
            for(int i=0;i<ln;i++){
              char *nm=lnames[i];
              char disp[48]={0}; size_t nlen=strlen(nm);
              if(nlen>=22){
                snprintf(disp,sizeof(disp),"%.2s.%.2s.%.4s  %.2s:%.2s",nm+10,nm+8,nm+4,nm+13,nm+15);
                if(nlen>23){
                  char cn[32]={0}; strncpy(cn,nm+20,31);
                  char *dot=strrchr(cn,'.'); if(dot)*dot=0;
                  char *p2=cn; while(*p2){if(*p2=='_')*p2=' ';p2++;}
                  strncat(disp,"  ",sizeof(disp)-strlen(disp)-1);
                  strncat(disp,cn,sizeof(disp)-strlen(disp)-1);
                }
              } else strncpy(disp,nm,31);
              H("<div data-bak-f='%s' class='bak-row'>",nm);
              H("<span class='bak-nm'>%s</span>",disp);
              H("<button type='button' title='%s' onclick='cfgRestore(\"%s\")' style='" _BA4 "'>" ICO("undo") "</button>",L(LS_BAK_RESTORE),nm);
              H("<button type='button' title='%s' onclick='cfgDownBak(\"%s\")' style='" _BW4 "'>" ICO("dl") "</button>",L(LS_BAK_DL),nm);
              H("<button type='button' title='%s' onclick='cfgUsbExport(\"%s\")' style='" _BW4 "'>" ICO("fout") "</button>",L(LS_USB_EXPORT),nm);
              H("<button type='button' title='%s' onclick='cfgMoveToUsb(\"%s\")' style='" _BW4 "'>" ICO("cut") "</button>",L(LS_MOVE_TO_USB),nm);
              H("<button type='button' title='%s' onclick='cfgDelBak(\"%s\")' style='" _BR4 "'>" ICO("del") "</button>",L(LS_BAK_DELETE),nm);
              H("</div>");
            }
            H("</div>");
          }
          /* Intern-Fußzeile */
          H("<div style='display:flex;gap:5px;flex-wrap:wrap;margin-top:8px;padding-top:8px;border-top:1px solid var(--border);margin-bottom:20px;'>");
          H("<button type='button' onclick='cfgDownAll()' style='" _BW6 "'>" ICO("dl") " %s</button>",L(LS_BAK_DL_ALL));
          if(usb_ok&&ln>0){
            H("<button type='button' onclick='cfgUsbExportAll()' style='" _BW6 "'>" ICO("fout") " %s</button>",L(LS_BAK_USB_ALL));
            H("<button type='button' onclick='cfgMoveAllToUsb()' style='" _BW6 "'>" ICO("cut") " %s</button>",L(LS_MOVE_ALL_USB));
          }
          if(ln>0)
            H("<button type='button' style='" _BRD "' onclick='cfgDelAll()'><b>" ICO("del") "</b> %s</button>",L(LS_BAK_DEL_ALL));
          H("</div>");

          /* ═══ USB-Backups — identische Struktur, gespiegelt ═══ */
          H("<div class='sublist-title'>%s</div>",L(LS_USB_BAKS));
          if(!usb_ok){
            H("<p class='hint'>&#10007; %s</p>",L(LS_USB_NO_DRIVE));
          } else {
            H("<p class='hint' style='margin-bottom:8px;'>&#9432; <code>%s</code></p>",usb_dir);
            H("<div style='display:flex;gap:6px;align-items:center;flex-wrap:wrap;margin-bottom:8px;'>");
            H("<span style='color:var(--dim);font-size:.8rem;white-space:nowrap;'>Name:</span>");
            H("<input id='bak-name-usb' type='text' placeholder='%s'"
              " style='flex:1;min-width:110px;background:var(--surface2);border:1px solid var(--accent);"
              "border-radius:8px;color:var(--text);padding:6px 10px;font-size:.82rem;'>",L(LS_BAK_NAME_PH));
            H("<button type='button' style='" _BGN "' onclick='cfgBackupToUsb()'>" ICO("new") " %s</button>",L(LS_BAK_CREATE));
            H("<button type='button' onclick='cfgUploadUsb()' style='" _BW6 "'>" ICO("ul") " %s</button>",L(LS_BAK_IMPORT_PC));
            H("</div>");
            if(un==0)
              H("<p class='hint'>%s</p>",L(LS_USB_NO_BAKS));
            else{
              H("<div style='font-family:var(--mono);font-size:.78rem;'>");
              for(int i=0;i<un;i++){
                char *nm=unames[i];
                char disp[48]={0}; size_t nlen=strlen(nm);
                if(nlen>=22){
                  snprintf(disp,sizeof(disp),"%.2s.%.2s.%.4s  %.2s:%.2s",nm+10,nm+8,nm+4,nm+13,nm+15);
                  if(nlen>23){
                    char cn[32]={0}; strncpy(cn,nm+20,31);
                    char *dot=strrchr(cn,'.'); if(dot)*dot=0;
                    char *p2=cn; while(*p2){if(*p2=='_')*p2=' ';p2++;}
                    strncat(disp,"  ",sizeof(disp)-strlen(disp)-1);
                    strncat(disp,cn,sizeof(disp)-strlen(disp)-1);
                  }
                } else strncpy(disp,nm,31);
                H("<div class='bak-row' style='background:rgba(59,130,246,.04);'>");
                H("<span class='bak-nm'>%s</span>",disp);
                H("<button type='button' title='%s' onclick='cfgUsbRestoreOne(\"%s\")' style='" _BA4 "'>" ICO("undo") "</button>",L(LS_BAK_RESTORE),nm);
                H("<button type='button' title='%s' onclick='cfgDownUsbBak(\"%s\")' style='" _BW4 "'>" ICO("dl") "</button>",L(LS_BAK_DL),nm);
                H("<button type='button' title='%s' onclick='cfgCopyToLocal(\"%s\")' style='" _BW4 "'>" ICO("fin") "</button>",L(LS_COPY_TO_LOCAL),nm);
                H("<button type='button' title='%s' onclick='cfgMoveToLocal(\"%s\")' style='" _BW4 "'>" ICO("cut") "</button>",L(LS_MOVE_FROM_USB),nm);
                H("<button type='button' title='%s' onclick='cfgDelUsbBak(\"%s\")' style='" _BR4 "'>" ICO("del") "</button>",L(LS_BAK_DELETE),nm);
                H("</div>");
              }
              H("</div>");
            }
            /* USB-Fußzeile — gleiche Labels wie Intern */
            H("<div style='display:flex;gap:5px;flex-wrap:wrap;margin-top:8px;padding-top:8px;border-top:1px solid var(--border);'>");
            H("<button type='button' onclick='cfgDownAllUsb()' style='" _BW6 "'>" ICO("dl") " %s</button>",L(LS_BAK_DL_ALL));
            if(un>0){
              H("<button type='button' onclick='cfgCopyAllToLocal()' style='" _BW6 "'>" ICO("fin") " %s</button>",L(LS_COPY_ALL_LOCAL));
              H("<button type='button' onclick='cfgMoveAllToLocal()' style='" _BW6 "'>" ICO("cut") " %s</button>",L(LS_MOVE_ALL_LOCAL));
            }
            if(un>0)
              H("<button type='button' style='" _BRD "' onclick='cfgDelAllUsb()'><b>" ICO("del") "</b> %s</button>",L(LS_BAK_DEL_ALL));
            H("</div>");
          }
          #undef _BW6
          #undef _BW4
          #undef _BA4
          #undef _BR4
          #undef _BGN
          #undef _BRD
          H("</div></div>"); }


        /* Panel: Image Overrides */
        H("<div id='panel-ovi' class='panel'><div class='section'>");
        H("<div class='sublist-title'>%s</div>",L(LS_IMG_OVERRIDES));
        H("<p class='hint'><b>&#128196; Beispiel:</b> &nbsp;<code>PPSA12345.exfat</code> &nbsp;%s&nbsp; <code>PPSA12345.ffpfs</code></p>",L(LS_OR));
        H("<div id='img-ro-list'>");
        for(int i=0;i<cfg.image_ro_count;i++)
            H("<div class='path-row'><input type='text' name='image_ro[]' value='%s' placeholder='PPSA12345.exfat'><button type='button' class='rm' onclick='this.parentElement.remove()'>&times;</button></div>",cfg.image_ro[i]);
        H("</div><button type='button' class='addbtn' onclick='addRow(\"img-ro-list\",\"image_ro[]\",\"PPSA12345.exfat\")'>+ image_ro</button>");
        H("<div class='sublist-title' style='margin-top:12px;'>image_rw</div>");
        H("<p class='hint'><b>&#128196; Beispiel:</b> &nbsp;<code>PPSA12345.exfat</code> &nbsp;&rarr;&nbsp; %s</p>",L(LS_MOUNTED_RW));
        H("<div id='img-rw-list'>");
        for(int i=0;i<cfg.image_rw_count;i++)
            H("<div class='path-row'><input type='text' name='image_rw[]' value='%s' placeholder='PPSA12345.exfat'><button type='button' class='rm' onclick='this.parentElement.remove()'>&times;</button></div>",cfg.image_rw[i]);
        H("</div><button type='button' class='addbtn' onclick='addRow(\"img-rw-list\",\"image_rw[]\",\"PPSA12345.exfat\")'>+ image_rw</button>");
        H("<div class='sublist-title' style='margin-top:12px;'>image_sector</div>");
        H("<p class='hint'><b>&#128196; Beispiel:</b> &nbsp;<code>PPSA12345.exfat:65536</code> &nbsp;(%s)</p>",L(LS_SECTOR_FMT));
        H("<div id='img-sec-list'>");
        for(int i=0;i<cfg.image_sector_count;i++)
            H("<div class='path-row'><input type='text' name='image_sector[]' value='%s' placeholder='PPSA12345.exfat:65536'><button type='button' class='rm' onclick='this.parentElement.remove()'>&times;</button></div>",cfg.image_sector[i]);
        H("</div><button type='button' class='addbtn' onclick='addRow(\"img-sec-list\",\"image_sector[]\",\"PPSA12345.exfat:65536\")'>+ image_sector</button>");
        H("</div></div>");

        /* Panel: Manual List */
        H("<div id='panel-man' class='panel'><div class='section'>");
        H("<div class='sublist-title'>%s</div>",L(LS_MANUAL));
        H("<p class='hint'>&#9432; %s</p>",
          L(LS_MANUAL_HINT));
        H("<p class='hint' style='margin-top:6px;'><b>&#128196; Beispiel:</b> &nbsp;<code>/mnt/usb0/PPSA12345</code> &nbsp;%s&nbsp; <code>/mnt/usb0/PPSA12345.ffpkg</code></p>",L(LS_OR));
        H("<div id='manual-list'>");
        for(int i=0;i<manual_count;i++)
            H("<div class='path-row'><input type='text' name='manual[]' value='%s' placeholder='/mnt/usb0/PPSA12345.ffpkg'><button type='button' class='rm' onclick='this.parentElement.remove()'>&times;</button></div>",manual_entries[i]);
        H("</div><button type='button' class='addbtn' onclick='addRow(\"manual-list\",\"manual[]\",\"/mnt/usb0/PPSA12345.ffpkg\")'>+ %s</button>",L(LS_ADD_ENTRY));
        H("</div></div>");

        /* Panel: Debug Log */
        H("<div id='panel-log' class='panel'><div class='section' style='padding:14px 16px;'>");
        H("<div class='sublist-title'>Log</div>");
        SW("dbg","debug",L(LS_DEBUG_LOG),cfg.debug);
        H("<div style='display:flex;justify-content:space-between;align-items:center;margin-bottom:6px;margin-top:12px;'>"
          "<code style='font-size:.72rem;color:var(--dim);'>/data/shadowmount/debug.log</code>"
          "<button type='button' onclick='refreshLog()' style='background:transparent;border:1px solid var(--border);border-radius:6px;color:var(--dim);padding:5px 12px;cursor:pointer;font-family:var(--mono);font-size:.75rem;' onmouseover=\"this.style.borderColor='var(--accent)';this.style.color='var(--accent)'\" onmouseout=\"this.style.borderColor='var(--border)';this.style.color='var(--dim)'\">" ICO("ref") " Refresh</button>"
          "<button type='button' onclick='clearLog()' style='background:transparent;border:1px solid #f87171;border-radius:6px;color:#f87171;padding:5px 12px;cursor:pointer;font-family:var(--mono);font-size:.75rem;margin-left:4px;'>" ICO("del") " %s</button>",L(LS_CLEAR_LOG));
          H("</div>");
        { const char *cats[]={"All","NOTIFY","SCAN","DB","GAME","KSTUFF","ERR",NULL};
          const char *keys[]={      "","NOTIFY","SCAN","DB","GAME","KSTUFF","ERR",NULL};
          H("<div style='display:flex;gap:4px;flex-wrap:wrap;margin-bottom:8px;'>");
          for(int ci=0;cats[ci];ci++)
            H("<button type='button' class='lf-btn' data-cat='%s' onclick='filterLog(\"%s\")'"
              " style='background:transparent;border:1px solid var(--border);border-radius:4px;"
              "color:%s;padding:3px 8px;cursor:pointer;font-family:var(--mono);font-size:.7rem;'>"
              "%s</button>",
              keys[ci],keys[ci],ci==0?"var(--accent)":"var(--dim)",cats[ci]);
          H("</div>"); }
        H("<pre id='log-output' class='log-out'>%s</pre>",
          L(LS_LOADING));
        H("</div></div>");

        /* Panel: Raw config.ini editor */
        H("<div id='panel-raw' class='panel'><div class='section' style='padding:14px 16px;'>");
        /* Toolbar: all buttons same dim-border style, no inline msg (toast is fixed position) */
        H("<div style='display:flex;align-items:center;gap:5px;flex-wrap:wrap;margin-bottom:8px;'>");
        H("<div class='sublist-title' style='margin:0;flex:1;min-width:120px;font-size:.85rem;'>" ICO("pen") " config.ini</div>");
        H("<button type='button' onclick='rawLoad()' id='raw-load-btn' style='%s'>" ICO("ref") " Laden</button>",
          "background:transparent;border:1px solid var(--dim);border-radius:6px;color:var(--dim);padding:5px 10px;cursor:pointer;font-size:.76rem;white-space:nowrap;");
        H("<button type='button' onclick='rawSave()' style='%s'>" ICO("dl") " Speichern</button>",
          "background:var(--accent);border:1px solid var(--accent);border-radius:6px;color:#021a18;font-weight:600;padding:5px 14px;cursor:pointer;font-size:.76rem;white-space:nowrap;");
        H("<button type='button' onclick='rawSaveClose()' style='%s'>" ICO("dl") " &amp; Schlie&szlig;en</button>",
          "background:transparent;border:1px solid var(--accent);border-radius:6px;color:var(--accent);padding:5px 10px;cursor:pointer;font-size:.76rem;white-space:nowrap;");
        H("<button type='button' onclick='closeRaw()' style='%s'>&#8592; Zur&uuml;ck</button>",
          "background:transparent;border:1px solid var(--dim);border-radius:6px;color:var(--dim);padding:5px 10px;cursor:pointer;font-size:.76rem;white-space:nowrap;");
        H("</div>");
        /* Search bar: integrated box with icon */
        H("<div style='display:flex;gap:5px;align-items:center;margin-bottom:8px;border:1px solid var(--border);border-radius:8px;padding:4px 6px;background:var(--surface2);'>");
        H("<span style='color:var(--dim);display:flex;align-items:center;flex-shrink:0;'>" ICO("search") "</span>");
        /* input wrapper: relative so × button can sit inside the field */
        H("<div style='position:relative;flex:1;min-width:0;max-width:150px;'>");
        H("<input id='raw-find' type='text' autocomplete='off' placeholder='Suchen...' onkeydown='if(event.key===\"Enter\"){event.stopPropagation();event.preventDefault();rawFind();}else if(event.key===\"Escape\"){event.stopPropagation();}'"
          " style='width:100%%;background:transparent;border:none;outline:none;"
          "color:var(--text);padding:3px 18px 3px 4px;font-family:var(--mono);font-size:.76rem;box-sizing:border-box;'>");
        H("<button type='button' title='L\u00f6schen' onclick=\"document.getElementById('raw-find').value='';document.getElementById('raw-find-status').textContent='';document.getElementById('raw-find').focus();\""
          " style='position:absolute;right:2px;top:50%%;transform:translateY(-50%%);background:transparent;border:none;"
          "color:var(--dim);padding:0 2px;cursor:pointer;font-size:.75rem;line-height:1;'>&times;</button>");
        H("</div>");
        H("<button type='button' onclick='rawFindPrev()' title='Vorheriger Treffer'"
          " style='background:transparent;border:none;color:var(--dim);padding:2px 6px;cursor:pointer;font-size:.82rem;flex-shrink:0;'>&#8592;</button>");
        H("<button type='button' onclick='rawFind()' title='N\u00e4chster Treffer'"
          " style='background:transparent;border:none;color:var(--dim);padding:2px 6px;cursor:pointer;font-size:.82rem;flex-shrink:0;'>&#8594;</button>");
        H("</div>");
        H("<span id='raw-find-status' style='font-family:var(--mono);font-size:.71rem;color:var(--accent);white-space:nowrap;display:block;min-height:1.2em;margin-bottom:4px;'></span>");
        H("<p class='hint' style='margin-bottom:8px;'>&#9432; <code>%s</code></p>",CONFIG_PATH);
        /* textarea: PS5 keyboard handled via visualViewport listener in JS */
        H("<textarea id='raw-editor' spellcheck='false'"
          " style='width:100%%;height:400px;background:var(--surface2);border:1px solid var(--border);"
          "border-radius:8px;color:var(--text);font-family:var(--mono);font-size:.78rem;"
          "padding:10px 12px;resize:vertical;line-height:1.5;-webkit-overflow-scrolling:touch;'"
          " placeholder='Klicke Laden...'></textarea>");
        H("<div id='raw-msg'></div>");
        H("</div></div>");

        /* close content + layout divs */
        H("</div></div>");

        H("<div class='btn-area'>");
        H("<button type='submit' class='submit'>%s</button>",
          L(LS_SAVE));
        H("<button type='button' class='reset' onclick='resetDefaults()'>" ICO("undo") " %s</button>",
          L(LS_RESTORE_DEF));
        H("<div id='save-msg' style='display:none;padding:8px 16px;border-radius:6px;"
          "border:1px solid;font-family:var(--mono);font-size:.82rem;'></div>");
        H("</div>");

        H("</form><footer></footer></div>");

        H("<script>var _t={elf_nf:'%s',sel_sm:'%s',cancel:'%s',saved:'%s',err:'%s'};"
          "var _ps5dl='%s',_ps5ul='%s';</script>",
          L(LS_ELF_NOT_FOUND),
          L(LS_SELECT_SM),
          L(LS_CANCEL),
          L(LS_SAVED),
          L(LS_ERROR_STR),
          L(LS_PS5_NO_DL),
          L(LS_PS5_NO_UL));
        H("<script>var _usb_no='%s',_usb_ok_exp='%s',_usb_ok_imp='%s',_imp_ok='%s',_bad_cfg='%s',"
          "_restore_q='%s',_delete_q='%s',_del_all_q='%s',_usb_no_cfg='%s',_usb_no_baks='%s',_skipped='%s',"
          "_del_usb_q='%s',_move_q='%s';</script>",
          L(LS_USB_NO_DRIVE),L(LS_USB_EXPORT_OK),L(LS_USB_IMPORT_OK),L(LS_IMPORT_OK),L(LS_BAD_CONFIG),
          L(LS_CONFIRM_RESTORE),L(LS_CONFIRM_DELETE),L(LS_CONFIRM_DEL_ALL),
          L(LS_NO_USB_CONFIG),L(LS_USB_NO_BAKS),L(LS_BAK_SKIPPED),
          L(LS_CONFIRM_DEL_USB),L(LS_CONFIRM_MOVE));
        /* JS */
        H("<script>"
          "function showP(el){"
          "document.querySelectorAll('.nav-item').forEach(function(b){b.classList.remove('active');});"
          "document.querySelectorAll('.panel').forEach(function(p){p.classList.remove('active');});"
          "el.classList.add('active');"
          "var p=document.getElementById('panel-'+el.getAttribute('data-p'));"
          "if(p)p.classList.add('active');"
          "sessionStorage.setItem('panel',el.getAttribute('data-p'));"
          "if(el.getAttribute('data-p')==='log')refreshLog();"
          "if(el.getAttribute('data-p')==='raw'){rawLoad();}"
          "var ct=document.querySelector('.content');if(ct)ct.scrollTop=0;}"          /* restore panel after USB-triggered reload */
          "var _rp=sessionStorage.getItem('panel');"
          "if(_rp){var _rb=document.querySelector('.nav-item[data-p=\"'+_rp+'\"]');if(_rb)showP(_rb);}"
          "function toggleSidebar(){var s=document.querySelector('.sidebar');if(!s)return;"
          "var collapsed=s.style.width==='0px';"
          "s.style.width=collapsed?'175px':'0px';"
          "s.style.padding=collapsed?'6px':'0';"
          "s.style.borderWidth=collapsed?'1px':'0';}"
          "function setASCustomPath(path){"
          "if(!path)return;"
          "var nm=path.split('/').pop();"
          "var btn=document.getElementById('as-elf-btn');"
          "if(btn)btn.textContent=nm+' \u25bc';"
          "fetch('/api/prefs/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
          "body:'preferred_elf='+encodeURIComponent(path)});}"
          "function onAS(){var cb=document.getElementById('as-chk');"
          "var willOn=!cb.checked;" /* onclick fires before checked toggles */
          "if(willOn&&(!eb||eb.style.display==='none')){"
          "cb.checked=false;" /* prevent toggle until ELF picked */
          "pickASElf(true);return;}"
          "fetch('/api/prefs/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
          "body:'auto_start='+(willOn?1:0)});}"
          "function pickASElf(enable){"
          "fetch('/api/sm/scan').then(function(r){return r.json();}).then(function(d){"
          "if(d.count===0){alert(_t.elf_nf);return;}"
          "var cb=function(p){setASElf(p,enable);};"
          "if(d.count===1)cb(d.paths[0]);else showElfPicker(null,d.paths,cb);"
          "}).catch(function(){alert(_t.err);});}"
          "function setASElf(path,enable){"
          "var nm=path.split('/').pop();"
          "var eb=document.getElementById('as-elf-btn');"
          "if(eb){eb.textContent=nm+' \u25bc';eb.style.display='';}"
          "if(enable){var cb=document.getElementById('as-chk');if(cb)cb.checked=true;}"
          "var body='preferred_elf='+encodeURIComponent(path)+(enable?'&auto_start=1':'');"
          "fetch('/api/prefs/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body});}"
          "function clearPrefElf(){fetch('/api/prefs/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'preferred_elf='}).then(function(){var l=document.getElementById('pref-elf-lbl'),b=document.getElementById('pref-elf-clr');if(l){l.style.display='none';l.textContent='';}if(b)b.style.display='none';});}"
          "function smVer(ver,maj,min,suf,num){""if(!ver)return false;""var m=ver.match(/^(\\d+)\\.(\\d+)(alpha|beta|test)?(\\d+)?/);""if(!m)return false;""var a=parseInt(m[1]),b=parseInt(m[2]),c=m[3]||'',d=parseInt(m[4]||'0');""if(a>maj)return true;if(a<maj)return false;""if(b>min)return true;if(b<min)return false;""var s={alpha:1,beta:2,test:3},vp=s[c]||4,mp=s[suf]||4;""if(vp>mp)return true;if(vp<mp)return false;""return d>=num;}""function refreshStatus(){""fetch('/api/status').then(r=>r.json()).then(d=>{""var c=document.getElementById('sm-status-chip');""if(!c)return;""var col=d.running?'#10b981':'#f87171';""c.innerHTML='<span style=\"width:7px;height:7px;border-radius:50%%;display:inline-block;background:'+col+';box-shadow:0 0 6px '+col+';'+(d.running?\"animation:pulse 2s infinite;\": \"\")+'\"></span> ShadowMount <b>'+d.status+'</b>';""var v=document.getElementById('sm-ver-chip');""if(v){v.style.display=(d.running&&d.version!=='nicht aktiv')?'flex':'none';""if(d.version!=='nicht aktiv')v.innerHTML='v <b>'+d.version+'</b>';}""var ver=d.version||'';""var ar=!d.running||smVer(ver,1,7,'alpha',3);""var pim=!d.running||smVer(ver,1,7,'alpha',4);""var as=document.getElementById('ar-wrap');""if(as){as.style.opacity=ar?\'1\':\'0.35\';as.style.pointerEvents=ar?\'auto\':\'none\';}""var ab=document.querySelectorAll('.ar-b');ab.forEach(function(e){e.style.display=ar?'none':'inline';});""var pb=document.getElementById('pim-badge');""if(pb)pb.style.display=pim?'none':'inline';""var pr=document.getElementById('pim-row');""if(pr)pr.style.opacity=pim?'1':'0.35';""if(pr)pr.style.pointerEvents=pim?'auto':'none';""var lb=document.getElementById('lang-badge');""if(lb)lb.style.display=pim?'none':'inline';""var lr=document.getElementById('lang-row');""if(lr)lr.style.opacity=pim?'1':'0.35';""if(lr)lr.style.pointerEvents=pim?'auto':'none';""var api=!d.running||smVer(ver,1,7,'alpha',3);""var aw=document.getElementById('api-wrap');""if(aw){aw.style.opacity=api?'1':'0.35';aw.style.pointerEvents=api?'auto':'none';}""var apib=null;""var fan=!d.running||smVer(ver,1,7,'alpha',5);""var fr=document.getElementById('fan-row');""if(fr){fr.style.opacity=fan?'1':'0.35';fr.style.pointerEvents=fan?'auto':'none';}""var fb=fr?fr.querySelector('.vbadge'):null;""if(fb)fb.style.display=fan?'none':'inline';""var cb=document.getElementById('sm-ctrl-btn');""if(cb){cb.setAttribute('data-action',d.running?'stop':'start');cb.className='sm-ctrl '+(d.running?'stop':'start');cb.textContent=d.running?'Stop':'Start';}""}).catch(()=>{});}""refreshStatus();""setInterval(refreshStatus,10000);""function smCtrl(){var btn=document.getElementById('sm-ctrl-btn');if(!btn)return;var action=btn.getAttribute('data-action');if(action==='stop'){btn.disabled=true;fetch('/api/sm/stop').then(function(){setTimeout(refreshStatus,1500);setTimeout(function(){btn.disabled=false;},2000);}).catch(function(){btn.disabled=false;});}else{btn.disabled=true;fetch('/api/sm/scan').then(function(r){return r.json();}).then(function(d){if(d.count===0){alert(_t.elf_nf);btn.disabled=false;}else if(d.count===1){launchElf(btn,d.paths[0]);}else{btn.disabled=false;showElfPicker(btn,d.paths);}}).catch(function(){btn.disabled=false;});}}""function launchElf(btn,path){if(btn)btn.disabled=true;fetch('/api/sm/start?path='+encodeURIComponent(path)).then(function(r){return r.json();}).then(function(d){"
          "if(btn)btn.disabled=false;"
          "if(d.ok){var nm=path.split('/').pop();var l=document.getElementById('pref-elf-lbl');var b=document.getElementById('pref-elf-clr');"
          "if(l){l.textContent=nm;l.title=path;l.style.display='inline';}if(b)b.style.display='inline';}"
          "setTimeout(refreshStatus,2000);})"
          ".catch(function(){if(btn)btn.disabled=false;});}""function showElfPicker(btn,paths,cb){var old=document.getElementById('sm-elf-picker');if(old)old.remove();var picker=document.createElement('div');picker.id='sm-elf-picker';picker.style.cssText='position:fixed;top:0;left:0;width:100%%;height:100%%;background:rgba(0,0,0,.75);display:flex;align-items:center;justify-content:center;z-index:9999;';var box=document.createElement('div');box.style.cssText='background:#0d111a;border:1px solid #1e2a42;border-radius:14px;padding:36px;min-width:500px;max-width:800px;';var title=document.createElement('p');title.style.cssText='margin:0 0 20px;font-size:.7rem;text-transform:uppercase;letter-spacing:.12em;color:#64748b;font-family:monospace;';title.textContent=_t.sel_sm;box.appendChild(title);paths.forEach(function(p){var b=document.createElement('button');b.style.cssText='display:block;width:100%%;margin-bottom:10px;padding:16px 18px;border-radius:8px;border:1px solid #1e2a42;background:#131927;color:#e2e8f0;cursor:pointer;text-align:left;font-family:monospace;font-size:.75rem;word-break:break-all;';b.textContent=p;b.onclick=function(){picker.remove();if(cb)cb(p);else launchElf(btn,p);};box.appendChild(b);});var cancel=document.createElement('button');cancel.style.cssText='display:block;width:100%%;padding:14px;border-radius:8px;border:1px solid #1e2a42;background:transparent;color:#64748b;cursor:pointer;font-family:monospace;font-size:.75rem;margin-top:6px;';cancel.textContent=_t.cancel;cancel.onclick=function(){picker.remove();};box.appendChild(cancel);picker.appendChild(box);document.body.appendChild(picker);}""document.querySelector('form').addEventListener('keydown',function(e){""if(e.key==='Enter'&&e.target.tagName!=='BUTTON')e.preventDefault();""});"
"function addPath(){"
          "var c=document.getElementById('paths-list');"
          "var d=document.createElement('div');d.className='path-row';"
          "d.innerHTML='<input type=\"text\" name=\"paths[]\" placeholder=\"/data/homebrew\"><button type=\"button\" '"
          "+'class=\"rm\" onclick=\"this.parentElement.remove()\">&times;</button>';"
          "c.appendChild(d);}"
          "function addRow(listId,name,ph){"
          "var c=document.getElementById(listId);"
          "var d=document.createElement('div');d.className='path-row';"
          "d.innerHTML='<input type=\"text\" name=\"'+name+'\"'+(ph?' placeholder=\"'+ph+'\"':'')+'>'"
          "+'<button type=\"button\" class=\"rm\" onclick=\"this.parentElement.remove()\">&times;</button>';"
          "c.appendChild(d);}"
          "function setChk(n,v){var e=document.querySelector('[name=\"'+n+'\"]');if(e)e.checked=v;}"
          "function setVal(n,v){var e=document.querySelector('[name=\"'+n+'\"]');if(e)e.value=v;}"
          "function resetDefaults(){"
          "if(!confirm('%s')) return;"
          "setChk('debug',false);setChk('quiet_mode',false);"
          "setVal('language','auto');"
          "setVal('api_bind_address','127.0.0.1');setVal('api_port',10101);"
          "setChk('mount_read_only',true);setChk('force_mount',false);"
          "setChk('persistent_image_mounts',false);setChk('app_install_all',false);"
          "setVal('scan_depth',1);"
          "setVal('scan_interval_seconds',15);setVal('stability_wait_seconds',10);"
          "setChk('auto_remove_missing_games',false);"
          "setChk('auto_remove_games_with_dlc',false);"
          "setVal('auto_remove_missing_delay_seconds',300);"
          "setChk('kstuff_game_auto_toggle',true);setChk('kstuff_crash_detection',true);"
          "setVal('kstuff_pause_delay_image_seconds',25);"
          "setVal('kstuff_pause_delay_direct_seconds',15);"
          "setChk('backport_fakelib',true);setChk('global_fakelib',true);"
          "setVal('global_fakelib_path','/data/shadowmount/fakelib');"
          "setVal('global_fakelib_priority','game');"
          "setVal('exfat_backend','lvd');setVal('ufs_backend','lvd');"
          "setVal('fan_target_temperature','auto');"
          "setVal('lvd_exfat_sector_size',512);setVal('lvd_ufs_sector_size',4096);"
          "setVal('lvd_pfs_sector_size',32768);setVal('md_exfat_sector_size',512);"
          "setVal('md_ufs_sector_size',512);doSave();}"
          "var _rawLog='',_logFilter='';function doSave(){"
          "var f=document.querySelector('form');"
          "fetch('/save',{method:'POST',"
          "headers:{'Content-Type':'application/x-www-form-urlencoded'},"
          "body:new URLSearchParams(new FormData(f)).toString()})"
          ".then(function(r){return r.json();})"
          ".then(function(d){"
          "var m=document.getElementById('save-msg');if(!m)return;"
          "m.textContent=d.ok?'\u2713 '+_t.saved:'\u2717 '+(d.err||_t.err);"
          "m.style.color=d.ok?'#10b981':'#f87171';"
          "m.style.borderColor=d.ok?'#10b981':'#f87171';"
          "m.style.display='inline-block';"
          "clearTimeout(m._st);m._st=setTimeout(function(){m.style.display='none';},3000);})"
          ".catch(function(){var m=document.getElementById('save-msg');if(!m)return;"
          "m.textContent='\u2717 '+_t.err;m.style.color='#f87171';"
          "m.style.borderColor='#f87171';m.style.display='inline-block';"
          "clearTimeout(m._st);m._st=setTimeout(function(){m.style.display='none';},3000);});}"
          "function cfgDelAll(){if(!confirm(_del_all_q))return;"
          "fetch('/api/config/delete-all-bak',{method:'POST'})"
          ".then(function(){location.reload();}).catch(function(){alert(_t.err);});}"
          "function cfgDownBak(f){"
          "if(/PlayStation/i.test(navigator.userAgent)){alert(_ps5dl);return;}"
          "window.location.href='/api/config/download?f='+encodeURIComponent(f);}"
          "function cfgImportDirect(){"
          "if(/PlayStation/i.test(navigator.userAgent)){alert(_ps5ul);return;}"
          "var inp=document.createElement('input');inp.type='file';inp.accept='.ini,.txt';"
          "inp.onchange=function(){var f=inp.files[0];if(!f)return;"
          "var reader=new FileReader();reader.onload=function(e){"
          "fetch('/api/config/upload-direct',{method:'POST',body:e.target.result,headers:{'Content-Type':'text/plain'}})"
          ".then(function(r){return r.json();})"
          ".then(function(d){if(d.ok){alert(_imp_ok);location.reload();}else if(d.err==='invalid')alert(_bad_cfg);else alert(_t.err);})"
          ".catch(function(){alert(_t.err);});};reader.readAsText(f);};"
          "inp.click();}"
          "function cfgUploadUsb(){"
          "if(/PlayStation/i.test(navigator.userAgent)){alert(_ps5ul);return;}"
          "var inp=document.createElement('input');inp.type='file';inp.accept='.ini,.txt';"
          "inp.onchange=function(){var f=inp.files[0];if(!f)return;"
          "var n=f.name.replace(/\\.ini$/i,'').replace(/[^a-zA-Z0-9._-]/g,'_');"
          "var reader=new FileReader();reader.onload=function(e){"
          "fetch('/api/config/upload-bak-usb?f='+encodeURIComponent(n+'.ini'),{method:'POST',body:e.target.result,headers:{'Content-Type':'text/plain'}})"
          ".then(function(r){return r.json();})"
          ".then(function(d){if(d.ok){alert(_imp_ok);location.reload();}else if(d.err==='invalid')alert(_bad_cfg);else if(d.err==='no_usb')alert(_usb_no);else alert(_t.err);})"
          ".catch(function(){alert(_t.err);});};reader.readAsText(f);};"
          "inp.click();}"
          "function cfgUpload(){"
          "if(/PlayStation/i.test(navigator.userAgent)){alert(_ps5ul);return;}"
          "var inp=document.createElement('input');inp.type='file';inp.accept='.ini,.txt';"
          "inp.onchange=function(){var f=inp.files[0];if(!f)return;"
          "var n=f.name.replace(/\\.ini$/i,'').replace(/[^a-zA-Z0-9._-]/g,'_');"
          "var reader=new FileReader();reader.onload=function(e){"
          "fetch('/api/config/upload-bak?f='+encodeURIComponent(n+'.ini'),{method:'POST',body:e.target.result,headers:{'Content-Type':'text/plain'}})"
          ".then(function(r){return r.json();})"
          ".then(function(d){if(d.ok){alert(_imp_ok);location.reload();}else if(d.err==='invalid')alert(_bad_cfg);else alert(_t.err);})"
          ".catch(function(){alert(_t.err);});};reader.readAsText(f);};"
          "inp.click();}"
          "function cfgBackupToUsb(){"
          "var ni=document.getElementById('bak-name-usb');"
          "var nm=ni?ni.value.trim().replace(/[^a-zA-Z0-9._-]/g,'_'):'';"
          "fetch('/api/config/backup-to-usb'+(nm?'?name='+encodeURIComponent(nm):''),{method:'POST'})"
          ".then(function(r){return r.json();})"
          ".then(function(d){if(d.ok)location.reload();else if(d.err==='no_usb')alert(_usb_no);else alert(_t.err);})"
          ".catch(function(){alert(_t.err);});}"
          "function cfgDelAllUsb(){if(!confirm(_del_all_q))return;"
          "fetch('/api/config/delete-all-usb-bak',{method:'POST'})"
          ".then(function(r){return r.json();})"
          ".then(function(d){if(d.ok)location.reload();else alert(_t.err);})"
          ".catch(function(){alert(_t.err);});}"
          "function cfgMoveAllToLocal(){if(!confirm(_move_q+'\\n'+_del_all_q))return;"
          "fetch('/api/config/usb-import-baks',{method:'POST'})"
          ".then(function(r){return r.json();})"
          ".then(function(d){if(!d.ok){if(d.err==='no_usb')alert(_usb_no);else alert(_t.err);return;}"
          "fetch('/api/config/delete-all-usb-bak',{method:'POST'})"
          ".then(function(){location.reload();}).catch(function(){location.reload();});})"
          ".catch(function(){alert(_t.err);});}"
          "function cfgCopyAllToLocal(){fetch('/api/config/usb-import-baks',{method:'POST'})"
          ".then(function(r){return r.json();})"
          ".then(function(d){if(!d.ok){if(d.err==='no_usb')alert(_usb_no);else alert(_t.err);return;}"
          "alert(_usb_ok_imp+' ('+d.count+')'+(d.skipped>0?' / '+d.skipped+' '+_skipped:''));"
          "location.reload();})"
          ".catch(function(){alert(_t.err);});}"
          "function cfgMoveToUsb(f){if(!confirm(f+'\\n'+_move_q))return;"
          "fetch('/api/config/usb-export?f='+encodeURIComponent(f),{method:'POST'})"
          ".then(function(r){return r.json();})"
          ".then(function(d){if(d.ok){"
          "fetch('/api/config/delete-bak?f='+encodeURIComponent(f),{method:'POST'})"
          ".then(function(){location.reload();}).catch(function(){location.reload();});"
          "}else{if(d.err==='no_usb')alert(_usb_no);else alert(_t.err);}}).catch(function(){alert(_t.err);});}"
          "function cfgDelUsbBak(f){if(!confirm(f+' '+_del_usb_q))return;"
          "fetch('/api/config/delete-usb-bak?f='+encodeURIComponent(f),{method:'POST'})"
          ".then(function(r){return r.json();})"
          ".then(function(d){if(d.ok)location.reload();else alert(_t.err);})"
          ".catch(function(){alert(_t.err);});}"
          "function cfgMoveToLocal(f){if(!confirm(f+'\\n'+_move_q))return;"
          "fetch('/api/config/usb-copy-to-local?f='+encodeURIComponent(f),{method:'POST'})"
          ".then(function(r){return r.json();})"
          ".then(function(d){if(d.ok){"
          "fetch('/api/config/delete-usb-bak?f='+encodeURIComponent(f),{method:'POST'})"
          ".then(function(){location.reload();}).catch(function(){location.reload();});"
          "}else{if(d.err==='no_usb')alert(_usb_no);else if(d.err==='invalid')alert(_bad_cfg);else alert(_t.err);}}).catch(function(){alert(_t.err);});}"
          "function cfgMoveAllToUsb(){if(!confirm(_move_q+'\\n'+_del_all_q))return;"
          "fetch('/api/config/usb-export-all',{method:'POST'})"
          ".then(function(r){return r.json();})"
          ".then(function(d){if(d.ok){"
          "fetch('/api/config/delete-all-bak',{method:'POST'})"
          ".then(function(){location.reload();}).catch(function(){location.reload();});"
          "}else{if(d.err==='no_usb')alert(_usb_no);else alert(_t.err);}}).catch(function(){alert(_t.err);});}"
          "function cfgUsbExportAll(){fetch('/api/config/usb-export-all',{method:'POST'})"
          ".then(function(r){return r.json();})"
          ".then(function(d){if(d.ok)alert(_usb_ok_exp+' ('+d.count+')');else if(d.err==='no_usb')alert(_usb_no);else alert(_t.err);})"
          ".catch(function(){alert(_t.err);});}"
          "function cfgCopyToLocal(f){fetch('/api/config/usb-copy-to-local?f='+encodeURIComponent(f),{method:'POST'})"
          ".then(function(r){return r.json();})"
          ".then(function(d){if(d.ok)location.reload();else if(d.err==='invalid')alert(_bad_cfg);else if(d.err==='no_usb')alert(_usb_no);else alert(_t.err);})"
          ".catch(function(){alert(_t.err);});}"
          "function cfgUsbRestoreOne(f){if(!confirm(f+' '+_restore_q))return;"
          "fetch('/api/config/usb-restore-bak?f='+encodeURIComponent(f),{method:'POST'})"
          ".then(function(r){return r.json();})"
          ".then(function(d){if(d.ok){alert(_imp_ok);location.reload();}else if(d.err==='invalid')alert(_bad_cfg);else alert(_t.err);})"
          ".catch(function(){alert(_t.err);});}"
          "function cfgDownUsbBak(f){"
          "if(/PlayStation/i.test(navigator.userAgent)){alert(_ps5dl);return;}"
          "window.location.href='/api/config/download-usb?f='+encodeURIComponent(f);}"
          "function cfgDownAllUsb(){"
          "if(/PlayStation/i.test(navigator.userAgent)){alert(_ps5dl);return;}"
          "fetch('/api/config/usb-bak-list')"
          ".then(function(r){return r.json();})"
          ".then(function(d){if(!d.ok||!d.files){alert(_t.err);return;}"
          "d.files.forEach(function(f,i){"
          "setTimeout(function(){window.location.href='/api/config/download-usb?f='+encodeURIComponent(f);},i*100);});})"
          ".catch(function(){alert(_t.err);});}"
          "function cfgDownAll(){"
          "if(/PlayStation/i.test(navigator.userAgent)){alert(_ps5dl);return;}"
          "var rows=document.querySelectorAll('[data-bak-f]');"
          "rows.forEach(function(r,i){"
          "setTimeout(function(){window.location.href='/api/config/download?f='+encodeURIComponent(r.dataset.bakF);},i*100);});}"
          "function cfgUsbExport(f){fetch('/api/config/usb-export'+(f?'?f='+encodeURIComponent(f):''),{method:'POST'})"
          ".then(function(r){return r.json();})"
          ".then(function(d){if(d.ok)alert(_usb_ok_exp+' '+d.path);else if(d.err==='no_usb')alert(_usb_no);else alert(_t.err);})"
          ".catch(function(){alert(_t.err);});}"
          "function cfgUsbImport(){fetch('/api/config/usb-import',{method:'POST'})"
          ".then(function(r){return r.json();})"
          ".then(function(d){if(d.ok){alert(_usb_ok_imp);location.reload();}else if(d.err==='no_usb')alert(_usb_no);else if(d.err==='no_file')alert(_usb_no_cfg);else if(d.err==='invalid')alert(_bad_cfg);else alert(_t.err);})"
          ".catch(function(){alert(_t.err);});}"
          "function cfgBackup(){var ni=document.getElementById('bak-name-intern');"
          "var nm=ni?ni.value.trim().replace(/[^a-zA-Z0-9._-]/g,'_'):'';"
          "fetch('/api/config/backup'+(nm?'?name='+encodeURIComponent(nm):''),{method:'POST'})"
          ".then(function(r){return r.json();})"
          ".then(function(d){if(d.ok)location.reload();else alert(_t.err);})"
          ".catch(function(){alert(_t.err);});}"
          "function cfgRestore(f){if(!confirm(f+' '+_restore_q))return;"
          "fetch('/api/config/restore?f='+encodeURIComponent(f),{method:'POST'})"
          ".then(function(r){return r.json();})"
          ".then(function(d){if(d.ok){alert(_imp_ok);location.reload();}else alert(_t.err);})"
          ".catch(function(){alert(_t.err);});}"
          "function cfgDelBak(f){if(!confirm(f+' '+_delete_q))return;"
          "fetch('/api/config/delete-bak?f='+encodeURIComponent(f),{method:'POST'})"
          ".then(function(r){return r.json();})"
          ".then(function(d){if(d.ok)location.reload();else alert(_t.err);})"
          ".catch(function(){alert(_t.err);});}"
          "var _rawPrev=null,_rawSaved=false;"
          "function openRaw(){"
          "var cur=document.querySelector('.panel.active');"
          "_rawPrev=cur?cur.id:null;_rawSaved=false;"
          "document.querySelectorAll('.nav-item').forEach(function(b){b.classList.remove('active');});"
          "document.querySelectorAll('.panel').forEach(function(p){p.classList.remove('active');});"
          "var rp=document.getElementById('panel-raw');"
          "if(rp)rp.classList.add('active');"
          "var ba=document.querySelector('.btn-area');if(ba)ba.style.display='none';"
          "doSave();setTimeout(rawLoad,500);}"
          "function closeRaw(){"
          "if(_rawSaved){location.reload();return;}"
          "document.querySelectorAll('.panel').forEach(function(p){p.classList.remove('active');});"
          "var prev=_rawPrev||'panel-sys';"
          "var pp=document.getElementById(prev);"
          "if(pp)pp.classList.add('active');"
          "var pid=prev.replace('panel-','');"
          "var nb=document.querySelector('[data-p=\"'+pid+'\"]');"
          "if(nb)nb.classList.add('active');"
          "var ba=document.querySelector('.btn-area');if(ba)ba.style.display='';"
          "_rawPrev=null;}"
          "function rawLoad(){"
          "var e=document.getElementById('raw-editor');"
          "var m=document.getElementById('raw-msg');"
          "var b=document.getElementById('raw-load-btn');"
          "if(!e)return;"
          "if(b)b.disabled=true;"
          "fetch('/api/config/raw')"
          ".then(function(r){"
          "if(!r.ok)throw new Error('HTTP '+r.status);"
          "return r.text();})"
          ".then(function(t){"
          "e.value=t;"
          "if(!('ontouchstart' in window))e.focus({preventScroll:true});"
          "if(m){m.textContent='\u2713 '+t.split('\\n').length+' Zeilen geladen';"
          "m.style.color='#10b981';m.style.borderColor='#10b981';m.style.display='inline-block';"
          "clearTimeout(m._st);m._st=setTimeout(function(){m.style.display='none';},2500);}"
          "if(b)b.disabled=false;})"
          ".catch(function(err){"
          "if(m){m.textContent='\u2717 '+err.message;"
          "m.style.color='#f87171';m.style.borderColor='#f87171';m.style.display='inline-block';}"
          "if(b)b.disabled=false;});}"
          "function rawSave(){"
          "var e=document.getElementById('raw-editor');if(!e)return;"
          "var m=document.getElementById('raw-msg');"
          "var b=document.getElementById('raw-load-btn');"
          "if(b)b.disabled=true;"
          "fetch('/api/config/raw',{method:'POST',body:e.value,headers:{'Content-Type':'text/plain'}})"
          ".then(function(r){return r.json();})"
          ".then(function(d){"
          "if(b)b.disabled=false;"
          "if(d.ok){"
          "_rawSaved=true;"
          "if(m){m.textContent='\\u2713 '+_t.saved;m.style.color='#10b981';"
          "m.style.borderColor='#10b981';m.style.display='inline-block';"
          "clearTimeout(m._st);m._st=setTimeout(function(){m.style.display='none';},3000);}"
          "}else{"
          "if(!m)return;"
          "m.textContent='\\u2717 '+(d.err||_t.err);"
          "m.style.color='#f87171';m.style.borderColor='#f87171';"
          "m.style.display='inline-block';"
          "clearTimeout(m._st);m._st=setTimeout(function(){m.style.display='none';},3000);}})"
          ".catch(function(){if(b)b.disabled=false;alert(_t.err);});}"
          "function rawSaveClose(){"
          "var e=document.getElementById('raw-editor');if(!e)return;"
          "fetch('/api/config/raw',{method:'POST',body:e.value,headers:{'Content-Type':'text/plain'}})"
          ".then(function(r){return r.json();})"
          ".then(function(d){"
          "if(d.ok)setTimeout(function(){location.reload();},400);"
          "else{var m=document.getElementById('raw-msg');if(m){"
          "m.textContent='\\u2717 '+(d.err||_t.err);"
          "m.style.color='#f87171';m.style.borderColor='#f87171';m.style.display='inline-block';}}})"
          ".catch(function(){alert(_t.err);});}"
          "var _findIdx=0,_findQ='';"
          "function _rawScroll(e,idx,q){"
          "var lh=parseFloat(getComputedStyle(e).lineHeight)||18;"
          "var lines=e.value.substr(0,idx).split('\\n');"
          "var ln=lines.length;"
          "e.scrollTop=Math.max(0,(ln-5)*lh);"
          "var s=document.getElementById('raw-find-status');"
          "if(s){var esc=q.replace(/[.*+?^${}()|[\\]\\\\]/g,'\\\\$&');"
          "var cnt=(e.value.match(new RegExp(esc,'g'))||[]).length;"
          "s.textContent='Zeile '+ln+' \u2014 '+cnt+' Treffer';}}"
          "function rawFind(){"
          "var e=document.getElementById('raw-editor');"
          "var q=document.getElementById('raw-find').value;"
          "if(!e||!q)return;"
          "if(q!==_findQ){_findIdx=0;_findQ=q;}"
          "var idx=e.value.indexOf(q,_findIdx);"
          "if(idx===-1){idx=e.value.indexOf(q,0);}"
          "if(idx===-1){var s=document.getElementById('raw-find-status');if(s)s.textContent='Nicht gefunden';return;}"
          "_findIdx=idx+q.length;"
          "if('ontouchstart' in window){_rawScroll(e,idx,q);}"
          "else if(/PlayStation/i.test(navigator.userAgent)){"
          "e.setAttribute('inputmode','none');"
          "e.focus({preventScroll:true});e.setSelectionRange(idx,idx+q.length);_rawScroll(e,idx,q);"
          "setTimeout(function(){e.removeAttribute('inputmode');},300);}"
          "else{e.focus({preventScroll:true});e.setSelectionRange(idx,idx+q.length);_rawScroll(e,idx,q);}}"
          "function rawFindNext(){rawFind();}"
          "function rawFindPrev(){"
          "var e=document.getElementById('raw-editor');"
          "var q=document.getElementById('raw-find').value;"
          "if(!e||!q)return;"
          "if(q!==_findQ){_findIdx=e.value.length;_findQ=q;}"
          "var from=Math.max(0,_findIdx-q.length-1);"
          "var idx=e.value.lastIndexOf(q,from);"
          "if(idx===-1){idx=e.value.lastIndexOf(q);}"
          "if(idx===-1){var s=document.getElementById('raw-find-status');if(s)s.textContent='Nicht gefunden';return;}"
          "_findIdx=idx;"
          "if('ontouchstart' in window){_rawScroll(e,idx,q);}"
          "else if(/PlayStation/i.test(navigator.userAgent)){"
          "e.setAttribute('inputmode','none');"
          "e.focus({preventScroll:true});e.setSelectionRange(idx,idx+q.length);_rawScroll(e,idx,q);"
          "setTimeout(function(){e.removeAttribute('inputmode');},300);}"
          "else{e.focus({preventScroll:true});e.setSelectionRange(idx,idx+q.length);_rawScroll(e,idx,q);}}"
          "var _rawEditing=false;"
          "function rawToggleEdit(){}"
          /* when PS5 keyboard appears (viewport shrinks), scroll page so textarea top is visible */
          "if(/PlayStation/i.test(navigator.userAgent)&&window.visualViewport){"
          "window.visualViewport.addEventListener('resize',function(){"
          "var e=document.getElementById('raw-editor');"
          "if(!e||document.activeElement!==e)return;"
          "var kh=window.innerHeight-window.visualViewport.height;"
          "if(kh>50)e.scrollIntoView({block:'start'});});}"
          "var _rawLog='',_logFilter='';"
          "function clearLog(){if(!confirm(_del_all_q))return;"
          "fetch('/api/log/clear',{method:'POST'})"
          ".then(function(){refreshLog();})"
          ".catch(function(){alert(_t.err);});}"
          "function refreshLog(){"
          "fetch('/api/log').then(function(r){return r.text();}).then(function(t){"
          "var el=document.getElementById('log-output');"
          "if(!el)return;"
          "var atBot=el.scrollTop+el.clientHeight>=el.scrollHeight-30;"
          "t=(t||'(leer)').split('\\n').reduce(function(a,l){"
          "if(l.length>0&&l[0]!=='['&&a.length>0)a[a.length-1]+=' '+l.trim();"
          "else a.push(l);return a;},[]).join('\\n');"
          "t=t.replace(/^(\\[\\d{2}:\\d{2}:\\d{2}\\]) ([^ ])/gm,'$1   $2');"
          "_rawLog=t;filterLog(_logFilter);"
          "if(atBot||el.scrollTop===0)el.scrollTop=el.scrollHeight;"
          "}).catch(function(e){"
          "var el=document.getElementById('log-output');"
          "if(el)el.textContent='[Fehler: '+(e&&e.message?e.message:'unbekannt')+']';"
          "});}"
          "setInterval(function(){"
          "var lp=document.getElementById('panel-log');"
          "if(lp&&lp.classList.contains('active'))refreshLog();"
          "},5000);"
          "var _usbPrev=null;"
          "setInterval(function(){"
          "fetch('/api/config/usb-status')"
          ".then(function(r){return r.json();})"
          ".then(function(d){"
          "if(_usbPrev!==null&&_usbPrev!==d.present)location.reload();"
          "_usbPrev=d.present;" /* sessionStorage keeps panel across USB reload */
          "}).catch(function(){if(_usbPrev===true)location.reload();});"
          "},5000);"
          "function filterLog(cat){"
          "_logFilter=cat;"
          "document.querySelectorAll('.lf-btn').forEach(function(b){"
          "var a=b.getAttribute('data-cat')===cat;"
          "b.style.color=a?'var(--accent)':'var(--dim)';"
          "b.style.borderColor=a?'var(--accent)':'var(--border)';});"
          "var el=document.getElementById('log-output');if(!el)return;"
          "var lines=_rawLog?_rawLog.split('\\n'):[];"
          "var f=cat?lines.filter(function(l){"
          "if(!l)return false;"
          "if(cat==='ERR')return /error|warn|fail/i.test(l);"
          "if(cat==='NOTIFY')return l.indexOf('NOTIFY')>=0;"
          "return l.indexOf('['+cat+']')>=0;}):lines;"
          "el.textContent=f.join('\\n');el.scrollTop=el.scrollHeight;}"
          "</script></body></html>",
          L(LS_RESET_CONFIRM));

        #undef H
        #undef SW
        #undef NF
        #undef TF
        #undef SEC

        mg_http_reply(c,200,"Content-Type: text/html; charset=utf-8\r\n","%s",html);
        free(html);
    }
    else if(mg_match(hm->uri,mg_str("/api/status"),NULL)){
        char sv[64]; get_sm_version(sv,sizeof(sv));
        /* Prozess-Check */
        const char *names[]={"shadowmount.elf","ShadowMount.elf",
            "shadowmountplus.elf","ShadowMountPlus.elf","shadowmount","ShadowMount",NULL};
        int run=0;
        for(int pi=0;names[pi];pi++) if(find_pid(names[pi])>0){run=1;break;}
        if(!run){
            int ts=socket(AF_INET,SOCK_STREAM,0);
            if(ts>=0){
                struct timeval tv={1,0};
                setsockopt(ts,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
                setsockopt(ts,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv));
                struct sockaddr_in ta; memset(&ta,0,sizeof(ta));
                ta.sin_family=AF_INET; ta.sin_port=htons(10101);
                ta.sin_addr.s_addr=inet_addr("127.0.0.1");
                if(connect(ts,(struct sockaddr*)&ta,sizeof(ta))==0) run=1;
                close(ts);
            }
        }
        char resp[256];
        snprintf(resp,sizeof(resp),
            "{\"running\":%s,\"version\":\"%s\",\"status\":\"%s\"}",
            run?"true":"false", sv,
            run ? L(LS_RUNNING) : L(LS_NOT_RUNNING));
        mg_http_reply(c,200,
            "Content-Type: application/json\r\n"
            "Cache-Control: no-cache\r\n","%s",resp);
    }
    else if(mg_match(hm->uri,mg_str("/save"),NULL)&&mg_strcmp(hm->method,mg_str("POST"))==0){
        ShadowConfig nc; set_defaults(&nc);
        char buf[PATH_LEN];
        #define GB(name,field) if(mg_http_get_var(&hm->body,name,buf,sizeof(buf))>0) nc.field=1;
        #define GI(name,field) if(mg_http_get_var(&hm->body,name,buf,sizeof(buf))>0) nc.field=atoi(buf);
        #define GS(name,field) if(mg_http_get_var(&hm->body,name,buf,sizeof(buf))>0) strncpy(nc.field,buf,sizeof(nc.field)-1);
        GB("debug",debug) GB("quiet_mode",quiet_mode)
        GS("language",language)
        GS("api_bind_address",api_bind_address) GI("api_port",api_port)
        GS("fan_target_temperature",fan_target_temperature)
        GB("mount_read_only",mount_read_only) GB("force_mount",force_mount)
        GB("persistent_image_mounts",persistent_image_mounts) GB("app_install_all",app_install_all)
        GB("recursive_scan",recursive_scan)
        GI("scan_depth",scan_depth) GI("scan_interval_seconds",scan_interval_seconds)
        GI("stability_wait_seconds",stability_wait_seconds)
        GB("auto_remove_missing_games",auto_remove_missing_games)
        GB("auto_remove_games_with_dlc",auto_remove_games_with_dlc)
        GI("auto_remove_missing_delay_seconds",auto_remove_missing_delay_seconds)
        GB("kstuff_game_auto_toggle",kstuff_game_auto_toggle)
        GB("kstuff_crash_detection",kstuff_crash_detection)
        GI("kstuff_pause_delay_image_seconds",kstuff_pause_delay_image_seconds)
        GI("kstuff_pause_delay_direct_seconds",kstuff_pause_delay_direct_seconds)
        GB("backport_fakelib",backport_fakelib) GB("global_fakelib",global_fakelib)
        GS("global_fakelib_path",global_fakelib_path)
        GS("global_fakelib_priority",global_fakelib_priority)
        GS("exfat_backend",exfat_backend) GS("ufs_backend",ufs_backend)
        GI("lvd_exfat_sector_size",lvd_exfat_sector_size)
        GI("lvd_ufs_sector_size",lvd_ufs_sector_size)
        GI("lvd_pfs_sector_size",lvd_pfs_sector_size)
        GI("md_exfat_sector_size",md_exfat_sector_size)
        GI("md_ufs_sector_size",md_ufs_sector_size)
        #undef GB
        #undef GI
        #undef GS
        char manual_save[MAX_MANUAL][PATH_LEN];
        int  manual_save_count=0;
        struct mg_str key,val; size_t ofs=0;
        while((ofs=next_kv_param(hm->body,ofs,&key,&val))>0){
            char dec[PATH_LEN]; dec[0]=0;
            if(val.len>0) mg_url_decode(val.buf,val.len,dec,sizeof(dec),0);
            #define LISTPUSH(kname,arr,cnt,max,len) \
                if(mg_strcmp(key,mg_str(kname))==0&&strlen(dec)>0&&cnt<max) \
                    strncpy(arr[cnt++],dec,len-1);
            if(mg_strcmp(key,mg_str("paths[]"))==0&&val.len>0&&nc.path_count<MAX_PATHS){
                if(strlen(dec)>0) strncpy(nc.scanpaths[nc.path_count++],dec,PATH_LEN-1);
            }
            LISTPUSH("image_ro[]",        nc.image_ro,             nc.image_ro_count,             MAX_IMG,  IMG_LEN)
            LISTPUSH("image_rw[]",        nc.image_rw,             nc.image_rw_count,             MAX_IMG,  IMG_LEN)
            LISTPUSH("image_sector[]",    nc.image_sector,         nc.image_sector_count,         MAX_IMG,  IMG_LEN)
            LISTPUSH("kstuff_no_pause[]", nc.kstuff_no_pause,      nc.kstuff_no_pause_count,      MAX_LIST, ID_LEN)
            LISTPUSH("kstuff_delay[]",    nc.kstuff_delay,         nc.kstuff_delay_count,         MAX_LIST, ID_LEN+8)
            LISTPUSH("global_fakelib_exclude[]", nc.global_fakelib_exclude, nc.global_fakelib_exclude_count, MAX_LIST, PATH_LEN)
            LISTPUSH("manual[]", manual_save, manual_save_count, MAX_MANUAL, PATH_LEN)
            #undef LISTPUSH
        }
        /* Write manual.lst (user-managed; SM only removes entries on game uninstall) */
        { FILE *mf=fopen(MANUAL_LST,"w"); if(mf){ for(int j=0;j<manual_save_count;j++) if(manual_save[j][0]) fprintf(mf,"%s\n",manual_save[j]); fclose(mf); } }
        if(save_config(&nc)){
            mg_http_reply(c,200,"Content-Type: application/json\r\n","{\"ok\":true}");
        } else
            mg_http_reply(c,500,"Content-Type: application/json\r\n","{\"ok\":false,\"err\":\"save_failed\"}");
    }
    else if(mg_match(hm->uri,mg_str("/api/sm/scan"),NULL)){
        char elfs[SM_MAX_ELFS][SM_EPATH];
        int n=sm_find_elfs(elfs);
        char json[4096]; int jp=0;
        jp+=snprintf(json+jp,sizeof(json)-jp,"{\"count\":%d,\"paths\":[",n);
        for(int i=0;i<n;i++)
            jp+=snprintf(json+jp,sizeof(json)-jp,"%s\"%s\"",i?",":"",elfs[i]);
        jp+=snprintf(json+jp,sizeof(json)-jp,"]}");
 
        mg_http_reply(c,200,"Content-Type: application/json\r\nCache-Control: no-cache\r\n","%s",json);
    }
    else if(mg_match(hm->uri,mg_str("/api/sm/stop"),NULL)){
        const char *names[]={"shadowmount.elf","ShadowMount.elf",
            "shadowmountplus.elf","ShadowMountPlus.elf","shadowmount","ShadowMount",NULL};
        int killed=0;
        for(int pi=0;names[pi];pi++){
            pid_t p=find_pid(names[pi]);
            if(p>0){kill(p,SIGTERM);killed=1;}
        }
        mg_http_reply(c,200,"Content-Type: application/json\r\nCache-Control: no-cache\r\n",
            "{\"ok\":%s}",killed?"true":"false");
    }
    else if(mg_match(hm->uri,mg_str("/api/sm/start"),NULL)){
        char lang[8]; read_lang(lang,sizeof(lang)); int en=(strcmp(lang,"en")==0||strcmp(lang,"fr")==0||strcmp(lang,"es")==0);
        char elf_path[SM_EPATH]={0};
        mg_http_get_var(&hm->query,"path",elf_path,sizeof(elf_path));
        if(!elf_path[0]){
            char elfs[SM_MAX_ELFS][SM_EPATH];
            if(sm_find_elfs(elfs)>0) strncpy(elf_path,elfs[0],SM_EPATH-1);
        }
        if(!elf_path[0]){ notify(en?"SM ELF not found":"SM ELF nicht gefunden"); mg_http_reply(c,200,"Content-Type: application/json\r\nCache-Control: no-cache\r\n","{\"ok\":false,\"err\":\"elf_not_found\"}"); return; }
        /* respond immediately — actual send happens on next poll to avoid blocking browser */
        strncpy(g_pending_elf, elf_path, SM_EPATH-1);
        /* do NOT save preferred ELF here — managed by auto-start panel only */
        mg_http_reply(c,200,"Content-Type: application/json\r\nCache-Control: no-cache\r\n","{\"ok\":true}");
    }
    else if(mg_match(hm->uri,mg_str("/api/config/backup"),NULL)){
        const char *bdir=BAK_DIR;
        mkdir("/data/SMPlusGui",0755); mkdir(bdir,0755);
        time_t now=time(NULL); struct tm *lt=localtime(&now);
        char cname[32]={0}; mg_http_get_var(&hm->query,"name",cname,sizeof(cname));
        /* sanitize custom name */
        for(char *p=cname;*p;p++) if(!isalnum(*p)&&*p!='_'&&*p!='-')*p='_';
        char fname[128];
        if(cname[0])
            snprintf(fname,sizeof(fname),"%s/cfg_%04d%02d%02d_%02d%02d%02d_%s.ini",
                bdir,lt->tm_year+1900,lt->tm_mon+1,lt->tm_mday,
                lt->tm_hour,lt->tm_min,lt->tm_sec,cname);
        else
            snprintf(fname,sizeof(fname),"%s/cfg_%04d%02d%02d_%02d%02d%02d.ini",
                bdir,lt->tm_year+1900,lt->tm_mon+1,lt->tm_mday,
                lt->tm_hour,lt->tm_min,lt->tm_sec);
        FILE *src=fopen(CONFIG_PATH,"r"); if(!src){mg_http_reply(c,404,"","no config");return;}
        FILE *dst=fopen(fname,"w"); if(!dst){fclose(src);mg_http_reply(c,500,"","write err");return;}
        char buf[512]; size_t n;
        while((n=fread(buf,1,sizeof(buf),src))>0) fwrite(buf,1,n,dst);
        fclose(src); fclose(dst);
        mg_http_reply(c,200,"Content-Type: application/json\r\n","{\"ok\":true}");
    }
    else if(mg_match(hm->uri,mg_str("/api/config/restore"),NULL)){
        char f[64]={0}; mg_http_get_var(&hm->query,"f",f,sizeof(f));
        if(!f[0]||strstr(f,"..")){{mg_http_reply(c,400,"","bad param");return;}}
        const char *bdir=BAK_DIR;
        char src_path[128]; snprintf(src_path,sizeof(src_path),"%s/%s",bdir,f);
        FILE *src=fopen(src_path,"r"); if(!src){mg_http_reply(c,404,"","not found");return;}
        FILE *dst=fopen(CONFIG_PATH,"w"); if(!dst){fclose(src);mg_http_reply(c,500,"","write err");return;}
        char buf[512]; size_t n;
        while((n=fread(buf,1,sizeof(buf),src))>0) fwrite(buf,1,n,dst);
        fclose(src); fclose(dst);
        mg_http_reply(c,200,"Content-Type: application/json\r\n","{\"ok\":true}");
    }
    else if(mg_match(hm->uri,mg_str("/api/config/upload-direct"),NULL)){
        if(hm->body.len==0||hm->body.len>65536){mg_http_reply(c,400,"","bad body");return;}
        if(!is_valid_sm_cfg(hm->body.buf,hm->body.len)){
            mg_http_reply(c,400,"Content-Type: application/json\r\n","{\"ok\":false,\"err\":\"invalid\"}");return;}
        FILE *fp=fopen(CONFIG_PATH,"w"); if(!fp){mg_http_reply(c,500,"","write err");return;}
        fwrite(hm->body.buf,1,hm->body.len,fp);
        fclose(fp);
        mg_http_reply(c,200,"Content-Type: application/json\r\n","{\"ok\":true}");
    }
    else if(mg_match(hm->uri,mg_str("/api/config/upload-bak-usb"),NULL)){
        char f[64]={0}; mg_http_get_var(&hm->query,"f",f,sizeof(f));
        if(!f[0]||strstr(f,"..")){{mg_http_reply(c,400,"","bad param");return;}}
        if(hm->body.len>0&&!is_valid_sm_cfg(hm->body.buf,hm->body.len)){
            mg_http_reply(c,400,"Content-Type: application/json\r\n","{\"ok\":false,\"err\":\"invalid\"}");return;}
        char usb[32]={0}; int found=0;
        for(int i=0;i<=7&&!found;i++){
            char p[24]; snprintf(p,sizeof(p),"/mnt/usb%d",i);
            DIR *d=opendir(p); if(d){
                struct dirent *de; int n=0;
                while((de=readdir(d))&&n<4) n++;
                closedir(d); if(n>2){strncpy(usb,p,31);found=1;}
            }
        }
        if(!found){mg_http_reply(c,404,"Content-Type: application/json\r\n","{\"ok\":false,\"err\":\"no_usb\"}");return;}
        char udir[64]; snprintf(udir,sizeof(udir),"%s/SMPlusGui",usb); mkdir(udir,0755);
        char fname[128];
        if(strncmp(f,"cfg_",4)!=0){
            time_t now=time(NULL); struct tm *lt=localtime(&now);
            char base[64]; strncpy(base,f,63); char *dot=strrchr(base,'.');if(dot)*dot=0;
            snprintf(fname,sizeof(fname),"%s/cfg_%04d%02d%02d_%02d%02d%02d_%s.ini",
                udir,lt->tm_year+1900,lt->tm_mon+1,lt->tm_mday,lt->tm_hour,lt->tm_min,lt->tm_sec,base);
        } else snprintf(fname,sizeof(fname),"%s/%s",udir,f);
        FILE *fp=fopen(fname,"w"); if(!fp){mg_http_reply(c,500,"","write err");return;}
        if(hm->body.len>0) fwrite(hm->body.buf,1,hm->body.len,fp);
        fclose(fp);
        mg_http_reply(c,200,"Content-Type: application/json\r\n","{\"ok\":true}");
    }
    else if(mg_match(hm->uri,mg_str("/api/config/upload-bak"),NULL)){
        char f[64]={0}; mg_http_get_var(&hm->query,"f",f,sizeof(f));
        if(!f[0]||strstr(f,"..")){{mg_http_reply(c,400,"","bad param");return;}}
        /* add timestamp prefix if file doesn't already start with cfg_ */
        char fname[128];
        if(strncmp(f,"cfg_",4)!=0){
            time_t now=time(NULL); struct tm *lt=localtime(&now);
            char base[64]; strncpy(base,f,63); char *dot=strrchr(base,'.');if(dot)*dot=0;
            snprintf(fname,sizeof(fname),BAK_DIR "/cfg_%04d%02d%02d_%02d%02d%02d_%s.ini",
                lt->tm_year+1900,lt->tm_mon+1,lt->tm_mday,lt->tm_hour,lt->tm_min,lt->tm_sec,base);
        } else snprintf(fname,sizeof(fname),BAK_DIR "/%s",f);
        FILE *fp=fopen(fname,"w"); if(!fp){mg_http_reply(c,500,"","write err");return;}
        if(hm->body.len>0&&!is_valid_sm_cfg(hm->body.buf,hm->body.len)){
            fclose(fp); unlink(fname);
            mg_http_reply(c,400,"Content-Type: application/json\r\n","{\"ok\":false,\"err\":\"invalid\"}");return;}
        if(hm->body.len>0) fwrite(hm->body.buf,1,hm->body.len,fp);
        fclose(fp);
        mg_http_reply(c,200,"Content-Type: application/json\r\n","{\"ok\":true}");
    }
    else if(mg_match(hm->uri,mg_str("/api/config/usb-export"),NULL)){
        char usb[32]={0}; int found=0;
        for(int i=0;i<=7&&!found;i++){
            char p[24]; snprintf(p,sizeof(p),"/mnt/usb%d",i);
            DIR *d=opendir(p); if(d){
                struct dirent *de; int n=0;
                while((de=readdir(d))&&n<4) n++;
                closedir(d); if(n>2){strncpy(usb,p,31);found=1;}
            }
        }
        if(!found){mg_http_reply(c,404,"Content-Type: application/json\r\n","{\"ok\":false,\"err\":\"no_usb\"}");return;}
        /* source: specific backup file or current config */
        char f[64]={0}; mg_http_get_var(&hm->query,"f",f,sizeof(f));
        char fsrc[128];
        if(f[0]&&!strstr(f,"..")) snprintf(fsrc,sizeof(fsrc),BAK_DIR "/%s",f);
        else strncpy(fsrc,CONFIG_PATH,sizeof(fsrc)-1);
        char dir[64]; snprintf(dir,sizeof(dir),"%s/SMPlusGui",usb); mkdir(dir,0755);
        char dst[128]; snprintf(dst,sizeof(dst),"%s/%s",dir,f[0]?f:"config.ini");
        FILE *src=fopen(fsrc,"r"); if(!src){mg_http_reply(c,404,"","{\"ok\":false}");return;}
        FILE *out=fopen(dst,"w"); if(!out){fclose(src);mg_http_reply(c,500,"","{\"ok\":false}");return;}
        char buf[512]; size_t n;
        while((n=fread(buf,1,sizeof(buf),src))>0) fwrite(buf,1,n,out);
        fclose(src); fclose(out);
        mg_http_reply(c,200,"Content-Type: application/json\r\n","{\"ok\":true,\"path\":\"%s\"}",dst);
    }
    else if(mg_match(hm->uri,mg_str("/api/config/usb-export-all"),NULL)){
        char usb[32]={0}; int found=0;
        for(int i=0;i<=7&&!found;i++){
            char p[24]; snprintf(p,sizeof(p),"/mnt/usb%d",i);
            DIR *d=opendir(p); if(d){
                struct dirent *de; int n=0;
                while((de=readdir(d))&&n<4) n++;
                closedir(d); if(n>2){strncpy(usb,p,31);found=1;}
            }
        }
        if(!found){mg_http_reply(c,404,"Content-Type: application/json\r\n","{\"ok\":false,\"err\":\"no_usb\"}");return;}
        char dir[64]; snprintf(dir,sizeof(dir),"%s/SMPlusGui",usb); mkdir(dir,0755);
        const char *bdir2=BAK_DIR;
        DIR *dp=opendir(bdir2); int cnt=0;
        if(dp){ struct dirent *de;
            while((de=readdir(dp))){
                if(de->d_name[0]=='.'||!strstr(de->d_name,".ini")) continue;
                char src[128],dst[128]; char buf[512]; size_t n;
                snprintf(src,sizeof(src),"%s/%s",bdir2,de->d_name);
                snprintf(dst,sizeof(dst),"%s/%s",dir,de->d_name);
                FILE *fi=fopen(src,"r"); if(!fi) continue;
                FILE *fo=fopen(dst,"w"); if(!fo){fclose(fi);continue;}
                while((n=fread(buf,1,sizeof(buf),fi))>0) fwrite(buf,1,n,fo);
                fclose(fi); fclose(fo); cnt++;
            }
            closedir(dp);
        }
        mg_http_reply(c,200,"Content-Type: application/json\r\n","{\"ok\":true,\"count\":%d}",cnt);
    }
    else if(mg_match(hm->uri,mg_str("/api/config/delete-usb-bak"),NULL)){
        char f[64]={0}; mg_http_get_var(&hm->query,"f",f,sizeof(f));
        if(!f[0]||strstr(f,"..")||strncmp(f,"cfg_",4)!=0){mg_http_reply(c,400,"","bad param");return;}
        char usb[32]={0}; int found=0;
        for(int i=0;i<=7&&!found;i++){
            char p[24]; snprintf(p,sizeof(p),"/mnt/usb%d",i);
            DIR *d=opendir(p); if(d){
                struct dirent *de; int n=0;
                while((de=readdir(d))&&n<4) n++;
                closedir(d); if(n>2){strncpy(usb,p,31);found=1;}
            }
        }
        if(!found){mg_http_reply(c,404,"Content-Type: application/json\r\n","{\"ok\":false,\"err\":\"no_usb\"}");return;}
        char path[128]; snprintf(path,sizeof(path),"%s/SMPlusGui/%s",usb,f);
        int r=unlink(path);
        mg_http_reply(c,200,"Content-Type: application/json\r\n","{\"ok\":%s}",r==0?"true":"false");
    }
    else if(mg_match(hm->uri,mg_str("/api/config/usb-copy-to-local"),NULL)){
        char f[64]={0}; mg_http_get_var(&hm->query,"f",f,sizeof(f));
        if(!f[0]||strstr(f,"..")||strncmp(f,"cfg_",4)!=0){mg_http_reply(c,400,"","bad param");return;}
        char usb[32]={0}; int found=0;
        for(int i=0;i<=7&&!found;i++){
            char p[24]; snprintf(p,sizeof(p),"/mnt/usb%d",i);
            DIR *d=opendir(p); if(d){
                struct dirent *de; int n=0;
                while((de=readdir(d))&&n<4) n++;
                closedir(d); if(n>2){strncpy(usb,p,31);found=1;}
            }
        }
        if(!found){mg_http_reply(c,404,"Content-Type: application/json\r\n","{\"ok\":false,\"err\":\"no_usb\"}");return;}
        char src[128]; snprintf(src,sizeof(src),"%s/SMPlusGui/%s",usb,f);
        FILE *fi=fopen(src,"r"); if(!fi){mg_http_reply(c,404,"Content-Type: application/json\r\n","{\"ok\":false}");return;}
        char vbuf[65537]; size_t vn=fread(vbuf,1,65536,fi); fclose(fi);
        if(!is_valid_sm_cfg(vbuf,vn)){mg_http_reply(c,400,"Content-Type: application/json\r\n","{\"ok\":false,\"err\":\"invalid\"}");return;}
        mkdir("/data/SMPlusGui",0755); mkdir(BAK_DIR,0755);
        char dst[128]; snprintf(dst,sizeof(dst),BAK_DIR "/%s",f);
        FILE *fo=fopen(dst,"w"); if(!fo){mg_http_reply(c,500,"Content-Type: application/json\r\n","{\"ok\":false}");return;}
        fwrite(vbuf,1,vn,fo); fclose(fo);
        mg_http_reply(c,200,"Content-Type: application/json\r\n","{\"ok\":true}");
    }
    else if(mg_match(hm->uri,mg_str("/api/config/backup-to-usb"),NULL)){
        char usb[32]={0}; int found=0;
        for(int i=0;i<=7&&!found;i++){
            char p[24]; snprintf(p,sizeof(p),"/mnt/usb%d",i);
            DIR *d=opendir(p); if(d){
                struct dirent *de; int n=0;
                while((de=readdir(d))&&n<4) n++;
                closedir(d); if(n>2){strncpy(usb,p,31);found=1;}
            }
        }
        if(!found){mg_http_reply(c,404,"Content-Type: application/json\r\n","{\"ok\":false,\"err\":\"no_usb\"}");return;}
        char ud[64]; snprintf(ud,sizeof(ud),"%s/SMPlusGui",usb); mkdir(ud,0755);
        time_t now=time(NULL); struct tm *lt=localtime(&now);
        char cname[32]={0}; mg_http_get_var(&hm->query,"name",cname,sizeof(cname));
        for(char *p=cname;*p;p++) if(!isalnum(*p)&&*p!='_'&&*p!='-')*p='_';
        char fname[128];
        if(cname[0]) snprintf(fname,sizeof(fname),"%s/cfg_%04d%02d%02d_%02d%02d%02d_%s.ini",
            ud,lt->tm_year+1900,lt->tm_mon+1,lt->tm_mday,lt->tm_hour,lt->tm_min,lt->tm_sec,cname);
        else snprintf(fname,sizeof(fname),"%s/cfg_%04d%02d%02d_%02d%02d%02d.ini",
            ud,lt->tm_year+1900,lt->tm_mon+1,lt->tm_mday,lt->tm_hour,lt->tm_min,lt->tm_sec);
        FILE *src=fopen(CONFIG_PATH,"r"); if(!src){mg_http_reply(c,404,"","no config");return;}
        FILE *dst=fopen(fname,"w"); if(!dst){fclose(src);mg_http_reply(c,500,"","write err");return;}
        char buf2[512]; size_t n2;
        while((n2=fread(buf2,1,sizeof(buf2),src))>0) fwrite(buf2,1,n2,dst);
        fclose(src); fclose(dst);
        mg_http_reply(c,200,"Content-Type: application/json\r\n","{\"ok\":true}");
    }
    else if(mg_match(hm->uri,mg_str("/api/config/delete-all-usb-bak"),NULL)){
        char usb[32]={0}; int found=0;
        for(int i=0;i<=7&&!found;i++){
            char p[24]; snprintf(p,sizeof(p),"/mnt/usb%d",i);
            DIR *d=opendir(p); if(d){
                struct dirent *de; int n=0;
                while((de=readdir(d))&&n<4) n++;
                closedir(d); if(n>2){strncpy(usb,p,31);found=1;}
            }
        }
        if(!found){mg_http_reply(c,200,"Content-Type: application/json\r\n","{\"ok\":true}");return;}
        char ud[64]; snprintf(ud,sizeof(ud),"%s/SMPlusGui",usb);
        DIR *dp=opendir(ud);
        if(dp){ struct dirent *de; char pp[128];
            while((de=readdir(dp)))
                if(de->d_name[0]!='.'&&strncmp(de->d_name,"cfg_",4)==0&&strstr(de->d_name,".ini")){
                    snprintf(pp,sizeof(pp),"%s/%s",ud,de->d_name); unlink(pp);}
            closedir(dp);
        }
        mg_http_reply(c,200,"Content-Type: application/json\r\n","{\"ok\":true}");
    }
    else if(mg_match(hm->uri,mg_str("/api/config/usb-status"),NULL)){
        char usb[32]={0}; int found=0;
        for(int i=0;i<=7&&!found;i++){
            char p[24]; snprintf(p,sizeof(p),"/mnt/usb%d",i);
            DIR *d=opendir(p); if(d){
                struct dirent *de; int n=0;
                while((de=readdir(d))&&n<4) n++;
                closedir(d); if(n>2){strncpy(usb,p,31);found=1;}
            }
        }
        mg_http_reply(c,200,"Content-Type: application/json\r\n",
            "{\"present\":%s,\"path\":\"%s\"}",found?"true":"false",found?usb:"");
    }
    else if(mg_match(hm->uri,mg_str("/api/config/usb-bak-list"),NULL)){
        char usb[32]={0}; int found=0;
        for(int i=0;i<=7&&!found;i++){
            char p[24]; snprintf(p,sizeof(p),"/mnt/usb%d",i);
            DIR *d=opendir(p); if(d){
                struct dirent *de; int n=0;
                while((de=readdir(d))&&n<4) n++;
                closedir(d); if(n>2){strncpy(usb,p,31);found=1;}
            }
        }
        if(!found){mg_http_reply(c,404,"Content-Type: application/json\r\n","{\"ok\":false,\"err\":\"no_usb\"}");return;}
        char usb_dir[64]; snprintf(usb_dir,sizeof(usb_dir),"%s/SMPlusGui",usb);
        char jbuf[2048]={0}; int jlen=0;
        jlen+=snprintf(jbuf+jlen,sizeof(jbuf)-jlen,"{\"ok\":true,\"files\":[");
        DIR *dp=opendir(usb_dir); int first=1;
        if(dp){ struct dirent *de;
            while((de=readdir(dp))&&jlen<(int)sizeof(jbuf)-80){
                char *nm=de->d_name; size_t nlen=strlen(nm);
                if(strncmp(nm,"cfg_",4)!=0||nlen<8||strcmp(nm+nlen-4,".ini")!=0||strstr(nm,"..")) continue;
                if(!first) jlen+=snprintf(jbuf+jlen,sizeof(jbuf)-jlen,",");
                jlen+=snprintf(jbuf+jlen,sizeof(jbuf)-jlen,"\"%s\"",nm);
                first=0;
            }
            closedir(dp);
        }
        snprintf(jbuf+jlen,sizeof(jbuf)-jlen,"]}");
        mg_http_reply(c,200,"Content-Type: application/json\r\n","%s",jbuf);
    }
    else if(mg_match(hm->uri,mg_str("/api/config/usb-restore-bak"),NULL)){
        char f[64]={0}; mg_http_get_var(&hm->query,"f",f,sizeof(f));
        if(!f[0]||strstr(f,"..")||strncmp(f,"cfg_",4)!=0){
            mg_http_reply(c,400,"","bad param");return;}
        char usb[32]={0}; int found=0;
        for(int i=0;i<=7&&!found;i++){
            char p[24]; snprintf(p,sizeof(p),"/mnt/usb%d",i);
            DIR *d=opendir(p); if(d){
                struct dirent *de; int n=0;
                while((de=readdir(d))&&n<4) n++;
                closedir(d); if(n>2){strncpy(usb,p,31);found=1;}
            }
        }
        if(!found){mg_http_reply(c,404,"Content-Type: application/json\r\n","{\"ok\":false,\"err\":\"no_usb\"}");return;}
        char src[128]; snprintf(src,sizeof(src),"%s/SMPlusGui/%s",usb,f);
        FILE *fi=fopen(src,"r"); if(!fi){mg_http_reply(c,404,"Content-Type: application/json\r\n","{\"ok\":false,\"err\":\"no_file\"}");return;}
        char vbuf[65537]; size_t vn=fread(vbuf,1,65536,fi); fclose(fi);
        if(!is_valid_sm_cfg(vbuf,vn)){
            mg_http_reply(c,400,"Content-Type: application/json\r\n","{\"ok\":false,\"err\":\"invalid\"}");return;}
        FILE *fo=fopen(CONFIG_PATH,"w"); if(!fo){mg_http_reply(c,500,"","{\"ok\":false}");return;}
        fwrite(vbuf,1,vn,fo); fclose(fo);
        mg_http_reply(c,200,"Content-Type: application/json\r\n","{\"ok\":true}");
    }
    else if(mg_match(hm->uri,mg_str("/api/config/usb-import-baks"),NULL)){
        char usb[32]={0}; int found=0;
        for(int i=0;i<=7&&!found;i++){
            char p[24]; snprintf(p,sizeof(p),"/mnt/usb%d",i);
            DIR *d=opendir(p); if(d){
                struct dirent *de; int n=0;
                while((de=readdir(d))&&n<4) n++;
                closedir(d); if(n>2){strncpy(usb,p,31);found=1;}
            }
        }
        if(!found){mg_http_reply(c,404,"Content-Type: application/json\r\n","{\"ok\":false,\"err\":\"no_usb\"}");return;}
        char usb_dir[64]; snprintf(usb_dir,sizeof(usb_dir),"%s/SMPlusGui",usb);
        mkdir("/data/SMPlusGui",0755); mkdir(BAK_DIR,0755);
        DIR *dp=opendir(usb_dir); int cnt=0,skip=0;
        if(!dp){mg_http_reply(c,200,"Content-Type: application/json\r\n","{\"ok\":true,\"count\":0,\"skipped\":0}");return;}
        struct dirent *de2;
        while((de2=readdir(dp))){
            char *nm=de2->d_name; size_t nlen=strlen(nm);
            if(strncmp(nm,"cfg_",4)!=0||nlen<8||strcmp(nm+nlen-4,".ini")!=0||strstr(nm,"..")) continue;
            char src[128],dst[128];
            snprintf(src,sizeof(src),"%s/%s",usb_dir,nm);
            snprintf(dst,sizeof(dst),BAK_DIR "/%s",nm);
            FILE *fi=fopen(src,"r"); if(!fi){skip++;continue;}
            char vbuf[65537]; size_t vn=fread(vbuf,1,65536,fi); fclose(fi);
            if(!is_valid_sm_cfg(vbuf,vn)){skip++;continue;}
            FILE *fo=fopen(dst,"w"); if(!fo){skip++;continue;}
            fwrite(vbuf,1,vn,fo); fclose(fo); cnt++;
        }
        closedir(dp);
        mg_http_reply(c,200,"Content-Type: application/json\r\n","{\"ok\":true,\"count\":%d,\"skipped\":%d}",cnt,skip);
    }
    else if(mg_match(hm->uri,mg_str("/api/config/usb-import"),NULL)){
        char usb[32]={0}; int found=0;
        for(int i=0;i<=7&&!found;i++){
            char p[24]; snprintf(p,sizeof(p),"/mnt/usb%d",i);
            DIR *d=opendir(p); if(d){
                struct dirent *de; int n=0;
                while((de=readdir(d))&&n<4) n++;
                closedir(d); if(n>2){strncpy(usb,p,31);found=1;}
            }
        }
        if(!found){mg_http_reply(c,404,"Content-Type: application/json\r\n","{\"ok\":false,\"err\":\"no_usb\"}");return;}
        char src_path[128]; snprintf(src_path,sizeof(src_path),"%s/SMPlusGui/config.ini",usb);
        FILE *src=fopen(src_path,"r");
        if(!src){mg_http_reply(c,404,"Content-Type: application/json\r\n","{\"ok\":false,\"err\":\"no_file\"}");return;}
        char buf[65537]; size_t n=fread(buf,1,65536,src); fclose(src);
        if(!is_valid_sm_cfg(buf,n)){
            mg_http_reply(c,400,"Content-Type: application/json\r\n","{\"ok\":false,\"err\":\"invalid\"}");return;}
        FILE *out=fopen(CONFIG_PATH,"w"); if(!out){mg_http_reply(c,500,"","{\"ok\":false}");return;}
        fwrite(buf,1,n,out); fclose(out);
        mg_http_reply(c,200,"Content-Type: application/json\r\n","{\"ok\":true}");
    }
    else if(mg_match(hm->uri,mg_str("/api/config/delete-all-bak"),NULL)){
        DIR *dp=opendir(BAK_DIR); if(!dp){mg_http_reply(c,200,"Content-Type: application/json\r\n","{\"ok\":true}");return;}
        struct dirent *de; char path[128];
        while((de=readdir(dp))) if(de->d_name[0]!='.'&&strstr(de->d_name,".ini")){
            snprintf(path,sizeof(path),BAK_DIR "/%s",de->d_name); unlink(path);
        }
        closedir(dp);
        mg_http_reply(c,200,"Content-Type: application/json\r\n","{\"ok\":true}");
    }
    else if(mg_match(hm->uri,mg_str("/api/config/delete-bak"),NULL)){
        char f[64]={0}; mg_http_get_var(&hm->query,"f",f,sizeof(f));
        if(!f[0]||strstr(f,"..")){{mg_http_reply(c,400,"","bad param");return;}}
        char path[128]; snprintf(path,sizeof(path),BAK_DIR "/%s",f);
        int r=unlink(path);
        mg_http_reply(c,200,"Content-Type: application/json\r\n","{\"ok\":%s}",r==0?"true":"false");
    }
    else if(mg_match(hm->uri,mg_str("/api/config/download"),NULL)){
        char f[64]={0}; mg_http_get_var(&hm->query,"f",f,sizeof(f));
        char src_path[128];
        if(f[0]&&!strstr(f,"..")) snprintf(src_path,sizeof(src_path),BAK_DIR "/%s",f);
        else strncpy(src_path,CONFIG_PATH,sizeof(src_path)-1);
        FILE *cf=fopen(src_path,"r");
        if(!cf){mg_http_reply(c,404,"","not found"); return;}
        fseek(cf,0,SEEK_END); long sz=ftell(cf); fseek(cf,0,SEEK_SET);
        char *buf=malloc(sz+1); if(!buf){fclose(cf);mg_http_reply(c,500,"","OOM");return;}
        fread(buf,1,sz,cf); fclose(cf); buf[sz]=0;
        const char *dlname=f[0]?f:"config.ini";
        char hdr[256]; snprintf(hdr,sizeof(hdr),
            "Content-Type: text/plain; charset=utf-8\r\n"
            "Content-Disposition: attachment; filename=\"%s\"\r\n",dlname);
        mg_http_reply(c,200,hdr,"%s",buf);
        free(buf);
    }
    else if(mg_match(hm->uri,mg_str("/api/sm/games"),NULL)){
        char *resp=sm_api_req("POST","/api/v1/games","{}");
        if(resp){
            mg_http_reply(c,200,"Content-Type: application/json\r\nCache-Control: no-cache\r\n","%s",resp);
            free(resp);
        } else mg_http_reply(c,503,"Content-Type: application/json\r\n","{\"error\":\"sm_not_active\"}");
    }
    else if(mg_match(hm->uri,mg_str("/api/config/raw"),NULL)&&mg_strcmp(hm->method,mg_str("GET"))==0){
        FILE *f=fopen(CONFIG_PATH,"r");
        if(!f){mg_http_reply(c,404,"Content-Type: text/plain\r\n","# config not found");return;}
        fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
        char *buf=malloc(sz+1); if(!buf){fclose(f);mg_http_reply(c,500,"","OOM");return;}
        fread(buf,1,sz,f); fclose(f); buf[sz]=0;
        mg_http_reply(c,200,"Content-Type: text/plain; charset=utf-8\r\nCache-Control: no-cache\r\n","%s",buf);
        free(buf);
    }
    else if(mg_match(hm->uri,mg_str("/api/config/raw"),NULL)&&mg_strcmp(hm->method,mg_str("POST"))==0){
        if(hm->body.len==0||hm->body.len>65536){mg_http_reply(c,400,"","bad body");return;}
        if(!is_valid_sm_cfg(hm->body.buf,hm->body.len)){
            mg_http_reply(c,400,"Content-Type: application/json\r\n","{\"ok\":false,\"err\":\"invalid\"}");return;}
        FILE *fp=fopen(CONFIG_PATH,"w"); if(!fp){mg_http_reply(c,500,"","write err");return;}
        fwrite(hm->body.buf,1,hm->body.len,fp); fclose(fp);
        mg_http_reply(c,200,"Content-Type: application/json\r\n","{\"ok\":true}");
    }
    else if(mg_match(hm->uri,mg_str("/api/log/clear"),NULL)){
        FILE *lf=fopen("/data/shadowmount/debug.log","w");
        if(lf) fclose(lf);
        mg_http_reply(c,200,"Content-Type: application/json\r\n","{\"ok\":true}");
    }
    else if(mg_match(hm->uri,mg_str("/api/config/download-usb"),NULL)){
        char f[64]={0}; mg_http_get_var(&hm->query,"f",f,sizeof(f));
        if(!f[0]||strstr(f,"..")||strncmp(f,"cfg_",4)!=0){mg_http_reply(c,400,"","bad param");return;}
        char usb[32]={0}; int found=0;
        for(int i=0;i<=7&&!found;i++){
            char p[24]; snprintf(p,sizeof(p),"/mnt/usb%d",i);
            DIR *d=opendir(p); if(d){
                struct dirent *de; int n=0;
                while((de=readdir(d))&&n<4) n++;
                closedir(d); if(n>2){strncpy(usb,p,31);found=1;}
            }
        }
        if(!found){mg_http_reply(c,404,"","no usb");return;}
        char src_path[128]; snprintf(src_path,sizeof(src_path),"%s/SMPlusGui/%s",usb,f);
        FILE *cf=fopen(src_path,"r"); if(!cf){mg_http_reply(c,404,"","not found");return;}
        fseek(cf,0,SEEK_END); long sz=ftell(cf); fseek(cf,0,SEEK_SET);
        char *buf=malloc(sz+1); if(!buf){fclose(cf);mg_http_reply(c,500,"","OOM");return;}
        fread(buf,1,sz,cf); fclose(cf); buf[sz]=0;
        char hdr2[256]; snprintf(hdr2,sizeof(hdr2),
            "Content-Type: text/plain; charset=utf-8\r\n"
            "Content-Disposition: attachment; filename=\"%s\"\r\n",f);
        mg_http_reply(c,200,hdr2,"%s",buf);
        free(buf);
    }
    else if(mg_match(hm->uri,mg_str("/api/log"),NULL)){
        FILE *lf=fopen("/data/shadowmount/debug.log","r");
        if(!lf){
            mg_http_reply(c,200,"Content-Type: text/plain; charset=utf-8\r\nCache-Control: no-cache\r\n","(log not found)");
            return;
        }
        /* seek backwards to find start of last 200 lines */
        fseek(lf,0,SEEK_END);
        long fsz=ftell(lf);
        int lines=0; long pos=fsz;
        while(pos>0&&lines<200){pos--;fseek(lf,pos,SEEK_SET);if(fgetc(lf)=='\n')lines++;}
        if(pos>0)fseek(lf,pos+1,SEEK_SET); else fseek(lf,0,SEEK_SET);
        long maxr=fsz-ftell(lf)+1;
        char *lb=malloc(maxr+2);
        if(!lb){fclose(lf);mg_http_reply(c,500,"","OOM");return;}
        size_t nr=fread(lb,1,maxr,lf);
        fclose(lf);
        lb[nr]=0;
        /* same pattern as main page (262KB) - proven to work for large content */
        mg_http_reply(c,200,"Content-Type: text/plain; charset=utf-8\r\nCache-Control: no-cache\r\n","%s",lb);
        free(lb);
    }
    else if(mg_match(hm->uri,mg_str("/api/prefs/get"),NULL)){
        SMPrefs p; read_prefs(&p);
        mg_http_reply(c,200,"Content-Type: application/json\r\n",
            "{\"auto_start\":%d,\"preferred_elf\":\"%s\"}",p.auto_start,p.preferred_elf);
    }
    else if(mg_match(hm->uri,mg_str("/api/prefs/save"),NULL)){
        char v[8]={0},ef[512]={0};
        mg_http_get_var(&hm->body,"auto_start",v,sizeof(v));
        mg_http_get_var(&hm->body,"preferred_elf",ef,sizeof(ef));
        SMPrefs p; read_prefs(&p);
        if(v[0]) p.auto_start=atoi(v);
        if(ef[0]||mg_http_get_var(&hm->body,"preferred_elf",ef,1)>=0)
            strncpy(p.preferred_elf,ef,511);
        write_prefs(&p);
        mg_http_reply(c,200,"Content-Type: application/json\r\n","{\"ok\":true}");
    }
    else mg_http_reply(c,404,"","404");
}

int payload_main(void) {
    (void)syscall(SYS_thr_set_name,-1,PAYLOAD_NAME);
    terminate_existing_instances(PAYLOAD_NAME);
    char _ip[48] = {0};
    get_local_ip(_ip, sizeof(_ip));
    char _url[64];
    if (_ip[0]) snprintf(_url, sizeof(_url), "http://%s:" HTTP_PORT, _ip);
    else        snprintf(_url, sizeof(_url), "Port " HTTP_PORT);
    _notify_send("SMPlusGui v" SMPLUS_VERSION, _url);
    struct mg_mgr mgr;
    mg_mgr_init(&mgr);
    mg_http_listen(&mgr,"http://0.0.0.0:" HTTP_PORT,fn,NULL);
    int polls=0, as_done=0;
    while(1){
        mg_mgr_poll(&mgr,1000); usleep(100000);
        if(g_pending_elf[0]){ send_elf_to_elfldr(g_pending_elf); g_pending_elf[0]=0; }
        if(++polls==3) smplus_install_if_needed();
        if(polls==2 && !as_done){ /* ~2s after start */
            as_done=1;
            SMPrefs prefs; read_prefs(&prefs);
            if(prefs.auto_start){
                char elf[SM_EPATH]={0};
                if(prefs.preferred_elf[0]) strncpy(elf,prefs.preferred_elf,SM_EPATH-1);
                else{ char elfs[SM_MAX_ELFS][SM_EPATH]; if(sm_find_elfs(elfs)>0) strncpy(elf,elfs[0],SM_EPATH-1); }
                if(elf[0]) send_elf_to_elfldr(elf);
            }
        }
    }
    mg_mgr_free(&mgr); return 0;
}

int main(void){ return payload_main(); }
