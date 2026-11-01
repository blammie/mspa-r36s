/*
 * mspa_reader.c - Homestuck / MSPA / comics reader for R36s-class handhelds, reading the
 * Unofficial Homestuck Collection "Asset Pack V2" directly (no conversion step).
 *
 *   <assets>/archive/data/mspa.json     stories (Jailbreak ... Homestuck, Ryanquest)
 *   <assets>/archive/data/comics.json   Paradox Space, Snaps, Hussie's old comics
 *   <assets>/sweetbroandhellajeff/...   SBaHJ (browsed as an image folder)
 *   galleries.txt                       optional extra "Name|/folder" image galleries
 *
 * Tabs (L2/R2), links on pages (Select), inline images, GIF/PNG/JPEG, Flash via Ruffle
 * (run_flash.sh) and video via run_video.sh.
 *
 * Usage: mspa_reader [assets_dir]     (or env MSPA_ASSETS)       Env: MSPA_SWAP_AB=1
 */
#include <SDL.h>
#include <SDL_ttf.h>
#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include <stb/stb_image.h>

#include "gifdec.h"
#include "mjson.h"

#define SCR_W 640
#define SCR_H 480
#define TB 22                       /* tab bar height */
#define VIEW_H (SCR_H - TB)
#define COL_W 580
#define COL_X ((SCR_W - COL_W) / 2)
#define PNL_W 620
#define PNL_X ((SCR_W - PNL_W) / 2)
#define COL_TOP 8
#define PAD 20
#define MAX_MEDIA 40
#define MAX_NEXT 8
#define MAX_LINKS 64
#define MAX_TABS 6
#define HIST_MAX 128
#define MAX_BM 600
#define MAX_TEX 4096

static const SDL_Color C_BG = {83, 83, 83, 255};
static const SDL_Color C_PANEL = {198, 198, 198, 255};
static const SDL_Color C_COL = {238, 238, 238, 255};
static const SDL_Color C_BLACK = {0, 0, 0, 255};
static const SDL_Color C_LINK = {0, 0, 238, 255};
static const SDL_Color C_GREY = {110, 110, 110, 255};
static const SDL_Color C_CARD = {52, 52, 52, 255};
static const SDL_Color C_CARDTXT = {235, 235, 235, 255};
static const SDL_Color C_TABBG = {40, 40, 40, 255};
static const SDL_Color C_TABOFF = {75, 75, 75, 255};
static const SDL_Color C_HILITE = {255, 247, 190, 255};

/* ---------------------------------------------------------------- types */
typedef struct { const char *k; uint32_t l; const JNode *n; } Key;
typedef struct { Key *keys; int n; } Section;

enum { SK_STORY, SK_JSON, SK_DIR, SK_SINGLE };

typedef struct {
    char id[110];       /* stable id for bookmarks / saved tabs */
    char name[110];
    char tag[16];       /* short label for the tab bar */
    int kind;
    int sect;           /* SK_STORY: section in mspa.json */
    char start[24];     /* SK_STORY: first page id */
    const JNode *node;  /* SK_JSON: comic object */
    char dir[256];      /* SK_DIR: folder below the asset root */
    char **files;
    int nfiles;
    bool scanned;
    char single[256];   /* SK_SINGLE: media path */
} Source;

typedef struct { char name[48]; int first, n; } Cat;

typedef struct {
    char id[24];
    char title[300];
    char *html;
    char media[MAX_MEDIA][256];
    int nmedia;
    char next[MAX_NEXT][24];
    int nnext;
    char prev[24];
    bool valid;
} Page;

typedef struct {
    int kind; /* 0 image, 1 flash, 2 message card, 3 video */
    char rel[256];
    char path[640];
    SDL_Texture *tex;
    int w, h;
    Gif *gif;
    uint32_t nextTick;
    int y, dw, dh;
    char label[256];
} Media;

typedef struct {
    char *text;
    SDL_Color col;
    TTF_Font *font;
    int x, y, w, h;
    SDL_Texture *tex;
} Item;

enum { LK_PAGE, LK_ASSET, LK_TSO, LK_WEB };
typedef struct {
    char url[300];
    char label[100];
    int kind;
    char target[260];
} Link;

typedef struct {
    int src;
    char key[24];
    int scroll;
    char hist[HIST_MAX][24];
    int nHist;
} Tab;

typedef struct { char sid[110]; char key[24]; } Bm;

/* ------------------------------------------------------------- globals */
static SDL_Window *win;
static SDL_Renderer *ren;
static TTF_Font *fTitle, *fBody, *fCmd, *fLink, *fFoot, *fFootU, *fMenu;
static bool swapAB;

static char assets[512] = "";
static char loadError[256];
static char *jsonBuf, *comicsBuf;
static JNode *jroot, *croot;
static Section sect[2];
static const char *SECT_NAMES[2] = {"story", "ryanquest"};

static Source *srcs;
static int nSrcs, capSrcs;
static Cat cats[8];
static int nCats;

static Tab tabs[MAX_TABS];
static int nTabs, curTab;
#define CT (tabs[curTab])

static Bm bms[MAX_BM];
static int nBms;

static Page page;
static Media media[MAX_MEDIA];
static int nMedia;
static Item *items;
static int nItems, capItems;
static Link links[MAX_LINKS];
static int nLinks;
static int pageH, scrollY, selChoice;
static int choiceTop[MAX_NEXT], choiceBot[MAX_NEXT];
static char choiceText[MAX_NEXT][300];
static bool havePage;
static bool menuMode = true;
static int overlay;         /* 0 none, 1 link list */
static int selLink;
static int libLevel, selCat, selEnt;
static int dpadDir, stickDir;
static char toastMsg[200];
static uint32_t toastUntil;
static bool g_links, g_inl;
static int curLinkIdx = -1;

typedef struct { const char *name, *tag; int sect; const char *start; } Story;
static const Story STORIES[] = {
    {"Jailbreak", "JB", 0, "000002"},      {"Bard Quest", "BQ", 0, "000136"},
    {"Problem Sleuth", "PS", 0, "000219"}, {"Homestuck Beta", "HSB", 0, "001893"},
    {"Homestuck", "HS", 0, "001901"},      {"Ryanquest", "RQ", 1, "000001"},
};
#define NSTORIES ((int)(sizeof STORIES / sizeof STORIES[0]))

/* ------------------------------------------------------------- helpers */
static void toast(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(toastMsg, sizeof toastMsg, fmt, ap);
    va_end(ap);
    toastUntil = SDL_GetTicks() + 2800;
}

static bool file_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static bool dir_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static uint8_t *read_file(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    if (len <= 0) { fclose(f); return NULL; }
    uint8_t *b = malloc((size_t)len + 1);
    if (!b) { fclose(f); return NULL; }
    if (fread(b, 1, (size_t)len, f) != (size_t)len) { free(b); fclose(f); return NULL; }
    b[len] = 0;
    fclose(f);
    if (n) *n = (size_t)len;
    return b;
}

static void write_small(const char *path, const char *s) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    fputs(s, f);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
}

/* case-insensitive path resolve below `assets` (the pack may live on a case-sensitive fs) */
static bool resolve_path(const char *rel, char *out, size_t n) {
    snprintf(out, n, "%s%s", assets, rel);
    if (file_exists(out)) return true;
    char cur[640];
    snprintf(cur, sizeof cur, "%s", assets);
    const char *p = rel;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char comp[256];
        size_t l = strcspn(p, "/");
        if (l >= sizeof comp) return false;
        memcpy(comp, p, l);
        comp[l] = 0;
        p += l;
        DIR *d = opendir(cur);
        if (!d) return false;
        struct dirent *e;
        bool found = false;
        while ((e = readdir(d))) {
            if (strcasecmp(e->d_name, comp) == 0) {
                size_t cl = strlen(cur);
                snprintf(cur + cl, sizeof cur - cl, "/%s", e->d_name);
                found = true;
                break;
            }
        }
        closedir(d);
        if (!found) return false;
    }
    snprintf(out, n, "%s", cur);
    return file_exists(out);
}

static int key_cmp(const void *a, const void *b) {
    const Key *x = a, *y = b;
    uint32_t m = x->l < y->l ? x->l : y->l;
    int c = memcmp(x->k, y->k, m);
    return c ? c : (x->l > y->l) - (x->l < y->l);
}

static const JNode *find_page(int s, const char *id) {
    Key k = {id, (uint32_t)strlen(id), NULL};
    Key *r = bsearch(&k, sect[s].keys, (size_t)sect[s].n, sizeof(Key), key_cmp);
    return r ? r->n : NULL;
}

static int natcmp(const char *a, const char *b) {
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            char *ea, *eb;
            unsigned long x = strtoul(a, &ea, 10), y = strtoul(b, &eb, 10);
            if (x != y) return x < y ? -1 : 1;
            a = ea; b = eb;
        } else {
            int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
            if (ca != cb) return ca < cb ? -1 : 1;
            a++; b++;
        }
    }
    return (*a != 0) - (*b != 0);
}
static int natcmp_ptr(const void *a, const void *b) { return natcmp(*(char *const *)a, *(char *const *)b); }

static void decode_entities(char *s) {
    char *o = s;
    while (*s) {
        if (*s == '&') {
            char *e = strchr(s, ';');
            if (e && e - s <= 8) {
                size_t l = (size_t)(e - s - 1);
                if (l == 4 && !strncmp(s + 1, "quot", 4)) { *o++ = '"'; s = e + 1; continue; }
                if (l == 3 && !strncmp(s + 1, "amp", 3)) { *o++ = '&'; s = e + 1; continue; }
                if (l == 2 && !strncmp(s + 1, "lt", 2)) { *o++ = '<'; s = e + 1; continue; }
                if (l == 2 && !strncmp(s + 1, "gt", 2)) { *o++ = '>'; s = e + 1; continue; }
                if (l == 4 && !strncmp(s + 1, "nbsp", 4)) { *o++ = ' '; s = e + 1; continue; }
                if (l >= 2 && s[1] == '#') {
                    unsigned cp = s[2] == 'x' ? (unsigned)strtoul(s + 3, NULL, 16) : (unsigned)strtoul(s + 2, NULL, 10);
                    if (cp && cp < 0x80) *o++ = (char)cp;
                    else if (cp < 0x800) { *o++ = (char)(0xC0 | (cp >> 6)); *o++ = (char)(0x80 | (cp & 0x3F)); }
                    else { *o++ = (char)(0xE0 | (cp >> 12)); *o++ = (char)(0x80 | ((cp >> 6) & 0x3F)); *o++ = (char)(0x80 | (cp & 0x3F)); }
                    s = e + 1;
                    continue;
                }
            }
        }
        *o++ = *s++;
    }
    *o = 0;
}

