/*
 * XboxNX — kernel de Xbox, etapa 3c: archivos, discos y dispositivos.
 *
 * \Device\CdRom0 es el disco del juego (XDVDFS, solo lectura) leído de la
 * ISO; \Device\Harddisk0\PartitionN son carpetas en la SD
 * (sd:/switch/XboxNX/hdd/PartitionN), con nombres sin distinguir mayúsculas
 * como FATX. Los nombres \??\X: son enlaces simbólicos (IoCreateSymbolicLink);
 * D: empieza en el disco. Los sectores "crudos" de la partición 0 y de las de
 * caché (3-5) que XAPI lee y escribe se guardan en raw-partitionN.bin.
 *
 * Los nombres del Xbox son ANSI: OBJECT_ATTRIBUTES { RootDirectory,
 * ObjectName (ANSI_STRING*), Attributes }. La E/S termina en la llamada: el
 * IoStatusBlock y el evento quedan puestos antes de volver.
 *
 * Semántica y distribución de estructuras según hakuX (native/kernel/io.c,
 * GPL-3.0-or-later); implementación propia sobre stdio para la Switch.
 */
/* recortar nombres largos en el registro es a propósito */
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "disc.h"
#include "kernel.h"

#define ST_PENDING                   0x00000103u
#define ST_NO_MORE_FILES             0x80000006u
#define ST_INVALID_INFO_CLASS        0xC0000003u
#define ST_NO_SUCH_FILE              0xC000000Fu
#define ST_INVALID_DEVICE_REQUEST    0xC0000010u
#define ST_END_OF_FILE               0xC0000011u
#define ST_NO_MEDIA                  0xC0000013u
#define ST_ACCESS_DENIED             0xC0000022u
#define ST_BUFFER_TOO_SMALL          0xC0000023u
#define ST_OBJECT_NAME_NOT_FOUND     0xC0000034u
#define ST_OBJECT_NAME_COLLISION     0xC0000035u
#define ST_OBJECT_PATH_NOT_FOUND     0xC000003Au
#define ST_DELETE_PENDING            0xC0000056u
#define ST_DISK_FULL                 0xC000007Fu
#define ST_DEVICE_DATA_ERROR         0xC000009Cu
#define ST_MEDIA_WRITE_PROTECTED     0xC00000A2u
#define ST_FILE_IS_A_DIRECTORY       0xC00000BAu
#define ST_DIRECTORY_NOT_EMPTY       0xC0000101u
#define ST_NOT_A_DIRECTORY           0xC0000103u

#define FILE_SUPERSEDE    0
#define FILE_OPEN         1
#define FILE_CREATE       2
#define FILE_OPEN_IF      3
#define FILE_OVERWRITE    4
#define FILE_OVERWRITE_IF 5
#define FILE_DIRECTORY_FILE     0x01u
#define FILE_SYNCHRONOUS_IO     0x30u
#define FILE_NON_DIRECTORY_FILE 0x40u

#define OB_DOS_DEVICES 0xFFFFFFFDu
#define FILE_ATTRIBUTE_READONLY  0x01u
#define FILE_ATTRIBUTE_DIRECTORY 0x10u
#define FILE_ATTRIBUTE_NORMAL    0x80u
#define DISC_TIME 0x01C0739BEB8F8000ull /* 2001-01-01 */
#define ACCESS_WRITES (0x2u | 0x4u | 0x40000000u | 0x10000000u | 0x02000000u)

/* FILE_OBJECT: Type +0, FsContext +8, Lock (KEVENT) +0x28, Event (KEVENT)
 * +0x38; después nuestro índice y el "FCB" al que apunta FsContext. */
#define FILE_OBJECT_SIZE 0x70u
#define FO_FSCONTEXT 0x08u
#define FO_LOCK      0x28u
#define FO_INDEX     0x48u
#define FO_FCB       0x4Cu

#define MAX_FILES 512
#define MAX_LINKS 64
#define MAX_DEVICES 32
#define RAW_KEEP 0x80000u
#define DISC_SECTOR 2048u

/* El disco duro de la consola (8 GB): offset y tamaño de cada partición. */
#define HDD_BYTES 0x1DD156000ull
static const uint64_t k_parts[6][2] = {
    {0x00000000ull, 0x00080000ull}, {0xABE80000ull, 0x1312D6000ull}, {0x8CA80000ull, 0x1F400000ull},
    {0x00080000ull, 0x2EE00000ull}, {0x2EE80000ull, 0x2EE00000ull}, {0x5DC80000ull, 0x2EE00000ull},
};

typedef enum { F_DISC, F_HOST, F_VOLUME, F_LINK } FKind;

typedef struct DirEntry {
    char name[64];
    uint64_t size;
    bool dir;
} DirEntry;

typedef struct KFile {
    bool used;
    FKind kind;
    char path[300];     /* ruta canónica del Xbox */
    XDiscFile disc;     /* F_DISC */
    char host[320];     /* F_HOST */
    FILE *fp;
    bool is_dir, async, can_write, del, wrote;
    int partition;      /* -1 disco, 0..5 partición */
    uint64_t pos;
    unsigned dir_next;
    char mask[64];
    DirEntry *listing;
    unsigned nlisting;
} KFile;

typedef struct KLink {
    char name[48];
    char target[200];
} KLink;

static KFile g_files[MAX_FILES];
static KLink g_links[MAX_LINKS];
static struct {
    char name[64];
    uint32_t object;
} g_devices[MAX_DEVICES];
static uint32_t g_cluster_sectors[6];
static uint8_t g_buf[1u << 16];

/* ------------------------------------------------------------- utilidades */

static void note(KCall *c, const char *fmt, const char *s)
{
    snprintf(c->note, sizeof(c->note), fmt, s);
}

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
static uint32_t get32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static bool prefix_ci(const char *s, const char *p)
{
    for (; *p; s++, p++) {
        if (toupper((unsigned char)*s) != toupper((unsigned char)*p)) {
            return false;
        }
    }
    return true;
}

/* ANSI_STRING { Length, MaximumLength, Buffer } */
static void ansi(Kernel *k, uint32_t str, char *out, size_t cap)
{
    out[0] = 0;
    if (!str || !vm_host(k->vm, str)) {
        return;
    }
    uint16_t len = (uint16_t)(vm_rd32(k->vm, str) & 0xFFFF);
    uint32_t buf = vm_rd32(k->vm, str + 4);
    if (len >= cap) {
        len = (uint16_t)(cap - 1);
    }
    if (!vm_read(k->vm, buf, out, len)) {
        len = 0;
    }
    out[len] = 0;
}

static bool wildcard(const char *pat, const char *s)
{
    if (!*pat) {
        return !*s;
    }
    if (*pat == '*') {
        for (;; s++) {
            if (wildcard(pat + 1, s)) {
                return true;
            }
            if (!*s) {
                return false;
            }
        }
    }
    return *s && (*pat == '?' || toupper((unsigned char)*pat) == toupper((unsigned char)*s)) &&
           wildcard(pat + 1, s + 1);
}

static void io_status(Kernel *k, uint32_t iosb, uint32_t status, uint32_t info)
{
    if (iosb) {
        vm_wr32(k->vm, iosb, status);
        vm_wr32(k->vm, iosb + 4, info);
    }
}

static uint64_t align4k(uint64_t v) { return (v + 4095) & ~4095ull; }

/* ------------------------------------------------------------- nombres */

static int link_find(const char *name)
{
    for (int i = 0; i < MAX_LINKS; i++) {
        if (g_links[i].name[0] && !strcasecmp(g_links[i].name, name)) {
            return i;
        }
    }
    return -1;
}

static bool link_set(const char *name, const char *target)
{
    int i = link_find(name);
    for (int j = 0; i < 0 && j < MAX_LINKS; j++) {
        if (!g_links[j].name[0]) {
            i = j;
        }
    }
    if (i < 0) {
        return false;
    }
    snprintf(g_links[i].name, sizeof(g_links[i].name), "%s", name);
    snprintf(g_links[i].target, sizeof(g_links[i].target), "%s", target);
    return true;
}

/* Ruta canónica: separadores '\', sin repetidos, '.' y '..' resueltos y los
 * enlaces \??\X: sustituidos. false si la unidad no existe. */
