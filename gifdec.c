#include "gifdec.h"
#include <stdlib.h>
#include <string.h>

static int rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }

Gif *gif_open_mem(uint8_t *d, size_t n) {
    if (!d || n < 13 || memcmp(d, "GIF8", 4) != 0) { free(d); return NULL; }
    Gif *g = calloc(1, sizeof *g);
    if (!g) { free(d); return NULL; }
    g->data = d; g->size = n;
    g->w = rd16(d + 6); g->h = rd16(d + 8);
    int flags = d[10];
    size_t pos = 13;
    if (g->w <= 0 || g->h <= 0 || g->w > 8192 || g->h > 8192) { gif_close(g); return NULL; }
    if (flags & 0x80) {
        g->gct_n = 2 << (flags & 7);
        if (pos + (size_t)g->gct_n * 3 > n) { gif_close(g); return NULL; }
        memcpy(g->gct, d + pos, (size_t)g->gct_n * 3);
        pos += (size_t)g->gct_n * 3;
    }
    g->first_off = g->pos = pos;
    g->canvas = calloc((size_t)g->w * g->h, 4);
    if (!g->canvas) { gif_close(g); return NULL; }
    return g;
}

void gif_close(Gif *g) {
    if (!g) return;
    free(g->data); free(g->canvas); free(g->saved); free(g);
}

/* skip sub-blocks starting at p; returns new position */
static size_t skip_blocks(const uint8_t *d, size_t n, size_t p) {
    while (p < n && d[p]) p += (size_t)d[p] + 1;
    return p < n ? p + 1 : n;
}

static void clear_rect(Gif *g, int x, int y, int w, int h) {
    for (int j = y; j < y + h && j < g->h; j++) {
        if (j < 0) continue;
        for (int i = x; i < x + w && i < g->w; i++)
            if (i >= 0) memset(g->canvas + ((size_t)j * g->w + i) * 4, 0, 4);
    }
}

/* LZW-decode into idx (w*h bytes). data = concatenated sub-block bytes. */
static int lzw(const uint8_t *in, size_t inlen, int minbits, uint8_t *out, size_t outn) {
    if (minbits < 1 || minbits > 11) return 0;
    static uint16_t prefix[4096];
    static uint8_t suffix[4096], stack[4097];
    int clear = 1 << minbits, eoi = clear + 1, next = eoi + 1, size = minbits + 1;
    int prev = -1, first = 0;
    uint32_t acc = 0; int nb = 0; size_t ip = 0, op = 0;
    for (;;) {
        while (nb < size && ip < inlen) { acc |= (uint32_t)in[ip++] << nb; nb += 8; }
        if (nb < size) break;
        int code = (int)(acc & ((1u << size) - 1));
        acc >>= size; nb -= size;
        if (code == clear) { next = eoi + 1; size = minbits + 1; prev = -1; continue; }
        if (code == eoi) break;
        if (prev < 0) {
            if (code >= clear) break;
            if (op < outn) out[op++] = (uint8_t)code;
            prev = code; first = code;
            continue;
        }
        int c = code, sp = 0;
        if (code >= next) {
            if (code > next) break;
            stack[sp++] = (uint8_t)first;
            c = prev;
        }
        while (c >= clear) {
            if (sp >= 4096) return 0;
            stack[sp++] = suffix[c];
            c = prefix[c];
        }
        first = c;
        stack[sp++] = (uint8_t)c;
        if (next < 4096) {
            prefix[next] = (uint16_t)prev; suffix[next] = (uint8_t)first; next++;
            if (next == (1 << size) && size < 12) size++;
        }
        prev = code;
        while (sp > 0 && op < outn) out[op++] = stack[--sp];
        if (op >= outn) break;
    }
    return 1;
}

static int peek_more(const Gif *g) {
    size_t p = g->pos;
    const uint8_t *d = g->data;
    while (p < g->size && d[p] == 0x21) {
        p += 2;
        p = skip_blocks(d, g->size, p);
    }
    return p < g->size && d[p] == 0x2C;
}

