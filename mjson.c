#include "mjson.h"
#include <stdlib.h>
#include <string.h>

#define BLOCK 8192
typedef struct Blk { JNode n[BLOCK]; struct Blk *next; } Blk;
static Blk *blocks;
static int used = BLOCK;

static JNode *alloc_node(void) {
    if (used == BLOCK) {
        Blk *b = calloc(1, sizeof(Blk));
        if (!b) return NULL;
        b->next = blocks;
        blocks = b;
        used = 0;
    }
    return &blocks->n[used++];
}

void json_free_arena(void) {
    while (blocks) { Blk *n = blocks->next; free(blocks); blocks = n; }
    used = BLOCK;
}

static const char *ws(const char *p, const char *e) {
    while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
    return p;
}

/* scan a string starting after the opening quote; returns pointer after closing quote */
static const char *scan_str(const char *p, const char *e, const char **s, uint32_t *len) {
    const char *st = p;
    while (p < e && *p != '"') p += (*p == '\\' && p + 1 < e) ? 2 : 1;
    if (p >= e) return NULL;
    *s = st;
    *len = (uint32_t)(p - st);
    return p + 1;
}

static const char *parse_val(const char *p, const char *e, JNode *out, int depth) {
    if (depth > 64) return NULL;
    p = ws(p, e);
    if (p >= e) return NULL;
    out->child = out->next = NULL;
    out->count = 0;
    out->key = NULL;
    out->klen = 0;
    if (*p == '"') {
        out->t = J_STR;
        return scan_str(p + 1, e, &out->s, &out->len);
    }
    if (*p == '{' || *p == '[') {
        int obj = *p == '{';
        char close = obj ? '}' : ']';
        out->t = obj ? J_OBJ : J_ARR;
        p = ws(p + 1, e);
        JNode *tail = NULL;
        if (p < e && *p == close) return p + 1;
        for (;;) {
            JNode *c = alloc_node();
            if (!c) return NULL;
            if (obj) {
                p = ws(p, e);
                if (p >= e || *p != '"') return NULL;
                const char *ks; uint32_t kl;
                p = scan_str(p + 1, e, &ks, &kl);
                if (!p) return NULL;
                p = ws(p, e);
                if (p >= e || *p != ':') return NULL;
                p++;
                p = parse_val(p, e, c, depth + 1);
                if (!p) return NULL;
                c->key = ks; c->klen = kl;
            } else {
                p = parse_val(p, e, c, depth + 1);
                if (!p) return NULL;
            }
            if (tail) tail->next = c; else out->child = c;
            tail = c;
            out->count++;
            p = ws(p, e);
            if (p >= e) return NULL;
            if (*p == ',') { p++; continue; }
            if (*p == close) return p + 1;
            return NULL;
        }
    }
    /* literal or number */
    const char *st = p;
    while (p < e && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') p++;
    out->s = st;
    out->len = (uint32_t)(p - st);
    out->t = (*st == 't' || *st == 'f') ? J_BOOL : (*st == 'n') ? J_NULL : J_NUM;
    return p;
}

JNode *json_parse(const char *buf, size_t n) {
    JNode *root = alloc_node();
    if (!root) return NULL;
    if (!parse_val(buf, buf + n, root, 0)) return NULL;
    return root;
}

int jkey_eq(const JNode *n, const char *s, size_t len) {
    return n->key && n->klen == len && memcmp(n->key, s, len) == 0;
}

const JNode *jget(const JNode *obj, const char *key) {
    if (!obj || obj->t != J_OBJ) return NULL;
    size_t kl = strlen(key);
    for (const JNode *c = obj->child; c; c = c->next)
        if (jkey_eq(c, key, kl)) return c;
    return NULL;
}

const JNode *jat(const JNode *arr, uint32_t idx) {
    if (!arr || arr->t != J_ARR) return NULL;
    const JNode *c = arr->child;
    while (c && idx--) c = c->next;
    return c;
}

static int hex4(const char *p) {
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return -1;
    }
    return v;
}

static char *put_utf8(char *o, unsigned cp) {
    if (cp < 0x80) *o++ = (char)cp;
    else if (cp < 0x800) { *o++ = (char)(0xC0 | (cp >> 6)); *o++ = (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { *o++ = (char)(0xE0 | (cp >> 12)); *o++ = (char)(0x80 | ((cp >> 6) & 0x3F)); *o++ = (char)(0x80 | (cp & 0x3F)); }
    else { *o++ = (char)(0xF0 | (cp >> 18)); *o++ = (char)(0x80 | ((cp >> 12) & 0x3F)); *o++ = (char)(0x80 | ((cp >> 6) & 0x3F)); *o++ = (char)(0x80 | (cp & 0x3F)); }
    return o;
}

char *jdup(const JNode *n) {
    if (!n || n->t != J_STR) { char *e = malloc(1); if (e) *e = 0; return e; }
    char *out = malloc((size_t)n->len + 4), *o = out;
    if (!out) return NULL;
    const char *p = n->s, *e = n->s + n->len;
    while (p < e) {
        if (*p != '\\') { *o++ = *p++; continue; }
        p++;
        if (p >= e) break;
        switch (*p) {
        case 'n': *o++ = '\n'; p++; break;
        case 't': *o++ = '\t'; p++; break;
        case 'r': p++; break;
        case 'b': case 'f': p++; break;
        case 'u': {
            int cp = (p + 4 < e + 0 && p + 4 <= e - 0) ? hex4(p + 1) : -1;
            if (cp < 0) { p++; break; }
            p += 5;
            if (cp >= 0xD800 && cp < 0xDC00 && p + 5 < e + 1 && p[0] == '\\' && p[1] == 'u') {
                int lo = hex4(p + 2);
                if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); p += 6; }
            }
            o = put_utf8(o, (unsigned)cp);
            break;
        }
        default: *o++ = *p++; break; /* \" \\ \/ */
        }
    }
    *o = '\0';
    return out;
}
