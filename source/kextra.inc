/*
 * XboxNX — v0.18.87: funciones del kernel que Halo 2 no usa y otros juegos
 * sí (antes devolvían 0 sin hacer nada). Va incluido al final de ksys.c.
 * Listas con interlock, enteros de 64 bits, cadenas, formato de texto.
 */

/* ---- listas ---- */

/* SLIST_HEADER { Next 0, Depth 4 (16 bits), Sequence 6 } */
static uint64_t InterlockedPushEntrySList(KCall *c)
{
    uint32_t h = c->a[0], e = c->a[1], first = vm_rd32(c->vm, h);
    vm_wr32(c->vm, e, first);
    vm_wr32(c->vm, h, e);
    wr16(c->k, h + 4, (uint16_t)(rd16(c->k, h + 4) + 1));
    wr16(c->k, h + 6, (uint16_t)(rd16(c->k, h + 6) + 1));
    return first;
}
static uint64_t InterlockedPopEntrySList(KCall *c)
{
    uint32_t h = c->a[0], first = vm_rd32(c->vm, h);
    if (!first) {
        return 0;
    }
    vm_wr32(c->vm, h, vm_rd32(c->vm, first));
    wr16(c->k, h + 4, (uint16_t)(rd16(c->k, h + 4) - 1));
    wr16(c->k, h + 6, (uint16_t)(rd16(c->k, h + 6) + 1));
    return first;
}
static uint64_t InterlockedFlushSList(KCall *c)
{
    uint32_t h = c->a[0], first = vm_rd32(c->vm, h);
    vm_wr32(c->vm, h, 0);
    wr16(c->k, h + 4, 0);
    return first;
}

/* LIST_ENTRY { Flink 0, Blink 4 }; devuelven la entrada que estaba (0: vacía) */
static uint64_t ExfInterlockedInsertHeadList(KCall *c)
{
    uint32_t h = c->a[0], e = c->a[1], first = vm_rd32(c->vm, h);
    vm_wr32(c->vm, e, first);
    vm_wr32(c->vm, e + 4, h);
    vm_wr32(c->vm, first + 4, e);
    vm_wr32(c->vm, h, e);
    return first == h ? 0 : first;
}
static uint64_t ExfInterlockedInsertTailList(KCall *c)
{
    uint32_t h = c->a[0], e = c->a[1], last = vm_rd32(c->vm, h + 4);
    vm_wr32(c->vm, e, h);
    vm_wr32(c->vm, e + 4, last);
    vm_wr32(c->vm, last, e);
    vm_wr32(c->vm, h + 4, e);
    return last == h ? 0 : last;
}
static uint64_t ExfInterlockedRemoveHeadList(KCall *c)
{
    uint32_t h = c->a[0], first = vm_rd32(c->vm, h);
    if (first == h) {
        return 0;
    }
    uint32_t next = vm_rd32(c->vm, first);
    vm_wr32(c->vm, h, next);
    vm_wr32(c->vm, next + 4, h);
    return first;
}

/* ---- 64 bits ---- */

static uint64_t rd64(Kernel *k, uint32_t va) { return vm_rd32(k->vm, va) | (uint64_t)vm_rd32(k->vm, va + 4) << 32; }
static void wr64(Kernel *k, uint32_t va, uint64_t v)
{
    vm_wr32(k->vm, va, (uint32_t)v);
    vm_wr32(k->vm, va + 4, (uint32_t)(v >> 32));
}

