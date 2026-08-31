#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "app_installer.h"

/* Assets einbetten - exakt wie Payload Manager und Game Compressor */
#define INCASSET(name, file) \
  __asm__(".section .rodata\n" \
          ".global " #name "\n" \
          ".global " #name "_size\n" \
          ".align 16\n" #name ":\n" \
          ".incbin \"" file "\"\n.L" #name "_end:\n" #name "_size:\n" \
          ".quad .L" #name "_end - " #name "\n" \
          ".previous\n"); \
  extern const uint8_t name[]; \
  extern const size_t name##_size

INCASSET(sm_param_json, "assets-app/param.json");
INCASSET(sm_icon0_png,  "assets-app/icon1.png");

/* Direkt verlinkt via -lSceAppInstUtil (wie SDK install_app Sample) */
int sceAppInstUtilInitialize(void);
int sceAppInstUtilTerminate(void);
int sceAppInstUtilAppInstallAll(void *);
int sceAppInstUtilAppUnInstall(const char *);

/* NID-basiert: sceAppInstUtilAppInstallTitleDir */
#include <ps5/kernel.h>
static int install_title_dir(const char *title_id, const char *dir) {
    int (*fn)(const char *, const char *, void *) = NULL;
    uint32_t handle = 0;
    if (kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &handle) == 0)
        fn = (void *)kernel_dynlib_resolve(-1, handle, "Wudg3Xe3heE");
    if (fn) return fn(title_id, dir, NULL);
    return sceAppInstUtilAppInstallAll(NULL);
}

#define TITLE_ID "SMPL00001"
#define APP_ROOT "/user/app"
#define MARKER   "/data/SMPlusGui/launcher.ok"

static int write_file(const char *p, const uint8_t *d, size_t s) {
    FILE *f = fopen(p, "wb");
    if (!f) return -1;
    size_t w = fwrite(d, 1, s, f);
    fclose(f);
    return w == s ? 0 : -1;
}

void smplus_install_if_needed(int icon_always_front, int http_port) {
    int port = http_port>1024 ? http_port : 7777;
    int need = icon_always_front;
    if(!need){
        /* reinstall if tile directory is missing */
        struct stat tst; char tdir[256];
        snprintf(tdir,sizeof(tdir),APP_ROOT "/%s",TITLE_ID);
        if(stat(tdir,&tst)!=0) need=1;
    }
    if(!need){
        /* reinstall if marker missing or port changed */
        FILE *m=fopen(MARKER,"r");
        if(!m){need=1;}
        else{char buf[16]={0};fread(buf,1,sizeof(buf)-1,m);fclose(m);
             if(atoi(buf)!=port) need=1;}
    }
    if(!need) return;

    if (sceAppInstUtilInitialize() != 0) return;
    char adir[256], sdir[256], par[256], ico[256];
    snprintf(adir, sizeof(adir), APP_ROOT "/%s",                    TITLE_ID);
    snprintf(sdir, sizeof(sdir), APP_ROOT "/%s/sce_sys",            TITLE_ID);
    snprintf(par,  sizeof(par),  APP_ROOT "/%s/sce_sys/param.json", TITLE_ID);
    snprintf(ico,  sizeof(ico),  APP_ROOT "/%s/sce_sys/icon0.png",  TITLE_ID);

    sceAppInstUtilAppUnInstall(TITLE_ID);
    mkdir(adir, 0755);
    mkdir(sdir, 0755);

    if (write_file(par, sm_param_json, sm_param_json_size) != 0) goto done;
    /* patch deeplink port in the written param.json */
    {FILE *pf=fopen(par,"r+");if(pf){
        char buf[1024]={0};fread(buf,1,sizeof(buf)-1,pf);
        char old_url[64],new_url[64];
        snprintf(old_url,sizeof(old_url),"http://127.0.0.1:7777/");
        snprintf(new_url,sizeof(new_url),"http://127.0.0.1:%d/",http_port>1024?http_port:7777);
        char *pos=strstr(buf,old_url);
        if(pos&&strlen(old_url)==strlen(new_url)){memcpy(pos,new_url,strlen(new_url));
            fseek(pf,0,SEEK_SET);fwrite(buf,1,strlen(buf),pf);}
        fclose(pf);}}
    if (write_file(ico, sm_icon0_png,  sm_icon0_png_size)  != 0) goto done;

    install_title_dir(TITLE_ID, APP_ROOT "/");
    mkdir("/data/SMPlusGui", 0755);
    /* write port to marker so changes trigger reinstall */
    {char pm[16]; snprintf(pm,sizeof(pm),"%d\n",port);
     write_file(MARKER,(const uint8_t *)pm,strlen(pm));}

done:
    sceAppInstUtilTerminate();
}
