/*
 * XboxNX — lector de cabeceras XBE (el ejecutable de Xbox).
 *
 * Adaptado de hakuX (hw/xbox/hle/core/xbe.c, GPL-3.0-or-later), con el
 * certificado (nombre, región, versión) agregado.
 */
#ifndef XBOXNX_XBE_H
#define XBOXNX_XBE_H

#include <stdbool.h>
#include <stdint.h>

#define XBE_MAGIC            0x48454258u  /* "XBEH" */
#define XBE_ENTRY_KEY_RETAIL 0xA8FC57ABu
#define XBE_ENTRY_KEY_DEBUG  0x94859D4Bu
#define XBE_THUNK_KEY_RETAIL 0x5B6D40B6u
#define XBE_THUNK_KEY_DEBUG  0xEFB1F152u

#define XBE_SEC_WRITABLE   0x01u
#define XBE_SEC_PRELOAD    0x02u
#define XBE_SEC_EXECUTABLE 0x04u

#define XBE_MAX_SECTIONS 256
#define XBE_MAX_LIBS     48

typedef struct XbeSection {
    char     name[9];
    uint32_t flags;
    uint32_t va;
    uint32_t vsize;
    uint32_t raw_off;
    uint32_t raw_size;
} XbeSection;

typedef struct XbeLib {
    char     name[9];
    uint16_t major, minor, build;
    uint16_t qfe;
    bool     debug_build;
} XbeLib;

typedef struct XbeInfo {
    uint32_t base;
    uint32_t sizeof_headers;
    uint32_t sizeof_image;
    uint32_t timedate;
    uint32_t entry;          /* decodificado */
    uint32_t kernel_thunk;   /* decodificado */
    bool     debug_keys;
    /* certificado */
    uint32_t title_id;
    char     title_name[41]; /* ASCII ('?' fuera de ASCII) */
    uint32_t region;
    uint32_t version;
    unsigned nsections;
    XbeSection sections[XBE_MAX_SECTIONS];
    unsigned nlibs;
    XbeLib libs[XBE_MAX_LIBS];
} XbeInfo;

enum {
    XBE_OK = 0,
    XBE_ERR_SHORT = -1,
    XBE_ERR_MAGIC = -2,
    XBE_ERR_RANGE = -3,
    XBE_ERR_ENTRY = -4,
};

/* hdr: los primeros sizeof_headers bytes del XBE. */
int xbe_parse(const uint8_t *hdr, uint32_t hdr_len, XbeInfo *out);
const char *xbe_error(int rc);
const XbeLib *xbe_find_lib(const XbeInfo *x, const char *name);
/* Región en texto ("NA JP EU"). */
void xbe_region_str(uint32_t region, char *out, unsigned cap);
/* Offset en el archivo XBE de una VA dentro de alguna sección (o de las
 * cabeceras). false si no está en el archivo. */
bool xbe_va_to_file(const XbeInfo *x, uint32_t va, uint32_t *file_off, uint32_t *avail);

#endif
