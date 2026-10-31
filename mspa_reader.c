/*
 * mspa_reader.c - SDL2 port of the MSPA-3DS reader for R36s-class handhelds
 * (RK3326, 640x480, aarch64 Linux, PortMaster).
 *
 * Reads the SAME pack folders the 3DS app uses:
 *   <packs>/<pack>/manifest.json
 *   <packs>/<pack>/pages/NNNNNN.json
 *   <packs>/<pack>/media/<base>-000.tex, <base>.anim, <audio>.wav
 *   <packs>/<pack>/bookmark.txt
 *
 * Usage: mspa_reader [packs_dir]      (default ./packs)
 * Env:   MSPA_SWAP_AB=1  swap the A/B buttons (Nintendo-style label layouts)
 *
 * Controls: A next | B prev | X pack menu | Y cycle layout | L1/R1 -/+10 pages
 *           D-pad up/down scroll text (hold) | START quit
 *
 * Build: see Makefile (needs SDL2 + SDL2_ttf).
 */
#include <SDL.h>
#include <SDL_ttf.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "mspa_page.h"

#define SCR_W 640
#define SCR_H 480
#define PANEL_W 320 /* builder pads/letterboxes every frame to 320x240 */
#define PANEL_H 240
#define TEX_MAGIC 0x58455435u  /* "TEX5" */
#define ANIM_MAGIC 0x53485432u /* "SHT2" */
#define TEX_FMT_RAW_RGBA 0x80u
#define MAX_PACKS 64
#define MARGIN 14

/* ---------------------------------------------------------------- types */
typedef struct {
    char folder[512];
    char id[64];
    char title[128];
    int firstPage, lastPage, pageCount;
} Pack;

typedef struct {
    SDL_Texture *tex;
    int w, h;
    SDL_Color color;
} TextItem;

typedef enum { ST_MENU, ST_READ } State;
typedef enum { LAYOUT_SPLIT, LAYOUT_IMAGE, LAYOUT_TEXT, LAYOUT_COUNT } Layout;

/* -------------------------------------------------------------- globals */
static SDL_Window *win;
static SDL_Renderer *ren;
static TTF_Font *fontBody, *fontTitle;
static char packsDir[512] = "./packs";
static bool swapAB = false;

static Pack packs[MAX_PACKS];
static int packCount, selPack;
static Pack *cur;

static State state = ST_MENU;
static Layout layout = LAYOUT_SPLIT;
static MspaPage page;
static bool pageValid;
static int pageNum, mediaIdx;

static SDL_Texture *frameTex;
static int frameW, frameH;
static char animBase[640];
static uint32_t animCount, *animDelays;
static uint32_t animFrame, animNextTick;

static TextItem *items;
static int itemCount, textTotalH, scrollY;

static SDL_AudioDeviceID audioDev;
static uint8_t *wavBuf;

/* -------------------------------------------------------------- helpers */
static bool file_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static char *read_all(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    if (len <= 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    buf[fread(buf, 1, (size_t)len, f)] = '\0';
    fclose(f);
    return buf;
}

static void json_str(const char *json, const char *key, char *dst, size_t n) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return;
    p = strchr(p + strlen(pat), '"');
    if (!p) return;
    p++;
    const char *e = strchr(p, '"');
    if (!e) return;
    size_t len = (size_t)(e - p);
    if (len >= n) len = n - 1;
    memcpy(dst, p, len);
    dst[len] = '\0';
}

static int json_int(const char *json, const char *key) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p = strchr(p, ':');
    return p ? atoi(p + 1) : 0;
}

/* ---------------------------------------------------------------- packs */
static bool open_pack(Pack *pk, const char *folder) {
    memset(pk, 0, sizeof *pk);
    snprintf(pk->folder, sizeof pk->folder, "%s", folder);
    const char *slash = strrchr(folder, '/');
    snprintf(pk->id, sizeof pk->id, "%s", slash ? slash + 1 : folder);
    char path[640];
    snprintf(path, sizeof path, "%s/manifest.json", folder);
    char *json = read_all(path);
    if (!json) return false;
    json_str(json, "pack_id", pk->id, sizeof pk->id);
    json_str(json, "title", pk->title, sizeof pk->title);
    pk->firstPage = json_int(json, "first_page");
    pk->lastPage = json_int(json, "last_page");
    pk->pageCount = json_int(json, "page_count");
    free(json);
    return true;
}

