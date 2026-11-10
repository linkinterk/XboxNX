/*
 * XboxNX — kernel de Xbox, etapa 3d: sistema.
 *
 * Rtl (cadenas, memoria, tiempo, códigos de error), la configuración de la
 * consola (EEPROM: idioma, video, región...), cerrojos de lectura/escritura,
 * secciones del XBE que se cargan a pedido, HAL (SMBus, bandeja, apagado),
 * AV (modos de video), red (PHY) y la criptografía Xc (SHA-1, HMAC, RC4, DES,
 * RSA).
 *
 * Semántica según hakuX (native/kernel/rtl.c, ex.c, xc.c y ke.c,
 * GPL-3.0-or-later); implementación propia salvo xc_crypto.c, que es de hakuX.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "disc.h"
#include "kernel.h"
#include "hw.h"
#include "xc_crypto.h"

#define ST_BUFFER_OVERFLOW 0x80000005u
#define ST_ACCESS_DENIED   0xC0000022u
#define ST_BUFFER_TOO_SMALL 0xC0000023u
#define ST_NAME_NOT_FOUND  0xC0000034u
#define ST_DATA_ERROR      0xC000009Cu

static uint8_t g_buf[1u << 16];

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static uint32_t get32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
static uint8_t rd8(Kernel *k, uint32_t va)
{
    uint8_t *p = vm_host(k->vm, va);
    return p ? *p : 0;
}
static void wr8(Kernel *k, uint32_t va, uint8_t v)
{
    uint8_t *p = vm_host(k->vm, va);
    if (p) {
        *p = v;
    }
}
static uint16_t rd16(Kernel *k, uint32_t va) { return (uint16_t)(rd8(k, va) | (rd8(k, va + 1) << 8)); }
static void wr16(Kernel *k, uint32_t va, uint16_t v)
{
    wr8(k, va, (uint8_t)v);
    wr8(k, va + 1, (uint8_t)(v >> 8));
}

/* ======================================================= Rtl: cadenas */

/* STRING / UNICODE_STRING { Length, MaximumLength, Buffer } */
static uint16_t str_len(Kernel *k, uint32_t s) { return rd16(k, s); }
static uint16_t str_max(Kernel *k, uint32_t s) { return rd16(k, s + 2); }
static uint32_t str_buf(Kernel *k, uint32_t s) { return vm_rd32(k->vm, s + 4); }
static void str_set(Kernel *k, uint32_t s, uint16_t len, uint16_t max, uint32_t buf)
{
    wr16(k, s, len);
    wr16(k, s + 2, max);
    vm_wr32(k->vm, s + 4, buf);
}

/* bloque del pool con su tamaño delante (para liberarlo) */
static uint32_t str_alloc(Kernel *k, uint32_t size)
{
    uint32_t p = kmm_pool_alloc(k, size + 16);
    if (!p) {
        return 0;
    }
    vm_wr32(k->vm, p, size);
    return p + 16;
}

static void str_free(Kernel *k, uint32_t buf)
{
    if (buf) {
        kmm_pool_free(k, buf - 16, vm_rd32(k->vm, buf - 16) + 16);
    }
}

/* RtlInitAnsiString(DestinationString, SourceString) */
static uint64_t RtlInitAnsiString(KCall *c)
{
    uint16_t len = 0;
    if (c->a[1]) {
        while (len < 0xFFFE && rd8(c->k, c->a[1] + len)) {
            len++;
        }
    }
    str_set(c->k, c->a[0], len, c->a[1] ? (uint16_t)(len + 1) : 0, c->a[1]);
    return 0;
}

/* RtlInitUnicodeString(DestinationString, SourceString) */
static uint64_t RtlInitUnicodeString(KCall *c)
{
    uint16_t len = 0;
    if (c->a[1]) {
        while (len < 0xFFFC && rd16(c->k, c->a[1] + len)) {
            len += 2;
        }
    }
    str_set(c->k, c->a[0], len, c->a[1] ? (uint16_t)(len + 2) : 0, c->a[1]);
    return 0;
}

static int str_compare(Kernel *k, uint32_t s1, uint32_t s2, bool nocase)
{
    uint32_t p1 = str_buf(k, s1), p2 = str_buf(k, s2);
    uint16_t n1 = str_len(k, s1), n2 = str_len(k, s2);
    for (uint16_t i = 0; i < n1 && i < n2; i++) {
        int a = rd8(k, p1 + i), b = rd8(k, p2 + i);
        if (nocase) {
            a = toupper(a);
            b = toupper(b);
        }
        if (a != b) {
            return a - b;
        }
    }
    return (int)n1 - (int)n2;
}

/* RtlCompareString(String1, String2, CaseInSensitive) */
static uint64_t RtlCompareString(KCall *c) { return (uint32_t)str_compare(c->k, c->a[0], c->a[1], c->a[2] & 0xFF); }

/* RtlEqualString(String1, String2, CaseInSensitive) */
static uint64_t RtlEqualString(KCall *c)
{
    return str_len(c->k, c->a[0]) == str_len(c->k, c->a[1]) && !str_compare(c->k, c->a[0], c->a[1], c->a[2] & 0xFF);
}

/* RtlCopyString(DestinationString, SourceString) */
static uint64_t RtlCopyString(KCall *c)
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
    uint32_t d = str_buf(k, c->a[0]), s = str_buf(k, c->a[1]);
    for (uint16_t i = 0; i < n; i++) {
        wr8(k, d + i, rd8(k, s + i));
    }
    wr16(k, c->a[0], n);
    return 0;
}

static uint16_t upcase16(uint16_t ch)
{
    if ((ch >= 'a' && ch <= 'z') || (ch >= 0xE0 && ch <= 0xFE && ch != 0xF7)) {
        return (uint16_t)(ch - 32);
    }
    if (ch == 0xFF) {
        return 0x178;
    }
    return ch;
}

static uint64_t RtlLowerChar(KCall *c)
{
    uint8_t ch = (uint8_t)c->a[0];
    return ch >= 'A' && ch <= 'Z' ? ch + 32u : ch;
}

static uint64_t RtlUpperChar(KCall *c)
{
    uint8_t ch = (uint8_t)c->a[0];
    return ch >= 'a' && ch <= 'z' ? ch - 32u : ch;
}

static uint64_t RtlUpcaseUnicodeChar(KCall *c) { return upcase16((uint16_t)c->a[0]); }

/* RtlUpcaseUnicodeString(DestinationString, SourceString, AllocateDestinationString) */
static uint64_t RtlUpcaseUnicodeString(KCall *c)
{
    Kernel *k = c->k;
    uint16_t n = str_len(k, c->a[1]) & ~1u;
    uint32_t buf = str_buf(k, c->a[0]);
    uint16_t max = str_max(k, c->a[0]);
    if (c->a[2] & 0xFF) {
        max = n;
        buf = str_alloc(k, max);
        if (!buf) {
            return ST_NO_MEMORY;
        }
    } else if (max < n) {
        return ST_BUFFER_OVERFLOW;
    }
    uint32_t src = str_buf(k, c->a[1]);
    for (uint16_t i = 0; i < n; i += 2) {
        wr16(k, buf + i, upcase16(rd16(k, src + i)));
    }
    str_set(k, c->a[0], n, max, buf);
    return ST_SUCCESS;
}