static bool canon(const char *in, char *out, size_t cap)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", in);
    for (char *p = tmp; *p; p++) {
        if (*p == '/') {
            *p = '\\';
        }
    }
    if (prefix_ci(tmp, "\\??\\")) {
        char *colon = strchr(tmp + 4, ':');
        if (colon) {
            char name[48];
            size_t n = (size_t)(colon - tmp) + 1;
            if (n >= sizeof(name)) {
                return false;
            }
            memcpy(name, tmp, n);
            name[n] = 0;
            int l = link_find(name);
            if (l < 0) {
                return false;
            }
            char joined[512];
            snprintf(joined, sizeof(joined), "%s\\%s", g_links[l].target, colon + 1);
            snprintf(tmp, sizeof(tmp), "%s", joined);
        }
    }
    char *parts[64];
    int n = 0;
    char *save = NULL;
    for (char *tok = strtok_r(tmp, "\\", &save); tok; tok = strtok_r(NULL, "\\", &save)) {
        if (!strcmp(tok, ".")) {
            continue;
        }
        if (!strcmp(tok, "..")) {
            if (n) {
                n--;
            }
            continue;
        }
        if (n < 64) {
            parts[n++] = tok;
        }
    }
    size_t len = 0;
    out[0] = 0;
    for (int i = 0; i < n && len + 2 < cap; i++) {
        len += (size_t)snprintf(out + len, cap - len, "\\%s", parts[i]);
    }
    if (!n) {
        snprintf(out, cap, "\\");
    }
    return true;
}

/* -1 el disco, 0..5 una partición, -2 desconocido; rel: la ruta dentro. */
static int device_of(const char *path, const char **rel)
{
    static const char cd[] = "\\Device\\CdRom0";
    static const char hd[] = "\\Device\\Harddisk0\\Partition";
    if (prefix_ci(path, cd) && (!path[sizeof(cd) - 1] || path[sizeof(cd) - 1] == '\\')) {
        *rel = path + sizeof(cd) - 1;
        return -1;
    }
    if (prefix_ci(path, hd) && isdigit((unsigned char)path[sizeof(hd) - 1])) {
        const char *p = path + sizeof(hd) - 1;
        int part = *p - '0';
        p++;
        if ((*p && *p != '\\') || part > 5) {
            return -2;
        }
        *rel = p;
        return part;
    }
    return -2;
}

static void mkdirs(const char *path)
{
    char tmp[400];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0775);
            *p = '/';
        }
    }
    mkdir(tmp, 0775);
}

/* Ruta en la SD de algo dentro de una partición, buscando cada nombre sin
 * distinguir mayúsculas. 1 si existe, 0 si solo falta el último, -1 si
 * falta una carpeta. */
static int host_path(Kernel *k, int part, const char *rel, char *out, size_t cap)
{
    size_t len = (size_t)snprintf(out, cap, "%s/Partition%d", k->hdd_root, part);
    mkdirs(out);
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s", rel);
    char *save = NULL;
    for (char *tok = strtok_r(tmp, "\\", &save); tok; tok = strtok_r(NULL, "\\", &save)) {
        bool last = !save || !*save;
        DIR *d = opendir(out);
        char match[256] = "";
        struct dirent *de;
        while (d && (de = readdir(d))) {
            if (!strcasecmp(de->d_name, tok)) {
                snprintf(match, sizeof(match), "%s", de->d_name);
                break;
            }
        }
        if (d) {
            closedir(d);
        }
        len += (size_t)snprintf(out + len, cap > len ? cap - len : 0, "/%s", match[0] ? match : tok);
        if (!match[0]) {
            return last ? 0 : -1;
        }
    }
    return 1;
}

/* Sectores crudos que deben durar: los de configuración (partición 0) y la
 * cabeza de cada partición de caché, en raw-partitionN.bin. */
static void raw_io(Kernel *k, int part, uint64_t off, uint8_t *buf, uint32_t len, bool write)
{
    uint32_t n = off < RAW_KEEP ? (uint32_t)(RAW_KEEP - off < len ? RAW_KEEP - off : len) : 0;
    if (!write) {
        memset(buf, 0, len);
    }
    if (!n) {
        return;
    }
    char p[260];
    snprintf(p, sizeof(p), "%s/raw-partition%d.bin", k->hdd_root, part);
    FILE *f = fopen(p, write ? "r+b" : "rb");
    if (!f && write) {
        f = fopen(p, "w+b");
    }
    if (!f) {
        return;
    }
    if (fseeko(f, (off_t)off, SEEK_SET) == 0) {
        if (write) {
            fwrite(buf, 1, n, f);
        } else {
            size_t got = fread(buf, 1, n, f);
            (void)got;
        }
    }
    fclose(f);
}

static void remove_tree(const char *dir, bool self)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    while (d && (de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) {
            continue;
        }
        char p[600];
        struct stat st;
        snprintf(p, sizeof(p), "%s/%s", dir, de->d_name);
        if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
            remove_tree(p, true);
        } else {
            remove(p);
        }
    }
    if (d) {
        closedir(d);
    }
    if (self) {
        rmdir(dir);
    }
}

/* ------------------------------------------------------------- archivos */

static KFile *file_new(void)
{
    for (int i = 0; i < MAX_FILES; i++) {
        if (!g_files[i].used) {
            memset(&g_files[i], 0, sizeof(g_files[i]));
            g_files[i].used = true;
            return &g_files[i];
        }
    }
    return NULL;
}

static void file_drop(Kernel *k, KFile *f)
{
    if (f->fp) {
        fclose(f->fp);
    }
    if (f->del && f->kind == F_HOST) {
        bool other = false;
        for (int i = 0; i < MAX_FILES; i++) {
            other |= &g_files[i] != f && g_files[i].used && !strcmp(g_files[i].host, f->host);
        }
        if (!other) {
            if (f->is_dir) {
                rmdir(f->host);
            } else {
                remove(f->host);
            }
        }
    }
    free(f->listing);
    memset(f, 0, sizeof(*f));
    if (k->files_open) {
        k->files_open--;
    }
}

static void event_init(Kernel *k, uint32_t ev, uint8_t type, bool signaled)
{
    uint8_t *p = vm_host(k->vm, ev);
    p[0] = type;
    p[1] = 0;
    p[2] = 4;
    p[3] = 0;
    vm_wr32(k->vm, ev + 4, signaled);
    vm_wr32(k->vm, ev + 8, ev + 8);
    vm_wr32(k->vm, ev + 12, ev + 8);
}

static uint32_t file_handle(Kernel *k, KFile *f)
{
    ObType type = f->kind == F_LINK ? OB_SYMLINK : OB_FILE;
    uint32_t obj = kob_create(k, type, FILE_OBJECT_SIZE);
    if (!obj) {
        return 0;
    }
    for (uint32_t i = 0; i < FILE_OBJECT_SIZE; i += 4) {
        vm_wr32(k->vm, obj + i, 0);
    }
    vm_wr32(k->vm, obj, 5); /* IO_TYPE_FILE */
    event_init(k, obj + FO_LOCK, 1, false);
    event_init(k, obj + K_FO_EVENT, 0, false);
    vm_wr32(k->vm, obj + FO_INDEX, (uint32_t)(f - g_files));
    vm_wr32(k->vm, obj + FO_FSCONTEXT, obj + FO_FCB);
    if (f->kind == F_DISC) {
        vm_wr32(k->vm, obj + FO_FCB, f->disc.sector); /* GDFX: el sector de inicio */
    } else {
        vm_wr32(k->vm, obj + FO_FCB, 1);              /* FATX: sin clave */
    }
    k->files_open++;
    return kob_handle(k, obj, type);
}

static KFile *file_of(Kernel *k, uint32_t handle)
{
    ObType type;
    uint32_t obj = kob_lookup(k, handle, &type);
    if (!obj || (type != OB_FILE && type != OB_SYMLINK)) {
        return NULL;
    }
    uint32_t i = vm_rd32(k->vm, obj + FO_INDEX);
    return i < MAX_FILES && g_files[i].used ? &g_files[i] : NULL;
}

void kio_close(Kernel *k, uint32_t object)
{
    uint32_t i = vm_rd32(k->vm, object + FO_INDEX);
    if (i < MAX_FILES && g_files[i].used) {
        file_drop(k, &g_files[i]);
    }
}

static uint64_t file_size(const KFile *f)
{
    if (f->kind == F_DISC) {
        return f->disc.size;
    }
    struct stat st;
    return f->kind == F_HOST && !f->is_dir && stat(f->host, &st) == 0 ? (uint64_t)st.st_size : 0;
}

/* Nombre relativo a RootDirectory: \?? para las rutas DOS de XAPI ("D:\x"),
 * o la carpeta de un handle abierto. */
static bool resolve_name(Kernel *k, uint32_t root, char *name, size_t cap)
{
    char joined[600];
    if (root == OB_DOS_DEVICES) {
        snprintf(joined, sizeof(joined), "\\??\\%s", name);
    } else if (root) {
        const KFile *rf = file_of(k, root);
        if (!rf) {
            return false;
        }
        snprintf(joined, sizeof(joined), "%s\\%s", rf->path, name);
    } else {
        return true;
    }
    snprintf(name, cap, "%s", joined);
    return true;
}