/* ExInterlockedAddLargeInteger(Addend, Increment (por valor), Lock) -> el valor de antes */
static uint64_t ExInterlockedAddLargeInteger(KCall *c)
{
    uint64_t old = rd64(c->k, c->a[0]);
    wr64(c->k, c->a[0], old + (c->a[1] | (uint64_t)c->a[2] << 32));
    return old;
}
/* ExInterlockedAddLargeStatistic(Addend (ECX), Increment (EDX)) */
static uint64_t ExInterlockedAddLargeStatistic(KCall *c)
{
    wr64(c->k, c->a[0], rd64(c->k, c->a[0]) + c->a[1]);
    return 0;
}
/* ExInterlockedCompareExchange64(Destination (ECX), Exchange (EDX), Comparand) -> el valor de antes */
static uint64_t ExInterlockedCompareExchange64(KCall *c)
{
    uint64_t old = rd64(c->k, c->a[0]);
    if (old == rd64(c->k, c->a[2])) {
        wr64(c->k, c->a[0], rd64(c->k, c->a[1]));
    }
    return old;
}
/* RtlExtendedIntegerMultiply(Multiplicand (64), Multiplier (32)) */
static uint64_t RtlExtendedIntegerMultiply(KCall *c)
{
    return (uint64_t)((int64_t)(c->a[0] | (uint64_t)c->a[1] << 32) * (int32_t)c->a[2]);
}
/* RtlExtendedLargeIntegerDivide(Dividend (64), Divisor (32), *Remainder) */
static uint64_t RtlExtendedLargeIntegerDivide(KCall *c)
{
    uint64_t n = c->a[0] | (uint64_t)c->a[1] << 32;
    if (!c->a[2]) {
        return 0;
    }
    if (c->a[3]) {
        vm_wr32(c->vm, c->a[3], (uint32_t)(n % c->a[2]));
    }
    return n / c->a[2];
}
/* RtlExtendedMagicDivide(Dividend (64), MagicDivisor (64), ShiftCount (8)) */
static uint64_t RtlExtendedMagicDivide(KCall *c)
{
    int64_t n = (int64_t)(c->a[0] | (uint64_t)c->a[1] << 32);
    uint64_t m = c->a[2] | (uint64_t)c->a[3] << 32;
    bool neg = n < 0;
    unsigned __int128 p = (unsigned __int128)(uint64_t)(neg ? -n : n) * m;
    uint64_t q = (uint64_t)(p >> 64) >> (c->a[4] & 0x3F);
    return neg ? (uint64_t)(-(int64_t)q) : q;
}
static uint64_t RtlUlongByteSwap(KCall *c) { return __builtin_bswap32(c->a[0]); }
static uint64_t RtlUshortByteSwap(KCall *c) { return __builtin_bswap16((uint16_t)c->a[0]); }

/* ---- cadenas ---- */

static uint16_t downcase16(uint16_t ch)
{
    if ((ch >= 'A' && ch <= 'Z') || (ch >= 0xC0 && ch <= 0xDE && ch != 0xD7)) {
        return (uint16_t)(ch + 32);
    }
    return ch;
}
static uint64_t RtlDowncaseUnicodeChar(KCall *c) { return downcase16((uint16_t)c->a[0]); }