/* RtlUpcaseUnicodeToMultiByteN(MbString, MbSize, ResultSize, UnicodeString, UnicodeSize) */
static uint64_t RtlUpcaseUnicodeToMultiByteN(KCall *c)
{
    uint32_t n = c->a[4] / 2 < c->a[1] ? c->a[4] / 2 : c->a[1];
    for (uint32_t i = 0; i < n; i++) {
        uint16_t ch = upcase16(rd16(c->k, c->a[3] + 2 * i));
        wr8(c->k, c->a[0] + i, ch < 0x100 ? (uint8_t)ch : '?');
    }
    if (c->a[2]) {
        vm_wr32(c->vm, c->a[2], n);
    }
    return ST_SUCCESS;
}

/* RtlUnicodeToMultiByteN(MbString, MbSize, ResultSize, UnicodeString, UnicodeSize) */
static uint64_t RtlUnicodeToMultiByteN(KCall *c)
{
    uint32_t n = c->a[4] / 2 < c->a[1] ? c->a[4] / 2 : c->a[1];
    for (uint32_t i = 0; i < n; i++) {
        uint16_t ch = rd16(c->k, c->a[3] + 2 * i);
        wr8(c->k, c->a[0] + i, ch < 0x100 ? (uint8_t)ch : '?');
    }
    if (c->a[2]) {
        vm_wr32(c->vm, c->a[2], n);
    }
    return ST_SUCCESS;
}

/* RtlMultiByteToUnicodeN(UnicodeString, UnicodeSize, ResultSize, MbString, MbSize) */
static uint64_t RtlMultiByteToUnicodeN(KCall *c)
{
    uint32_t n = c->a[1] / 2 < c->a[4] ? c->a[1] / 2 : c->a[4];
    for (uint32_t i = 0; i < n; i++) {
        wr16(c->k, c->a[0] + 2 * i, rd8(c->k, c->a[3] + i));
    }
    if (c->a[2]) {
        vm_wr32(c->vm, c->a[2], n * 2);
    }
    return ST_SUCCESS;
}

/* RtlUpperString(DestinationString, SourceString) */
static uint64_t RtlUpperString(KCall *c)
{
    Kernel *k = c->k;
    uint16_t max = str_max(k, c->a[0]);
    uint16_t n = str_len(k, c->a[1]) < max ? str_len(k, c->a[1]) : max;
    uint32_t s = str_buf(k, c->a[1]), d = str_buf(k, c->a[0]);
    for (uint16_t i = 0; i < n; i++) {
        wr8(k, d + i, (uint8_t)toupper(rd8(k, s + i)));
    }
    wr16(k, c->a[0], n);
    return 0;
}

/* RtlAnsiStringToUnicodeString(DestinationString, SourceString, AllocateDestinationString) */
static uint64_t RtlAnsiStringToUnicodeString(KCall *c)
{
    Kernel *k = c->k;
    uint16_t n = str_len(k, c->a[1]);
    uint32_t buf = str_buf(k, c->a[0]);
    uint16_t max = str_max(k, c->a[0]);
    if (c->a[2] & 0xFF) {
        max = (uint16_t)(n * 2 + 2);
        buf = str_alloc(k, max);
        if (!buf) {
            return ST_NO_MEMORY;
        }
    } else if (max < n * 2) {
        return ST_BUFFER_OVERFLOW;
    }
    uint32_t src = str_buf(k, c->a[1]);
    for (uint16_t i = 0; i < n; i++) {
        wr16(k, buf + 2 * i, rd8(k, src + i));
    }
    if (max >= n * 2 + 2) {
        wr16(k, buf + 2 * n, 0);
    }
    str_set(k, c->a[0], (uint16_t)(n * 2), max, buf);
    return ST_SUCCESS;
}

/* RtlUnicodeStringToAnsiString(DestinationString, SourceString, AllocateDestinationString) */
static uint64_t RtlUnicodeStringToAnsiString(KCall *c)
{
    Kernel *k = c->k;
    uint16_t n = str_len(k, c->a[1]) / 2;
    uint32_t buf = str_buf(k, c->a[0]);
    uint16_t max = str_max(k, c->a[0]);
    if (c->a[2] & 0xFF) {
        max = (uint16_t)(n + 1);
        buf = str_alloc(k, max);
        if (!buf) {
            return ST_NO_MEMORY;
        }
    } else if (max < n) {
        return ST_BUFFER_OVERFLOW;
    }
    uint32_t src = str_buf(k, c->a[1]);
    for (uint16_t i = 0; i < n; i++) {
        uint16_t ch = rd16(k, src + 2 * i);
        wr8(k, buf + i, ch < 0x100 ? (uint8_t)ch : '?');
    }
    if (max > n) {
        wr8(k, buf + n, 0);
    }
    str_set(k, c->a[0], n, max, buf);
    return ST_SUCCESS;
}

/* RtlFreeAnsiString / RtlFreeUnicodeString(String) */
static uint64_t RtlFreeString(KCall *c)
{
    str_free(c->k, str_buf(c->k, c->a[0]));
    str_set(c->k, c->a[0], 0, 0, 0);
    return 0;
}

/* RtlCharToInteger(String, Base, Value) */
static uint64_t RtlCharToInteger(KCall *c)
{
    char s[40];
    unsigned i = 0;
    for (; i + 1 < sizeof(s); i++) {
        s[i] = (char)rd8(c->k, c->a[0] + i);
        if (!s[i]) {
            break;
        }
    }
    s[i] = 0;
    vm_wr32(c->vm, c->a[2], (uint32_t)strtoul(s, NULL, (int)c->a[1]));
    return ST_SUCCESS;
}

/* ======================================================= Rtl: memoria */

/* RtlCompareMemory(Source1, Source2, Length) -> bytes iguales */
static uint64_t RtlCompareMemory(KCall *c)
{
    uint32_t n = 0;
    while (n < c->a[2] && rd8(c->k, c->a[0] + n) == rd8(c->k, c->a[1] + n)) {
        n++;
    }
    return n;
}

/* RtlCompareMemoryUlong(Source, Length, Pattern) */
static uint64_t RtlCompareMemoryUlong(KCall *c)
{
    uint32_t n = 0;
    while (n + 4 <= c->a[1] && vm_rd32(c->vm, c->a[0] + n) == c->a[2]) {
        n += 4;
    }
    return n;
}

static void fill(Kernel *k, uint32_t va, uint32_t n, uint8_t v)
{
    memset(g_buf, v, n < sizeof(g_buf) ? n : sizeof(g_buf));
    for (uint32_t done = 0; done < n;) {
        uint32_t chunk = n - done < sizeof(g_buf) ? n - done : (uint32_t)sizeof(g_buf);
        vm_write(k->vm, va + done, g_buf, chunk);
        done += chunk;
    }
}

