/* mjson.h - tiny in-place JSON DOM (strings stay as slices of the input buffer) */
#ifndef MJSON_H
#define MJSON_H
#include <stddef.h>
#include <stdint.h>

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;

typedef struct JNode {
    JType t;
    const char *s;      /* raw string slice (between quotes) / number text */
    uint32_t len;
    const char *key;    /* object member name slice */
    uint32_t klen;
    struct JNode *child, *next;
    uint32_t count;     /* number of children for ARR/OBJ */
} JNode;

/* parse `buf` (must stay alive). Returns root or NULL. Free arena with json_free_arena(). */
JNode *json_parse(const char *buf, size_t n);
void json_free_arena(void);

const JNode *jget(const JNode *obj, const char *key);   /* object member or NULL */
const JNode *jat(const JNode *arr, uint32_t idx);        /* array element or NULL */
char *jdup(const JNode *n);                              /* unescaped malloc'd copy of a string node ("" if not a string) */
int jkey_eq(const JNode *n, const char *s, size_t len);  /* member name equals s? */
#endif