static int ustr_compare(Kernel *k, uint32_t s1, uint32_t s2, bool nocase)
{
    uint32_t p1 = str_buf(k, s1), p2 = str_buf(k, s2);
    unsigned n1 = str_len(k, s1) / 2u, n2 = str_len(k, s2) / 2u;
    for (unsigned i = 0; i < n1 && i < n2; i++) {
        int a = rd16(k, p1 + 2 * i), b = rd16(k, p2 + 2 * i);
        if (nocase) {
            a = upcase16((uint16_t)a);
            b = upcase16((uint16_t)b);
        }
        if (a != b) {
            return a - b;
        }
    }
    return (int)n1 - (int)n2;
}
/* RtlCompareUnicodeString / RtlEqualUnicodeString(String1, String2, CaseInSensitive) */
static uint64_t RtlCompareUnicodeString(KCall *c)
{
    return (uint32_t)ustr_compare(c->k, c->a[0], c->a[1], c->a[2] & 0xFF);
}
static uint64_t RtlEqualUnicodeString(KCall *c)
{
    return str_len(c->k, c->a[0]) == str_len(c->k, c->a[1]) && !ustr_compare(c->k, c->a[0], c->a[1], c->a[2] & 0xFF);
}
/* RtlCopyUnicodeString(Destination, Source) */
static uint64_t RtlCopyUnicodeString(KCall *c)
{
    Kernel *k = c->k;
    if (!c->a[1]) {
        wr16(k, c->a[0], 0);
        return 0;
    }
    uint16_t n = str_len(k, c->a[1]), max = str_max(k, c->a[0]);
    if (n > max) {
        n = max;
    }
    n &= (uint16_t)~1u;
    uint32_t d = str_buf(k, c->a[0]), s = str_buf(k, c->a[1]);
    for (uint16_t i = 0; i < n; i++) {
        wr8(k, d + i, rd8(k, s + i));
    }
    wr16(k, c->a[0], n);
    if (n + 2u <= max) {
        wr16(k, d + n, 0);
    }
    return 0;
}
/* añadir n bytes de src al final de la cadena dst (unidad: 1 o 2 bytes) */
static uint64_t str_append(Kernel *k, uint32_t dst, uint32_t src, uint16_t n, unsigned unit)
{
    uint16_t len = str_len(k, dst), max = str_max(k, dst);
    if (!n) {
        return ST_SUCCESS;
    }
    if ((unsigned)len + n > max) {
        return ST_BUFFER_TOO_SMALL;
    }
    uint32_t d = str_buf(k, dst) + len;
    for (uint16_t i = 0; i < n; i++) {
        wr8(k, d + i, rd8(k, src + i));
    }
    wr16(k, dst, (uint16_t)(len + n));
    if (unit == 2 && (unsigned)len + n + 2u <= max) {
        wr16(k, d + n, 0);
    }
    return ST_SUCCESS;
}
static uint64_t RtlAppendStringToString(KCall *c)
{
    return str_append(c->k, c->a[0], str_buf(c->k, c->a[1]), str_len(c->k, c->a[1]), 1);
}
static uint64_t RtlAppendUnicodeStringToString(KCall *c)
{
    return str_append(c->k, c->a[0], str_buf(c->k, c->a[1]), str_len(c->k, c->a[1]), 2);
}
static uint64_t RtlAppendUnicodeToString(KCall *c)
{
    uint16_t n = 0;
    while (c->a[1] && n < 0xFFFC && rd16(c->k, c->a[1] + n)) {
        n += 2;
    }
    return str_append(c->k, c->a[0], c->a[1], n, 2);
}
/* RtlCreateUnicodeString(Destination, Source) -> BOOLEAN */
static uint64_t RtlCreateUnicodeString(KCall *c)
{
    Kernel *k = c->k;
    uint16_t n = 0;
    while (n < 0xFFFC && rd16(k, c->a[1] + n)) {
        n += 2;
    }
    uint32_t buf = str_alloc(k, n + 2u);
    if (!buf) {
        return 0;
    }
    for (uint16_t i = 0; i < n + 2; i++) {
        wr8(k, buf + i, rd8(k, c->a[1] + i));
    }
    str_set(k, c->a[0], n, (uint16_t)(n + 2), buf);
    return 1;
}
/* RtlDowncaseUnicodeString(Destination, Source, Allocate) */
static uint64_t RtlDowncaseUnicodeString(KCall *c)
{
    Kernel *k = c->k;
    uint16_t n = str_len(k, c->a[1]) & (uint16_t)~1u;
    uint32_t buf = str_buf(k, c->a[0]);
    if (c->a[2] & 0xFF) {
        buf = str_alloc(k, n);
        if (!buf) {
            return ST_NO_MEMORY;
        }
        wr16(k, c->a[0] + 2, n);
        vm_wr32(k->vm, c->a[0] + 4, buf);
    } else if (str_max(k, c->a[0]) < n) {
        return ST_BUFFER_OVERFLOW;
    }
    uint32_t s = str_buf(k, c->a[1]);
    for (uint16_t i = 0; i < n; i += 2) {
        wr16(k, buf + i, downcase16(rd16(k, s + i)));
    }
    wr16(k, c->a[0], n);
    return ST_SUCCESS;
}
/* RtlMultiByteToUnicodeSize(*BytesInUnicode, MultiByteString, BytesInMultiByte) */
static uint64_t RtlMultiByteToUnicodeSize(KCall *c)
{
    vm_wr32(c->vm, c->a[0], c->a[2] * 2u);
    return ST_SUCCESS;
}
/* RtlUnicodeToMultiByteSize(*BytesInMultiByte, UnicodeString, BytesInUnicode) */
static uint64_t RtlUnicodeToMultiByteSize(KCall *c)
{
    vm_wr32(c->vm, c->a[0], c->a[2] / 2u);
    return ST_SUCCESS;
}