/* RtlZeroMemory(Destination, Length) */
static uint64_t RtlZeroMemory(KCall *c)
{
    fill(c->k, c->a[0], c->a[1], 0);
    return 0;
}

/* RtlFillMemory(Destination, Length, Fill) */
static uint64_t RtlFillMemory(KCall *c)
{
    fill(c->k, c->a[0], c->a[1], (uint8_t)c->a[2]);
    return 0;
}

/* RtlFillMemoryUlong(Destination, Length, Pattern) */
static uint64_t RtlFillMemoryUlong(KCall *c)
{
    for (uint32_t n = 0; n + 4 <= c->a[1]; n += 4) {
        vm_wr32(c->vm, c->a[0] + n, c->a[2]);
    }
    return 0;
}

/* RtlMoveMemory(Destination, Source, Length): con solapamiento */
static uint64_t RtlMoveMemory(KCall *c)
{
    uint32_t d = c->a[0], s = c->a[1], n = c->a[2];
    if (!n || d == s) {
        return 0;
    }
    bool back = d > s && d < s + n;
    for (uint32_t done = 0; done < n;) {
        uint32_t chunk = n - done < sizeof(g_buf) ? n - done : (uint32_t)sizeof(g_buf);
        uint32_t off = back ? n - done - chunk : done;
        vm_read(c->vm, s + off, g_buf, chunk);
        vm_write(c->vm, d + off, g_buf, chunk);
        done += chunk;
    }
    return 0;
}

/* ======================================================= Rtl: errores y tiempo */

static uint32_t status_to_dos(uint32_t status)
{
    if (status & 0x20000000u) {
        return status;
    }
    if ((status & 0xFFFF0000u) == 0x80070000u) {
        return status & 0xFFFFu;
    }
    if ((status & 0xF0000000u) == 0xD0000000u) {
        status &= 0xCFFFFFFFu;
    }
    static const uint32_t map[][2] = {
        {0x00000000u, 0},    {0x00000102u, 258},  {0x00000103u, 997}, {0x80000005u, 234}, {0x80000006u, 18},
        {0xC0000001u, 31},   {0xC0000002u, 1},    {0xC0000003u, 87},  {0xC0000008u, 6},   {0xC000000Du, 87},
        {0xC000000Fu, 2},    {0xC0000010u, 1},    {0xC0000011u, 38},  {0xC0000013u, 21},  {0xC0000017u, 8},
        {0xC0000018u, 487},  {0xC0000022u, 5},    {0xC0000023u, 122}, {0xC0000024u, 6},   {0xC000002Du, 487},
        {0xC0000033u, 123},  {0xC0000034u, 2},    {0xC0000035u, 183}, {0xC000003Au, 3},   {0xC0000043u, 32},
        {0xC0000047u, 298},  {0xC0000056u, 5},    {0xC000007Fu, 112}, {0xC000009Au, 1450}, {0xC000009Cu, 23},
        {0xC000009Fu, 487},  {0xC00000A0u, 487},  {0xC00000A2u, 19},  {0xC00000BAu, 5},   {0xC0000101u, 145},
        {0xC0000103u, 267},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (map[i][0] == status) {
            return map[i][1];
        }
    }
    return 317; /* ERROR_MR_MID_NOT_FOUND */
}

static uint64_t RtlNtStatusToDosError(KCall *c) { return status_to_dos(c->a[0]); }

uint32_t kernel_status_to_dos(uint32_t status) { return status_to_dos(status); }