/* Abre (y crea) para NtCreateFile/NtOpenFile. *info: lo que va al IoStatusBlock. */
static uint32_t do_open(KCall *c, uint32_t access, uint32_t attrs, uint32_t disposition, uint32_t options,
                        uint32_t *handle, uint32_t *info)
{
    Kernel *k = c->k;
    char name[300], path[300];
    ansi(k, vm_rd32(k->vm, attrs + 4), name, sizeof(name));
    note(c, "%s", name);
    if (!resolve_name(k, vm_rd32(k->vm, attrs), name, sizeof(name))) {
        return ST_INVALID_HANDLE;
    }
    if (!canon(name, path, sizeof(path))) {
        return ST_OBJECT_PATH_NOT_FOUND;
    }
    note(c, "%s", path);
    const char *rel;
    int dev = device_of(path, &rel);
    *info = 5; /* FILE_DOES_NOT_EXIST */
    KFile *f = NULL;
    if (dev == -2) {
        const char *last = strrchr(path, '\\');
        size_t at = last ? (size_t)(last - path) : 0;
        bool in_dir = at == 0 || (at == 3 && !strncmp(path, "\\??", 3)) || (at == 7 && prefix_ci(path, "\\Device"));
        return in_dir ? ST_OBJECT_NAME_NOT_FOUND : ST_OBJECT_PATH_NOT_FOUND;
    }
    if (dev == -1) {
        if (!k->disc) {
            return ST_NO_MEDIA;
        }
        if (disposition != FILE_OPEN && disposition != FILE_OPEN_IF) {
            return ST_MEDIA_WRITE_PROTECTED;
        }
        XDiscFile df;
        if (!xdisc_find(k->disc, rel, &df)) {
            return ST_OBJECT_NAME_NOT_FOUND;
        }
        bool dir = df.attr & 0x10;
        if ((options & FILE_DIRECTORY_FILE) && !dir) {
            return ST_NOT_A_DIRECTORY;
        }
        if ((options & FILE_NON_DIRECTORY_FILE) && dir) {
            return ST_FILE_IS_A_DIRECTORY;
        }
        f = file_new();
        if (!f) {
            return ST_INSUFFICIENT_RESOURCES;
        }
        f->kind = *rel ? F_DISC : F_VOLUME;
        f->partition = -1;
        f->disc = df;
        f->is_dir = dir;
        *info = 1; /* FILE_OPENED */
    } else if (dev == 0) {
        /* la partición 0 es el disco entero, abierto crudo */
        if (*rel) {
            return ST_OBJECT_NAME_NOT_FOUND;
        }
        f = file_new();
        if (!f) {
            return ST_INSUFFICIENT_RESOURCES;
        }
        f->kind = F_VOLUME;
        f->partition = 0;
        *info = 1;
    } else {
        char host[320];
        int exists = host_path(k, dev, rel, host, sizeof(host));
        if (exists < 0) {
            return ST_OBJECT_PATH_NOT_FOUND;
        }
        struct stat st;
        bool is_dir = exists && stat(host, &st) == 0 && S_ISDIR(st.st_mode);
        if (!*rel) {
            is_dir = true;
        }
        if (exists && !is_dir) {
            for (int i = 0; i < MAX_FILES; i++) {
                if (g_files[i].used && g_files[i].del && !strcmp(g_files[i].host, host)) {
                    return ST_DELETE_PENDING;
                }
            }
        }
        if (exists && disposition == FILE_CREATE) {
            return ST_OBJECT_NAME_COLLISION;
        }
        if (!exists && (disposition == FILE_OPEN || disposition == FILE_OVERWRITE)) {
            return ST_OBJECT_NAME_NOT_FOUND;
        }
        if (exists && is_dir && (options & FILE_NON_DIRECTORY_FILE)) {
            return ST_FILE_IS_A_DIRECTORY;
        }
        if (exists && !is_dir && (options & FILE_DIRECTORY_FILE)) {
            return ST_NOT_A_DIRECTORY;
        }
        FILE *fp = NULL;
        if (!exists && (options & FILE_DIRECTORY_FILE)) {
            if (mkdir(host, 0775) != 0) {
                return ST_ACCESS_DENIED;
            }
            is_dir = true;
            *info = 2; /* FILE_CREATED */
        } else if (!is_dir) {
            const char *mode = "r+b";
            if (!exists) {
                mode = "w+b";
                *info = 2;
            } else if (disposition == FILE_OVERWRITE || disposition == FILE_OVERWRITE_IF ||
                       disposition == FILE_SUPERSEDE) {
                mode = "w+b";
                *info = disposition == FILE_SUPERSEDE ? 0 : 3;
            } else {
                *info = 1;
            }
            fp = fopen(host, mode);
            if (!fp) {
                return ST_ACCESS_DENIED;
            }
        } else {
            *info = 1;
        }
        f = file_new();
        if (!f) {
            if (fp) {
                fclose(fp);
            }
            return ST_INSUFFICIENT_RESOURCES;
        }
        f->kind = F_HOST;
        f->partition = dev;
        f->fp = fp;
        f->is_dir = is_dir;
        snprintf(f->host, sizeof(f->host), "%s", host);
    }
    snprintf(f->path, sizeof(f->path), "%s", path);
    f->async = !(options & FILE_SYNCHRONOUS_IO);
    f->can_write = (access & ACCESS_WRITES) != 0;
    *handle = file_handle(k, f);
    if (!*handle) {
        file_drop(k, f);
        return ST_INSUFFICIENT_RESOURCES;
    }
    return ST_SUCCESS;
}

/* NtCreateFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, AllocationSize,
 *   FileAttributes, ShareAccess, CreateDisposition, CreateOptions) */
static uint64_t NtCreateFile(KCall *c)
{
    uint32_t h = 0, info = 0;
    uint32_t st = do_open(c, c->a[1], c->a[2], c->a[7], c->a[8], &h, &info);
    io_status(c->k, c->a[3], st, info);
    if (st == ST_SUCCESS) {
        vm_wr32(c->vm, c->a[0], h);
    }
    return st;
}

/* NtOpenFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, ShareAccess, OpenOptions) */
static uint64_t NtOpenFile(KCall *c)
{
    uint32_t h = 0, info = 0;
    uint32_t st = do_open(c, c->a[1], c->a[2], FILE_OPEN, c->a[5], &h, &info);
    io_status(c->k, c->a[3], st, info);
    if (st == ST_SUCCESS) {
        vm_wr32(c->vm, c->a[0], h);
    }
    return st;
}

/* ------------------------------------------------------------- leer, escribir */

static void set_event(KCall *c, uint32_t ev)
{
    vm_wr32(c->vm, ev + 4, 1);
    kps_signaled(c);
}

/* Terminó una E/S con a[] = (FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, ...):
 * su evento (o el del objeto archivo) queda señalado. */
static void complete(KCall *c)
{
    if (c->a[1]) {
        uint32_t ev = kob_lookup(c->k, c->a[1], NULL);
        if (ev) {
            set_event(c, ev);
        }
    } else {
        ObType t;
        uint32_t fo = kob_lookup(c->k, c->a[0], &t);
        if (fo && t == OB_FILE) {
            set_event(c, fo + K_FO_EVENT);
        }
    }
}

/* Una petición asíncrona que terminó informa STATUS_PENDING (como el DMA del
 * Xbox); una que falla de entrada devuelve el error y deja todo como estaba. */
/* Para la pantalla: "L ...ruta" o "E ...ruta" (el final de la ruta). */
static void set_last_io(Kernel *k, char kind, const char *path)
{
    size_t n = strlen(path), cap = sizeof(k->last_io) - 3;
    snprintf(k->last_io, sizeof(k->last_io), "%c %s", kind, n > cap ? path + n - cap : path);
}

static uint32_t io_finish(KCall *c, const KFile *f, uint32_t st, uint32_t n, bool pend)
{
    if (f->async && st >= 0xC0000000u) {
        return st;
    }
    io_status(c->k, c->a[4], st, n);
    complete(c);
    return pend && f->async && (c->a[1] || c->a[2] || f->kind == F_DISC) ? ST_PENDING : st;
}

static uint64_t io_offset(Kernel *k, const KFile *f, uint32_t byte_offset, bool *named)
{
    *named = false;
    if (byte_offset) {
        uint64_t o = (uint64_t)vm_rd32(k->vm, byte_offset) | ((uint64_t)vm_rd32(k->vm, byte_offset + 4) << 32);
        if (o != 0xFFFFFFFFFFFFFFFEull) { /* FILE_USE_FILE_POINTER_POSITION */
            *named = true;
            return o;
        }
    }
    return f->pos;
}

static void io_moved(KFile *f, bool named, uint64_t end)
{
    if (!f->async || !named) {
        f->pos = end;
    }
}

/* Partición de una carpeta raíz abierta (para leerla cruda), o -1. */
static int partition_root(const KFile *f)
{
    if (f->kind != F_HOST || !f->is_dir) {
        return -1;
    }
    const char *rel;
    int d = device_of(f->path, &rel);
    return d >= 0 && !*rel ? d : -1;
}