static unsigned fmt_uint(char *out, uint64_t v, unsigned base, bool upper)
{
    char t[72];
    unsigned n = 0;
    do {
        unsigned d = (unsigned)(v % base);
        t[n++] = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10);
        v /= base;
    } while (v);
    for (unsigned i = 0; i < n; i++) {
        out[i] = t[n - 1 - i];
    }
    return n;
}
/* RtlIntegerToChar(Value, Base, OutputLength, String) */
static uint64_t RtlIntegerToChar(KCall *c)
{
    unsigned base = c->a[1] ? c->a[1] : 10;
    if (base != 2 && base != 8 && base != 10 && base != 16) {
        return ST_INVALID_PARAMETER;
    }
    char t[72];
    unsigned n = fmt_uint(t, c->a[0], base, true);
    int32_t len = (int32_t)c->a[2];
    unsigned room = (unsigned)(len < 0 ? -len : len);
    if (n > room) {
        return ST_BUFFER_OVERFLOW;
    }
    if (len < 0) { /* alineado a la derecha, con ceros */
        for (unsigned i = 0; i < room - n; i++) {
            wr8(c->k, c->a[3] + i, '0');
        }
        vm_write(c->vm, c->a[3] + room - n, t, n);
    } else {
        vm_write(c->vm, c->a[3], t, n);
        if (n < room) {
            wr8(c->k, c->a[3] + n, 0);
        }
    }
    return ST_SUCCESS;
}
/* RtlIntegerToUnicodeString(Value, Base, String) */
static uint64_t RtlIntegerToUnicodeString(KCall *c)
{
    unsigned base = c->a[1] ? c->a[1] : 10;
    if (base != 2 && base != 8 && base != 10 && base != 16) {
        return ST_INVALID_PARAMETER;
    }
    char t[72];
    unsigned n = fmt_uint(t, c->a[0], base, true);
    if (n * 2u > str_max(c->k, c->a[2])) {
        return ST_BUFFER_OVERFLOW;
    }
    uint32_t d = str_buf(c->k, c->a[2]);
    for (unsigned i = 0; i < n; i++) {
        wr16(c->k, d + 2 * i, (uint8_t)t[i]);
    }
    wr16(c->k, c->a[2], (uint16_t)(n * 2u));
    if (n * 2u + 2u <= str_max(c->k, c->a[2])) {
        wr16(c->k, d + 2 * n, 0);
    }
    return ST_SUCCESS;
}
/* texto -> entero como RtlCharToInteger (espacios, signo, 0x / 0o / 0b) */
static uint64_t parse_int(Kernel *k, uint32_t p, unsigned n, unsigned unit, unsigned base, uint32_t out)
{
    unsigned i = 0;
#define CH(j) ((j) < n ? (unit == 2 ? rd16(k, p + 2 * (j)) : rd8(k, p + (j))) : 0)
    while (i < n && CH(i) && CH(i) <= ' ') {
        i++;
    }
    bool neg = false;
    if (CH(i) == '+' || CH(i) == '-') {
        neg = CH(i) == '-';
        i++;
    }
    if (!base) {
        base = 10;
        if (CH(i) == '0') {
            unsigned x = CH(i + 1) | 0x20u;
            unsigned nb = x == 'x' ? 16 : x == 'o' ? 8 : x == 'b' ? 2 : 0;
            if (nb) {
                base = nb;
                i += 2;
            }
        }
    } else if (base != 2 && base != 8 && base != 10 && base != 16) {
        return ST_INVALID_PARAMETER;
    }
    uint32_t v = 0;
    for (; i < n; i++) {
        unsigned ch = CH(i), d;
        if (ch >= '0' && ch <= '9') {
            d = ch - '0';
        } else if ((ch | 0x20u) >= 'a' && (ch | 0x20u) <= 'f') {
            d = (ch | 0x20u) - 'a' + 10;
        } else {
            break;
        }
        if (d >= base) {
            break;
        }
        v = v * base + d;
    }
#undef CH
    vm_wr32(k->vm, out, neg ? (uint32_t)-(int32_t)v : v);
    return ST_SUCCESS;
}
/* RtlUnicodeStringToInteger(String, Base, *Value) */
static uint64_t RtlUnicodeStringToInteger(KCall *c)
{
    return parse_int(c->k, str_buf(c->k, c->a[0]), str_len(c->k, c->a[0]) / 2u, 2, c->a[1], c->a[2]);
}

/* ---- formato de texto (sprintf del kernel: lo que usa el formato de la CRT) ---- */