static const char *ext_of(const char *s) {
    const char *d = strrchr(s, '.');
    return d ? d + 1 : "";
}

static const char *base_of(const char *s) {
    const char *b = strrchr(s, '/');
    return b ? b + 1 : s;
}

/* "http://www.mspaintadventures.com/a/b.gif?x" -> "/a/b.gif" */
static void url_path(const char *url, char *out, size_t n) {
    const char *p = url;
    if (!strncasecmp(p, "http://", 7)) p += 7;
    else if (!strncasecmp(p, "https://", 8)) p += 8;
    else { snprintf(out, n, "%s", *p == '/' ? p : ""); goto strip; }
    p = strchr(p, '/');
    snprintf(out, n, "%s", p ? p : "");
strip:;
    char *q = strchr(out, '?');
    if (q) *q = 0;
}

/* ------------------------------------------------------------ sources */
static int src_add(void) {
    if (nSrcs == capSrcs) {
        capSrcs = capSrcs ? capSrcs * 2 : 128;
        srcs = realloc(srcs, sizeof(Source) * (size_t)capSrcs);
    }
    memset(&srcs[nSrcs], 0, sizeof(Source));
    return nSrcs++;
}

static int src_find(const char *id) {
    for (int i = 0; i < nSrcs; i++) if (!strcmp(srcs[i].id, id)) return i;
    return -1;
}

static void cat_begin(const char *name) {
    snprintf(cats[nCats].name, sizeof cats[nCats].name, "%s", name);
    cats[nCats].first = nSrcs;
}
static void cat_end(void) {
    cats[nCats].n = nSrcs - cats[nCats].first;
    if (cats[nCats].n > 0 && nCats < 7) nCats++;
}

static void add_json_comic(const char *prefix, const char *key, const JNode *comicsObj) {
    const JNode *node = jget(comicsObj, key);
    const JNode *pages = node ? jget(node, "pages") : NULL;
    if (!node || !pages || pages->count == 0) return;
    int s = src_add();
    snprintf(srcs[s].id, sizeof srcs[s].id, "J:%s/%s", prefix, key);
    char *nm = jdup(jget(node, "name"));
    if (nm) { decode_entities(nm); snprintf(srcs[s].name, sizeof srcs[s].name, "%s", nm[0] ? nm : key); free(nm); }
    snprintf(srcs[s].tag, sizeof srcs[s].tag, "%s", prefix);
    srcs[s].kind = SK_JSON;
    srcs[s].node = node;
}

static void add_list(const char *prefix, const JNode *list, const JNode *comicsObj) {
    if (!list) return;
    for (const JNode *c = list->child; c; c = c->next) {
        if (c->t == J_STR) { char *k = jdup(c); add_json_comic(prefix, k, comicsObj); free(k); }
        else if (c->t == J_OBJ) add_list(prefix, jget(c, "list"), comicsObj);
    }
}

static void add_dir_source(const char *name, const char *dir) {
    int s = src_add();
    snprintf(srcs[s].id, sizeof srcs[s].id, "D:%s", dir);
    snprintf(srcs[s].name, sizeof srcs[s].name, "%s", name);
    snprintf(srcs[s].tag, sizeof srcs[s].tag, "%s", name);
    snprintf(srcs[s].dir, sizeof srcs[s].dir, "%s", dir);
    srcs[s].kind = SK_DIR;
}

static int src_single(const char *path) {
    char id[110];
    snprintf(id, sizeof id, "I:%s", path);
    int f = src_find(id);
    if (f >= 0) return f;
    int s = src_add();
    snprintf(srcs[s].id, sizeof srcs[s].id, "%s", id);
    snprintf(srcs[s].name, sizeof srcs[s].name, "%s", base_of(path));
    snprintf(srcs[s].tag, sizeof srcs[s].tag, "%s", base_of(path));
    snprintf(srcs[s].single, sizeof srcs[s].single, "%s", path);
    srcs[s].kind = SK_SINGLE;
    return s;
}

static void build_sources(void) {
    nSrcs = nCats = 0;
    cat_begin("Stories");
    for (int i = 0; i < NSTORIES; i++) {
        int s = src_add();
        snprintf(srcs[s].id, sizeof srcs[s].id, "S:%s", STORIES[i].name);
        snprintf(srcs[s].name, sizeof srcs[s].name, "%s", STORIES[i].name);
        snprintf(srcs[s].tag, sizeof srcs[s].tag, "%s", STORIES[i].tag);
        srcs[s].kind = SK_STORY;
        srcs[s].sect = STORIES[i].sect;
        snprintf(srcs[s].start, sizeof srcs[s].start, "%s", STORIES[i].start);
    }
    cat_end();
    if (croot) {
        const JNode *pxs = jget(croot, "pxs"), *snaps = jget(croot, "snaps"), *tso = jget(croot, "tso");
        if (pxs) { cat_begin("Paradox Space"); add_list("PXS", jget(pxs, "list"), jget(pxs, "comics")); cat_end(); }
        if (snaps) { cat_begin("Snaps"); add_list("Snaps", jget(snaps, "list"), jget(snaps, "comics")); cat_end(); }
        if (tso) { cat_begin("Hussie's old comics"); add_list("TSO", jget(tso, "list"), jget(tso, "comics")); cat_end(); }
    }
    char full[700];
    cat_begin("Sweet Bro and Hella Jeff");
    snprintf(full, sizeof full, "%s/sweetbroandhellajeff/archive", assets);
    if (dir_exists(full)) add_dir_source("Sweet Bro and Hella Jeff", "/sweetbroandhellajeff/archive");
    else {
        snprintf(full, sizeof full, "%s/sweetbroandhellajeff", assets);
        if (dir_exists(full)) add_dir_source("Sweet Bro and Hella Jeff", "/sweetbroandhellajeff");
    }
    cat_end();
    char *g = (char *)read_file("galleries.txt", NULL);
    if (g) {
        cat_begin("My galleries");
        for (char *line = strtok(g, "\n"); line; line = strtok(NULL, "\n")) {
            line[strcspn(line, "\r")] = 0;
            char *bar = strchr(line, '|');
            if (line[0] == '#' || !bar) continue;
            *bar = 0;
            add_dir_source(line, bar + 1);
        }
        cat_end();
        free(g);
    }
}

static void collect_images(const char *rel, int depth, char ***arr, int *n, int *cap) {
    char full[1100];
    snprintf(full, sizeof full, "%s%s", assets, rel);
    DIR *d = opendir(full);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char r2[300], f2[1500];
        if (snprintf(r2, sizeof r2, "%s/%s", rel, e->d_name) >= (int)sizeof r2 - 1) continue;
        snprintf(f2, sizeof f2, "%s%s", assets, r2);
        if (dir_exists(f2)) { if (depth < 3) collect_images(r2, depth + 1, arr, n, cap); continue; }
        const char *x = ext_of(e->d_name);
        if (strcasecmp(x, "gif") && strcasecmp(x, "png") && strcasecmp(x, "jpg") && strcasecmp(x, "jpeg")) continue;
        if (strlen(r2) > 250) continue;
        if (*n >= *cap) { *cap = *cap ? *cap * 2 : 256; *arr = realloc(*arr, sizeof(char *) * (size_t)*cap); }
        (*arr)[(*n)++] = strdup(r2);
    }
    closedir(d);
}

static void src_scan(int si) {
    if (srcs[si].scanned) return;
    int n = 0, cap = 0;
    char **arr = NULL;
    collect_images(srcs[si].dir, 0, &arr, &n, &cap);
    if (n > 1) qsort(arr, (size_t)n, sizeof(char *), natcmp_ptr);
    srcs[si].files = arr;
    srcs[si].nfiles = n;
    srcs[si].scanned = true;
}

static int src_count(int si) {
    switch (srcs[si].kind) {
    case SK_JSON: { const JNode *p = jget(srcs[si].node, "pages"); return p ? (int)p->count : 0; }
    case SK_DIR: src_scan(si); return srcs[si].nfiles;
    case SK_SINGLE: return 1;
    }
    return 0;
}

static bool src_has_page(int si, const char *key) {
    if (srcs[si].kind == SK_STORY) return find_page(srcs[si].sect, key) != NULL;
    for (const char *c = key; *c; c++) if (!isdigit((unsigned char)*c)) return false;
    if (!*key) return false;
    int idx = atoi(key);
    return idx >= 0 && idx < src_count(si);
}

static void src_start_key(int si, char *out, size_t n) {
    snprintf(out, n, "%s", srcs[si].kind == SK_STORY ? srcs[si].start : "0");
}

/* which story does a page id belong to? (for ?s=..&p=.. links) */
static int story_for_id(const char *id) {
    bool digits = *id != 0;
    for (const char *c = id; *c; c++) if (!isdigit((unsigned char)*c)) digits = false;
    if (!digits) return !strncmp(id, "jb2_", 4) || !strcmp(id, "pony") ? 0 : 4;
    int n = atoi(id);
    return n < 136 ? 0 : n < 219 ? 1 : n < 1893 ? 2 : n < 1901 ? 3 : 4;
}