/* Lee n bytes del archivo en off hacia la memoria del Xbox en va. */
static bool read_into(Kernel *k, KFile *f, uint64_t off, uint32_t va, uint32_t n)
{
    while (n) {
        uint32_t chunk = n < sizeof(g_buf) ? n : (uint32_t)sizeof(g_buf);
        bool ok;
        if (f->kind == F_DISC) {
            ok = xdisc_read(k->disc, &f->disc, off, g_buf, chunk);
        } else {
            ok = f->fp && fseeko(f->fp, (off_t)off, SEEK_SET) == 0 && fread(g_buf, 1, chunk, f->fp) == chunk;
        }
        if (!ok || !vm_write(k->vm, va, g_buf, chunk)) {
            return false;
        }
        off += chunk;
        va += chunk;
        n -= chunk;
    }
    return true;
}

/* NtReadFile(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, Buffer, Length, ByteOffset) */
static uint64_t NtReadFile(KCall *c)
{
    Kernel *k = c->k;
    KFile *f = file_of(k, c->a[0]);
    if (!f) {
        return ST_INVALID_HANDLE;
    }
    note(c, "%s", f->path);
    set_last_io(k, 'L', f->path);
    uint32_t len = c->a[6];
    bool named;
    uint64_t off = io_offset(k, f, c->a[7], &named);
    int raw = f->kind == F_VOLUME && f->partition == 0 ? 0 : partition_root(f);
    if (raw == 0 || raw >= 3) {
        for (uint32_t done = 0; done < len;) {
            uint32_t chunk = len - done < sizeof(g_buf) ? len - done : (uint32_t)sizeof(g_buf);
            raw_io(k, raw, off + done, g_buf, chunk, false);
            vm_write(k->vm, c->a[5] + done, g_buf, chunk);
            done += chunk;
        }
        io_moved(f, named, off + len);
        return io_finish(c, f, ST_SUCCESS, len, true);
    }
    if (f->is_dir || f->kind == F_VOLUME) {
        io_status(k, c->a[4], ST_INVALID_DEVICE_REQUEST, 0);
        return ST_INVALID_DEVICE_REQUEST;
    }
    uint64_t size = file_size(f);
    bool async = f->async && !(f->kind == F_DISC && ((off | len) & (DISC_SECTOR - 1)));
    uint32_t st = ST_SUCCESS, n = 0;
    if (!len) {
        /* 0 bytes: éxito, también pasado el final (Halo pide lo que queda) */
    } else if (off >= size) {
        st = ST_END_OF_FILE;
    } else {
        n = (uint32_t)(size - off < len ? size - off : len);
        if (!read_into(k, f, off, c->a[5], n)) {
            st = ST_DEVICE_DATA_ERROR;
            n = 0;
        }
    }
    k->reads++;
    k->bytes_read += n;
    io_moved(f, named, off + n);
    return io_finish(c, f, st, n, async);
}

/* NtReadFileScatter(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock,
 *   SegmentArray, Length, ByteOffset): una dirección de página por cada página */
static uint64_t NtReadFileScatter(KCall *c)
{
    Kernel *k = c->k;
    KFile *f = file_of(k, c->a[0]);
    if (!f) {
        return ST_INVALID_HANDLE;
    }
    note(c, "%s", f->path);
    set_last_io(k, 'L', f->path);
    if (f->is_dir || f->kind == F_VOLUME) {
        return ST_INVALID_DEVICE_REQUEST;
    }
    bool named;
    uint64_t off = io_offset(k, f, c->a[7], &named), size = file_size(f);
    uint32_t len = c->a[6], st = ST_SUCCESS, n = 0;
    if (len && off >= size) {
        st = ST_END_OF_FILE;
    } else {
        n = (uint32_t)(size - off < len ? size - off : len);
        for (uint32_t done = 0, i = 0; done < n; i++) {
            uint32_t page = vm_rd32(k->vm, c->a[5] + 4 * i) & ~(VM_PAGE - 1);
            uint32_t chunk = n - done < VM_PAGE ? n - done : VM_PAGE;
            if (!read_into(k, f, off + done, page, chunk)) {
                st = ST_DEVICE_DATA_ERROR;
                n = done;
                break;
            }
            done += chunk;
        }
    }
    k->reads++;
    k->bytes_read += n;
    io_moved(f, named, off + n);
    return io_finish(c, f, st, n, true);
}

/* NtWriteFile(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, Buffer, Length, ByteOffset) */
static uint64_t NtWriteFile(KCall *c)
{
    Kernel *k = c->k;
    KFile *f = file_of(k, c->a[0]);
    if (!f) {
        return ST_INVALID_HANDLE;
    }
    note(c, "%s", f->path);
    set_last_io(k, 'E', f->path);
    uint32_t len = c->a[6];
    bool named;
    uint64_t off = io_offset(k, f, c->a[7], &named);
    if (f->kind == F_VOLUME) {
        if (f->partition == 0) {
            for (uint32_t done = 0; done < len;) {
                uint32_t chunk = len - done < sizeof(g_buf) ? len - done : (uint32_t)sizeof(g_buf);
                vm_read(k->vm, c->a[5] + done, g_buf, chunk);
                raw_io(k, 0, off + done, g_buf, chunk, true);
                done += chunk;
            }
            io_moved(f, named, off + len);
        }
        return io_finish(c, f, ST_SUCCESS, len, true);
    }
    int raw = partition_root(f);
    if (raw >= 3) {
        /* una partición de caché formateada en crudo (XMountUtilityDrive):
         * escribir el superbloque FATX la vacía */
        uint32_t first = len < sizeof(g_buf) ? len : (uint32_t)sizeof(g_buf);
        vm_read(k->vm, c->a[5], g_buf, first);
        raw_io(k, raw, off, g_buf, first, true);
        if (off == 0 && len >= 12 && !memcmp(g_buf, "FATX", 4)) {
            g_cluster_sectors[raw] = get32(g_buf + 8);
            remove_tree(f->host, false);
        }
        io_moved(f, named, off + len);
        return io_finish(c, f, ST_SUCCESS, len, true);
    }
    if (f->kind != F_HOST || f->is_dir || !f->can_write || !f->fp) {
        io_status(k, c->a[4], ST_ACCESS_DENIED, 0);
        return ST_ACCESS_DENIED;
    }
    if (c->a[7]) {
        uint64_t o = (uint64_t)vm_rd32(k->vm, c->a[7]) | ((uint64_t)vm_rd32(k->vm, c->a[7] + 4) << 32);
        if (o == 0xFFFFFFFFFFFFFFFFull) { /* FILE_WRITE_TO_END_OF_FILE */
            off = file_size(f);
        }
    }
    uint32_t st = ST_SUCCESS, done = 0;
    if (fseeko(f->fp, (off_t)off, SEEK_SET) != 0) {
        st = ST_DISK_FULL;
    }
    while (st == ST_SUCCESS && done < len) {
        uint32_t chunk = len - done < sizeof(g_buf) ? len - done : (uint32_t)sizeof(g_buf);
        if (!vm_read(k->vm, c->a[5] + done, g_buf, chunk) || fwrite(g_buf, 1, chunk, f->fp) != chunk) {
            st = ST_DISK_FULL;
            break;
        }
        done += chunk;
    }
    fflush(f->fp);
    f->wrote |= done > 0;
    k->writes++;
    io_moved(f, named, off + done);
    return io_finish(c, f, st, done, false);
}

/* ------------------------------------------------------------- información */

static uint64_t host_time(time_t t) { return (uint64_t)(t + 11644473600ll) * 10000000ull; }

static void file_facts(const KFile *f, uint64_t t[4], uint64_t *size, uint32_t *attrs)
{
    struct stat st;
    if (f->kind == F_HOST && stat(f->host, &st) == 0) {
        t[0] = host_time(st.st_ctime);
        t[1] = host_time(st.st_atime);
        t[2] = t[3] = host_time(st.st_mtime);
        *size = S_ISDIR(st.st_mode) ? 0 : (uint64_t)st.st_size;
        *attrs = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        return;
    }
    t[0] = t[1] = t[2] = t[3] = DISC_TIME;
    *size = f->is_dir ? 0 : file_size(f);
    *attrs = (f->is_dir ? FILE_ATTRIBUTE_DIRECTORY : 0) | (f->kind == F_HOST ? 0 : FILE_ATTRIBUTE_READONLY);
    if (!*attrs) {
        *attrs = FILE_ATTRIBUTE_NORMAL;
    }
}

/* FILE_NETWORK_OPEN_INFORMATION: 4 tiempos, AllocationSize, EndOfFile, FileAttributes */
static void network_open_info(const KFile *f, uint8_t *o)
{
    uint64_t t[4], size;
    uint32_t attrs;
    file_facts(f, t, &size, &attrs);
    memset(o, 0, 0x38);
    for (int i = 0; i < 4; i++) {
        put64(o + 8 * i, t[i]);
    }
    put64(o + 0x20, align4k(size));
    put64(o + 0x28, size);
    put32(o + 0x30, attrs);
}