static void scan_packs(void) {
    packCount = 0;
    DIR *d = opendir(packsDir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && packCount < MAX_PACKS) {
        if (e->d_name[0] == '.') continue;
        char folder[512];
        snprintf(folder, sizeof folder, "%s/%s", packsDir, e->d_name);
        if (open_pack(&packs[packCount], folder)) packCount++;
    }
    closedir(d);
}

static int read_bookmark(const Pack *pk) {
    char path[640];
    snprintf(path, sizeof path, "%s/bookmark.txt", pk->folder);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int v = -1;
    if (fscanf(f, "%d", &v) != 1) v = -1;
    fclose(f);
    return v;
}

static void save_bookmark(const Pack *pk, int n) {
    char path[640];
    snprintf(path, sizeof path, "%s/bookmark.txt", pk->folder);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%d", n);
    fclose(f);
}

/* ----------------------------------------------------- page JSON access */
static bool load_page_json(const Pack *pk, int n, MspaPage *out) {
    char path[640];
    snprintf(path, sizeof path, "%s/pages/%06d.json", pk->folder, n);
    char *json = read_all(path);
    if (!json) return false;
    bool ok = parse_page_json(json, out);
    free(json);
    if (ok && out->page == 0) out->page = n;
    return ok;
}

/* highest existing page number below `n` (legacy bundles without "prev") */
static int find_previous_page(const Pack *pk, int n) {
    char dir[640];
    snprintf(dir, sizeof dir, "%s/pages", pk->folder);
    DIR *d = opendir(dir);
    if (!d) return 0;
    int best = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        int v = atoi(e->d_name);
        if (v > 0 && v < n && v > best) best = v;
    }
    closedir(d);
    return best;
}

/* ---------------------------------------------------------- .tex / .anim */
static bool load_tex_into(const char *path, SDL_Texture **texOut, int *wOut, int *hOut) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint32_t h[7]; /* magic w h texW texH format dataSize */
    if (fread(h, 4, 7, f) != 7 || h[0] != TEX_MAGIC || h[5] != TEX_FMT_RAW_RGBA) {
        fclose(f); /* format 0 (3DS GPU-tiled) is not portable; builder never emits it */
        return false;
    }
    uint32_t w = h[1], ht = h[2], texW = h[3], size = h[6];
    size_t tight = (size_t)w * ht * 4, padded = (size_t)texW * h[4] * 4;
    if (w == 0 || ht == 0 || w > 2048 || ht > 2048 || (size != tight && size != padded)) {
        fclose(f);
        return false;
    }
    uint8_t *raw = malloc(size);
    if (!raw || fread(raw, 1, size, f) != size) { free(raw); fclose(f); return false; }
    fclose(f);

    SDL_Texture *t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, (int)w, (int)ht);
    if (!t) { free(raw); return false; }
    if (size == padded && texW != w) { /* repack padded rows */
        uint8_t *tmp = malloc(tight);
        if (!tmp) { free(raw); SDL_DestroyTexture(t); return false; }
        for (uint32_t y = 0; y < ht; y++)
            memcpy(tmp + (size_t)y * w * 4, raw + (size_t)y * texW * 4, (size_t)w * 4);
        free(raw);
        raw = tmp;
    }
    SDL_UpdateTexture(t, NULL, raw, (int)w * 4);
    free(raw);
    SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
    *texOut = t;
    *wOut = (int)w;
    *hOut = (int)ht;
    return true;
}

static void load_frame(uint32_t idx) {
    char path[704];
    snprintf(path, sizeof path, "%s-%03u.tex", animBase, idx);
    SDL_Texture *t = NULL;
    int w, h;
    if (load_tex_into(path, &t, &w, &h)) {
        if (frameTex) SDL_DestroyTexture(frameTex);
        frameTex = t;
        frameW = w;
        frameH = h;
    }
}

static void free_media(void) {
    if (frameTex) { SDL_DestroyTexture(frameTex); frameTex = NULL; }
    free(animDelays);
    animDelays = NULL;
    animCount = 0;
    if (audioDev) { SDL_CloseAudioDevice(audioDev); audioDev = 0; }
    if (wavBuf) { SDL_FreeWAV(wavBuf); wavBuf = NULL; }
}