/* ------------------------------------------------------------ data load */
static bool load_data(void) {
    char path[600];
    snprintf(path, sizeof path, "%s/archive/data/mspa.json", assets);
    size_t n = 0;
    jsonBuf = (char *)read_file(path, &n);
    if (!jsonBuf) { snprintf(loadError, sizeof loadError, "Cannot read %.200s", path); return false; }
    jroot = json_parse(jsonBuf, n);
    if (!jroot) { snprintf(loadError, sizeof loadError, "mspa.json could not be parsed"); return false; }
    for (int s = 0; s < 2; s++) {
        const JNode *o = jget(jroot, SECT_NAMES[s]);
        if (!o) { snprintf(loadError, sizeof loadError, "mspa.json has no \"%s\" section", SECT_NAMES[s]); return false; }
        sect[s].keys = malloc(sizeof(Key) * (o->count ? o->count : 1));
        sect[s].n = 0;
        for (const JNode *c = o->child; c; c = c->next) {
            sect[s].keys[sect[s].n].k = c->key;
            sect[s].keys[sect[s].n].l = c->klen;
            sect[s].keys[sect[s].n].n = c;
            sect[s].n++;
        }
        qsort(sect[s].keys, (size_t)sect[s].n, sizeof(Key), key_cmp);
    }
    snprintf(path, sizeof path, "%s/archive/data/comics.json", assets);
    comicsBuf = (char *)read_file(path, &n);
    if (comicsBuf) croot = json_parse(comicsBuf, n);   /* optional */
    build_sources();
    return true;
}

/* ------------------------------------------------------------ page load */
static bool story_load(int s, const char *id, Page *p) {
    memset(p, 0, sizeof *p);
    const JNode *n = find_page(s, id);
    if (!n) return false;
    snprintf(p->id, sizeof p->id, "%s", id);
    char *t = jdup(jget(n, "title"));
    if (t) { decode_entities(t); snprintf(p->title, sizeof p->title, "%s", t); free(t); }
    p->html = jdup(jget(n, "content"));
    const JNode *m = jget(n, "media");
    for (const JNode *c = m ? m->child : NULL; c && p->nmedia < 8; c = c->next) {
        char *s2 = jdup(c);
        if (s2) { int mi = p->nmedia++; snprintf(p->media[mi], 256, "%s", s2); free(s2); }
    }
    const JNode *nx = jget(n, "next");
    for (const JNode *c = nx ? nx->child : NULL; c && p->nnext < MAX_NEXT; c = c->next) {
        char *s2 = jdup(c);
        if (s2) { int ni = p->nnext++; snprintf(p->next[ni], 24, "%s", s2); free(s2); }
    }
    char *pv = jdup(jget(n, "previous"));
    if (pv) { snprintf(p->prev, sizeof p->prev, "%s", pv); free(pv); }
    p->valid = true;
    return true;
}

static void append(char *buf, size_t cap, size_t *o, const char *sep, const char *s) {
    if (!s || !*s || *o + 8 >= cap) return;
    int w = snprintf(buf + *o, cap - *o, "%s%s", *o ? sep : "", s);
    if (w > 0) *o += (size_t)w < cap - *o ? (size_t)w : cap - *o - 1;
}

static bool src_load_page(int si, const char *key, Page *p) {
    memset(p, 0, sizeof *p);
    if (si < 0 || si >= nSrcs) return false;
    int kind = srcs[si].kind;
    if (kind == SK_STORY) return story_load(srcs[si].sect, key, p);
    if (!src_has_page(si, key)) return false;
    int idx = atoi(key), n = src_count(si);
    snprintf(p->id, sizeof p->id, "%d", idx);
    char title[220] = "";
    if (kind == SK_JSON) {
        const JNode *node = srcs[si].node, *pages = jget(node, "pages");
        char *path = jdup(jat(pages, (uint32_t)idx));
        if (path) { snprintf(p->media[0], 256, "%s", path); free(path); p->nmedia = 1; }
        snprintf(title, sizeof title, "%s", srcs[si].name);
        char buf[4096];
        size_t o = 0;
        buf[0] = 0;
        if (idx == 0) {
            char *desc = jdup(jget(node, "desc")), *credit = jdup(jget(node, "credit"));
            append(buf, sizeof buf, &o, "<br /><br />", desc);
            if (credit && *credit) { char c2[300]; snprintf(c2, sizeof c2, "by %s", credit); append(buf, sizeof buf, &o, "<br /><br />", c2); }
            free(desc); free(credit);
        }
        const JNode *tt = jget(node, "titleText");
        if (tt) {
            char k[16];
            snprintf(k, sizeof k, "%d", idx + 1);
            char *txt = jdup(jget(tt, k));
            append(buf, sizeof buf, &o, "<br /><br />", txt);
            free(txt);
        }
        p->html = strdup(buf);
    } else if (kind == SK_DIR) {
        snprintf(p->media[0], 256, "%s", srcs[si].files[idx]);
        p->nmedia = 1;
        char b[256];
        snprintf(b, sizeof b, "%s", base_of(srcs[si].files[idx]));
        char *d = strrchr(b, '.');
        if (d) *d = 0;
        snprintf(title, sizeof title, "%s", b);
        p->html = strdup("");
    } else {
        snprintf(p->media[0], 256, "%s", srcs[si].single);
        p->nmedia = 1;
        snprintf(title, sizeof title, "%s", srcs[si].name);
        p->html = strdup("");
        n = 1;
    }
    if (n > 1) snprintf(p->title, sizeof p->title, "%.200s (%d/%d)", title, idx + 1, n);
    else snprintf(p->title, sizeof p->title, "%s", title);
    if (idx + 1 < n) { snprintf(p->next[0], 24, "%d", idx + 1); p->nnext = 1; }
    if (idx > 0) snprintf(p->prev, sizeof p->prev, "%d", idx - 1);
    p->valid = true;
    return true;
}

static void page_free(Page *p) { free(p->html); p->html = NULL; p->valid = false; }

/* ------------------------------------------------------------ bookmarks / tabs file */
static const char *bm_get(const char *sid) {
    for (int i = 0; i < nBms; i++) if (!strcmp(bms[i].sid, sid)) return bms[i].key;
    return NULL;
}

static void bm_save(void) {
    FILE *f = fopen("conf/bookmarks.txt", "w");
    if (!f) return;
    for (int i = 0; i < nBms; i++) fprintf(f, "%s\t%s\n", bms[i].sid, bms[i].key);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
}

static void bm_set(const char *sid, const char *key) {
    int i;
    for (i = 0; i < nBms; i++) if (!strcmp(bms[i].sid, sid)) break;
    if (i == nBms) { if (nBms >= MAX_BM) return; nBms++; snprintf(bms[i].sid, sizeof bms[i].sid, "%s", sid); }
    snprintf(bms[i].key, sizeof bms[i].key, "%s", key);
    bm_save();
}

static void bm_load(void) {
    char *s = (char *)read_file("conf/bookmarks.txt", NULL);
    if (!s) return;
    for (char *line = strtok(s, "\n"); line; line = strtok(NULL, "\n")) {
        char *tab = strchr(line, '\t');
        if (!tab || nBms >= MAX_BM) continue;
        *tab = 0;
        snprintf(bms[nBms].sid, sizeof bms[nBms].sid, "%s", line);
        snprintf(bms[nBms].key, sizeof bms[nBms].key, "%s", tab + 1);
        nBms++;
    }
    free(s);
}

static void save_tabs(void) {
    FILE *f = fopen("conf/tabs.txt", "w");
    if (!f) return;
    fprintf(f, "cur\t%d\n", curTab);
    for (int i = 0; i < nTabs; i++)
        fprintf(f, "tab\t%s\t%s\t%d\n", srcs[tabs[i].src].id, tabs[i].key, i == curTab ? scrollY : tabs[i].scroll);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
}

/* ------------------------------------------------------------ text layout */
static void items_clear(void) {
    for (int i = 0; i < nItems; i++) {
        if (items[i].tex) SDL_DestroyTexture(items[i].tex);
        free(items[i].text);
    }
    nItems = 0;
}

static void item_add(TTF_Font *f, const char *text, SDL_Color c, int x, int y) {
    if (!text || !*text) return;
    if (nItems == capItems) {
        int nc = capItems ? capItems * 2 : 512;
        Item *n = realloc(items, sizeof(Item) * (size_t)nc);
        if (!n) return;
        items = n;
        capItems = nc;
    }
    Item *it = &items[nItems++];
    memset(it, 0, sizeof *it);
    it->text = strdup(text);
    it->col = c;
    it->font = f;
    it->x = x;
    it->y = y;
    TTF_SizeUTF8(f, text, &it->w, &it->h);
}

typedef struct { char *w; SDL_Color c; bool glue; } Tok;
static Tok *ltoks;
static int ln, lcap, lineW;

static void line_reset(void) {
    for (int i = 0; i < ln; i++) free(ltoks[i].w);
    ln = lineW = 0;
}

static void line_emit(TTF_Font *f, int boxX, int boxW, bool center, int *y) {
    if (ln == 0) return;
    int sp = 0;
    TTF_SizeUTF8(f, " ", &sp, NULL);
    int total = lineW > boxW ? boxW : lineW;
    int x = center ? boxX + (boxW - total) / 2 : boxX;
    int i = 0;
    while (i < ln) {
        char buf[1024];
        size_t o = 0;
        SDL_Color c0 = ltoks[i].c;
        int j = i;
        while (j < ln && ltoks[j].c.r == c0.r && ltoks[j].c.g == c0.g && ltoks[j].c.b == c0.b) {
            size_t wl = strlen(ltoks[j].w);
            if (j > i && !ltoks[j].glue && o + 1 < sizeof buf) buf[o++] = ' ';
            if (o + wl >= sizeof buf) wl = sizeof buf - 1 - o;
            memcpy(buf + o, ltoks[j].w, wl);
            o += wl;
            j++;
        }
        buf[o] = 0;
        item_add(f, buf, c0, x, *y);
        int w = 0;
        TTF_SizeUTF8(f, buf, &w, NULL);
        x += w;
        if (j < ln && !ltoks[j].glue) x += sp;
        i = j;
    }
    *y += TTF_FontLineSkip(f);
    line_reset();
}

