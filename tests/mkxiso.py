#!/usr/bin/env python3
"""Crea una XISO mínima de prueba (sin contenido de Microsoft).

Raíz con default.xbe, una carpeta media/ y un archivo, más un XBE falso con
cabecera, certificado, 2 secciones, 2 librerías y una tabla del kernel.
Uso: mkxiso.py salida.iso [--full-xgd2]
"""
import hashlib
import struct
import sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
ENTRY_KEY = 0xA8FC57AB
THUNK_KEY = 0x5B6D40B6


def entry(left, right, sector, size, attr, name):
    n = name.encode()
    e = struct.pack("<HHIIBB", left, right, sector, size, attr, len(n)) + n
    return e + b"\0" * ((-len(e)) % 4)


def make_xbe():
    base = 0x10000
    hsize = 0x1000
    h = bytearray(hsize)
    struct.pack_into("<I", h, 0x000, 0x48454258)          # XBEH
    struct.pack_into("<I", h, 0x104, base)
    struct.pack_into("<I", h, 0x108, hsize)
    struct.pack_into("<I", h, 0x10C, 0x5000)              # tamaño de imagen
    struct.pack_into("<I", h, 0x114, 0x40000000)
    struct.pack_into("<I", h, 0x118, base + 0x200)        # certificado
    struct.pack_into("<I", h, 0x11C, 3)                   # secciones
    struct.pack_into("<I", h, 0x120, base + 0x400)
    struct.pack_into("<I", h, 0x128, (base + 0x1000) ^ ENTRY_KEY)
    struct.pack_into("<I", h, 0x158, (base + 0x2000) ^ THUNK_KEY)
    struct.pack_into("<I", h, 0x160, 2)                   # librerías
    struct.pack_into("<I", h, 0x164, base + 0x600)
    # certificado
    c = 0x200
    struct.pack_into("<I", h, c + 0x08, 0x58580001)       # "XX-001"
    name = "Juego de Prueba".encode("utf-16-le")
    h[c + 0x0C:c + 0x0C + len(name)] = name
    struct.pack_into("<I", h, c + 0xA0, 0x1 | 0x4)        # NA + EU
    struct.pack_into("<I", h, c + 0xAC, 3)
    # secciones (56 bytes): flags, va, vsize, raw_off, raw_size, name_va
    struct.pack_into("<IIIIII", h, 0x400, 0x06, base + 0x1000, 0x1000, 0x1000, 0x1000, base + 0x500)
    struct.pack_into("<IIIIII", h, 0x400 + 56, 0x03, base + 0x2000, 0x1000, 0x2000, 0x1000, base + 0x508)
    struct.pack_into("<IIIIII", h, 0x400 + 112, 0x00, base + 0x3000, 0x1000, 0x3000, 0x1000, base + 0x510)
    h[0x500:0x506] = b".text\0"
    h[0x510:0x51A] = b"$$XTIMAGE\0"
    h[0x508:0x50F] = b".rdata\0"
    # librerías (16 bytes)
    struct.pack_into("<8sHHHH", h, 0x600, b"XAPILIB", 1, 0, 5849, 1)
    struct.pack_into("<8sHHHH", h, 0x610, b"D3D8", 1, 0, 5849, 0x8000)
    text = bytearray(0x1000)
    # push 0x1234; call [NtClose]; push 2; call [HalReturnToFirmware]; jmp $
    text[0:21] = (b"\x68\x34\x12\x00\x00" b"\xff\x15\x08\x20\x01\x00"
                  b"\x6a\x02" b"\xff\x15\x04\x20\x01\x00" b"\xeb\xfe")
    rdata = bytearray(0x1000)
    for i, ordn in enumerate([1, 49, 187, 366]):           # 4 imports, termina en 0
        struct.pack_into("<I", rdata, i * 4, 0x80000000 | ordn)
    # $$XTIMAGE: XPR0 de 4x4 ARGB8888 con swizzle; el píxel (x, y) es R=x*40, G=y*40.
    img = bytearray(0x1000)
    fmt = (2 << 24) | (2 << 20) | (0x06 << 8)
    struct.pack_into("<4sIIIIIII", img, 0, b"XPR0", 0x60, 0x20, 0, 0, 0, fmt, 0)
    def dep(v, mask):
        r, bit = 0, 1
        while mask:
            low = mask & -mask
            if v & 1:
                r |= low
            v >>= 1
            mask &= mask - 1
        return r
    for y in range(4):
        for x in range(4):
            i = dep(y, 0b1010) | dep(x, 0b0101)
            struct.pack_into("<I", img, 0x20 + i * 4, 0xFF000000 | ((x * 40) << 16) | ((y * 40) << 8) | 7)
    # Firma SHA-1 de cada sección (tamaño de 32 bits + bytes) en su cabecera +0x24.
    for i, data in enumerate([text, rdata, img]):
        dig = hashlib.sha1(struct.pack("<I", len(data)) + bytes(data)).digest()
        h[0x400 + i * 56 + 0x24:0x400 + i * 56 + 0x38] = dig
    if "--bad-sha" in sys.argv:
        h[0x400 + 0x24] ^= 0xFF
    return bytes(h) + bytes(text) + bytes(rdata) + bytes(img)


def main():
    out = sys.argv[1]
    full = "--full-xgd2" in sys.argv
    xbe = make_xbe()
    readme = b"hola desde la iso de prueba\n"

    root_sector, media_sector, xbe_sector, readme_sector = 33, 34, 35, 35 + (len(xbe) + SECTOR - 1) // SECTOR
    # Raíz: árbol con "default.xbe" arriba, "media" a la derecha.
    e0 = entry(0, 0, xbe_sector, len(xbe), 0x20, "default.xbe")
    e1_off = len(e0)
    e0 = entry(0, e1_off // 4, xbe_sector, len(xbe), 0x20, "default.xbe")
    e1 = entry(0, 0, media_sector, SECTOR, 0x10, "media")
    root = (e0 + e1).ljust(SECTOR, b"\xFF")
    media = entry(0, 0, readme_sector, len(readme), 0x20, "leeme.txt").ljust(SECTOR, b"\xFF")

    img = bytearray(SECTOR * (readme_sector + 2))
    vd = bytearray(SECTOR)
    vd[0:20] = MAGIC
    struct.pack_into("<II", vd, 20, root_sector, SECTOR)
    vd[0x7EC:0x7EC + 20] = MAGIC
    img[32 * SECTOR:33 * SECTOR] = vd
    img[root_sector * SECTOR:(root_sector + 1) * SECTOR] = root
    img[media_sector * SECTOR:(media_sector + 1) * SECTOR] = media
    img[xbe_sector * SECTOR:xbe_sector * SECTOR + len(xbe)] = xbe
    img[readme_sector * SECTOR:readme_sector * SECTOR + len(readme)] = readme

    with open(out, "wb") as f:
        if full:
            f.seek(0xFD90000)                              # partición XGD2
        f.write(img)


if __name__ == "__main__":
    main()