/* NtQueryInformationFile(FileHandle, IoStatusBlock, FileInformation, Length, FileInformationClass) */
static uint64_t NtQueryInformationFile(KCall *c)
{
    KFile *f = file_of(c->k, c->a[0]);
    if (!f) {
        return ST_INVALID_HANDLE;
    }
    note(c, "%s", f->path);
    uint8_t o[0x38];
    uint64_t t[4], size;
    uint32_t attrs, len;
    file_facts(f, t, &size, &attrs);
    switch (c->a[4]) {
    case 4: /* FileBasicInformation */
        len = 0x28;
        memset(o, 0, len);
        for (int i = 0; i < 4; i++) {
            put64(o + 8 * i, t[i]);
        }
        put32(o + 0x20, attrs);
        break;
    case 5: /* FileStandardInformation */
        len = 0x18;
        memset(o, 0, len);
        put64(o, align4k(size));
        put64(o + 8, size);
        put32(o + 0x10, 1);
        o[0x15] = f->is_dir;
        break;
    case 6: /* FileInternalInformation */
        len = 8;
        put64(o, f->kind == F_DISC ? f->disc.sector : (uint64_t)(f - g_files) + 1);
        break;
    case 14: /* FilePositionInformation */
        len = 8;
        put64(o, f->pos);
        break;
    case 17: /* FileAlignmentInformation */
        len = 4;
        put32(o, 0);
        break;
    case 34: /* FileNetworkOpenInformation */
        len = 0x38;
        network_open_info(f, o);
        break;
    default:
        io_status(c->k, c->a[1], ST_INVALID_INFO_CLASS, 0);
        return ST_INVALID_INFO_CLASS;
    }
    if (c->a[3] < len) {
        return ST_BUFFER_TOO_SMALL;
    }
    vm_write(c->vm, c->a[2], o, len);
    io_status(c->k, c->a[1], ST_SUCCESS, len);
    return ST_SUCCESS;
}

/* NtSetInformationFile(FileHandle, IoStatusBlock, FileInformation, Length, FileInformationClass) */
static uint64_t NtSetInformationFile(KCall *c)
{
    Kernel *k = c->k;
    KFile *f = file_of(k, c->a[0]);
    if (!f) {
        return ST_INVALID_HANDLE;
    }
    note(c, "%s", f->path);
    uint8_t in[16] = {0};
    vm_read(k->vm, c->a[2], in, c->a[3] < 16 ? c->a[3] : 16);
    uint64_t v;
    memcpy(&v, in, 8);
    uint32_t st = ST_SUCCESS;
    switch (c->a[4]) {
    case 14: /* FilePositionInformation */
        f->pos = v;
        break;
    case 19: /* FileAllocationInformation */
    case 4:  /* FileBasicInformation (tiempos: no se guardan) */
        break;
    case 20: /* FileEndOfFileInformation */
        if (f->kind != F_HOST || !f->fp || !f->can_write || ftruncate(fileno(f->fp), (off_t)v) != 0) {
            st = ST_ACCESS_DENIED;
        } else {
            f->wrote = true;
        }
        break;
    case 13: /* FileDispositionInformation: borrar al cerrar */
        if (f->kind != F_HOST) {
            st = ST_ACCESS_DENIED;
        } else {
            f->del = in[0] != 0;
            if (f->del && f->is_dir) {
                DIR *d = opendir(f->host);
                struct dirent *de;
                bool empty = true;
                while (d && (de = readdir(d))) {
                    empty &= !strcmp(de->d_name, ".") || !strcmp(de->d_name, "..");
                }
                if (d) {
                    closedir(d);
                }
                if (!empty) {
                    f->del = false;
                    st = ST_DIRECTORY_NOT_EMPTY;
                }
            }
        }
        break;
    case 10: { /* FileRenameInformation: ReplaceIfExists, RootDirectory, FileName */
        char name[300], path[300], host[320];
        ansi(k, c->a[2] + 8, name, sizeof(name));
        const char *rel;
        int dev, exists = -1;
        if (f->kind != F_HOST || !resolve_name(k, get32(in + 4), name, sizeof(name)) ||
            !canon(name, path, sizeof(path)) || (dev = device_of(path, &rel)) < 1 ||
            (exists = host_path(k, dev, rel, host, sizeof(host))) < 0) {
            st = exists < 0 && f->kind == F_HOST ? ST_OBJECT_PATH_NOT_FOUND : ST_ACCESS_DENIED;
        } else if (exists && !in[0]) {
            st = ST_OBJECT_NAME_COLLISION;
        } else {
            if (f->fp) {
                fflush(f->fp);
            }
            if (exists) {
                remove(host);
            }
            if (rename(f->host, host) != 0) {
                st = ST_ACCESS_DENIED;
            } else {
                snprintf(f->host, sizeof(f->host), "%s", host);
                snprintf(f->path, sizeof(f->path), "%s", path);
            }
        }
        break;
    }
    default:
        st = ST_INVALID_INFO_CLASS;
        break;
    }
    io_status(k, c->a[1], st, 0);
    return st;
}

static uint32_t cluster_sectors(Kernel *k, int part)
{
    if (!g_cluster_sectors[part] && part >= 3) {
        uint8_t sb[12];
        raw_io(k, part, 0, sb, sizeof(sb), false);
        if (!memcmp(sb, "FATX", 4)) {
            g_cluster_sectors[part] = get32(sb + 8);
        }
    }
    return g_cluster_sectors[part] ? g_cluster_sectors[part] : 32;
}

static uint64_t dir_used(const char *dir, uint64_t cluster, unsigned depth)
{
    uint64_t used = 0;
    DIR *d = depth < 16 ? opendir(dir) : NULL;
    struct dirent *de;
    while (d && (de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) {
            continue;
        }
        char p[600];
        struct stat st;
        snprintf(p, sizeof(p), "%s/%s", dir, de->d_name);
        if (stat(p, &st) != 0) {
            continue;
        }
        used += S_ISDIR(st.st_mode) ? dir_used(p, cluster, depth + 1)
                                    : ((uint64_t)st.st_size + cluster - 1) / cluster * cluster;
    }
    if (d) {
        closedir(d);
    }
    return used;
}

static uint32_t volume_info(Kernel *k, const KFile *f, uint32_t cls, uint32_t buf, uint32_t cap, uint32_t *len_out)
{
    bool disc = f->partition < 0;
    uint8_t o[0x20];
    uint32_t len;
    memset(o, 0, sizeof(o));
    switch (cls) {
    case 1: /* FileFsVolumeInformation: CreationTime, SerialNumber, LabelLength, SupportsObjects */
        len = 0x12;
        put64(o, DISC_TIME);
        put32(o + 8, disc ? 0x12345678u : 0x5678ABCDu);
        break;
    case 3:   /* FileFsSizeInformation */
    case 7: { /* FileFsFullSizeInformation */
        uint64_t total, avail;
        uint32_t spu, bps;
        if (disc) {
            total = 7ull * 1024 * 1024 * 1024 / 2048;
            avail = 0;
            spu = 1;
            bps = 2048;
        } else {
            int part = f->partition >= 1 && f->partition <= 5 ? f->partition : 1;
            spu = cluster_sectors(k, part);
            bps = 512;
            uint64_t cluster = (uint64_t)spu * 512u, size = k_parts[part][1];
            char dir[260];
            snprintf(dir, sizeof(dir), "%s/Partition%d", k->hdd_root, part);
            uint64_t used = dir_used(dir, cluster, 0);
            total = size / cluster;
            avail = (used < size ? size - used : 0) / cluster;
        }
        if (cls == 3) {
            len = 0x18;
            put64(o, total);
            put64(o + 8, avail);
            put32(o + 0x10, spu);
            put32(o + 0x14, bps);
        } else {
            len = 0x20;
            put64(o, total);
            put64(o + 8, avail);
            put64(o + 0x10, avail);
            put32(o + 0x18, spu);
            put32(o + 0x1C, bps);
        }
        break;
    }
    case 4: /* FileFsDeviceInformation: DeviceType, Characteristics */
        len = 8;
        put32(o, disc ? 0x02u : 0x07u);
        put32(o + 4, disc ? 0x01u : 0u);
        break;
    case 5: /* FileFsAttributeInformation: Attributes, MaxComponentNameLength, NameLength, Name */
        len = 16;
        put32(o + 4, 42);
        put32(o + 8, 4);
        memcpy(o + 12, disc ? "GDFX" : "FATX", 4);
        break;
    default:
        *len_out = 0;
        return ST_INVALID_INFO_CLASS;
    }
    if (cap < len) {
        len = cap; /* lo que cabe; el resto se pierde (como el nombre de la etiqueta) */
    }
    vm_write(k->vm, buf, o, len);
    *len_out = len;
    return ST_SUCCESS;
}