static void tok_add(TTF_Font *f, const char *word, SDL_Color c, bool glue, int boxX, int boxW, bool center, int *y) {
    int w = 0, sp = 0;
    TTF_SizeUTF8(f, word, &w, NULL);
    TTF_SizeUTF8(f, " ", &sp, NULL);
    int add = (ln > 0 && !glue) ? sp : 0;
    if (ln > 0 && lineW + add + w > boxW) {
        line_emit(f, boxX, boxW, center, y);
        add = 0;
        glue = false;
    }
    if (ln == lcap) {
        lcap = lcap ? lcap * 2 : 64;
        ltoks = realloc(ltoks, sizeof(Tok) * (size_t)lcap);
    }
    ltoks[ln].w = strdup(word);
    ltoks[ln].c = c;
    ltoks[ln].glue = glue;
    ln++;
    lineW += add + w;
}

static SDL_Color fix_color(SDL_Color c) {
    if (c.r + c.g + c.b > 560) { c.r = c.g = c.b = 90; }
    return c;
}

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* ---- links */
static bool tag_attr(const char *p, const char *e, const char *name, char *out, size_t n) {
    size_t nl = strlen(name);
    for (const char *q = p; q + nl + 1 < e; q++) {
        if ((q == p || q[-1] == ' ') && !strncasecmp(q, name, nl) && q[nl] == '=') {
            q += nl + 1;
            char quote = *q;
            const char *r;
            if (quote == '"' || quote == '\'') { q++; r = q; while (r < e && *r != quote) r++; }
            else { r = q; while (r < e && *r != ' ') r++; }
            size_t l = (size_t)(r - q);
            if (l >= n) l = n - 1;
            memcpy(out, q, l);
            out[l] = 0;
            return true;
        }
    }
    return false;
}

static void classify_link(Link *lk) {
    const char *u = lk->url;
    lk->kind = LK_WEB;
    lk->target[0] = 0;
    bool local = u[0] == '/' || u[0] == '?' || strstr(u, "mspaintadventures.com") || strstr(u, "homestuck.com");
    if (!local) return;
    const char *q = strstr(u, "?s=");
    if (q) {
        const char *pp = strstr(q, "p=");
        if (pp && isdigit((unsigned char)pp[2])) {
            int len = 0;
            while (isdigit((unsigned char)pp[2 + len]) && len < 12) len++;
            snprintf(lk->target, sizeof lk->target, "%.*s%.*s", len < 6 ? 6 - len : 0, "000000", len, pp + 2);
            lk->kind = LK_PAGE;
        } else {
            int s = atoi(q + 3);
            const char *start = s == 1 ? "000002" : s == 2 ? "000136" : s == 4 ? "000219" : s == 5 ? "001893" : s == 6 ? "001901" : NULL;
            if (start) { snprintf(lk->target, sizeof lk->target, "%s", start); lk->kind = LK_PAGE; }
        }
        return;
    }
    const char *cid = strstr(u, "cid=");
    if (cid && strstr(u, "sweetbroandhellajeff")) {
        snprintf(lk->target, sizeof lk->target, "/sweetbroandhellajeff/archive/%s", cid + 4);
        lk->target[strcspn(lk->target, "&")] = 0;
        lk->kind = LK_ASSET;
        return;
    }
    char path[300];
    url_path(u, path, sizeof path);
    if (!strncmp(path, "/tso/", 5)) { snprintf(lk->target, sizeof lk->target, "%s", path + 5); lk->kind = LK_TSO; return; }
    const char *x = ext_of(path);
    if (!strcasecmp(x, "gif") || !strcasecmp(x, "png") || !strcasecmp(x, "jpg") || !strcasecmp(x, "jpeg") ||
        !strcasecmp(x, "swf") || !strcasecmp(x, "mp4")) {
        snprintf(lk->target, sizeof lk->target, "%s", path);
        lk->kind = LK_ASSET;
    }
}

static void link_label_add(const char *w) {
    if (curLinkIdx < 0) return;
    Link *l = &links[curLinkIdx];
    size_t n = strlen(l->label);
    if (n + strlen(w) + 2 >= sizeof l->label) return;
    if (n) l->label[n++] = ' ';
    strcpy(l->label + n, w);
}

/* ---- media (defined below) */
static void media_load(Media *m);
static void place_inline(const char *url, int boxW, int *y);

/* lay out html-ish `html` into [boxX, boxX+boxW); logs (|PESTERLOG| etc.) switch to left alignment */
static void layout_html(TTF_Font *f, const char *html, SDL_Color base, int boxX, int boxW, bool centerDefault, int *y) {
    SDL_Color stack[24];
    int sp = 0;
    stack[0] = base;
    bool center = centerDefault;
    char word[512];
    size_t wl = 0;
    bool glue = false, sawSpace = true;
    int skip = TTF_FontLineSkip(f);
    const char *p = html;
    line_reset();

#define FLUSH_WORD() do { if (wl) { word[wl] = 0; tok_add(f, word, stack[sp], glue, boxX, boxW, center, y); link_label_add(word); wl = 0; } } while (0)

    while (*p) {
        unsigned char ch = (unsigned char)*p;
        if (ch == '<') {
            const char *e = strchr(p, '>');
            if (!e) break;
            FLUSH_WORD();
            const char *t = p + 1;
            bool closing = *t == '/';
            if (closing) t++;
            char name[8];
            size_t nl = 0;
            while (t < e && nl < 7 && isalnum((unsigned char)*t)) { name[nl++] = (char)tolower((unsigned char)*t); t++; }
            name[nl] = 0;
            if (!strcmp(name, "br")) {
                if (ln == 0) *y += skip; else line_emit(f, boxX, boxW, center, y);
                sawSpace = true;
            } else if (!strcmp(name, "span") && !closing) {
                SDL_Color c = stack[sp];
                for (const char *q = p; q + 6 < e; q++) {
                    if (!strncasecmp(q, "color:", 6)) {
                        q += 6;
                        while (*q == ' ') q++;
                        if (*q == '#') {
                            int d[6], k = 0;
                            q++;
                            while (k < 6 && hexv(q[k]) >= 0) { d[k] = hexv(q[k]); k++; }
                            if (k == 6) { c.r = (uint8_t)(d[0] * 16 + d[1]); c.g = (uint8_t)(d[2] * 16 + d[3]); c.b = (uint8_t)(d[4] * 16 + d[5]); }
                            else if (k == 3) { c.r = (uint8_t)(d[0] * 17); c.g = (uint8_t)(d[1] * 17); c.b = (uint8_t)(d[2] * 17); }
                        }
                        break;
                    }
                }
                if (sp < 23) stack[++sp] = fix_color(c); else stack[sp] = fix_color(c);
            } else if (!strcmp(name, "a") && !closing) {
                if (g_links && nLinks < MAX_LINKS) {
                    Link *lk = &links[nLinks];
                    memset(lk, 0, sizeof *lk);
                    if (tag_attr(p, e, "href", lk->url, sizeof lk->url)) {
                        decode_entities(lk->url);
                        classify_link(lk);
                        curLinkIdx = nLinks++;
                    }
                }
                if (sp < 23) stack[++sp] = C_LINK;
            } else if ((!strcmp(name, "span") || !strcmp(name, "a")) && closing) {
                if (!strcmp(name, "a")) curLinkIdx = -1;
                if (sp > 0) sp--;
            } else if (!strcmp(name, "img") && !closing && g_inl) {
                char src[300];
                if (tag_attr(p, e, "src", src, sizeof src)) {
                    if (ln) line_emit(f, boxX, boxW, center, y);
                    place_inline(src, boxW, y);
                }
            }
            p = e + 1;
            glue = !sawSpace;
            continue;
        }
        if (ch == '|') { /* |PESTERLOG| style marker */
            const char *q = p + 1;
            while ((*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || *q == '_') q++;
            if (*q == '|' && q - p >= 5) {
                FLUSH_WORD();
                if (ln) line_emit(f, boxX, boxW, center, y);
                center = false;
                p = q + 1;
                sawSpace = true;
                continue;
            }
        }
        if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') {
            FLUSH_WORD();
            sawSpace = true;
            glue = false;
            p++;
            continue;
        }
        if (wl == 0) glue = !sawSpace;
        sawSpace = false;
        if (ch == '&') {
            char tmp[16];
            const char *e = strchr(p, ';');
            if (e && e - p < 10) {
                size_t l = (size_t)(e - p + 1);
                memcpy(tmp, p, l);
                tmp[l] = 0;
                decode_entities(tmp);
                size_t tl = strlen(tmp);
                if (wl + tl < sizeof word - 1) { memcpy(word + wl, tmp, tl); wl += tl; }
                p = e + 1;
                continue;
            }
        }
        if (wl < sizeof word - 2) word[wl++] = *p;
        p++;
    }
    FLUSH_WORD();
    line_emit(f, boxX, boxW, center, y);
    curLinkIdx = -1;
#undef FLUSH_WORD
}

/* ------------------------------------------------------------- media */
static void media_free(void) {
    for (int i = 0; i < nMedia; i++) {
        if (media[i].tex) SDL_DestroyTexture(media[i].tex);
        gif_close(media[i].gif);
    }
    memset(media, 0, sizeof media);
    nMedia = 0;
}

static SDL_Texture *tex_from_rgba(const uint8_t *px, int w, int h) {
    if (w > MAX_TEX || h > MAX_TEX) return NULL;
    SDL_Texture *t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!t) return NULL;
    SDL_UpdateTexture(t, NULL, px, w * 4);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
    return t;
}

static bool is_video_ext(const char *x) {
    return !strcasecmp(x, "mp4") || !strcasecmp(x, "webm") || !strcasecmp(x, "mkv") || !strcasecmp(x, "avi") || !strcasecmp(x, "mov");
}

