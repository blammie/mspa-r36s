/* gifdec.h - small streaming GIF decoder: decodes one frame at a time into an RGBA canvas */
#ifndef GIFDEC_H
#define GIFDEC_H
#include <stddef.h>
#include <stdint.h>

typedef struct Gif {
    uint8_t *data; size_t size;
    int w, h;
    uint8_t gct[256 * 3]; int gct_n;
    size_t first_off, pos;
    uint8_t *canvas;   /* w*h*4, bytes R,G,B,A */
    uint8_t *saved;    /* snapshot for disposal method 3 */
    int pd, px, py, pw, ph;   /* pending disposal of last frame + its rect */
    int frame_index;
    int delay_ms;
    int animated;      /* 1 if more frames follow the first */
} Gif;

Gif *gif_open_mem(uint8_t *data, size_t size); /* takes ownership of data */
int  gif_next(Gif *g);   /* decode next frame into g->canvas, loops at end. Returns 1 on success. */
void gif_close(Gif *g);
#endif