/* NtQueryVolumeInformationFile(FileHandle, IoStatusBlock, FsInformation, Length, FsInformationClass) */
static uint64_t NtQueryVolumeInformationFile(KCall *c)
{
    KFile *f = file_of(c->k, c->a[0]);
    if (!f) {
        return ST_INVALID_HANDLE;
    }
    note(c, "%s", f->path);
    uint32_t len;
    uint32_t st = volume_info(c->k, f, c->a[4], c->a[2], c->a[3], &len);
    io_status(c->k, c->a[1], st, len);
    return st;
}

/* IoQueryVolumeInformation(FileObject, FsInformationClass, Length, FsInformation, ReturnedLength) */
static uint64_t IoQueryVolumeInformation(KCall *c)
{
    if (!c->a[0] || (vm_rd32(c->vm, c->a[0]) & 0xFFFF) != 5) {
        return ST_INVALID_HANDLE;
    }
    uint32_t i = vm_rd32(c->vm, c->a[0] + FO_INDEX);
    if (i >= MAX_FILES || !g_files[i].used) {
        return ST_INVALID_HANDLE;
    }
    note(c, "%s", g_files[i].path);
    uint32_t len;
    uint32_t st = volume_info(c->k, &g_files[i], c->a[1], c->a[3], c->a[2], &len);
    if (c->a[4]) {
        vm_wr32(c->vm, c->a[4], len);
    }
    return st;
}

/* ------------------------------------------------------------- carpetas */

typedef struct ListCtx {
    DirEntry *out;
    unsigned n, cap;
} ListCtx;

static void list_add(void *ctx, const char *name, uint32_t size, bool is_dir)
{
    ListCtx *l = ctx;
    if (l->n < l->cap && strlen(name) < sizeof(l->out->name)) {
        DirEntry *e = &l->out[l->n++];
        snprintf(e->name, sizeof(e->name), "%s", name);
        e->size = size;
        e->dir = is_dir;
    }
}

static unsigned list_dir(Kernel *k, const KFile *f, DirEntry *out, unsigned cap)
{
    ListCtx l = {out, 0, cap};
    if (f->kind != F_HOST) {
        if (k->disc && f->disc.size) {
            xdisc_list(k->disc, &f->disc, list_add, &l);
        }
        return l.n;
    }
    DIR *d = opendir(f->host);
    struct dirent *de;
    while (d && (de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) {
            continue;
        }
        char p[600];
        struct stat st;
        snprintf(p, sizeof(p), "%s/%s", f->host, de->d_name);
        bool dir = stat(p, &st) == 0 && S_ISDIR(st.st_mode);
        list_add(&l, de->d_name, dir ? 0 : (uint32_t)st.st_size, dir);
    }
    if (d) {
        closedir(d);
    }
    return l.n;
}

/* NtQueryDirectoryFile(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, FileInformation,
 *   Length, FileInformationClass, FileMask, RestartScan): una entrada por llamada.
 * FILE_DIRECTORY_INFORMATION: NextEntryOffset, FileIndex, 4 tiempos, EndOfFile,
 * AllocationSize, FileAttributes 0x38, FileNameLength 0x3C, FileName 0x40. */
static uint64_t NtQueryDirectoryFile(KCall *c)
{
    Kernel *k = c->k;
    KFile *f = file_of(k, c->a[0]);
    if (!f || !f->is_dir) {
        return f ? ST_INVALID_DEVICE_REQUEST : ST_INVALID_HANDLE;
    }
    if (c->a[7] != 1) {
        return ST_INVALID_INFO_CLASS;
    }
    char mask[64] = "*";
    if (c->a[8]) {
        ansi(k, c->a[8], mask, sizeof(mask));
        if (!mask[0]) {
            snprintf(mask, sizeof(mask), "*");
        }
    } else if (f->listing) {
        snprintf(mask, sizeof(mask), "%s", f->mask);
    }
    char nt[128];
    snprintf(nt, sizeof(nt), "%s\\%s", f->path, mask);
    note(c, "%s", nt);
    if ((c->a[9] & 0xFF) || strcmp(mask, f->mask) != 0 || !f->listing) {
        static DirEntry scratch[1024];
        f->dir_next = 0;
        snprintf(f->mask, sizeof(f->mask), "%s", mask);
        unsigned n = list_dir(k, f, scratch, 1024);
        free(f->listing);
        f->listing = malloc((n ? n : 1) * sizeof(DirEntry));
        f->nlisting = f->listing ? n : 0;
        if (f->listing && n) {
            memcpy(f->listing, scratch, n * sizeof(DirEntry));
        }
    }
    unsigned i = f->dir_next;
    while (i < f->nlisting && !wildcard(f->mask, f->listing[i].name)) {
        i++;
    }
    if (i >= f->nlisting) {
        uint32_t st = f->dir_next == 0 ? ST_NO_SUCH_FILE : ST_NO_MORE_FILES;
        io_status(k, c->a[4], st, 0);
        complete(c);
        return st;
    }
    f->dir_next = i + 1;
    const DirEntry *e = &f->listing[i];
    uint32_t nlen = (uint32_t)strlen(e->name);
    if (c->a[6] < 0x40 + nlen) {
        return ST_BUFFER_TOO_SMALL;
    }
    uint8_t o[0x40 + 64];
    memset(o, 0, 0x40);
    put32(o + 4, i);
    for (int t = 0; t < 4; t++) {
        put64(o + 8 + 8 * t, DISC_TIME);
    }
    put64(o + 0x28, e->size);
    put64(o + 0x30, align4k(e->size));
    put32(o + 0x38, e->dir ? FILE_ATTRIBUTE_DIRECTORY : (f->kind == F_HOST ? FILE_ATTRIBUTE_NORMAL : FILE_ATTRIBUTE_READONLY));
    put32(o + 0x3C, nlen);
    memcpy(o + 0x40, e->name, nlen);
    vm_write(k->vm, c->a[5], o, 0x40 + nlen);
    io_status(k, c->a[4], ST_SUCCESS, 0x40 + nlen);
    complete(c);
    return ST_SUCCESS;
}

/* ------------------------------------------------------------- por ruta */

/* NtQueryFullAttributesFile(ObjectAttributes, FileInformation) */
static uint64_t NtQueryFullAttributesFile(KCall *c)
{
    uint32_t h = 0, info = 0;
    uint32_t st = do_open(c, 0, c->a[0], FILE_OPEN, 0, &h, &info);
    if (st != ST_SUCCESS) {
        return st;
    }
    uint8_t o[0x38];
    network_open_info(file_of(c->k, h), o);
    vm_write(c->vm, c->a[1], o, sizeof(o));
    kob_close(c->k, h);
    return ST_SUCCESS;
}

/* NtDeleteFile(ObjectAttributes) */
static uint64_t NtDeleteFile(KCall *c)
{
    Kernel *k = c->k;
    char name[300], path[300], host[320];
    ansi(k, vm_rd32(k->vm, c->a[0] + 4), name, sizeof(name));
    note(c, "%s", name);
    if (!resolve_name(k, vm_rd32(k->vm, c->a[0]), name, sizeof(name))) {
        return ST_INVALID_HANDLE;
    }
    const char *rel;
    int dev;
    if (!canon(name, path, sizeof(path)) || (dev = device_of(path, &rel)) < 1) {
        return ST_ACCESS_DENIED;
    }
    note(c, "%s", path);
    if (host_path(k, dev, rel, host, sizeof(host)) != 1) {
        return ST_OBJECT_NAME_NOT_FOUND;
    }
    struct stat st;
    bool dir = stat(host, &st) == 0 && S_ISDIR(st.st_mode);
    for (int i = 0; !dir && i < MAX_FILES; i++) {
        if (g_files[i].used && !strcmp(g_files[i].host, host)) {
            g_files[i].del = true; /* se va con el último handle */
            return ST_SUCCESS;
        }
    }
    return (dir ? rmdir(host) : remove(host)) == 0 ? ST_SUCCESS : ST_ACCESS_DENIED;
}

/* NtFlushBuffersFile(FileHandle, IoStatusBlock) */
static uint64_t NtFlushBuffersFile(KCall *c)
{
    KFile *f = file_of(c->k, c->a[0]);
    if (f && f->fp) {
        fflush(f->fp);
    }
    io_status(c->k, c->a[1], ST_SUCCESS, 0);
    return ST_SUCCESS;
}

/* ------------------------------------------------------------- control */

#define IOCTL_DISK_GET_DRIVE_GEOMETRY  0x00070000u
#define IOCTL_DISK_GET_PARTITION_INFO  0x00074004u
#define IOCTL_SCSI_PASS_THROUGH_DIRECT 0x0004D014u
#define FSCTL_LOCK_VOLUME     0x00090018u
#define FSCTL_UNLOCK_VOLUME   0x0009001Cu
#define FSCTL_DISMOUNT_VOLUME 0x00090020u