static void media_load(Media *m) {
    const char *ext = ext_of(m->rel);
    if (!strcasecmp(ext, "swf") || is_video_ext(ext)) {
        m->kind = strcasecmp(ext, "swf") ? 3 : 1;
        if (!resolve_path(m->rel, m->path, sizeof m->path)) {
            m->kind = 2;
            snprintf(m->label, sizeof m->label, "[ File missing ]<br />%s", m->rel);
        }
        return;
    }
    if (!strcasecmp(ext, "gif") || !strcasecmp(ext, "png") || !strcasecmp(ext, "jpg") || !strcasecmp(ext, "jpeg")) {
        if (!resolve_path(m->rel, m->path, sizeof m->path)) { m->kind = 2; snprintf(m->label, sizeof m->label, "[ Image missing ]<br />%s", m->rel); return; }
        size_t n = 0;
        uint8_t *buf = read_file(m->path, &n);
        if (!buf) { m->kind = 2; snprintf(m->label, sizeof m->label, "[ Cannot read image ]"); return; }
        if (!strcasecmp(ext, "gif")) {
            Gif *g = gif_open_mem(buf, n);
            if (g && gif_next(g)) {
                m->w = g->w; m->h = g->h;
                m->tex = tex_from_rgba(g->canvas, g->w, g->h);
                if (m->tex && g->animated) { m->gif = g; m->nextTick = SDL_GetTicks() + (uint32_t)g->delay_ms; }
                else gif_close(g);
            } else gif_close(g);
        } else {
            int w, h, c;
            uint8_t *px = stbi_load_from_memory(buf, (int)n, &w, &h, &c, 4);
            free(buf);
            if (px) { m->w = w; m->h = h; m->tex = tex_from_rgba(px, w, h); stbi_image_free(px); }
        }
        if (!m->tex) { m->kind = 2; snprintf(m->label, sizeof m->label, "[ Image too large or unreadable ]<br />%s", m->rel); }
        return;
    }
    m->kind = 2;
    snprintf(m->label, sizeof m->label, "[ %s content ]<br />Not playable on this device", ext[0] ? ext : "unknown");
}

static bool has_kind(int k) {
    for (int i = 0; i < nMedia; i++) if (media[i].kind == k) return true;
    return false;
}

static void read_hint(const char *swfPath, char *out, size_t n) {
    out[0] = 0;
    char base[128], path[300];
    snprintf(base, sizeof base, "%s", base_of(swfPath));
    char *d = strrchr(base, '.');
    if (d) *d = 0;
    snprintf(path, sizeof path, "ruffle/keymap/%s.hint", base);
    char *s = (char *)read_file(path, NULL);
    if (!s) return;
    for (char *q = s; *q; q++) if (*q == '\n' || *q == '\r') *q = ' ';
    snprintf(out, n, "%s", s);
    free(s);
}

/* inline <img>: only shown if the file exists in the Asset Pack and is a GIF/PNG/JPEG */
static void place_inline(const char *url, int boxW, int *y) {
    if (nMedia >= MAX_MEDIA) return;
    char path[300];
    url_path(url, path, sizeof path);
    if (!path[0]) return;
    const char *x = ext_of(path);
    if (strcasecmp(x, "gif") && strcasecmp(x, "png") && strcasecmp(x, "jpg") && strcasecmp(x, "jpeg")) return;
    Media *m = &media[nMedia];
    memset(m, 0, sizeof *m);
    snprintf(m->rel, sizeof m->rel, "%s", path);
    media_load(m);
    if (m->kind != 0 || !m->tex) {
        if (m->tex) SDL_DestroyTexture(m->tex);
        gif_close(m->gif);
        memset(m, 0, sizeof *m);
        return;
    }
    float s = m->w > boxW ? (float)boxW / (float)m->w : 1.0f;
    m->dw = (int)((float)m->w * s + 0.5f);
    m->dh = (int)((float)m->h * s + 0.5f);
    m->y = *y + 4;
    *y += m->dh + 10;
    nMedia++;
}

/* ------------------------------------------------------------- page build */
static void build_page(void) {
    items_clear();
    nLinks = 0;
    curLinkIdx = -1;
    int y = COL_TOP + 12;
    int bx = PAD, bw = COL_W - 2 * PAD;
    int mainMedia = nMedia;

    g_links = g_inl = false;
    layout_html(fTitle, page.title[0] ? page.title : "...", C_BLACK, bx, bw, true, &y);
    y += 10;

    for (int i = 0; i < mainMedia; i++) {
        Media *m = &media[i];
        m->y = y;
        if (m->kind == 0) {
            float s = m->w > COL_W ? (float)COL_W / (float)m->w : 1.0f;
            m->dw = (int)((float)m->w * s + 0.5f);
            m->dh = (int)((float)m->h * s + 0.5f);
            y += m->dh + 2;
        } else {
            int cy = y + 12;
            char txt[900];
            if (m->kind == 1) {
                char hint[200];
                read_hint(m->path, hint, sizeof hint);
                snprintf(txt, sizeof txt, "[ Flash animation ]<br />Press Y to play (Ruffle)<br />Select+Start exits the flash%s%s", hint[0] ? "<br />" : "", hint);
            } else if (m->kind == 3) {
                char conv[700];
                snprintf(conv, sizeof conv, "%s", m->path);
                char *d = strrchr(conv, '.');
                if (d) *d = 0;
                strncat(conv, ".r36s.mp4", sizeof conv - strlen(conv) - 1);
                snprintf(txt, sizeof txt, "[ Video ]<br />Press Y to play<br />%s", file_exists(conv) ? "(converted copy found)" : "(run convert_videos.ps1 on a PC for a handheld-friendly copy)");
            } else {
                snprintf(txt, sizeof txt, "%s", m->label);
            }
            layout_html(fBody, txt, C_CARDTXT, bx + 10, bw - 20, true, &cy);
            m->dw = COL_W;
            m->dh = cy + 12 - y;
            y = cy + 14;
        }
    }

    y += 14;
    if (page.html && page.html[0]) {
        g_links = g_inl = true;
        layout_html(fBody, page.html, C_BLACK, bx, bw, true, &y);
        g_links = g_inl = false;
        y += 6;
    }

    /* commands */
    int nChoice = page.nnext;
    for (int i = 0; i < nChoice; i++) {
        choiceText[i][0] = 0;
        if (srcs[CT.src].kind == SK_STORY) {
            Page tmp;
            if (story_load(srcs[CT.src].sect, page.next[i], &tmp)) {
                snprintf(choiceText[i], sizeof choiceText[i], "%s", tmp.title);
                page_free(&tmp);
            }
        } else snprintf(choiceText[i], sizeof choiceText[i], "==>");
    }
    if (selChoice >= nChoice) selChoice = 0;
    y += 8;
    for (int i = 0; i < nChoice; i++) {
        bool arrow = strstr(choiceText[i], "=>") != NULL;
        choiceTop[i] = y;
        int pw = 0;
        if (!arrow) { TTF_SizeUTF8(fCmd, "> ", &pw, NULL); item_add(fCmd, ">", C_BLACK, bx, y); }
        int cy = y;
        layout_html(fLink, choiceText[i][0] ? choiceText[i] : "...", C_LINK, bx + pw, bw - pw, false, &cy);
        y = cy + 10;
        choiceBot[i] = y - 6;
    }

    /* footer */
    y += 18;
    int fs = TTF_FontLineSkip(fFoot);
    const char *fl[6];
    int nf = 0;
    fl[nf++] = "Go Back (B)";
    fl[nf++] = "Menu (X)";
    if (has_kind(1)) fl[nf++] = "Play Flash (Y)";
    if (has_kind(3)) fl[nf++] = "Play Video (Y)";
    if (nLinks > 0) fl[nf++] = "Links (Select)";
    fl[nf++] = "Tabs (L2/R2)";
    int fx = bx;
    for (int i = 0; i < nf; i++) {
        int w = 0, sw = 0;
        TTF_SizeUTF8(fFootU, fl[i], &w, NULL);
        TTF_SizeUTF8(fFoot, " | ", &sw, NULL);
        if (fx + w > bx + bw && fx > bx) { fx = bx; y += fs + 2; }
        item_add(fFootU, fl[i], C_LINK, fx, y);
        fx += w;
        if (i + 1 < nf) { item_add(fFoot, " | ", C_BLACK, fx, y); fx += sw; }
    }
    y += fs + 6;
    const char *r1 = "Auto-Saved!", *r2 = " | L1/R1: 10 pages";
    int w1 = 0, w2 = 0;
    TTF_SizeUTF8(fFootU, r1, &w1, NULL);
    TTF_SizeUTF8(fFoot, r2, &w2, NULL);
    item_add(fFootU, r1, C_LINK, bx + bw - w1 - w2, y);
    item_add(fFoot, r2, C_BLACK, bx + bw - w2, y);
    y += fs + 16;
    pageH = y + COL_TOP;
}

/* ------------------------------------------------------------- video / window */
static bool video_init(void) {
    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) return false;
    win = SDL_CreateWindow("MSPA", 0, 0, SCR_W, SCR_H, SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (!win) return false;
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) return false;
    SDL_RenderSetLogicalSize(ren, SCR_W, SCR_H);
    SDL_ShowCursor(SDL_DISABLE);
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i)) SDL_GameControllerOpen(i);
    return true;
}

static void video_shutdown(void) {
    items_clear();
    media_free();
    if (ren) SDL_DestroyRenderer(ren);
    if (win) SDL_DestroyWindow(win);
    ren = NULL;
    win = NULL;
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER);
}

/* ------------------------------------------------------------- page flow */
static int max_scroll(void) { return pageH > VIEW_H ? pageH - VIEW_H : 0; }

static void open_page(const char *key, bool push) {
    Page np;
    if (!src_load_page(CT.src, key, &np)) { toast("Page not found: %.60s", key); return; }
    media_free();
    if (push && havePage) {
        if (CT.nHist >= HIST_MAX) { memmove(CT.hist[0], CT.hist[1], (size_t)(HIST_MAX - 1) * 24); CT.nHist = HIST_MAX - 1; }
        { int hi = CT.nHist++; snprintf(CT.hist[hi], 24, "%s", page.id); }
    }
    page_free(&page);
    page = np;
    havePage = true;
    nMedia = page.nmedia;
    for (int i = 0; i < nMedia; i++) {
        snprintf(media[i].rel, sizeof media[i].rel, "%s", page.media[i]);
        media_load(&media[i]);
    }
    scrollY = 0;
    selChoice = 0;
    overlay = 0;
    build_page();
    snprintf(CT.key, sizeof CT.key, "%s", page.id);
    if (srcs[CT.src].kind != SK_SINGLE) bm_set(srcs[CT.src].id, page.id);
    save_tabs();
}

