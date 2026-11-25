/* Minimal protobuf encoder/decoder: just what the ESPHome API needs. */
#include <stdlib.h>
#include <string.h>

#include "sat.h"

void pb_reset(struct pbuf *b) { b->len = 0; }

void pb_free(struct pbuf *b)
{
    free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
}

void pb_raw(struct pbuf *b, const void *data, size_t len)
{
    if (b->len + len > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 256;
        while (cap < b->len + len)
            cap *= 2;
        uint8_t *p = realloc(b->p, cap);
        if (!p) {
            loge("out of memory");
            abort();
        }
        b->p = p;
        b->cap = cap;
    }
    if (len)
        memcpy(b->p + b->len, data, len);
    b->len += len;
}

void pb_varint(struct pbuf *b, uint64_t v)
{
    uint8_t tmp[10];
    int n = 0;
    do {
        tmp[n] = v & 0x7f;
        v >>= 7;
        if (v)
            tmp[n] |= 0x80;
        n++;
    } while (v);
    pb_raw(b, tmp, n);
}

static void pb_key(struct pbuf *b, int field, int wire)
{
    pb_varint(b, ((uint64_t)field << 3) | wire);
}

void pb_uint(struct pbuf *b, int field, uint64_t v)
{
    if (!v)
        return;
    pb_key(b, field, 0);
    pb_varint(b, v);
}

void pb_bytes(struct pbuf *b, int field, const void *d, size_t len)
{
    pb_key(b, field, 2);
    pb_varint(b, len);
    pb_raw(b, d, len);
}

void pb_str(struct pbuf *b, int field, const char *s)
{
    if (s && *s)
        pb_bytes(b, field, s, strlen(s));
}

void pb_fixed32(struct pbuf *b, int field, uint32_t v)
{
    uint8_t le[4] = { v, v >> 8, v >> 16, v >> 24 };
    pb_key(b, field, 5);
    pb_raw(b, le, 4);
}

void pb_float(struct pbuf *b, int field, float v)
{
    uint32_t u;
    if (v == 0.0f)
        return;
    memcpy(&u, &v, 4);
    pb_fixed32(b, field, u);
}

void pb_sub(struct pbuf *b, int field, const struct pbuf *sub)
{
    pb_bytes(b, field, sub->p, sub->len);
}

int pb_read_varint(struct pbr *r, uint64_t *v)
{
    uint64_t out = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (r->p >= r->end)
            return -1;
        uint8_t c = *r->p++;
        out |= (uint64_t)(c & 0x7f) << shift;
        if (!(c & 0x80)) {
            *v = out;
            return 0;
        }
    }
    return -1;
}

int pb_next(struct pbr *r, struct pbfield *f)
{
    uint64_t key, len;

    if (r->p >= r->end)
        return 0;
    if (pb_read_varint(r, &key) < 0)
        return -1;
    f->num = (uint32_t)(key >> 3);
    f->wire = (int)(key & 7);
    f->v = 0;
    f->data = NULL;
    f->len = 0;
    switch (f->wire) {
    case 0:
        return pb_read_varint(r, &f->v) < 0 ? -1 : 1;
    case 1:
        if (r->end - r->p < 8)
            return -1;
        for (int i = 7; i >= 0; i--)
            f->v = (f->v << 8) | r->p[i];
        r->p += 8;
        return 1;
    case 2:
        if (pb_read_varint(r, &len) < 0 || len > (uint64_t)(r->end - r->p))
            return -1;
        f->data = r->p;
        f->len = (size_t)len;
        r->p += len;
        return 1;
    case 5:
        if (r->end - r->p < 4)
            return -1;
        for (int i = 3; i >= 0; i--)
            f->v = (f->v << 8) | r->p[i];
        r->p += 4;
        return 1;
    default:
        return -1;
    }
}

void pb_field_str(const struct pbfield *f, char *dst, size_t cap)
{
    size_t n = f->len < cap - 1 ? f->len : cap - 1;
    if (f->wire != 2)
        n = 0;
    if (n)
        memcpy(dst, f->data, n);
    dst[n] = 0;
}