static uint32_t disk_ioctl(Kernel *k, const KFile *f, uint32_t code, uint32_t out, uint32_t out_len, uint32_t *status)
{
    int part = f->partition;
    uint8_t o[32];
    memset(o, 0, sizeof(o));
    if (part < 0 || part > 5) {
        *status = ST_INVALID_DEVICE_REQUEST;
        return 0;
    }
    switch (code) {
    case IOCTL_DISK_GET_DRIVE_GEOMETRY:
        if (out_len < 24) {
            break;
        }
        put64(o, HDD_BYTES / 512);
        put32(o + 8, 12); /* FixedMedia */
        put32(o + 12, 1);
        put32(o + 16, 1);
        put32(o + 20, 512);
        vm_write(k->vm, out, o, 24);
        return 24;
    case IOCTL_DISK_GET_PARTITION_INFO:
        if (out_len < 32) {
            break;
        }
        put64(o, k_parts[part][0]);
        put64(o + 8, part ? k_parts[part][1] : HDD_BYTES);
        put32(o + 16, (uint32_t)(k_parts[part][0] / 512));
        put32(o + 20, (uint32_t)part);
        o[26] = 1;
        vm_write(k->vm, out, o, 32);
        return 32;
    default:
        *status = ST_INVALID_DEVICE_REQUEST;
        return 0;
    }
    *status = ST_BUFFER_TOO_SMALL;
    return 0;
}

/* SCSI al lector: XAPI pide la página de seguridad del Xbox (MODE SENSE 0x3E)
 * y vuelve al menú si el disco no sale autenticado. */
static void scsi_pass_through(Kernel *k, uint32_t sptd, uint32_t len)
{
    if (!sptd || len < 0x2C) {
        return;
    }
    uint8_t cdb[16];
    vm_read(k->vm, sptd + 0x1C, cdb, sizeof(cdb));
    uint32_t n = vm_rd32(k->vm, sptd + 0x0C), buf = vm_rd32(k->vm, sptd + 0x14);
    *vm_host(k->vm, sptd + 2) = 0;
    if (!buf || !n) {
        return;
    }
    for (uint32_t i = 0; i < n; i += 4) {
        vm_wr32(k->vm, buf + i, 0);
    }
    if (cdb[0] == 0x5A && (cdb[2] & 0x3F) == 0x3E && n >= 13) {
        uint8_t d[13] = {0};
        d[1] = (uint8_t)(n < 28 ? n - 2 : 26);
        d[8] = 0x3E;
        d[9] = 0x12;
        d[10] = 1; /* CDFValid */
        d[11] = 1; /* la partición del Xbox */
        d[12] = 1; /* autenticado */
        vm_write(k->vm, buf, d, sizeof(d));
    }
}

/* NtFsControlFile / NtDeviceIoControlFile(FileHandle, Event, ApcRoutine, ApcContext,
 *   IoStatusBlock, ControlCode, InputBuffer, InputBufferLength, OutputBuffer, OutputBufferLength) */
static uint64_t NtControlFile(KCall *c)
{
    Kernel *k = c->k;
    KFile *f = file_of(k, c->a[0]);
    char b[64];
    snprintf(b, sizeof(b), "%08X %s", (unsigned)c->a[5], f ? f->path : "?");
    note(c, "%s", b);
    uint32_t status = ST_SUCCESS, info = 0;
    if (f && f->partition >= 0 && (c->a[5] >> 16) == 7) { /* FILE_DEVICE_DISK */
        info = disk_ioctl(k, f, c->a[5], c->a[8], c->a[8] ? c->a[9] : 0, &status);
    } else if (c->a[5] == IOCTL_SCSI_PASS_THROUGH_DIRECT) {
        scsi_pass_through(k, c->a[6], c->a[7]);
    } else if (c->a[5] != FSCTL_LOCK_VOLUME && c->a[5] != FSCTL_UNLOCK_VOLUME && c->a[5] != FSCTL_DISMOUNT_VOLUME) {
        status = ST_INVALID_DEVICE_REQUEST;
    }
    io_status(k, c->a[4], status, info);
    complete(c);
    return status;
}

/* IoSynchronousDeviceIoControlRequest(IoControlCode, DeviceObject, InputBuffer, InputBufferLength,
 *   OutputBuffer, OutputBufferLength, ReturnedOutputBufferLength, InternalDeviceIoControl) */
static uint64_t IoSynchronousDeviceIoControlRequest(KCall *c)
{
    for (uint32_t i = 0; c->a[4] && i < c->a[5]; i++) {
        *vm_host(c->vm, c->a[4] + i) = 0;
    }
    if (c->a[6]) {
        vm_wr32(c->vm, c->a[6], 0);
    }
    return ST_SUCCESS;
}

/* ------------------------------------------------------------- enlaces */

/* IoCreateSymbolicLink(SymbolicLinkName, DeviceName) */
static uint64_t IoCreateSymbolicLink(KCall *c)
{
    char name[48], target[200], ct[200], b[64];
    ansi(c->k, c->a[0], name, sizeof(name));
    ansi(c->k, c->a[1], target, sizeof(target));
    snprintf(b, sizeof(b), "%s -> %s", name, target);
    note(c, "%s", b);
    if (!canon(target, ct, sizeof(ct))) {
        return ST_OBJECT_PATH_NOT_FOUND;
    }
    return link_set(name, ct) ? ST_SUCCESS : ST_INSUFFICIENT_RESOURCES;
}

/* IoDeleteSymbolicLink(SymbolicLinkName) */
static uint64_t IoDeleteSymbolicLink(KCall *c)
{
    char name[48];
    ansi(c->k, c->a[0], name, sizeof(name));
    note(c, "%s", name);
    int i = link_find(name);
    if (i < 0) {
        return ST_OBJECT_NAME_NOT_FOUND;
    }
    g_links[i].name[0] = 0;
    return ST_SUCCESS;
}

/* NtOpenSymbolicLinkObject(LinkHandle, ObjectAttributes) */
static uint64_t NtOpenSymbolicLinkObject(KCall *c)
{
    char name[48];
    ansi(c->k, vm_rd32(c->vm, c->a[1] + 4), name, sizeof(name));
    note(c, "%s", name);
    int i = link_find(name);
    if (i < 0) {
        return ST_OBJECT_NAME_NOT_FOUND;
    }
    KFile *f = file_new();
    if (!f) {
        return ST_INSUFFICIENT_RESOURCES;
    }
    f->kind = F_LINK;
    f->partition = -2;
    snprintf(f->path, sizeof(f->path), "%s", g_links[i].target);
    uint32_t h = file_handle(c->k, f);
    if (!h) {
        file_drop(c->k, f);
        return ST_INSUFFICIENT_RESOURCES;
    }
    vm_wr32(c->vm, c->a[0], h);
    return ST_SUCCESS;
}

/* NtQuerySymbolicLinkObject(LinkHandle, LinkTarget (ANSI_STRING), ReturnedLength) */
static uint64_t NtQuerySymbolicLinkObject(KCall *c)
{
    KFile *f = file_of(c->k, c->a[0]);
    if (!f || f->kind != F_LINK) {
        return ST_INVALID_HANDLE;
    }
    note(c, "%s", f->path);
    uint16_t len = (uint16_t)strlen(f->path);
    uint16_t max = (uint16_t)(vm_rd32(c->vm, c->a[1]) >> 16);
    if (c->a[2]) {
        vm_wr32(c->vm, c->a[2], len);
    }
    if (max < len) {
        return ST_BUFFER_TOO_SMALL;
    }
    vm_write(c->vm, vm_rd32(c->vm, c->a[1] + 4), f->path, len);
    vm_wr32(c->vm, c->a[1], ((uint32_t)max << 16) | len);
    return ST_SUCCESS;
}

/* ------------------------------------------------------------- dispositivos */

/* DEVICE_OBJECT: Type 0, Size 2, DriverObject 8, Flags 0x14, DeviceExtension
 * 0x18, DeviceType 0x1C, StackSize 0x1E, DeletePending 0x1F, DeviceQueue
 * 0x28, DeviceLock 0x34 (KEVENT); 0x48 bytes y la extensión detrás. */
#define DEVOBJ_SIZE 0x48u

static uint32_t device_named(const char *path)
{
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (g_devices[i].object && !strcasecmp(g_devices[i].name, path)) {
            return g_devices[i].object;
        }
    }
    return 0;
}

static void device_add(const char *path, uint32_t object)
{
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (!g_devices[i].object) {
            snprintf(g_devices[i].name, sizeof(g_devices[i].name), "%s", path);
            g_devices[i].object = object;
            return;
        }
    }
}