static void go_next(void) {
    if (!havePage || page.nnext == 0) return;
    char k[24];
    snprintf(k, sizeof k, "%s", page.next[selChoice]);
    open_page(k, true);
}

static void go_prev(void) {
    if (!havePage) return;
    char k[24];
    if (CT.nHist > 0) { snprintf(k, sizeof k, "%s", CT.hist[--CT.nHist]); open_page(k, false); return; }
    if (page.prev[0]) { snprintf(k, sizeof k, "%s", page.prev); open_page(k, false); }
}

static void jump(int steps) {
    if (!havePage) return;
    char id[24];
    snprintf(id, sizeof id, "%s", page.id);
    if (srcs[CT.src].kind != SK_STORY) {
        int n = src_count(CT.src), idx = atoi(id) + steps;
        if (idx < 0) idx = 0;
        if (idx >= n) idx = n - 1;
        snprintf(id, sizeof id, "%d", idx);
    } else {
        for (int i = 0; i < (steps < 0 ? -steps : steps); i++) {
            Page t;
            memset(&t, 0, sizeof t);
            if (!story_load(srcs[CT.src].sect, id, &t)) break;
            char tmp[24];
            snprintf(tmp, sizeof tmp, "%s", steps > 0 ? (t.nnext ? t.next[0] : "") : t.prev);
            page_free(&t);
            if (!tmp[0]) break;
            snprintf(id, sizeof id, "%s", tmp);
        }
    }
    if (strcmp(id, page.id) != 0) open_page(id, true);
}

/* ------------------------------------------------------------- tabs */
static void tab_new(int src, const char *keyIn) {
    if (nTabs >= MAX_TABS) { toast("Tab limit (%d). Close one: menu (X), then Select", MAX_TABS); return; }
    char key[24];
    snprintf(key, sizeof key, "%s", keyIn);
    if (!src_has_page(src, key)) src_start_key(src, key, sizeof key);
    if (!src_has_page(src, key)) { toast("Nothing to open in \"%.60s\"", srcs[src].name); return; }
    if (nTabs > 0) CT.scroll = scrollY;
    int prevTab = curTab, prevN = nTabs;
    curTab = nTabs++;
    memset(&tabs[curTab], 0, sizeof(Tab));
    tabs[curTab].src = src;
    snprintf(tabs[curTab].key, sizeof tabs[curTab].key, "%s", key);
    bool hadPage = havePage;
    havePage = false;
    menuMode = false;
    overlay = 0;
    open_page(key, false);
    if (!havePage) {              /* failed: roll back */
        nTabs = prevN;
        curTab = prevTab;
        havePage = hadPage;
        if (!nTabs) menuMode = true;
    }
}

static void tab_show(int i) {
    curTab = i;
    havePage = false;
    menuMode = false;
    overlay = 0;
    int sc = CT.scroll;
    char k[24];
    snprintf(k, sizeof k, "%s", CT.key);
    open_page(k, false);
    scrollY = sc > max_scroll() ? max_scroll() : sc;
    save_tabs();
}

static void tab_switch(int dir) {
    if (nTabs < 2) { toast("Only one tab open"); return; }
    CT.scroll = scrollY;
    tab_show((curTab + dir + nTabs) % nTabs);
}

static void tab_close_current(void) {
    if (nTabs < 2) { toast("Can't close the last tab"); return; }
    for (int i = curTab; i + 1 < nTabs; i++) tabs[i] = tabs[i + 1];
    nTabs--;
    if (curTab >= nTabs) curTab = nTabs - 1;
    toast("Tab closed");
    tab_show(curTab);
}

static void goto_here(int src, const char *keyIn) {
    if (nTabs == 0) { tab_new(src, keyIn); return; }
    Page t;
    if (!src_load_page(src, keyIn, &t)) { toast("Page not found: %.60s", keyIn); return; }
    page_free(&t);
    char key[24];
    snprintf(key, sizeof key, "%s", keyIn);
    bool same = src == CT.src;
    if (!same) { CT.nHist = 0; CT.src = src; }
    menuMode = false;
    overlay = 0;
    open_page(key, same);
}

static void lib_open(int src, bool newTab) {
    char key[24];
    const char *bm = bm_get(srcs[src].id);
    if (bm && src_has_page(src, bm)) snprintf(key, sizeof key, "%s", bm);
    else src_start_key(src, key, sizeof key);
    if (newTab || nTabs == 0) tab_new(src, key); else goto_here(src, key);
}

static void navigate(int src, const char *key, bool newTab) {
    if (newTab) tab_new(src, key); else goto_here(src, key);
}

static void open_link(int i, bool newTab) {
    Link *lk = &links[i];
    char target[260];
    snprintf(target, sizeof target, "%s", lk->target);
    switch (lk->kind) {
    case LK_PAGE: {
        int s = story_for_id(target);
        if (!find_page(srcs[s].sect, target)) { toast("Page %s isn't in the story data", target); return; }
        navigate(s, target, newTab);
        break;
    }
    case LK_TSO: {
        char id[110];
        snprintf(id, sizeof id, "J:TSO/%s", target);
        int s = src_find(id);
        if (s < 0) { toast("Comic \"%.60s\" isn't in your Asset Pack list", target); return; }
        navigate(s, "0", newTab);
        break;
    }
    case LK_ASSET: {
        char res[640];
        if (!resolve_path(target, res, sizeof res)) {
            const char *a = strstr(target, "/archive/");
            char alt[260];
            if (a && !strncmp(target, "/sweetbroandhellajeff", 21)) {
                snprintf(alt, sizeof alt, "/sweetbroandhellajeff/%s", base_of(target));
                if (resolve_path(alt, res, sizeof res)) snprintf(target, sizeof target, "%s", alt);
                else { toast("File not found: %.100s", lk->target); return; }
            } else { toast("File not found: %.100s", lk->target); return; }
        }
        navigate(src_single(target), "0", newTab);
        break;
    }
    default:
        toast("Web link, can't open here: %.120s", lk->url);
    }
}

static void play_external(int kind) {
    const char *path = NULL;
    for (int i = 0; i < nMedia; i++) if (media[i].kind == kind) { path = media[i].path; break; }
    if (!path) return;
    char p[640], key[24];
    snprintf(p, sizeof p, "%s", path);
    snprintf(key, sizeof key, "%s", page.id);
    int sc = scrollY;
    CT.scroll = scrollY;
    save_tabs();
    video_shutdown();
    setenv(kind == 1 ? "MSPA_SWF" : "MSPA_VIDEO", p, 1);
    int rc = system(kind == 1 ? "bash ./run_flash.sh" : "bash ./run_video.sh");
    dpadDir = stickDir = 0;
    video_init();
    open_page(key, false);
    scrollY = sc > max_scroll() ? max_scroll() : sc;
    if (kind == 3 && WIFEXITED(rc) && WEXITSTATUS(rc) == 2) toast("No video player found on this system");
}

static void restore_tabs(void) {
    char *s = (char *)read_file("conf/tabs.txt", NULL);
    if (!s) return;
    int want = 0;
    for (char *line = strtok(s, "\n"); line; line = strtok(NULL, "\n")) {
        if (!strncmp(line, "cur\t", 4)) { want = atoi(line + 4); continue; }
        if (strncmp(line, "tab\t", 4) != 0 || nTabs >= MAX_TABS) continue;
        char *id = line + 4, *k = strchr(id, '\t');
        if (!k) continue;
        *k++ = 0;
        char *sc = strchr(k, '\t');
        if (!sc) continue;
        *sc++ = 0;
        int src = src_find(id);
        if (src < 0 && !strncmp(id, "I:", 2)) src = src_single(id + 2);
        if (src < 0) continue;
        char key[24];
        snprintf(key, sizeof key, "%s", k);
        if (!src_has_page(src, key)) { src_start_key(src, key, sizeof key); if (!src_has_page(src, key)) continue; }
        memset(&tabs[nTabs], 0, sizeof(Tab));
        tabs[nTabs].src = src;
        snprintf(tabs[nTabs].key, sizeof tabs[nTabs].key, "%s", key);
        tabs[nTabs].scroll = atoi(sc);
        nTabs++;
    }
    free(s);
    if (nTabs) {
        if (want < 0 || want >= nTabs) want = 0;
        tab_show(want);
    }
}

/* ------------------------------------------------------------- rendering */
static void fill(SDL_Color c, int x, int y, int w, int h) {
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, 255);
    SDL_Rect r = {x, y, w, h};
    SDL_RenderFillRect(ren, &r);
}

static void fill_a(SDL_Color c, int a, int x, int y, int w, int h) {
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, (uint8_t)a);
    SDL_Rect r = {x, y, w, h};
    SDL_RenderFillRect(ren, &r);
}

static void draw_str(TTF_Font *f, const char *s, int x, int y, SDL_Color c) {
    if (!s || !*s) return;
    SDL_Surface *surf = TTF_RenderUTF8_Blended(f, s, c);
    if (!surf) return;
    SDL_Texture *t = SDL_CreateTextureFromSurface(ren, surf);
    SDL_Rect r = {x, y, surf->w, surf->h};
    SDL_RenderCopy(ren, t, NULL, &r);
    SDL_DestroyTexture(t);
    SDL_FreeSurface(surf);
}

static void draw_center(TTF_Font *f, const char *s, int y, SDL_Color c) {
    int w = 0;
    TTF_SizeUTF8(f, s, &w, NULL);
    draw_str(f, s, (SCR_W - w) / 2, y, c);
}

