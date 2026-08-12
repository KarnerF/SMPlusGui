#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
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

void smplus_install_if_needed(void) {
    /* Only install once — skip if marker exists so icon stays at user's position */
    FILE *m = fopen(MARKER, "r");
    if (m) { fclose(m); return; }

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
    if (write_file(ico, sm_icon0_png,  sm_icon0_png_size)  != 0) goto done;

    if (install_title_dir(TITLE_ID, APP_ROOT "/") != 0) goto done;

    mkdir("/data/SMPlusGui", 0755);
    write_file(MARKER, (const uint8_t *)"ok\n", 3);

done:
    sceAppInstUtilTerminate();
}