static bool leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static const int k_mdays[2][12] = {
    {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
    {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
};

/* RtlTimeToTimeFields(Time, TimeFields): Year, Month, Day, Hour, Minute, Second, Milliseconds, Weekday */
static uint64_t RtlTimeToTimeFields(KCall *c)
{
    uint64_t t = (uint64_t)vm_rd32(c->vm, c->a[0]) | ((uint64_t)vm_rd32(c->vm, c->a[0] + 4) << 32);
    uint64_t ms = t / 10000u, days = ms / 86400000u;
    uint32_t ms_day = (uint32_t)(ms % 86400000u);
    int16_t tf[8];
    tf[7] = (int16_t)((days + 1) % 7); /* 1601-01-01 fue lunes */
    int y = 1601;
    while (days >= (uint64_t)(leap(y) ? 366 : 365)) {
        days -= leap(y) ? 366 : 365;
        y++;
    }
    int m = 0;
    while (days >= (uint64_t)k_mdays[leap(y)][m]) {
        days -= (uint64_t)k_mdays[leap(y)][m];
        m++;
    }
    tf[0] = (int16_t)y;
    tf[1] = (int16_t)(m + 1);
    tf[2] = (int16_t)(days + 1);
    tf[3] = (int16_t)(ms_day / 3600000u);
    tf[4] = (int16_t)(ms_day / 60000u % 60);
    tf[5] = (int16_t)(ms_day / 1000u % 60);
    tf[6] = (int16_t)(ms_day % 1000u);
    vm_write(c->vm, c->a[1], tf, sizeof(tf));
    return 0;
}

/* RtlTimeFieldsToTime(TimeFields, Time) -> BOOLEAN */
static uint64_t RtlTimeFieldsToTime(KCall *c)
{
    int16_t tf[8];
    vm_read(c->vm, c->a[0], tf, sizeof(tf));
    if (tf[0] < 1601 || tf[1] < 1 || tf[1] > 12 || tf[2] < 1 || tf[2] > k_mdays[leap(tf[0])][tf[1] - 1]) {
        return 0;
    }
    uint64_t days = 0;
    for (int y = 1601; y < tf[0]; y++) {
        days += leap(y) ? 366 : 365;
    }
    for (int m = 0; m < tf[1] - 1; m++) {
        days += (uint64_t)k_mdays[leap(tf[0])][m];
    }
    days += (uint64_t)(tf[2] - 1);
    uint64_t ms = days * 86400000ull + (uint64_t)tf[3] * 3600000u + (uint64_t)tf[4] * 60000u +
                  (uint64_t)tf[5] * 1000u + (uint64_t)tf[6];
    uint64_t t = ms * 10000u;
    vm_wr32(c->vm, c->a[1], (uint32_t)t);
    vm_wr32(c->vm, c->a[1] + 4, (uint32_t)(t >> 32));
    return 1;
}

/* ======================================================= configuración (EEPROM) */

/* La EEPROM del Xbox (256 bytes): sección cifrada 0x00-0x2F (región del
 * juego en 0x2C), de fábrica 0x30-0x5F (serie 0x34, MAC 0x40, clave en
 * línea 0x48, región de video 0x58), de usuario 0x60-0xBF (zona horaria
 * 0x64, idioma 0x90, video 0x94, audio 0x98, control parental 0x9C...,
 * región DVD 0xBC). Aquí la sección "cifrada" se guarda en claro. */
#define EEPROM_SIZE 0x100u
#define REG_BINARY 3
#define REG_DWORD 4

typedef struct NvSetting {
    uint16_t index, offset, size;
} NvSetting;

static const NvSetting k_nv[] = {
    {0x00, 0x64, 4},   {0x01, 0x68, 4},    {0x02, 0x6C, 4},    {0x03, 0x78, 4},    {0x04, 0x7C, 4},
    {0x05, 0x88, 4},   {0x06, 0x8C, 4},    {0x07, 0x90, 4},    {0x08, 0x94, 4},    {0x09, 0x98, 4},
    {0x0A, 0x9C, 4},   {0x0B, 0xA0, 4},    {0x0C, 0xA4, 4},    {0x0D, 0xA8, 4},    {0x0E, 0xAC, 4},
    {0x0F, 0xB0, 4},   {0x10, 0xB4, 4},    {0x11, 0xB8, 4},    {0x12, 0xBC, 4},    {0x100, 0x34, 12},
    {0x101, 0x40, 6},  {0x102, 0x48, 16},  {0x103, 0x58, 4},   {0x104, 0x2C, 4},   {0xFF, 0x60, 0x60},
    {0x1FF, 0x30, 0x30}, {0xFFFE, 0x00, 0x30}, {0xFFFF, 0x00, EEPROM_SIZE},
};

static uint8_t g_eeprom[EEPROM_SIZE];
static bool g_eeprom_ready;

static uint32_t eeprom_checksum(const uint8_t *p, unsigned len)
{
    uint32_t high = 0, low = 0;
    for (unsigned i = 0; i + 4 <= len; i += 4) {
        uint32_t v = get32(p + i);
        high += (uint32_t)(((uint64_t)low + v) >> 32);
        low += v;
    }
    return ~(high + low);
}

static void eeprom_seal(void)
{
    put32(g_eeprom + 0x30, eeprom_checksum(g_eeprom + 0x34, 0x2C));
    put32(g_eeprom + 0x60, eeprom_checksum(g_eeprom + 0x64, 0x5C));
}

static void eeprom_path(Kernel *k, char *p, size_t n) { snprintf(p, n, "%s/eeprom.bin", k->hdd_root); }

static void eeprom_store(Kernel *k)
{
    char p[260];
    eeprom_path(k, p, sizeof(p));
    FILE *f = fopen(p, "wb");
    if (f) {
        fwrite(g_eeprom, 1, sizeof(g_eeprom), f);
        fclose(f);
    }
}

/* La región del juego según su certificado (cabecera 0x118, GameRegion 0xA0):
 * 1 Norteamérica, 2 Japón, 4 resto del mundo. */
static uint32_t game_region(Kernel *k)
{
    if (!k->xbe_base) {
        return 1;
    }
    uint32_t cert = vm_rd32(k->vm, k->xbe_base + 0x118);
    uint32_t r = cert ? vm_rd32(k->vm, cert + 0xA0) : 1;
    return r & 1 ? 1 : r & 2 ? 2 : r & 4 ? 4 : 1;
}

static uint8_t *eeprom(Kernel *k)
{
    if (g_eeprom_ready) {
        return g_eeprom;
    }
    char p[260];
    eeprom_path(k, p, sizeof(p));
    FILE *f = fopen(p, "rb");
    bool have = f && fread(g_eeprom, 1, sizeof(g_eeprom), f) == sizeof(g_eeprom);
    if (f) {
        fclose(f);
    }
    if (!have) {
        /* una consola nueva: inglés, estéreo, sin control parental, una
         * identidad propia */
        memset(g_eeprom, 0, sizeof(g_eeprom));
        memcpy(g_eeprom + 0x34, "XBOXNX000001", 12);
        static const uint8_t mac[6] = {0x00, 0x50, 0xF2, 0x4E, 0x58, 0x01};
        memcpy(g_eeprom + 0x40, mac, 6);
        static const uint8_t hd_key[16] = {'0', '0', '0', '0', '0', '0', '0', '0', '0', '1',
                                           0x00, 0x50, 0xF2, 0x00, 0x00, 0x01};
        memcpy(g_eeprom + 0x1C, hd_key, 16);
        put32(g_eeprom + 0x90, 1); /* inglés */
    }
    /* la región y el video siguen al juego (NTSC o PAL) */
    uint32_t region = game_region(k);
    bool pal = region == 4;
    put32(g_eeprom + 0x2C, region);
    put32(g_eeprom + 0x58, pal ? 0x00800300u : 0x00400100u);
    put32(g_eeprom + 0xBC, pal ? 2 : 1);
    eeprom_seal();
    if (!have) {
        eeprom_store(k);
    }
    g_eeprom_ready = true;
    /* XboxHDKey: con ella se firman las partidas guardadas */
    uint32_t hd = kernel_data_va(k, "XboxHDKey");
    if (hd) {
        vm_write(k->vm, hd, g_eeprom + 0x1C, 16);
    }
    return g_eeprom;
}

static const NvSetting *nv_find(uint32_t index)
{
    for (unsigned i = 0; i < sizeof(k_nv) / sizeof(k_nv[0]); i++) {
        if (k_nv[i].index == index) {
            return &k_nv[i];
        }
    }
    return NULL;
}

/* ExQueryNonVolatileSetting(ValueIndex, Type, Value, ValueLength, ResultLength) */
static uint64_t ExQueryNonVolatileSetting(KCall *c)
{
    const NvSetting *s = nv_find(c->a[0]);
    if (!s) {
        return ST_NAME_NOT_FOUND;
    }
    if (c->a[1]) {
        vm_wr32(c->vm, c->a[1], s->size == 4 ? REG_DWORD : REG_BINARY);
    }
    if (c->a[4]) {
        vm_wr32(c->vm, c->a[4], s->size);
    }
    if (c->a[3] < s->size) {
        return ST_BUFFER_TOO_SMALL;
    }
    vm_write(c->vm, c->a[2], eeprom(c->k) + s->offset, s->size);
    return ST_SUCCESS;
}

/* ExSaveNonVolatileSetting(ValueIndex, Type, Value, ValueLength): solo la sección de usuario */
static uint64_t ExSaveNonVolatileSetting(KCall *c)
{
    const NvSetting *s = nv_find(c->a[0]);
    if (!s || s->index > 0x103 || (s->index >= 0x100 && s->index != 0x103)) {
        return ST_ACCESS_DENIED;
    }
    uint8_t *e = eeprom(c->k);
    uint32_t n = c->a[3] < s->size ? c->a[3] : s->size;
    memset(e + s->offset, 0, s->size);
    vm_read(c->vm, c->a[2], e + s->offset, n);
    eeprom_seal();
    eeprom_store(c->k);
    return ST_SUCCESS;
}

/* ======================================================= cerrojos de lectura/escritura */

/* ERWLOCK: LockCount 0, WritersWaitingCount 4, ReadersWaitingCount 8,
 * ReadersEntryCount 0xC, WriterEvent (KEVENT) 0x10, ReaderSemaphore 0x20 */
#define RW_LOCK_COUNT    0x00
#define RW_WRITERS_WAIT  0x04
#define RW_READERS_WAIT  0x08
#define RW_READERS_ENTRY 0x0C
#define RW_WRITER_EVENT  0x10
#define RW_READER_SEM    0x20

static void rw_add(Kernel *k, uint32_t va, int32_t d) { vm_wr32(k->vm, va, vm_rd32(k->vm, va) + (uint32_t)d); }

static void header(Kernel *k, uint32_t o, uint8_t type, uint8_t size, int32_t signal)
{
    wr8(k, o, type);
    wr8(k, o + 1, 0);
    wr8(k, o + 2, size);
    wr8(k, o + 3, 0);
    vm_wr32(k->vm, o + 4, (uint32_t)signal);
    vm_wr32(k->vm, o + 8, o + 8);
    vm_wr32(k->vm, o + 12, o + 8);
}

static uint64_t ExInitializeReadWriteLock(KCall *c)
{
    Kernel *k = c->k;
    uint32_t l = c->a[0];
    vm_wr32(k->vm, l + RW_LOCK_COUNT, 0xFFFFFFFFu);
    vm_wr32(k->vm, l + RW_WRITERS_WAIT, 0);
    vm_wr32(k->vm, l + RW_READERS_WAIT, 0);
    vm_wr32(k->vm, l + RW_READERS_ENTRY, 0);
    header(k, l + RW_WRITER_EVENT, 1, 4, 0);
    header(k, l + RW_READER_SEM, 5, 5, 0);
    vm_wr32(k->vm, l + RW_READER_SEM + 0x10, 0x7FFFFFFFu);
    return 0;
}

static uint64_t ExAcquireReadWriteLockExclusive(KCall *c)
{
    Kernel *k = c->k;
    uint32_t l = c->a[0];
    rw_add(k, l + RW_LOCK_COUNT, 1);
    if (vm_rd32(k->vm, l + RW_LOCK_COUNT) != 0) {
        rw_add(k, l + RW_WRITERS_WAIT, 1);
        kps_wait_one(c, l + RW_WRITER_EVENT, "ExAcquireReadWriteLockExclusive");
    }
    return 0;
}

static uint64_t ExAcquireReadWriteLockShared(KCall *c)
{
    Kernel *k = c->k;
    uint32_t l = c->a[0];
    bool held = (int32_t)vm_rd32(k->vm, l + RW_LOCK_COUNT) >= 0;
    bool must_wait = held && (vm_rd32(k->vm, l + RW_READERS_ENTRY) == 0 || vm_rd32(k->vm, l + RW_WRITERS_WAIT) != 0);
    rw_add(k, l + RW_LOCK_COUNT, 1);
    if (must_wait) {
        rw_add(k, l + RW_READERS_WAIT, 1);
        kps_wait_one(c, l + RW_READER_SEM, "ExAcquireReadWriteLockShared");
    } else {
        rw_add(k, l + RW_READERS_ENTRY, 1);
    }
    return 0;
}

static uint64_t ExReleaseReadWriteLock(KCall *c)
{
    Kernel *k = c->k;
    uint32_t l = c->a[0];
    rw_add(k, l + RW_LOCK_COUNT, -1);
    if ((int32_t)vm_rd32(k->vm, l + RW_LOCK_COUNT) == -1) {
        vm_wr32(k->vm, l + RW_READERS_ENTRY, 0);
        return 0;
    }
    if (vm_rd32(k->vm, l + RW_READERS_ENTRY) == 0) {
        uint32_t readers = vm_rd32(k->vm, l + RW_READERS_WAIT);
        if (readers) {
            vm_wr32(k->vm, l + RW_READERS_WAIT, 0);
            vm_wr32(k->vm, l + RW_READERS_ENTRY, readers);
            rw_add(k, l + RW_READER_SEM + 4, (int32_t)readers);
            kps_signaled(c);
            return 0;
        }
    } else {
        rw_add(k, l + RW_READERS_ENTRY, -1);
        if (vm_rd32(k->vm, l + RW_READERS_ENTRY) != 0) {
            return 0;
        }
    }
    /* el siguiente escritor */
    rw_add(k, l + RW_WRITERS_WAIT, -1);
    vm_wr32(k->vm, l + RW_WRITER_EVENT + 4, 1);
    kps_signaled(c);
    return 0;
}

/* ======================================================= secciones del XBE */

/* Cabecera de sección: Flags 0, VirtualAddress 4, VirtualSize 8, FileAddress
 * 0xC, FileSize 0x10, SectionName 0x14, SectionReferenceCount 0x18. */
static uint64_t XeLoadSection(KCall *c)
{
    Kernel *k = c->k;
    uint32_t s = c->a[0];
    uint32_t refs = vm_rd32(k->vm, s + 0x18);
    char name[16] = "";
    uint32_t np = vm_rd32(k->vm, s + 0x14);
    for (unsigned i = 0; np && i + 1 < sizeof(name); i++) {
        name[i] = (char)rd8(k, np + i);
        if (!name[i]) {
            break;
        }
    }
    snprintf(c->note, sizeof(c->note), "%s", name);
    vm_wr32(k->vm, s + 0x18, refs + 1);
    if (refs) {
        return ST_SUCCESS;
    }
    uint32_t va = vm_rd32(k->vm, s + 0x04), vsize = vm_rd32(k->vm, s + 0x08);
    uint32_t raw = vm_rd32(k->vm, s + 0x0C), rsize = vm_rd32(k->vm, s + 0x10);
    if (!vm_alloc_at(k->vm, va & ~(VM_PAGE - 1), (va & (VM_PAGE - 1)) + vsize)) {
        vm_wr32(k->vm, s + 0x18, 0);
        return ST_NO_MEMORY;
    }
    fill(k, va, vsize, 0);
    uint32_t n = rsize < vsize ? rsize : vsize;
    if (!k->disc) {
        return ST_DATA_ERROR;
    }
    XDiscFile xf = {k->xbe_sector, k->xbe_size, 0};
    for (uint32_t done = 0; done < n;) {
        uint32_t chunk = n - done < sizeof(g_buf) ? n - done : (uint32_t)sizeof(g_buf);
        if (!xdisc_read(k->disc, &xf, raw + done, g_buf, chunk) || !vm_write(k->vm, va + done, g_buf, chunk)) {
            return ST_DATA_ERROR;
        }
        done += chunk;
    }
    return ST_SUCCESS;
}

/* XeUnloadSection(Section): la memoria se queda (otra sección puede compartir páginas) */
static uint64_t XeUnloadSection(KCall *c)
{
    uint32_t refs = vm_rd32(c->vm, c->a[0] + 0x18);
    if (!refs) {
        return ST_INVALID_PARAMETER;
    }
    vm_wr32(c->vm, c->a[0] + 0x18, refs - 1);
    return ST_SUCCESS;
}

/* ======================================================= depuración */

/* DbgPrint(Format, ...): el texto va al registro */
static uint64_t DbgPrint(KCall *c)
{
    char s[64];
    unsigned i = 0;
    for (; i + 1 < sizeof(s); i++) {
        s[i] = (char)rd8(c->k, c->a[0] + i);
        if (!s[i] || s[i] == '\n') {
            break;
        }
    }
    s[i] = 0;
    snprintf(c->note, sizeof(c->note), "%s", s);
    return 0;
}

static uint64_t Nothing(KCall *c)
{
    (void)c;
    return 0;
}

/* ======================================================= HAL, AV, red */

#define AV_QUERY_AV_CAPABILITIES 6u

/* AvGetSavedDataAddress() */
static uint64_t AvGetSavedDataAddress(KCall *c) { return c->k->av_saved; }

/* AvSetSavedDataAddress(Address) */
static uint64_t AvSetSavedDataAddress(KCall *c)
{
    c->k->av_saved = c->a[0];
    return 0;
}

/* AvSendTVEncoderOption(RegisterBase, Option, Param, Result): el cable es de
 * componentes (HDTV) y los modos del video de la EEPROM */
static uint64_t AvSendTVEncoderOption(KCall *c)
{
    uint32_t r = 0;
    if (c->a[1] == AV_QUERY_AV_CAPABILITIES) {
        const uint8_t *e = eeprom(c->k);
        r = 4 | (get32(e + 0x58) & (0x00000F00u | 0x00C00000u)) | (get32(e + 0x94) & ~(0x00000F00u | 0xFFu));
    }
    if (c->a[3]) {
        vm_wr32(c->vm, c->a[3], r);
    }
    return 0;
}

/* AvSetDisplayMode(RegisterBase, Step, Mode, Format, Pitch, FrameBuffer): etapa 4 */
static uint64_t AvSetDisplayMode(KCall *c)
{
    char b[64];
    snprintf(b, sizeof(b), "modo %08X, pitch %u, fb %08X", (unsigned)c->a[2], (unsigned)c->a[4], (unsigned)c->a[5]);
    snprintf(c->note, sizeof(c->note), "%s", b);
    hw_set_display(c->k, c->a[3], c->a[4], c->a[5]);
    return 0;
}

/* HalReadWritePCISpace(BusNumber, SlotNumber, RegisterNumber, Buffer, Length, WritePCISpace) */
static uint64_t HalReadWritePCISpace(KCall *c)
{
    if (!(c->a[5] & 0xFF)) {
        fill(c->k, c->a[3], c->a[4], 0);
    }
    return 0;
}

/* HalReadSMBusValue(Address, Command, ReadWord, DataValue): el SMC (versión,
 * bandeja cerrada con disco, cable de componentes) */
static uint64_t HalReadSMBusValue(KCall *c)
{
    static const char version[3] = {'P', '0', '1'};
    static unsigned vi;
    uint32_t v = 0;
    if ((c->a[0] & 0xFE) == 0x20) {
        switch (c->a[1]) {
        case 0x01: v = (uint8_t)version[vi++ % 3]; break;
        case 0x03: v = c->k->disc ? 0x60 : 0x40; break;
        case 0x04: v = 0x01; break;
        default: break;
        }
    }
    if (c->a[3]) {
        vm_wr32(c->vm, c->a[3], v);
    }
    return ST_SUCCESS;
}

/* HalReadSMCTrayState(TrayState, TrayStateChangeCount) */
static uint64_t HalReadSMCTrayState(KCall *c)
{
    vm_wr32(c->vm, c->a[0], c->k->disc ? 0x60 : 0x40);
    if (c->a[1]) {
        vm_wr32(c->vm, c->a[1], 0);
    }
    return ST_SUCCESS;
}

static uint64_t FscGetCacheSize(KCall *c)
{
    (void)c;
    return 16;
}

/* PhyGetLinkState(Update): sin red todavía */
static uint64_t PhyGetLinkState(KCall *c)
{
    (void)c;
    return 0;
}

static uint64_t Success(KCall *c)
{
    (void)c;
    return ST_SUCCESS;
}

/* ======================================================= criptografía Xc */

/* Los contextos viven en la memoria del juego: se copian, se usan y vuelven. */
static bool ctx_in(Kernel *k, uint32_t va, void *ctx, size_t n) { return vm_read(k->vm, va, ctx, n); }
static void ctx_out(Kernel *k, uint32_t va, const void *ctx, size_t n) { vm_write(k->vm, va, ctx, n); }

/* Lee [va, va+n) en un búfer del host (malloc); NULL si no se puede. */
static uint8_t *guest_copy(Kernel *k, uint32_t va, uint32_t n)
{
    uint8_t *p = malloc(n ? n : 1);
    if (p && n && !vm_read(k->vm, va, p, n)) {
        free(p);
        return NULL;
    }
    return p;
}

/* XcSHAInit(pbSHAContext) */
static uint64_t XcSHAInit(KCall *c)
{
    NcSha1 s;
    nc_sha1_init(&s);
    ctx_out(c->k, c->a[0], &s, sizeof(s));
    return 0;
}

/* XcSHAUpdate(pbSHAContext, pbInput, dwInputLength) */
static uint64_t XcSHAUpdate(KCall *c)
{
    NcSha1 s;
    ctx_in(c->k, c->a[0], &s, sizeof(s));
    for (uint32_t done = 0; done < c->a[2];) {
        uint32_t chunk = c->a[2] - done < sizeof(g_buf) ? c->a[2] - done : (uint32_t)sizeof(g_buf);
        vm_read(c->vm, c->a[1] + done, g_buf, chunk);
        nc_sha1_update(&s, g_buf, chunk);
        done += chunk;
    }
    ctx_out(c->k, c->a[0], &s, sizeof(s));
    return 0;
}

/* XcSHAFinal(pbSHAContext, pbDigest) */
static uint64_t XcSHAFinal(KCall *c)
{
    NcSha1 s;
    uint8_t d[20];
    ctx_in(c->k, c->a[0], &s, sizeof(s));
    nc_sha1_final(&s, d);
    ctx_out(c->k, c->a[0], &s, sizeof(s));
    vm_write(c->vm, c->a[1], d, sizeof(d));
    return 0;
}

/* XcHMAC(pbKey, dwKeyLength, pbInput, dwInputLength, pbInput2, dwInputLength2, pbDigest) */
static uint64_t XcHMAC(KCall *c)
{
    uint8_t *key = guest_copy(c->k, c->a[0], c->a[1]);
    uint8_t *in1 = c->a[2] ? guest_copy(c->k, c->a[2], c->a[3]) : NULL;
    uint8_t *in2 = c->a[4] ? guest_copy(c->k, c->a[4], c->a[5]) : NULL;
    uint8_t d[20];
    if (key) {
        nc_hmac_sha1(key, c->a[1], in1, in1 ? c->a[3] : 0, in2, in2 ? c->a[5] : 0, d);
        vm_write(c->vm, c->a[6], d, sizeof(d));
    }
    free(key);
    free(in1);
    free(in2);
    return 0;
}

/* XcRC4Key(pbKeyStruct, dwKeyLength, pbKey) */
static uint64_t XcRC4Key(KCall *c)
{
    NcRc4 r;
    uint8_t *key = guest_copy(c->k, c->a[2], c->a[1]);
    if (key) {
        nc_rc4_key(&r, key, c->a[1]);
        ctx_out(c->k, c->a[0], &r, sizeof(r));
    }
    free(key);
    return 0;
}

/* XcRC4Crypt(pbKeyStruct, dwInputLength, pbInput) */
static uint64_t XcRC4Crypt(KCall *c)
{
    NcRc4 r;
    ctx_in(c->k, c->a[0], &r, sizeof(r));
    for (uint32_t done = 0; done < c->a[1];) {
        uint32_t chunk = c->a[1] - done < sizeof(g_buf) ? c->a[1] - done : (uint32_t)sizeof(g_buf);
        vm_read(c->vm, c->a[2] + done, g_buf, chunk);
        nc_rc4_crypt(&r, g_buf, chunk);
        vm_write(c->vm, c->a[2] + done, g_buf, chunk);
        done += chunk;
    }
    ctx_out(c->k, c->a[0], &r, sizeof(r));
    return 0;
}

#define XC_DES3 1

/* XcKeyTable(dwCipher, pbKeyTable, pbKey) */
static uint64_t XcKeyTable(KCall *c)
{
    NcDes d[3];
    unsigned keys = c->a[0] == XC_DES3 ? 3 : 1;
    for (unsigned i = 0; i < keys; i++) {
        uint8_t key[8];
        vm_read(c->vm, c->a[2] + 8 * i, key, 8);
        nc_des_key(&d[i], key);
    }
    ctx_out(c->k, c->a[1], d, keys * sizeof(NcDes));
    return 0;
}

static void des_block(uint32_t cipher, const NcDes *d, uint8_t *out, const uint8_t *in, bool enc)
{
    if (cipher == XC_DES3) {
        nc_des3_block(d, out, in, enc);
    } else {
        nc_des_block(d, out, in, enc);
    }
}

/* XcBlockCrypt(dwCipher, pbOutput, pbInput, pbKeyTable, dwOp) */
static uint64_t XcBlockCrypt(KCall *c)
{
    NcDes d[3];
    uint8_t in[8], out[8];
    ctx_in(c->k, c->a[3], d, (c->a[0] == XC_DES3 ? 3 : 1) * sizeof(NcDes));
    vm_read(c->vm, c->a[2], in, 8);
    des_block(c->a[0], d, out, in, c->a[4] != 0);
    vm_write(c->vm, c->a[1], out, 8);
    return 0;
}

/* XcBlockCryptCBC(dwCipher, dwInputLength, pbOutput, pbInput, pbKeyTable, dwOp, pbFeedback) */
static uint64_t XcBlockCryptCBC(KCall *c)
{
    NcDes d[3];
    uint8_t fb[8];
    ctx_in(c->k, c->a[4], d, (c->a[0] == XC_DES3 ? 3 : 1) * sizeof(NcDes));
    vm_read(c->vm, c->a[6], fb, 8);
    for (uint32_t off = 0; off + 8 <= c->a[1]; off += 8) {
        uint8_t in[8], x[8], out[8];
        vm_read(c->vm, c->a[3] + off, in, 8);
        if (c->a[5]) {
            for (int i = 0; i < 8; i++) {
                x[i] = in[i] ^ fb[i];
            }
            des_block(c->a[0], d, out, x, true);
            memcpy(fb, out, 8);
        } else {
            des_block(c->a[0], d, x, in, false);
            for (int i = 0; i < 8; i++) {
                out[i] = x[i] ^ fb[i];
            }
            memcpy(fb, in, 8);
        }
        vm_write(c->vm, c->a[2] + off, out, 8);
    }
    vm_write(c->vm, c->a[6], fb, 8);
    return 0;
}

/* XcDESKeyParity(pbKey, dwKeyLength) */
static uint64_t XcDESKeyParity(KCall *c)
{
    uint8_t *key = guest_copy(c->k, c->a[0], c->a[1]);
    if (key) {
        nc_des_parity(key, c->a[1]);
        vm_write(c->vm, c->a[0], key, c->a[1]);
    }
    free(key);
    return 0;
}

/* XcModExp(pA, pB, pC, pD, dwN): A = B^C mod D en dwN palabras */
static uint64_t XcModExp(KCall *c)
{
    unsigned n = c->a[4];
    if (!n || n > 1024) {
        return 0;
    }
    uint32_t *r = calloc(n, 4);
    uint32_t *b = (uint32_t *)guest_copy(c->k, c->a[1], n * 4);
    uint32_t *e = (uint32_t *)guest_copy(c->k, c->a[2], n * 4);
    uint32_t *m = (uint32_t *)guest_copy(c->k, c->a[3], n * 4);
    bool ok = r && b && e && m && nc_modexp(r, b, e, m, n);
    if (ok) {
        vm_write(c->vm, c->a[0], r, n * 4);
    }
    free(r);
    free(b);
    free(e);
    free(m);
    return ok;
}

/* XcPKGetKeyLen(pbPubKey) */
static uint64_t XcPKGetKeyLen(KCall *c) { return vm_rd32(c->vm, c->a[0] + 4); }

/* La operación de clave pública sobre una clave BSAFE ("RSA1", keylen,
 * bitlen, datalen, exponente, módulo): out = in^e mod n. */
static bool pk_public(Kernel *k, uint32_t pub, uint32_t in_va, uint8_t *out)
{
    uint32_t keylen = vm_rd32(k->vm, pub + 4), bitlen = vm_rd32(k->vm, pub + 8);
    if (vm_rd32(k->vm, pub) != 0x31415352u || keylen < bitlen / 8 || keylen > 1024 || keylen % 4) {
        return false;
    }
    unsigned n = keylen / 4;
    uint32_t *x = calloc(n, 4), *e = calloc(n, 4), *r = calloc(n, 4);
    uint32_t *m = (uint32_t *)guest_copy(k, pub + 0x14, keylen);
    bool ok = x && e && r && m && vm_read(k->vm, in_va, x, bitlen / 8);
    if (ok) {
        e[0] = vm_rd32(k->vm, pub + 0x10);
        ok = nc_modexp(r, x, e, m, n);
    }
    if (ok) {
        memcpy(out, r, keylen);
    }
    free(x);
    free(e);
    free(r);
    free(m);
    return ok;
}

/* XcPKEncPublic(pbPubKey, pbInput, pbOutput) */
static uint64_t XcPKEncPublic(KCall *c)
{
    uint8_t out[1024];
    uint32_t keylen = vm_rd32(c->vm, c->a[0] + 4);
    if (!pk_public(c->k, c->a[0], c->a[1], out)) {
        return 0;
    }
    vm_write(c->vm, c->a[2], out, keylen);
    return 1;
}

/* XcVerifyPKCS1Signature(pbSig, pbPubKey, pbDigest): la firma descifrada
 * termina en el resumen (en cualquier orden de bytes) */
static uint64_t XcVerifyPKCS1Signature(KCall *c)
{
    uint8_t le[1024], digest[20];
    if (!pk_public(c->k, c->a[1], c->a[0], le)) {
        return 0;
    }
    uint32_t len = vm_rd32(c->vm, c->a[1] + 8) / 8;
    vm_read(c->vm, c->a[2], digest, 20);
    bool fwd = true, rev = true;
    for (uint32_t i = 0; i < 20; i++) {
        fwd &= le[19 - i] == digest[i];          /* el final del bloque big-endian */
        rev &= le[i] == digest[19 - i];
    }
    (void)len;
    return fwd || rev;
}

void ksys_register(Kernel *k)
{
    g_eeprom_ready = false;
    kernel_impl(k, "RtlInitAnsiString", RtlInitAnsiString);
    kernel_impl(k, "RtlInitUnicodeString", RtlInitUnicodeString);
    kernel_impl(k, "RtlCompareString", RtlCompareString);
    kernel_impl(k, "RtlEqualString", RtlEqualString);
    kernel_impl(k, "RtlCopyString", RtlCopyString);
    kernel_impl(k, "RtlLowerChar", RtlLowerChar);
    kernel_impl(k, "RtlUpperChar", RtlUpperChar);
    kernel_impl(k, "RtlUpcaseUnicodeChar", RtlUpcaseUnicodeChar);
    kernel_impl(k, "RtlUpcaseUnicodeString", RtlUpcaseUnicodeString);
    kernel_impl(k, "RtlUpcaseUnicodeToMultiByteN", RtlUpcaseUnicodeToMultiByteN);
    kernel_impl(k, "RtlUnicodeToMultiByteN", RtlUnicodeToMultiByteN);
    kernel_impl(k, "RtlMultiByteToUnicodeN", RtlMultiByteToUnicodeN);
    kernel_impl(k, "RtlUpperString", RtlUpperString);
    kernel_impl(k, "RtlAnsiStringToUnicodeString", RtlAnsiStringToUnicodeString);
    kernel_impl(k, "RtlUnicodeStringToAnsiString", RtlUnicodeStringToAnsiString);
    kernel_impl(k, "RtlFreeAnsiString", RtlFreeString);
    kernel_impl(k, "RtlFreeUnicodeString", RtlFreeString);
    kernel_impl(k, "RtlCharToInteger", RtlCharToInteger);
    kernel_impl(k, "RtlCompareMemory", RtlCompareMemory);
    kernel_impl(k, "RtlCompareMemoryUlong", RtlCompareMemoryUlong);
    kernel_impl(k, "RtlZeroMemory", RtlZeroMemory);
    kernel_impl(k, "RtlFillMemory", RtlFillMemory);
    kernel_impl(k, "RtlFillMemoryUlong", RtlFillMemoryUlong);
    kernel_impl(k, "RtlMoveMemory", RtlMoveMemory);
    kernel_impl(k, "RtlNtStatusToDosError", RtlNtStatusToDosError);
    kernel_impl(k, "RtlTimeToTimeFields", RtlTimeToTimeFields);
    kernel_impl(k, "RtlTimeFieldsToTime", RtlTimeFieldsToTime);
    kernel_impl(k, "ExQueryNonVolatileSetting", ExQueryNonVolatileSetting);
    kernel_impl(k, "ExSaveNonVolatileSetting", ExSaveNonVolatileSetting);
    kernel_impl(k, "ExInitializeReadWriteLock", ExInitializeReadWriteLock);
    kernel_impl(k, "ExAcquireReadWriteLockExclusive", ExAcquireReadWriteLockExclusive);
    kernel_impl(k, "ExAcquireReadWriteLockShared", ExAcquireReadWriteLockShared);
    kernel_impl(k, "ExReleaseReadWriteLock", ExReleaseReadWriteLock);
    kernel_impl(k, "XeLoadSection", XeLoadSection);
    kernel_impl(k, "XeUnloadSection", XeUnloadSection);
    kernel_impl(k, "DbgPrint", DbgPrint);
    kernel_impl(k, "DbgBreakPoint", Nothing);
    kernel_impl(k, "AvGetSavedDataAddress", AvGetSavedDataAddress);
    kernel_impl(k, "AvSetSavedDataAddress", AvSetSavedDataAddress);
    kernel_impl(k, "AvSendTVEncoderOption", AvSendTVEncoderOption);
    kernel_impl(k, "AvSetDisplayMode", AvSetDisplayMode);
    kernel_impl(k, "HalReadWritePCISpace", HalReadWritePCISpace);
    kernel_impl(k, "HalRegisterShutdownNotification", Nothing);
    kernel_impl(k, "HalIsResetOrShutdownPending", Nothing);
    kernel_impl(k, "HalReadSMBusValue", HalReadSMBusValue);
    kernel_impl(k, "HalWriteSMBusValue", Success);
    kernel_impl(k, "HalReadSMCTrayState", HalReadSMCTrayState);
    kernel_impl(k, "HalEnableSecureTrayEject", Nothing);
    kernel_impl(k, "FscGetCacheSize", FscGetCacheSize);
    kernel_impl(k, "FscSetCacheSize", Success);
    kernel_impl(k, "PhyGetLinkState", PhyGetLinkState);
    kernel_impl(k, "PhyInitialize", Success);
    kernel_impl(k, "XcSHAInit", XcSHAInit);
    kernel_impl(k, "XcSHAUpdate", XcSHAUpdate);
    kernel_impl(k, "XcSHAFinal", XcSHAFinal);
    kernel_impl(k, "XcHMAC", XcHMAC);
    kernel_impl(k, "XcRC4Key", XcRC4Key);
    kernel_impl(k, "XcRC4Crypt", XcRC4Crypt);
    kernel_impl(k, "XcKeyTable", XcKeyTable);
    kernel_impl(k, "XcBlockCrypt", XcBlockCrypt);
    kernel_impl(k, "XcBlockCryptCBC", XcBlockCryptCBC);
    kernel_impl(k, "XcDESKeyParity", XcDESKeyParity);
    kernel_impl(k, "XcModExp", XcModExp);
    kernel_impl(k, "XcPKGetKeyLen", XcPKGetKeyLen);
    kernel_impl(k, "XcPKEncPublic", XcPKEncPublic);
    kernel_impl(k, "XcVerifyPKCS1Signature", XcVerifyPKCS1Signature);
    kernel_impl(k, "XcCryptService", Nothing);
    kernel_impl(k, "XcUpdateCrypto", Nothing);
}