/* shorten `in` with ".." until it fits maxw pixels */
static void fit_text(TTF_Font *f, const char *in, int maxw, char *out, size_t n) {
    snprintf(out, n, "%s", in);
    int w = 0;
    TTF_SizeUTF8(f, out, &w, NULL);
    while (w > maxw && strlen(out) > 3) {
        size_t l = strlen(out) - 1;
        while (l > 0 && ((unsigned char)out[l] & 0xC0) == 0x80) l--;
        out[l] = 0;
        char tmp[320];
        snprintf(tmp, sizeof tmp, "%s..", out);
        TTF_SizeUTF8(f, tmp, &w, NULL);
        if (w <= maxw) { snprintf(out, n, "%s", tmp); return; }
    }
}

static void tab_label(int i, char *out, size_t n) {
    const Source *s = &srcs[tabs[i].src];
    if (s->kind == SK_STORY) {
        const char *k = tabs[i].key;
        while (*k == '0' && k[1]) k++;
        snprintf(out, n, "%s %s", s->tag, k);
    } else if (s->kind == SK_SINGLE) snprintf(out, n, "%s", s->name);
    else snprintf(out, n, "%s %d", s->name, atoi(tabs[i].key) + 1);
}

static void draw_tabbar(void) {
    fill(C_TABBG, 0, 0, SCR_W, TB);
    if (nTabs == 0) { draw_str(fFoot, "MSPA Reader", 8, 4, C_CARDTXT); return; }
    int w = SCR_W / nTabs;
    for (int i = 0; i < nTabs; i++) {
        bool sel = i == curTab && !menuMode;
        fill(sel ? C_COL : C_TABOFF, i * w + 1, 2, w - 2, TB - 2);
        char lab[160], fit[200], num[8];
        tab_label(i, lab, sizeof lab);
        snprintf(num, sizeof num, "%d ", i + 1);
        char full[200];
        snprintf(full, sizeof full, "%s%s", num, lab);
        fit_text(fFoot, full, w - 12, fit, sizeof fit);
        draw_str(fFoot, fit, i * w + 6, 5, sel ? C_BLACK : (SDL_Color){215, 215, 215, 255});
    }
}

static int cat_of_src(int s) {
    for (int c = 0; c < nCats; c++) if (s >= cats[c].first && s < cats[c].first + cats[c].n) return c;
    return -1;
}

static void draw_library(void) {
    fill(C_BG, 0, 0, SCR_W, SCR_H);
    fill(C_PANEL, PNL_X, TB, PNL_W, VIEW_H);
    fill(C_COL, COL_X, TB + COL_TOP, COL_W, VIEW_H - COL_TOP);
    int top = TB + COL_TOP;
    if (loadError[0]) {
        draw_center(fTitle, "MSPA Reader", top + 14, C_BLACK);
        draw_center(fFoot, "Could not load the Asset Pack:", top + 70, C_BLACK);
        draw_center(fFoot, loadError, top + 92, C_GREY);
        draw_center(fFoot, "Put the Asset Pack folder (the one containing 'archive') at:", top + 130, C_BLACK);
        draw_center(fFoot, "<port folder>/assets   or write its path into assetpack.txt", top + 152, C_GREY);
        draw_center(fFoot, "Start: quit", SCR_H - 30, C_GREY);
        return;
    }
    char head[120];
    snprintf(head, sizeof head, "%s", libLevel == 0 ? "MSPA Reader" : cats[selCat].name);
    draw_center(fTitle, head, top + 10, C_BLACK);
    int line = TTF_FontLineSkip(fMenu) + 16;
    if (libLevel == 0) {
        for (int i = 0; i < nCats; i++) {
            int y = top + 54 + i * line;
            bool sel = i == selCat;
            if (sel) fill(C_HILITE, COL_X + 6, y - 4, COL_W - 12, line - 2);
            draw_str(fMenu, sel ? "> " : "  ", COL_X + PAD, y, C_BLACK);
            draw_str(sel ? fLink : fMenu, cats[i].name, COL_X + PAD + 26, y, sel ? C_LINK : C_BLACK);
            char b[48];
            snprintf(b, sizeof b, "%d %s", cats[i].n, cats[i].n == 1 ? "entry" : "entries");
            draw_str(fFoot, b, COL_X + PAD + 26, y + TTF_FontLineSkip(fMenu) - 2, C_GREY);
        }
        draw_center(fFoot, "A: open  |  B: back to page  |  Select: close tab  |  Start: quit", SCR_H - 28, C_GREY);
    } else {
        int rows = (VIEW_H - 100) / line;
        if (rows < 3) rows = 3;
        int first = selEnt - rows / 2;
        if (first + rows > cats[selCat].n) first = cats[selCat].n - rows;
        if (first < 0) first = 0;
        for (int r = 0; r < rows && first + r < cats[selCat].n; r++) {
            int e = first + r, si = cats[selCat].first + e, y = top + 50 + r * line;
            bool sel = e == selEnt;
            if (sel) fill(C_HILITE, COL_X + 6, y - 4, COL_W - 12, line - 2);
            char nm[200];
            fit_text(sel ? fLink : fMenu, srcs[si].name, COL_W - 2 * PAD - 30, nm, sizeof nm);
            draw_str(fMenu, sel ? "> " : "  ", COL_X + PAD, y, C_BLACK);
            draw_str(sel ? fLink : fMenu, nm, COL_X + PAD + 26, y, sel ? C_LINK : C_BLACK);
            const char *bm = bm_get(srcs[si].id);
            char b[80];
            if (bm && srcs[si].kind == SK_STORY) snprintf(b, sizeof b, "resume: page %s", bm);
            else if (bm) snprintf(b, sizeof b, "resume: page %d", atoi(bm) + 1);
            else snprintf(b, sizeof b, "not started");
            draw_str(fFoot, b, COL_X + PAD + 26, y + TTF_FontLineSkip(fMenu) - 2, C_GREY);
        }
        draw_center(fFoot, "A: open here | Y: new tab | B: back | L1/R1: page | Select: close tab", SCR_H - 28, C_GREY);
    }
}

static void draw_page(void) {
    int ms = max_scroll();
    if (scrollY > ms) scrollY = ms;
    if (scrollY < 0) scrollY = 0;
    int oy = TB - scrollY;
    fill(C_BG, 0, 0, SCR_W, SCR_H);
    fill(C_PANEL, PNL_X, TB, PNL_W, VIEW_H);
    fill(C_COL, COL_X, COL_TOP + oy, COL_W, pageH - COL_TOP);

    for (int i = 0; i < nMedia; i++) {
        Media *m = &media[i];
        int sy = m->y + oy;
        if (sy + m->dh < TB || sy > SCR_H) continue;
        if (m->kind == 0 && m->tex) {
            SDL_Rect dst = {COL_X + (COL_W - m->dw) / 2, sy, m->dw, m->dh};
            SDL_RenderCopy(ren, m->tex, NULL, &dst);
        } else if (m->kind != 0) {
            fill(C_CARD, COL_X, sy, COL_W, m->dh);
        }
    }
    if (page.nnext > 1) {
        int t = choiceTop[selChoice] + oy, h = choiceBot[selChoice] - choiceTop[selChoice];
        fill(C_HILITE, COL_X + 6, t - 3, COL_W - 12, h + 6);
    }
    int lo = scrollY - 6, hi = scrollY + VIEW_H + 6;
    for (int i = 0; i < nItems; i++) {
        Item *it = &items[i];
        if (it->y + it->h < lo || it->y > hi) {
            if (it->tex && (it->y + it->h < scrollY - 1500 || it->y > scrollY + VIEW_H + 1500)) { SDL_DestroyTexture(it->tex); it->tex = NULL; }
            continue;
        }
        if (!it->tex) {
            SDL_Surface *s = TTF_RenderUTF8_Blended(it->font, it->text, it->col);
            if (!s) continue;
            it->tex = SDL_CreateTextureFromSurface(ren, s);
            SDL_FreeSurface(s);
            if (!it->tex) continue;
        }
        SDL_Rect r = {COL_X + it->x, it->y + oy, it->w, it->h};
        SDL_RenderCopy(ren, it->tex, NULL, &r);
    }
}

static const char *link_tag(const Link *l) {
    if (l->kind == LK_PAGE) return "PAGE";
    if (l->kind == LK_TSO) return "COMIC";
    if (l->kind == LK_WEB) return "WEB";
    const char *x = ext_of(l->target);
    if (!strcasecmp(x, "swf")) return "FLASH";
    if (is_video_ext(x)) return "VIDEO";
    return "IMAGE";
}

static void draw_links(void) {
    fill_a((SDL_Color){0, 0, 0, 255}, 170, 0, TB, SCR_W, VIEW_H);
    int px = 24, py = TB + 12, pw = SCR_W - 48, ph = VIEW_H - 24;
    fill(C_COL, px, py, pw, ph);
    char head[80];
    snprintf(head, sizeof head, "Links on this page (%d)", nLinks);
    draw_str(fBody, head, px + 12, py + 8, C_BLACK);
    int line = 28, rows = (ph - 100) / line;
    int first = selLink - rows / 2;
    if (first + rows > nLinks) first = nLinks - rows;
    if (first < 0) first = 0;
    for (int r = 0; r < rows && first + r < nLinks; r++) {
        int i = first + r, y = py + 40 + r * line;
        bool sel = i == selLink;
        if (sel) fill(C_HILITE, px + 4, y - 3, pw - 8, line - 2);
        char text[260], fit[300];
        const Link *l = &links[i];
        snprintf(text, sizeof text, "[%s] %s", link_tag(l), l->label[0] ? l->label : (l->target[0] ? l->target : l->url));
        fit_text(fBody, text, pw - 28, fit, sizeof fit);
        draw_str(fBody, fit, px + 12, y, l->kind == LK_WEB ? C_GREY : (sel ? C_LINK : C_BLACK));
    }
    char url[320];
    fit_text(fFoot, links[selLink].url, pw - 24, url, sizeof url);
    draw_str(fFoot, url, px + 12, py + ph - 52, C_GREY);
    draw_str(fFoot, links[selLink].kind == LK_WEB ? "Web links can't be opened on this device   |   B: close"
                                                  : "A: open in new tab   |   Y: open in this tab   |   B: close",
             px + 12, py + ph - 28, C_BLACK);
}