static uint32_t device_new(Kernel *k, uint32_t driver, uint32_t ext_size, uint32_t type)
{
    uint32_t ext = (ext_size + 15) & ~15u;
    uint32_t d = kob_create(k, OB_DEVICE, DEVOBJ_SIZE + ext);
    if (!d) {
        return 0;
    }
    for (uint32_t i = 0; i < DEVOBJ_SIZE + ext; i += 4) {
        vm_wr32(k->vm, d + i, 0);
    }
    vm_wr32(k->vm, d, 3 | ((DEVOBJ_SIZE + ext) << 16)); /* IO_TYPE_DEVICE, Size */
    vm_wr32(k->vm, d + 0x08, driver);
    vm_wr32(k->vm, d + 0x14, 0x80);                      /* DO_DEVICE_INITIALIZING */
    vm_wr32(k->vm, d + 0x18, ext_size ? d + DEVOBJ_SIZE : 0);
    vm_wr32(k->vm, d + 0x1C, (type & 0xFF) | (1u << 16)); /* DeviceType, StackSize 1 */
    vm_wr32(k->vm, d + 0x28, 0x14 | (3u << 16));          /* DeviceQueueObject */
    vm_wr32(k->vm, d + 0x2C, d + 0x2C);
    vm_wr32(k->vm, d + 0x30, d + 0x2C);
    event_init(k, d + 0x34, 1, true);
    return d;
}

/* IoCreateDevice(DriverObject, DeviceExtensionSize, DeviceName, DeviceType, Exclusive, DeviceObject) */
static uint64_t IoCreateDevice(KCall *c)
{
    char name[128] = "", path[200] = "";
    if (c->a[2]) {
        ansi(c->k, c->a[2], name, sizeof(name));
        note(c, "%s", name);
        if (!canon(name, path, sizeof(path))) {
            return ST_OBJECT_PATH_NOT_FOUND;
        }
        if (device_named(path)) {
            return ST_OBJECT_NAME_COLLISION;
        }
    }
    uint32_t d = device_new(c->k, c->a[0], c->a[1], c->a[3]);
    if (!d) {
        return ST_INSUFFICIENT_RESOURCES;
    }
    if (path[0]) {
        device_add(path, d);
    }
    vm_wr32(c->vm, c->a[5], d);
    return ST_SUCCESS;
}

/* IoDeleteDevice(DeviceObject) */
static uint64_t IoDeleteDevice(KCall *c)
{
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (g_devices[i].object == c->a[0]) {
            g_devices[i].object = 0;
        }
    }
    *vm_host(c->vm, c->a[0] + 0x1F) = 1;
    return 0;
}

/* ObReferenceObjectByName(ObjectName, Attributes, ObjectType, ParseContext, Object) */
static uint64_t ObReferenceObjectByName(KCall *c)
{
    char name[200], path[200];
    ansi(c->k, c->a[0], name, sizeof(name));
    note(c, "%s", name);
    const char *rel;
    if (!canon(name, path, sizeof(path))) {
        return ST_OBJECT_NAME_NOT_FOUND;
    }
    uint32_t d = device_named(path);
    if (!d) {
        int dev = device_of(path, &rel);
        if (dev == -2 || *rel) {
            return ST_OBJECT_NAME_NOT_FOUND;
        }
        d = device_new(c->k, 0, 0, dev == -1 ? 0x02 : 0x07);
        if (!d) {
            return ST_INSUFFICIENT_RESOURCES;
        }
        device_add(path, d);
    }
    vm_wr32(c->vm, c->a[4], d);
    return ST_SUCCESS;
}

/* IoDismountVolumeByName(DeviceName): carpetas, nada que vaciar */
static uint64_t IoDismountVolumeByName(KCall *c)
{
    char name[128];
    ansi(c->k, c->a[0], name, sizeof(name));
    note(c, "%s", name);
    return ST_SUCCESS;
}

/* ExReadWriteRefurbInfo(RefurbInfo, ValueLength, DoWrite): sector 3 de la configuración */
static uint64_t ExReadWriteRefurbInfo(KCall *c)
{
    if (c->a[1] != 16) {
        return ST_INVALID_PARAMETER;
    }
    uint8_t sector[512];
    raw_io(c->k, 0, 3 * 512, sector, sizeof(sector), false);
    if (c->a[2] & 0xFF) {
        vm_read(c->vm, c->a[0], sector, 16);
        put32(sector, 0x52465242u); /* 'RFRB' */
        raw_io(c->k, 0, 3 * 512, sector, sizeof(sector), true);
    } else if (get32(sector) == 0x52465242u) {
        vm_write(c->vm, c->a[0], sector, 16);
    } else {
        uint8_t z[16] = {0};
        vm_write(c->vm, c->a[0], z, 16);
    }
    return ST_SUCCESS;
}

/* ---- el modelo de controladores (IRP): ningún juego lo usa en serio ---- */

static uint64_t IoInvalidDeviceRequest(KCall *c)
{
    (void)c;
    return ST_INVALID_DEVICE_REQUEST;
}

/* IoBuildSynchronousFsdRequest(...): sin IRP */
static uint64_t IoBuildSynchronousFsdRequest(KCall *c)
{
    (void)c;
    return 0;
}

static uint64_t IoNothing(KCall *c)
{
    (void)c;
    return 0;
}

void kio_set_media(Kernel *k, const XDisc *disc, const char *hdd_root)
{
    k->disc = disc;
    snprintf(k->hdd_root, sizeof(k->hdd_root), "%s", hdd_root);
    mkdirs(k->hdd_root);
}

void kio_register(Kernel *k)
{
    for (int i = 0; i < MAX_FILES; i++) {
        if (g_files[i].used) {
            if (g_files[i].fp) {
                fclose(g_files[i].fp);
            }
            free(g_files[i].listing);
        }
    }
    memset(g_files, 0, sizeof(g_files));
    memset(g_links, 0, sizeof(g_links));
    memset(g_devices, 0, sizeof(g_devices));
    memset(g_cluster_sectors, 0, sizeof(g_cluster_sectors));
    link_set("\\??\\D:", "\\Device\\CdRom0");
    link_set("\\??\\CdRom0:", "\\Device\\CdRom0");
    if (!k->hdd_root[0]) {
        snprintf(k->hdd_root, sizeof(k->hdd_root), "hdd");
    }
    kernel_impl(k, "NtCreateFile", NtCreateFile);
    kernel_impl(k, "NtOpenFile", NtOpenFile);
    kernel_impl(k, "NtReadFile", NtReadFile);
    kernel_impl(k, "NtReadFileScatter", NtReadFileScatter);
    kernel_impl(k, "NtWriteFile", NtWriteFile);
    kernel_impl(k, "NtQueryInformationFile", NtQueryInformationFile);
    kernel_impl(k, "NtSetInformationFile", NtSetInformationFile);
    kernel_impl(k, "NtQueryVolumeInformationFile", NtQueryVolumeInformationFile);
    kernel_impl(k, "IoQueryVolumeInformation", IoQueryVolumeInformation);
    kernel_impl(k, "NtQueryDirectoryFile", NtQueryDirectoryFile);
    kernel_impl(k, "NtQueryFullAttributesFile", NtQueryFullAttributesFile);
    kernel_impl(k, "NtDeleteFile", NtDeleteFile);
    kernel_impl(k, "NtFlushBuffersFile", NtFlushBuffersFile);
    kernel_impl(k, "NtFsControlFile", NtControlFile);
    kernel_impl(k, "NtDeviceIoControlFile", NtControlFile);
    kernel_impl(k, "IoSynchronousDeviceIoControlRequest", IoSynchronousDeviceIoControlRequest);
    kernel_impl(k, "IoCreateSymbolicLink", IoCreateSymbolicLink);
    kernel_impl(k, "IoDeleteSymbolicLink", IoDeleteSymbolicLink);
    kernel_impl(k, "NtOpenSymbolicLinkObject", NtOpenSymbolicLinkObject);
    kernel_impl(k, "NtQuerySymbolicLinkObject", NtQuerySymbolicLinkObject);
    kernel_impl(k, "ObReferenceObjectByName", ObReferenceObjectByName);
    kernel_impl(k, "IoCreateDevice", IoCreateDevice);
    kernel_impl(k, "IoDeleteDevice", IoDeleteDevice);
    kernel_impl(k, "IoDismountVolumeByName", IoDismountVolumeByName);
    kernel_impl(k, "ExReadWriteRefurbInfo", ExReadWriteRefurbInfo);
    kernel_impl(k, "IoInvalidDeviceRequest", IoInvalidDeviceRequest);
    kernel_impl(k, "IoBuildSynchronousFsdRequest", IoBuildSynchronousFsdRequest);
    kernel_impl(k, "IoSynchronousFsdRequest", IoInvalidDeviceRequest);
    kernel_impl(k, "IofCallDriver", IoInvalidDeviceRequest);
    kernel_impl(k, "IofCompleteRequest", IoNothing);
    kernel_impl(k, "IoStartPacket", IoNothing);
    kernel_impl(k, "IoStartNextPacket", IoNothing);
    kernel_impl(k, "IoMarkIrpMustComplete", IoNothing);
}