static void load_anim(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    uint32_t hdr[3];
    if (fread(hdr, 4, 3, f) != 3 || hdr[0] != ANIM_MAGIC || hdr[1] != 2 || hdr[2] < 2 || hdr[2] > 4096) {
        fclose(f);
        return;
    }
    uint32_t *d = calloc(hdr[2], 4);
    if (d && fread(d, 4, hdr[2], f) == hdr[2]) {
        animDelays = d;
        animCount = hdr[2];
    } else {
        free(d);
    }
    fclose(f);
}

static void start_audio(const char *wavPath) {
    SDL_AudioSpec spec, have;
    uint32_t len;
    if (!SDL_LoadWAV(wavPath, &spec, &wavBuf, &len)) { wavBuf = NULL; return; }
    audioDev = SDL_OpenAudioDevice(NULL, 0, &spec, &have, 0);
    if (!audioDev) return;
    SDL_QueueAudio(audioDev, wavBuf, len);
    SDL_PauseAudioDevice(audioDev, 0);
}

/* ------------------------------------------------------------------ text */
static void free_text(void) {
    for (int i = 0; i < itemCount; i++)
        if (items[i].tex) SDL_DestroyTexture(items[i].tex);
    free(items);
    items = NULL;
    itemCount = textTotalH = scrollY = 0;
}

static void add_item(TTF_Font *font, const char *s, SDL_Color col, int wrapW) {
    if (!s || !*s) return;
    SDL_Surface *surf = TTF_RenderUTF8_Blended_Wrapped(font, s, col, (Uint32)wrapW);
    if (!surf) return;
    TextItem *n = realloc(items, sizeof(TextItem) * (size_t)(itemCount + 1));
    if (!n) { SDL_FreeSurface(surf); return; }
    items = n;
    items[itemCount].tex = SDL_CreateTextureFromSurface(ren, surf);
    items[itemCount].w = surf->w;
    items[itemCount].h = surf->h;
    items[itemCount].color = col;
    textTotalH += surf->h + 8;
    itemCount++;
    SDL_FreeSurface(surf);
}

static void build_text(void) {
    free_text();
    SDL_Color black = {0, 0, 0, 255}, blue = {0, 70, 200, 255};
    int w = SCR_W - 2 * MARGIN;
    add_item(fontTitle, (page.type && page.type[0]) ? page.type : "PAGE", black, w);
    for (size_t i = 0; i < page.textCount; i++) add_item(fontBody, page.text[i], black, w);
    if (page.command && page.command[0]) add_item(fontBody, page.command, blue, w);
}

/* ------------------------------------------------------------- page flow */
static void open_page(int n, int mi) {
    free_media();
    mspa_page_free(&page);
    pageValid = load_page_json(cur, n, &page);
    if (!pageValid) { free_text(); return; }
    pageNum = n;
    mediaIdx = (mi < 0 || (size_t)mi >= page.mediaCount) ? 0 : mi;
    animBase[0] = '\0';

    if (page.media && page.mediaCount > 0) {
        char rel[256];
        snprintf(rel, sizeof rel, "%s", page.media[mediaIdx]);
        char *dot = strrchr(rel, '.');
        if (dot) *dot = '\0';
        snprintf(animBase, sizeof animBase, "%s/%s", cur->folder, rel);
        load_frame(0);
        char ap[704];
        snprintf(ap, sizeof ap, "%s.anim", animBase);
        load_anim(ap);
        animFrame = 0;
        animNextTick = SDL_GetTicks() + (animDelays ? (animDelays[0] ? animDelays[0] : 100) : 0);
    }
    if (page.audio && page.audio[0]) {
        char wp[704];
        snprintf(wp, sizeof wp, "%s/%s", cur->folder, page.audio);
        if (file_exists(wp)) start_audio(wp);
    }
    build_text();
    save_bookmark(cur, n);
}

static void go_next(void) {
    if (!pageValid) return;
    if (page.mediaCount > 1 && (size_t)mediaIdx + 1 < page.mediaCount) { open_page(pageNum, mediaIdx + 1); return; }
    int nx = page.next;
    if (nx <= pageNum) nx = pageNum + 1;
    char probe[640];
    snprintf(probe, sizeof probe, "%s/pages/%06d.json", cur->folder, nx);
    if (file_exists(probe)) open_page(nx, 0);
}

static void go_prev(void) {
    if (!pageValid) return;
    if (page.mediaCount > 1 && mediaIdx > 0) { open_page(pageNum, mediaIdx - 1); return; }
    int pv = page.prev;
    if (pv <= 0 || pv >= pageNum) pv = find_previous_page(cur, pageNum);
    if (pv > 0) open_page(pv, 0);
}