static void draw_toast(void) {
    if (SDL_GetTicks() >= toastUntil || !toastMsg[0]) return;
    char fit[220];
    fit_text(fFoot, toastMsg, SCR_W - 60, fit, sizeof fit);
    int w = 0;
    TTF_SizeUTF8(fFoot, fit, &w, NULL);
    fill_a((SDL_Color){20, 20, 20, 255}, 225, (SCR_W - w) / 2 - 12, SCR_H - 44, w + 24, 28);
    draw_str(fFoot, fit, (SCR_W - w) / 2, SCR_H - 38, C_CARDTXT);
}

static void animate(bool *dirty) {
    uint32_t now = SDL_GetTicks();
    for (int i = 0; i < nMedia; i++) {
        Media *m = &media[i];
        if (!m->gif || now < m->nextTick) continue;
        int sy = m->y + TB - scrollY;
        if (sy + m->dh < TB || sy > SCR_H) { m->nextTick = now + 100; continue; }
        if (gif_next(m->gif)) {
            SDL_UpdateTexture(m->tex, NULL, m->gif->canvas, m->w * 4);
            m->nextTick = now + (uint32_t)m->gif->delay_ms;
            *dirty = true;
        }
    }
}

/* ------------------------------------------------------------- main */
static TTF_Font *open_font(const char *path, int size, int style) {
    TTF_Font *f = TTF_OpenFont(path, size);
    if (f && style) TTF_SetFontStyle(f, style);
    return f;
}

static void open_library(void) {
    save_tabs();
    menuMode = true;
    overlay = 0;
    if (nTabs > 0) {
        int c = cat_of_src(CT.src);
        if (c >= 0) { selCat = c; selEnt = CT.src - cats[c].first; libLevel = 1; return; }
    }
    libLevel = 0;
}

static void handle_library(int b) {
    if (b == SDL_CONTROLLER_BUTTON_BACK) { if (nTabs > 0) tab_close_current(); return; }
    if (loadError[0]) return;
    if (libLevel == 0) {
        if (b == SDL_CONTROLLER_BUTTON_DPAD_UP && selCat > 0) selCat--;
        if (b == SDL_CONTROLLER_BUTTON_DPAD_DOWN && selCat + 1 < nCats) selCat++;
        if (b == SDL_CONTROLLER_BUTTON_A) { libLevel = 1; selEnt = 0; }
        if ((b == SDL_CONTROLLER_BUTTON_B || b == SDL_CONTROLLER_BUTTON_X) && nTabs > 0) menuMode = false;
    } else {
        int n = cats[selCat].n;
        if (b == SDL_CONTROLLER_BUTTON_DPAD_UP && selEnt > 0) selEnt--;
        if (b == SDL_CONTROLLER_BUTTON_DPAD_DOWN && selEnt + 1 < n) selEnt++;
        if (b == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) selEnt = selEnt > 7 ? selEnt - 8 : 0;
        if (b == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) selEnt = selEnt + 8 < n ? selEnt + 8 : n - 1;
        if (b == SDL_CONTROLLER_BUTTON_A) lib_open(cats[selCat].first + selEnt, false);
        if (b == SDL_CONTROLLER_BUTTON_Y) lib_open(cats[selCat].first + selEnt, true);
        if (b == SDL_CONTROLLER_BUTTON_B) libLevel = 0;
        if (b == SDL_CONTROLLER_BUTTON_X && nTabs > 0) menuMode = false;
    }
}

static void handle_links(int b) {
    if (b == SDL_CONTROLLER_BUTTON_B || b == SDL_CONTROLLER_BUTTON_BACK) { overlay = 0; return; }
    if (b == SDL_CONTROLLER_BUTTON_DPAD_UP && selLink > 0) selLink--;
    if (b == SDL_CONTROLLER_BUTTON_DPAD_DOWN && selLink + 1 < nLinks) selLink++;
    if (b == SDL_CONTROLLER_BUTTON_A) { int i = selLink; overlay = 0; open_link(i, true); }
    if (b == SDL_CONTROLLER_BUTTON_Y) { int i = selLink; overlay = 0; open_link(i, false); }
}

static void handle_reading(int b) {
    if (b == SDL_CONTROLLER_BUTTON_A) go_next();
    if (b == SDL_CONTROLLER_BUTTON_B) go_prev();
    if (b == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) jump(-10);
    if (b == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) jump(10);
    if (b == SDL_CONTROLLER_BUTTON_DPAD_LEFT && page.nnext > 1 && selChoice > 0) selChoice--;
    if (b == SDL_CONTROLLER_BUTTON_DPAD_RIGHT && page.nnext > 1 && selChoice + 1 < page.nnext) selChoice++;
    if (b == SDL_CONTROLLER_BUTTON_Y) {
        if (has_kind(1)) play_external(1);
        else if (has_kind(3)) play_external(3);
    }
    if (b == SDL_CONTROLLER_BUTTON_X) open_library();
    if (b == SDL_CONTROLLER_BUTTON_BACK) {
        if (nLinks > 0) { overlay = 1; selLink = 0; }
        else toast("No links on this page");
    }
}

int main(int argc, char **argv) {
    const char *env = getenv("MSPA_ASSETS");
    if (argc > 1) snprintf(assets, sizeof assets, "%s", argv[1]);
    else if (env && *env) snprintf(assets, sizeof assets, "%s", env);
    else snprintf(assets, sizeof assets, "assets");
    size_t al = strlen(assets);
    while (al > 1 && assets[al - 1] == '/') assets[--al] = 0;
    swapAB = getenv("MSPA_SWAP_AB") && getenv("MSPA_SWAP_AB")[0] == '1';
    mkdir("conf", 0755);

    if (SDL_Init(0) != 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    TTF_Init();
    fTitle = open_font("fonts/FreeMonoBold.ttf", 24, 0);
    fBody = open_font("fonts/FreeMonoBold.ttf", 16, 0);
    fCmd = open_font("fonts/FreeMono.ttf", 20, 0);
    fLink = open_font("fonts/FreeMono.ttf", 20, TTF_STYLE_UNDERLINE);
    fMenu = open_font("fonts/FreeMono.ttf", 22, 0);
    fFoot = open_font("fonts/LiberationSans-Bold.ttf", 12, 0);
    fFootU = open_font("fonts/LiberationSans-Bold.ttf", 12, TTF_STYLE_UNDERLINE);
    if (!fTitle || !fBody || !fCmd || !fLink || !fMenu || !fFoot || !fFootU) {
        fprintf(stderr, "font load failed: %s\n", TTF_GetError());
        return 1;
    }
    if (!video_init()) { fprintf(stderr, "video init failed: %s\n", SDL_GetError()); return 1; }

    fill(C_BG, 0, 0, SCR_W, SCR_H);
    draw_center(fTitle, "Loading...", SCR_H / 2 - 12, C_CARDTXT);
    SDL_RenderPresent(ren);

    if (load_data()) { bm_load(); restore_tabs(); }

    bool running = true, dirty = true, trigL = false, trigR = false;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
            if (e.type == SDL_CONTROLLERAXISMOTION) {
                if (e.caxis.axis == SDL_CONTROLLER_AXIS_RIGHTY)
                    stickDir = e.caxis.value > 12000 ? 1 : e.caxis.value < -12000 ? -1 : 0;
                bool l = e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT, r = e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
                if (l || r) {
                    bool on = e.caxis.value > 16000;
                    bool *st = l ? &trigL : &trigR;
                    if (on && !*st && !menuMode && !overlay && nTabs > 0) { tab_switch(l ? -1 : 1); dirty = true; }
                    if (on && !*st && menuMode && nTabs > 1) { /* switch tab straight from the library */
                        CT.scroll = scrollY; tab_show((curTab + (l ? -1 : 1) + nTabs) % nTabs); dirty = true;
                    }
                    *st = on;
                }
            }
            if (e.type != SDL_CONTROLLERBUTTONDOWN && e.type != SDL_CONTROLLERBUTTONUP) continue;
            int b = e.cbutton.button;
            bool down = e.type == SDL_CONTROLLERBUTTONDOWN;
            if (swapAB) {
                if (b == SDL_CONTROLLER_BUTTON_A) b = SDL_CONTROLLER_BUTTON_B;
                else if (b == SDL_CONTROLLER_BUTTON_B) b = SDL_CONTROLLER_BUTTON_A;
            }
            if (b == SDL_CONTROLLER_BUTTON_DPAD_UP) dpadDir = down ? -1 : (dpadDir == -1 ? 0 : dpadDir);
            if (b == SDL_CONTROLLER_BUTTON_DPAD_DOWN) dpadDir = down ? 1 : (dpadDir == 1 ? 0 : dpadDir);
            if (!down) continue;
            if (b == SDL_CONTROLLER_BUTTON_START) { running = false; continue; }
            if (menuMode) handle_library(b);
            else if (overlay) handle_links(b);
            else handle_reading(b);
            dirty = true;
        }
        if (!menuMode && !overlay) {
            int dir = dpadDir ? dpadDir : stickDir;
            if (dir) {
                scrollY += dir * 8;
                int ms = max_scroll();
                if (scrollY < 0) scrollY = 0;
                if (scrollY > ms) scrollY = ms;
                dirty = true;
            }
            animate(&dirty);
        }
        if (toastUntil && SDL_GetTicks() < toastUntil + 50) dirty = true;
        if (dirty) {
            if (menuMode) draw_library();
            else { draw_page(); if (overlay) draw_links(); }
            draw_tabbar();
            draw_toast();
            SDL_RenderPresent(ren);
            dirty = false;
        }
        SDL_Delay(8);
    }

    save_tabs();
    video_shutdown();
    TTF_Quit();
    SDL_Quit();
    return 0;
}