typedef struct {
    Kernel *k;
    uint32_t dst, room, n; /* n: lo que haría falta; se escribe hasta room */
} FmtOut;

static void fo_put(FmtOut *o, unsigned ch)
{
    if (o->n < o->room) {
        wr8(o->k, o->dst + o->n, (uint8_t)ch);
    }
    o->n++;
}

static uint32_t k_format(Kernel *k, uint32_t dst, uint32_t room, uint32_t fmt, uint32_t ap)
{
    FmtOut o = {k, dst, room, 0};
    for (unsigned guard = 0; guard < 65536; guard++) {
        unsigned ch = rd8(k, fmt++);
        if (!ch) {
            break;
        }
        if (ch != '%') {
            fo_put(&o, ch);
            continue;
        }
        bool left = false, zero = false, plus = false, space = false, alt = false;
        for (;; fmt++) {
            ch = rd8(k, fmt);
            if (ch == '-') {
                left = true;
            } else if (ch == '0') {
                zero = true;
            } else if (ch == '+') {
                plus = true;
            } else if (ch == ' ') {
                space = true;
            } else if (ch == '#') {
                alt = true;
            } else {
                break;
            }
        }
        int width = 0, prec = -1;
        if (ch == '*') {
            width = (int32_t)vm_rd32(k->vm, ap);
            ap += 4;
            if (width < 0) {
                left = true;
                width = -width;
            }
            ch = rd8(k, ++fmt);
        } else {
            while (ch >= '0' && ch <= '9') {
                width = width * 10 + (int)(ch - '0');
                ch = rd8(k, ++fmt);
            }
        }
        if (ch == '.') {
            prec = 0;
            ch = rd8(k, ++fmt);
            if (ch == '*') {
                prec = (int32_t)vm_rd32(k->vm, ap);
                ap += 4;
                ch = rd8(k, ++fmt);
            } else {
                while (ch >= '0' && ch <= '9') {
                    prec = prec * 10 + (int)(ch - '0');
                    ch = rd8(k, ++fmt);
                }
            }
        }
        if (width > 512) {
            width = 512;
        }
        bool wide = false, ll = false;
        for (;; ch = rd8(k, ++fmt)) {
            if (ch == 'l') {
                if (rd8(k, fmt + 1) == 'l') {
                    ll = true;
                    fmt++;
                } else {
                    wide = true;
                }
            } else if (ch == 'w') {
                wide = true;
            } else if (ch == 'I' && rd8(k, fmt + 1) == '6' && rd8(k, fmt + 2) == '4') {
                ll = true;
                fmt += 2;
            } else if (ch != 'h') {
                break;
            }
        }
        fmt++;
        char body[80];
        unsigned bn = 0;
        const char *pre = "";
        switch (ch) {
        case 0:
            return o.n;
        case '%':
            fo_put(&o, '%');
            continue;
        case 'c':
        case 'C': {
            unsigned v = vm_rd32(k->vm, ap) & 0xFFu;
            ap += 4;
            for (int i = 1; !left && i < width; i++) {
                fo_put(&o, ' ');
            }
            fo_put(&o, v);
            for (int i = 1; left && i < width; i++) {
                fo_put(&o, ' ');
            }
            continue;
        }
        case 's':
        case 'S':
        case 'Z': {
            uint32_t p = vm_rd32(k->vm, ap);
            ap += 4;
            bool w = wide != (ch == 'S');
            unsigned len = 0;
            if (ch == 'Z') { /* STRING / UNICODE_STRING contados */
                len = p ? str_len(k, p) / (w ? 2u : 1u) : 0;
                p = p ? str_buf(k, p) : 0;
            } else if (!p) {
                static const char nul[] = "(null)";
                for (unsigned i = 0; i < 6 && (prec < 0 || (int)i < prec); i++) {
                    fo_put(&o, (unsigned)nul[i]);
                }
                continue;
            } else {
                while (len < 4096 && (prec < 0 || (int)len < prec) && (w ? rd16(k, p + 2 * len) : rd8(k, p + len))) {
                    len++;
                }
            }
            if (prec >= 0 && len > (unsigned)prec) {
                len = (unsigned)prec;
            }
            for (int i = (int)len; !left && i < width; i++) {
                fo_put(&o, ' ');
            }
            for (unsigned i = 0; i < len; i++) {
                fo_put(&o, w ? (rd16(k, p + 2 * i) & 0xFFu) : rd8(k, p + i));
            }
            for (int i = (int)len; left && i < width; i++) {
                fo_put(&o, ' ');
            }
            continue;
        }
        case 'd':
        case 'i': {
            int64_t v = ll ? (int64_t)(vm_rd32(k->vm, ap) | (uint64_t)vm_rd32(k->vm, ap + 4) << 32)
                           : (int32_t)vm_rd32(k->vm, ap);
            ap += ll ? 8 : 4;
            pre = v < 0 ? "-" : plus ? "+" : space ? " " : "";
            bn = fmt_uint(body, v < 0 ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v, 10, false);
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'o':
        case 'p': {
            uint64_t v = ll ? (vm_rd32(k->vm, ap) | (uint64_t)vm_rd32(k->vm, ap + 4) << 32) : vm_rd32(k->vm, ap);
            ap += ll ? 8 : 4;
            if (ch == 'p') {
                prec = 8;
            }
            bn = fmt_uint(body, v, ch == 'u' ? 10 : ch == 'o' ? 8 : 16, ch == 'X' || ch == 'p');
            if (alt && v && (ch == 'x' || ch == 'X')) {
                pre = ch == 'x' ? "0x" : "0X";
            }
            break;
        }
        case 'e':
        case 'E':
        case 'f':
        case 'g':
        case 'G': {
            uint64_t bits = vm_rd32(k->vm, ap) | (uint64_t)vm_rd32(k->vm, ap + 4) << 32;
            ap += 8;
            double dv;
            memcpy(&dv, &bits, 8);
            char spec[8] = "%.*", big[400];
            spec[3] = (char)ch;
            int m = snprintf(big, sizeof(big), spec, prec < 0 ? 6 : prec > 60 ? 60 : prec, dv < 0 ? -dv : dv);
            pre = dv < 0 ? "-" : plus ? "+" : space ? " " : "";
            int total = m + (int)strlen(pre);
            for (int i = total; !left && !zero && i < width; i++) {
                fo_put(&o, ' ');
            }
            for (const char *q = pre; *q; q++) {
                fo_put(&o, (unsigned)*q);
            }
            for (int i = total; !left && zero && i < width; i++) {
                fo_put(&o, '0');
            }
            for (int i = 0; i < m; i++) {
                fo_put(&o, (unsigned)big[i]);
            }
            for (int i = total; left && i < width; i++) {
                fo_put(&o, ' ');
            }
            continue;
        }
        default: /* desconocido: tal cual */
            fo_put(&o, '%');
            fo_put(&o, ch);
            continue;
        }
        /* enteros: prefijo, ceros de la precisión o del ancho, cifras */
        if (prec == 0 && bn == 1 && body[0] == '0') {
            bn = 0;
        }
        int zeros = prec > (int)bn ? prec - (int)bn : 0;
        if (zeros > 64) {
            zeros = 64;
        }
        int total = (int)strlen(pre) + zeros + (int)bn;
        if (zero && !left && prec < 0 && width > total) {
            zeros += width - total;
            total = width;
        }
        for (int i = total; !left && i < width; i++) {
            fo_put(&o, ' ');
        }
        for (const char *q = pre; *q; q++) {
            fo_put(&o, (unsigned)*q);
        }
        for (int i = 0; i < zeros; i++) {
            fo_put(&o, '0');
        }
        for (unsigned i = 0; i < bn; i++) {
            fo_put(&o, (unsigned)body[i]);
        }
        for (int i = total; left && i < width; i++) {
            fo_put(&o, ' ');
        }
    }
    return o.n;
}

/* cdecl: los argumentos se leen de la pila (KCall solo trae 12) */
static uint32_t k_arg_ptr(KCall *c, unsigned i) { return c->cpu->r[R_ESP] + 4 + 4 * i; }

/* RtlSprintf(buf, fmt, ...) / RtlVsprintf(buf, fmt, va) */
static uint64_t RtlSprintf(KCall *c)
{
    uint32_t n = k_format(c->k, c->a[0], 0x100000, c->a[1], k_arg_ptr(c, 2));
    wr8(c->k, c->a[0] + n, 0);
    return n;
}
static uint64_t RtlVsprintf(KCall *c)
{
    uint32_t n = k_format(c->k, c->a[0], 0x100000, c->a[1], c->a[2]);
    wr8(c->k, c->a[0] + n, 0);
    return n;
}
/* RtlSnprintf(buf, count, fmt, ...) / RtlVsnprintf(buf, count, fmt, va): como
 * _snprintf: -1 si no cabe, y el cero final solo si cabe */
static uint64_t snprintf_end(KCall *c, uint32_t n)
{
    if (n > c->a[1]) {
        return 0xFFFFFFFFu;
    }
    if (n < c->a[1]) {
        wr8(c->k, c->a[0] + n, 0);
    }
    return n;
}
static uint64_t RtlSnprintf(KCall *c)
{
    return snprintf_end(c, k_format(c->k, c->a[0], c->a[1], c->a[2], k_arg_ptr(c, 3)));
}
static uint64_t RtlVsnprintf(KCall *c) { return snprintf_end(c, k_format(c->k, c->a[0], c->a[1], c->a[2], c->a[3])); }

static void kextra_register(Kernel *k)
{
    kernel_impl(k, "InterlockedPushEntrySList", InterlockedPushEntrySList);
    kernel_impl(k, "InterlockedPopEntrySList", InterlockedPopEntrySList);
    kernel_impl(k, "InterlockedFlushSList", InterlockedFlushSList);
    kernel_impl(k, "ExfInterlockedInsertHeadList", ExfInterlockedInsertHeadList);
    kernel_impl(k, "ExfInterlockedInsertTailList", ExfInterlockedInsertTailList);
    kernel_impl(k, "ExfInterlockedRemoveHeadList", ExfInterlockedRemoveHeadList);
    kernel_impl(k, "ExInterlockedAddLargeInteger", ExInterlockedAddLargeInteger);
    kernel_impl(k, "ExInterlockedAddLargeStatistic", ExInterlockedAddLargeStatistic);
    kernel_impl(k, "ExInterlockedCompareExchange64", ExInterlockedCompareExchange64);
    kernel_impl(k, "RtlExtendedIntegerMultiply", RtlExtendedIntegerMultiply);
    kernel_impl(k, "RtlExtendedLargeIntegerDivide", RtlExtendedLargeIntegerDivide);
    kernel_impl(k, "RtlExtendedMagicDivide", RtlExtendedMagicDivide);
    kernel_impl(k, "RtlUlongByteSwap", RtlUlongByteSwap);
    kernel_impl(k, "RtlUshortByteSwap", RtlUshortByteSwap);
    kernel_impl(k, "RtlDowncaseUnicodeChar", RtlDowncaseUnicodeChar);
    kernel_impl(k, "RtlCompareUnicodeString", RtlCompareUnicodeString);
    kernel_impl(k, "RtlEqualUnicodeString", RtlEqualUnicodeString);
    kernel_impl(k, "RtlCopyUnicodeString", RtlCopyUnicodeString);
    kernel_impl(k, "RtlAppendStringToString", RtlAppendStringToString);
    kernel_impl(k, "RtlAppendUnicodeStringToString", RtlAppendUnicodeStringToString);
    kernel_impl(k, "RtlAppendUnicodeToString", RtlAppendUnicodeToString);
    kernel_impl(k, "RtlCreateUnicodeString", RtlCreateUnicodeString);
    kernel_impl(k, "RtlDowncaseUnicodeString", RtlDowncaseUnicodeString);
    kernel_impl(k, "RtlMultiByteToUnicodeSize", RtlMultiByteToUnicodeSize);
    kernel_impl(k, "RtlUnicodeToMultiByteSize", RtlUnicodeToMultiByteSize);
    kernel_impl(k, "RtlIntegerToChar", RtlIntegerToChar);
    kernel_impl(k, "RtlIntegerToUnicodeString", RtlIntegerToUnicodeString);
    kernel_impl(k, "RtlUnicodeStringToInteger", RtlUnicodeStringToInteger);
    kernel_impl(k, "RtlSprintf", RtlSprintf);
    kernel_impl(k, "RtlVsprintf", RtlVsprintf);
    kernel_impl(k, "RtlSnprintf", RtlSnprintf);
    kernel_impl(k, "RtlVsnprintf", RtlVsnprintf);
}