/* jump ~steps pages by following next/prev links */
static void jump(int steps) {
    int n = pageNum;
    for (int i = 0; i < (steps < 0 ? -steps : steps); i++) {
        MspaPage tmp = {0};
        if (!load_page_json(cur, n, &tmp)) break;
        int t = steps > 0 ? tmp.next : tmp.prev;
        if (steps > 0 && t <= n) t = 0;
        if (steps < 0 && (t <= 0 || t >= n)) t = find_previous_page(cur, n);
        mspa_page_free(&tmp);
        if (t <= 0) break;
        n = t;
    }
    if (n != pageNum) open_page(n, 0);
}

static void enter_pack(int idx) {
    cur = &packs[idx];
    int bm = read_bookmark(cur);
    int start = bm > 0 ? bm : cur->firstPage;
    char probe[640];
    snprintf(probe, sizeof probe, "%s/pages/%06d.json", cur->folder, start);
    if (!file_exists(probe)) start = cur->firstPage;
    state = ST_READ;
    open_page(start, 0);
}

/* ------------------------------------------------------------- rendering */
static void draw_text_line(TTF_Font *font, const char *s, int x, int y, SDL_Color c) {
    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, s, c);
    if (!surf) return;
    SDL_Texture *t = SDL_CreateTextureFromSurface(ren, surf);
    SDL_Rect r = {x, y, surf->w, surf->h};
    SDL_RenderCopy(ren, t, NULL, &r);
    SDL_DestroyTexture(t);
    SDL_FreeSurface(surf);
}

static void draw_image(SDL_Rect area) {
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderFillRect(ren, &area);
    if (!frameTex) return;
    float sx = (float)area.w / PANEL_W, sy = (float)area.h / PANEL_H;
    float s = sx < sy ? sx : sy;
    SDL_Rect dst = {0, 0, (int)(frameW * s), (int)(frameH * s)};
    dst.x = area.x + (area.w - dst.w) / 2;
    dst.y = area.y + (area.h - dst.h) / 2;
    SDL_RenderCopy(ren, frameTex, NULL, &dst);
}

static void draw_text_area(SDL_Rect area) {
    SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
    SDL_RenderFillRect(ren, &area);
    SDL_RenderSetClipRect(ren, &area);
    int y = area.y + 6 - scrollY;
    for (int i = 0; i < itemCount; i++) {
        SDL_Rect r = {MARGIN, y, items[i].w, items[i].h};
        if (r.y + r.h >= area.y && r.y <= area.y + area.h) SDL_RenderCopy(ren, items[i].tex, NULL, &r);
        y += items[i].h + 8;
    }
    SDL_RenderSetClipRect(ren, NULL);
}

static int max_scroll(int areaH) {
    int m = textTotalH + 12 - areaH;
    return m > 0 ? m : 0;
}

static void render(void) {
    SDL_Color black = {0, 0, 0, 255}, grey = {110, 110, 110, 255};
    SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
    SDL_RenderClear(ren);
    if (state == ST_MENU) {
        draw_text_line(fontTitle, "MSPA-3DS  (R36s)", MARGIN, 10, black);
        if (packCount == 0) {
            draw_text_line(fontBody, "No packs found. Put pack folders in:", MARGIN, 60, grey);
            draw_text_line(fontBody, packsDir, MARGIN, 90, black);
        } else {
            for (int i = 0; i < packCount; i++) {
                char line[300];
                snprintf(line, sizeof line, "%s %s", i == selPack ? ">" : " ", packs[i].title[0] ? packs[i].title : packs[i].id);
                draw_text_line(fontBody, line, MARGIN, 56 + i * 28, i == selPack ? black : grey);
            }
        }
        draw_text_line(fontBody, "A: open   Up/Down: choose   START: quit", MARGIN, SCR_H - 34, grey);
    } else {
        SDL_Rect img, txt;
        switch (layout) {
        case LAYOUT_IMAGE: img = (SDL_Rect){0, 0, SCR_W, SCR_H}; txt = (SDL_Rect){0, 0, 0, 0}; break;
        case LAYOUT_TEXT:  img = (SDL_Rect){0, 0, 0, 0}; txt = (SDL_Rect){0, 0, SCR_W, SCR_H}; break;
        default:           img = (SDL_Rect){0, 0, SCR_W, 288}; txt = (SDL_Rect){0, 288, SCR_W, SCR_H - 288}; break;
        }
        if (scrollY > max_scroll(txt.h)) scrollY = max_scroll(txt.h);
        if (img.w) draw_image(img);
        if (txt.w) draw_text_area(txt);
    }
    SDL_RenderPresent(ren);
}

