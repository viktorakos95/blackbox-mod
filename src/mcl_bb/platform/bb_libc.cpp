/* The few C library functions MCL's core needs, for a blob that links without a C library (-nostdlib): string and memory functions, isspace, rand, snprintf / vsnprintf
 * (the formats MCL uses: %d %u %ld %lu %x %lx %X with 0-flag, width and precision, %s %c %% and %.*f), errno for libm (powf / sinf come from newlib's libm). */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#define HOOK extern "C"

HOOK size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

HOOK char *strcpy(char *d, const char *s)
{
    char *r = d;
    while ((*d++ = *s++)) {}
    return r;
}

HOOK char *strncpy(char *d, const char *s, size_t n)
{
    size_t i = 0;
    for (; i < n && s[i]; i++)
        d[i] = s[i];
    for (; i < n; i++)
        d[i] = 0;
    return d;
}

HOOK char *strcat(char *d, const char *s)
{
    strcpy(d + strlen(d), s);
    return d;
}

HOOK char *strncat(char *d, const char *s, size_t n)
{
    char *e = d + strlen(d);
    size_t i = 0;
    for (; i < n && s[i]; i++)
        e[i] = s[i];
    e[i] = 0;
    return d;
}

HOOK int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}

HOOK int strncmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || !a[i])
            return (unsigned char)a[i] - (unsigned char)b[i];
    }
    return 0;
}

HOOK char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c)
            return (char *)s;
        if (!*s)
            return 0;
    }
}

HOOK char *strrchr(const char *s, int c)
{
    const char *r = 0;
    for (;; s++) {
        if (*s == (char)c)
            r = s;
        if (!*s)
            return (char *)r;
    }
}

HOOK int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i])
            return x[i] - y[i];
    return 0;
}

HOOK void *memmove(void *d, const void *s, size_t n)
{
    uint8_t *dd = (uint8_t *)d;
    const uint8_t *ss = (const uint8_t *)s;
    if (dd < ss) {
        for (size_t i = 0; i < n; i++)
            dd[i] = ss[i];
    } else {
        for (size_t i = n; i-- > 0;)
            dd[i] = ss[i];
    }
    return d;
}

HOOK int isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

static uint32_t rand_state = 1;
HOOK int rand(void)
{
    rand_state = rand_state * 1103515245u + 12345u;
    return (int)((rand_state >> 16) & 0x7fff);
}
HOOK void srand(unsigned s) { rand_state = s; }

static int errno_value;
HOOK int *__errno(void) { return &errno_value; }

/* ---- vsnprintf ---- */
namespace {
struct Out {
    char *p;
    size_t cap, n;
    void put(char c)
    {
        if (n + 1 < cap)
            p[n] = c;
        n++;
    }
};

void put_uint(Out &o, uint32_t v, unsigned base, bool upper, int width, bool zero, bool neg)
{
    char t[16];
    int k = 0;
    do {
        unsigned d = v % base;
        t[k++] = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10);
        v /= base;
    } while (v);
    int len = k + (neg ? 1 : 0);
    if (!zero)
        for (int i = len; i < width; i++)
            o.put(' ');
    if (neg)
        o.put('-');
    if (zero)
        for (int i = len; i < width; i++)
            o.put('0');
    while (k)
        o.put(t[--k]);
}

void put_float(Out &o, double v, int prec, int width, bool zero)
{
    bool neg = v < 0;
    if (neg)
        v = -v;
    if (prec < 0)
        prec = 6;
    if (prec > 9)
        prec = 9;
    double scale = 1;
    for (int i = 0; i < prec; i++)
        scale *= 10;
    double r = v * scale + 0.5;
    uint64_t all = (uint64_t)r;
    uint32_t ip = (uint32_t)(all / (uint64_t)scale), fp = (uint32_t)(all % (uint64_t)scale);
    char t[40];
    int k = 0;
    if (prec) {
        for (int i = 0; i < prec; i++) {
            t[k++] = (char)('0' + fp % 10);
            fp /= 10;
        }
        t[k++] = '.';
    }
    do {
        t[k++] = (char)('0' + ip % 10);
        ip /= 10;
    } while (ip);
    int len = k + (neg ? 1 : 0);
    if (!zero)
        for (int i = len; i < width; i++)
            o.put(' ');
    if (neg)
        o.put('-');
    if (zero)
        for (int i = len; i < width; i++)
            o.put('0');
    while (k)
        o.put(t[--k]);
}
}

HOOK int vsnprintf(char *buf, size_t cap, const char *f, va_list ap)
{
    Out o{buf, cap, 0};
    for (; *f; f++) {
        if (*f != '%') {
            o.put(*f);
            continue;
        }
        f++;
        bool zero = false, left = false;
        for (;; f++) {
            if (*f == '0')
                zero = true;
            else if (*f == '-')
                left = true;
            else
                break;
        }
        (void)left;
        int width = 0, prec = -1;
        if (*f == '*') {
            width = va_arg(ap, int);
            f++;
        } else {
            while (*f >= '0' && *f <= '9')
                width = width * 10 + (*f++ - '0');
        }
        if (*f == '.') {
            f++;
            prec = 0;
            if (*f == '*') {
                prec = va_arg(ap, int);
                f++;
            } else {
                while (*f >= '0' && *f <= '9')
                    prec = prec * 10 + (*f++ - '0');
            }
        }
        while (*f == 'l' || *f == 'h' || *f == 'z')
            f++;
        switch (*f) {
        case 'd':
        case 'i': {
            int32_t v = va_arg(ap, int32_t);
            put_uint(o, v < 0 ? (uint32_t)-(int64_t)v : (uint32_t)v, 10, false, width, zero, v < 0);
            break;
        }
        case 'u': put_uint(o, va_arg(ap, uint32_t), 10, false, width, zero, false); break;
        case 'x': put_uint(o, va_arg(ap, uint32_t), 16, false, width, zero, false); break;
        case 'X': put_uint(o, va_arg(ap, uint32_t), 16, true, width, zero, false); break;
        case 'c': o.put((char)va_arg(ap, int)); break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            int len = (int)strlen(s);
            if (prec >= 0 && prec < len)
                len = prec;
            for (int i = len; i < width; i++)
                o.put(' ');
            for (int i = 0; i < len; i++)
                o.put(s[i]);
            break;
        }
        case 'f': put_float(o, va_arg(ap, double), prec, width, zero); break;
        case '%': o.put('%'); break;
        case 0: f--; break;
        default: o.put('%'); o.put(*f); break;
        }
    }
    if (cap)
        buf[o.n < cap ? o.n : cap - 1] = 0;
    return (int)o.n;
}

HOOK int snprintf(char *buf, size_t cap, const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    int n = vsnprintf(buf, cap, f, ap);
    va_end(ap);
    return n;
}
