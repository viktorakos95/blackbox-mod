/*
 * TCM probe (temporary): which 1 KB blocks of the core's fast memories does the running firmware use?
 * ITCM 0x00000000..0x0000ffff (64 blocks) and DTCM 0x20000000..0x2001ffff (128 blocks). The stock startup code initialises
 * neither (only a 2 KB interrupt stack at the top of DTCM), but code could still reach them through a pointer, so this only
 * reads: at boot each block's contents are hashed, then the blocks are hashed again a few at a time (from the page's
 * draws) and a block that ever differs from its boot hash is marked used. Nothing in either memory is written.
 * The Looper footer shows the result (looper_page.c): "TCM I<n> D<n> <first>-<last>", the used blocks of each and the
 * DTCM range. To be removed once the free space is known.
 */
#include <stdint.h>

#define NB 192                                              /* 64 ITCM + 128 DTCM blocks */
struct tp {
    uint32_t magic;
    uint16_t next;
    uint8_t used[NB];
    uint16_t h[NB];
};
#define TP (*(struct tp *volatile *)0x38800fd8u)            /* the backup SRAM word after the page state (looper_page.c) */
#define TP_MAGIC 0x54434d31u
typedef void *(*alloc_fn)(uint32_t n);
typedef uint32_t (*free_fn)(void);
#define fw_alloc    ((alloc_fn)0x08044405u)
#define fw_ext_free ((free_fn)0x080443f1u)

static uint16_t hash(int b)
{
    uint32_t a = b < 64 ? (uint32_t)b * 1024u : 0x20000000u + (uint32_t)(b - 64) * 1024u;
    __asm volatile("" : "+r"(a));                           /* (ITCM starts at address 0: the compiler must not see a null pointer) */
    const volatile uint32_t *p = (const volatile uint32_t *)a;
    uint32_t h = 2166136261u;
    for (int i = 0; i < 256; i++)
        h = (h ^ p[i]) * 16777619u;
    return (uint16_t)(h ^ h >> 16);
}

void tcm_boot(void)
{
    TP = 0;
    if (fw_ext_free() < 64u * 1024u)
        return;
    struct tp *t = (struct tp *)fw_alloc(sizeof(struct tp));
    if (!t)
        return;
    for (int b = 0; b < NB; b++) {
        t->h[b] = hash(b);
        t->used[b] = 0;
    }
    t->next = 0;
    t->magic = TP_MAGIC;
    TP = t;
}

static struct tp *tp(void)
{
    struct tp *t = TP;
    return t && t->magic == TP_MAGIC ? t : 0;
}

void tcm_step(void)                                         /* from the page's draw: four blocks a time */
{
    struct tp *t = tp();
    if (!t)
        return;
    for (int k = 0; k < 4; k++) {
        int b = t->next;
        if (!t->used[b] && hash(b) != t->h[b])
            t->used[b] = 1;
        t->next = (uint16_t)(b + 1 < NB ? b + 1 : 0);
    }
}

/* "TCM I<n> D<n> <first>-<last>" into b (24 bytes): used blocks of ITCM and DTCM, the DTCM ones' range (KB from 0x20000000). */
static char *put_u(char *p, unsigned v)
{
    char d[6];
    int n = 0;
    do
        d[n++] = (char)('0' + v % 10u);
    while ((v /= 10u) && n < 6);
    while (n)
        *p++ = d[--n];
    return p;
}

void tcm_text(char *b)
{
    struct tp *t = tp();
    char *p = b;
    for (const char *s = "TCM "; *s;)
        *p++ = *s++;
    if (!t) {
        *p++ = '-';
        *p = 0;
        return;
    }
    unsigned ni = 0, nd = 0;
    int lo = -1, hi = -1;
    for (int k = 0; k < NB; k++) {
        if (!t->used[k])
            continue;
        if (k < 64) {
            ni++;
        } else {
            nd++;
            lo = lo < 0 ? k - 64 : lo;
            hi = k - 64;
        }
    }
    *p++ = 'I';
    p = put_u(p, ni);
    *p++ = ' ';
    *p++ = 'D';
    p = put_u(p, nd);
    if (nd) {
        *p++ = ' ';
        p = put_u(p, (unsigned)lo);
        *p++ = '-';
        p = put_u(p, (unsigned)hi);
    }
    *p = 0;
}