static int text_area_h(void) {
    return layout == LAYOUT_TEXT ? SCR_H : layout == LAYOUT_SPLIT ? SCR_H - 288 : 0;
}

/* ------------------------------------------------------------------ main */
int main(int argc, char **argv) {
    if (argc > 1) snprintf(packsDir, sizeof packsDir, "%s", argv[1]);
    const char *sw = getenv("MSPA_SWAP_AB");
    swapAB = sw && sw[0] == '1';

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    TTF_Init();
    win = SDL_CreateWindow("MSPA", 0, 0, SCR_W, SCR_H, SDL_WINDOW_FULLSCREEN_DESKTOP);
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    SDL_RenderSetLogicalSize(ren, SCR_W, SCR_H);
    SDL_ShowCursor(SDL_DISABLE);

    fontBody = TTF_OpenFont("fonts/DejaVuSans.ttf", 18);
    fontTitle = TTF_OpenFont("fonts/DejaVuSans.ttf", 22);
    if (!fontBody || !fontTitle) {
        fprintf(stderr, "font load failed: %s\n", TTF_GetError());
        return 1;
    }
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i)) SDL_GameControllerOpen(i);

    scan_packs();
    bool running = true, dirty = true;
    int scrollDir = 0;

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            int btn = -1;
            if (e.type == SDL_QUIT) running = false;
            if (e.type == SDL_CONTROLLERBUTTONDOWN || e.type == SDL_CONTROLLERBUTTONUP) btn = e.cbutton.button;
            if (btn < 0) continue;
            bool down = e.type == SDL_CONTROLLERBUTTONDOWN;
            if (swapAB) {
                if (btn == SDL_CONTROLLER_BUTTON_A) btn = SDL_CONTROLLER_BUTTON_B;
                else if (btn == SDL_CONTROLLER_BUTTON_B) btn = SDL_CONTROLLER_BUTTON_A;
            }
            if (btn == SDL_CONTROLLER_BUTTON_DPAD_UP) scrollDir = down ? -1 : (scrollDir == -1 ? 0 : scrollDir);
            if (btn == SDL_CONTROLLER_BUTTON_DPAD_DOWN) scrollDir = down ? 1 : (scrollDir == 1 ? 0 : scrollDir);
            if (!down) continue;
            if (btn == SDL_CONTROLLER_BUTTON_START) running = false;
            if (state == ST_MENU) {
                if (btn == SDL_CONTROLLER_BUTTON_DPAD_UP && selPack > 0) selPack--;
                if (btn == SDL_CONTROLLER_BUTTON_DPAD_DOWN && selPack + 1 < packCount) selPack++;
                if (btn == SDL_CONTROLLER_BUTTON_A && packCount) enter_pack(selPack);
            } else {
                if (btn == SDL_CONTROLLER_BUTTON_A) go_next();
                if (btn == SDL_CONTROLLER_BUTTON_B) go_prev();
                if (btn == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) jump(-10);
                if (btn == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) jump(10);
                if (btn == SDL_CONTROLLER_BUTTON_Y) layout = (Layout)((layout + 1) % LAYOUT_COUNT);
                if (btn == SDL_CONTROLLER_BUTTON_X) {
                    free_media();
                    free_text();
                    mspa_page_free(&page);
                    pageValid = false;
                    scan_packs();
                    selPack = 0;
                    state = ST_MENU;
                }
            }
            dirty = true;
        }

        if (state == ST_READ) {
            if (scrollDir) {
                scrollY += scrollDir * 6;
                if (scrollY < 0) scrollY = 0;
                int ms = max_scroll(text_area_h());
                if (scrollY > ms) scrollY = ms;
                dirty = true;
            }
            if (animCount > 1 && SDL_GetTicks() >= animNextTick) {
                animFrame = (animFrame + 1) % animCount;
                load_frame(animFrame);
                uint32_t d = animDelays[animFrame] ? animDelays[animFrame] : 100;
                animNextTick = SDL_GetTicks() + d;
                dirty = true;
            }
        }
        if (dirty) { render(); dirty = false; }
        SDL_Delay(8);
    }

    free_media();
    free_text();
    mspa_page_free(&page);
    TTF_Quit();
    SDL_Quit();
    return 0;
}