int gif_next(Gif *g) {
    if (!g) return 0;
    const uint8_t *d = g->data;
    size_t n = g->size;
    int disposal = 0, trans = -1, delay = 10;
    for (int attempt = 0; attempt < 2; attempt++) {
        while (g->pos < n) {
            uint8_t b = d[g->pos];
            if (b == 0x21) { /* extension */
                if (g->pos + 2 > n) break;
                uint8_t label = d[g->pos + 1];
                if (label == 0xF9 && g->pos + 8 <= n && d[g->pos + 2] == 4) {
                    int f = d[g->pos + 3];
                    disposal = (f >> 2) & 7;
                    trans = (f & 1) ? d[g->pos + 6] : -1;
                    delay = rd16(d + g->pos + 4);
                }
                g->pos = skip_blocks(d, n, g->pos + 2);
                continue;
            }
            if (b == 0x2C) {
                if (g->pos + 10 > n) return 0;
                int fx = rd16(d + g->pos + 1), fy = rd16(d + g->pos + 3);
                int fw = rd16(d + g->pos + 5), fh = rd16(d + g->pos + 7);
                int fl = d[g->pos + 9];
                size_t p = g->pos + 10;
                const uint8_t *pal = g->gct; int pn = g->gct_n;
                uint8_t lct[256 * 3];
                if (fl & 0x80) {
                    pn = 2 << (fl & 7);
                    if (p + (size_t)pn * 3 > n) return 0;
                    memcpy(lct, d + p, (size_t)pn * 3); pal = lct; p += (size_t)pn * 3;
                }
                if (p >= n || fw <= 0 || fh <= 0 || fw > 8192 || fh > 8192) return 0;
                int minbits = d[p++];
                /* gather sub-blocks */
                size_t q = p, total = 0;
                while (q < n && d[q]) { total += d[q]; q += (size_t)d[q] + 1; }
                uint8_t *comp = malloc(total ? total : 1);
                uint8_t *idx = malloc((size_t)fw * fh);
                if (!comp || !idx) { free(comp); free(idx); return 0; }
                size_t cp = 0; q = p;
                while (q < n && d[q]) { memcpy(comp + cp, d + q + 1, d[q]); cp += d[q]; q += (size_t)d[q] + 1; }
                g->pos = q < n ? q + 1 : n;
                memset(idx, 0, (size_t)fw * fh);
                lzw(comp, total, minbits, idx, (size_t)fw * fh);
                free(comp);

                /* dispose previous frame */
                if (g->pd == 2) clear_rect(g, g->px, g->py, g->pw, g->ph);
                else if (g->pd == 3 && g->saved) memcpy(g->canvas, g->saved, (size_t)g->w * g->h * 4);
                if (disposal == 3) {
                    if (!g->saved) g->saved = malloc((size_t)g->w * g->h * 4);
                    if (g->saved) memcpy(g->saved, g->canvas, (size_t)g->w * g->h * 4);
                }
                /* draw */
                int interlaced = fl & 0x40;
                static const int start[4] = {0, 4, 2, 1}, step[4] = {8, 8, 4, 2};
                int pass = 0, row = 0, y = 0;
                for (int r = 0; r < fh; r++) {
                    if (interlaced) {
                        y = row;
                        row += step[pass];
                        while (row >= fh && pass < 3) { pass++; row = start[pass]; }
                    } else y = r;
                    int cy = fy + y;
                    if (cy < 0 || cy >= g->h) continue;
                    for (int x = 0; x < fw; x++) {
                        int cx = fx + x;
                        if (cx < 0 || cx >= g->w) continue;
                        int ci = idx[(size_t)r * fw + x];
                        if (ci == trans || ci >= pn) continue;
                        uint8_t *o = g->canvas + ((size_t)cy * g->w + cx) * 4;
                        o[0] = pal[ci * 3]; o[1] = pal[ci * 3 + 1]; o[2] = pal[ci * 3 + 2]; o[3] = 255;
                    }
                }
                free(idx);
                g->pd = disposal; g->px = fx; g->py = fy; g->pw = fw; g->ph = fh;
                g->delay_ms = delay <= 1 ? 100 : delay * 10;
                g->frame_index++;
                if (g->frame_index == 1) g->animated = peek_more(g);
                return 1;
            }
            break; /* trailer or garbage */
        }
        /* end of stream: loop */
        if (g->frame_index == 0) return 0;
        g->pos = g->first_off; g->frame_index = 0; g->pd = 0;
        memset(g->canvas, 0, (size_t)g->w * g->h * 4);
    }
    return 0;
}
