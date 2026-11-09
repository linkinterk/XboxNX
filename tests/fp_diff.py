#!/usr/bin/env python3
"""Prueba diferencial de x87, SSE y MMX del intérprete contra Unicorn.

Cada caso es una secuencia: carga valores de la memoria en registros, hace
la operación y guarda los registros en memoria; se compara la memoria, los
registros y los flags. Uso: fp_diff.py ruta/al/cpu_diff [casos] [semilla]
"""
import math
import random
import struct
import subprocess
import sys

from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_32
from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
                               UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI,
                               UC_X86_REG_EFLAGS)

CODE, MEM, STACK = 0x1000, 0x100000, 0x200000
REGS = [UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
        UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI]
ARITH = 0x8D5
rnd = random.Random(int(sys.argv[3]) if len(sys.argv) > 3 else 1)


def nice():
    k = rnd.randrange(12)
    if k == 0:
        return rnd.choice([0.0, -0.0, 1.0, -1.0, 0.5, 2.0, 3.0, -2.5, 1e10, -1e-3])
    if k == 1:
        return rnd.choice([math.inf, -math.inf, math.nan])
    if k == 2:
        return float(rnd.randrange(-100000, 100000))
    if k == 3:
        return rnd.choice([0.5, 1.5, 2.5, -0.5, -1.5, -2.5, 3.5, 2147483647.0, 2147483648.0, -2147483649.0, 4e9])
    return rnd.uniform(-1000, 1000) * 10 ** rnd.randrange(-6, 7)


def f32(v):
    try:
        return struct.pack("<f", v)
    except OverflowError:
        return struct.pack("<f", math.copysign(math.inf, v))


def mem_floats():
    m = b"".join(f32(nice()) for _ in range(32))      # 0x00-0x7F: floats
    m += b"".join(struct.pack("<d", nice()) for _ in range(12))  # 0x80-0xDF: doubles
    m += bytes(rnd.getrandbits(8) for _ in range(16))  # 0xE0-0xEF
    cw = rnd.choice([0x027F, 0x067F, 0x0A7F, 0x0E7F, 0x027F, 0x007F])
    m += struct.pack("<H", cw) + bytes(14)             # 0xF0: palabra de control
    return m

# esi = MEM (floats), edi = MEM+0x80 (doubles), ebx = MEM+0x40 (destino), ebp = MEM+0xF0 (cw)
LOAD_SSE = bytes.fromhex("0f100e" "0f105610" "0f105e20")         # movups xmm1..3, [esi], [esi+16], [esi+32]
STORE_SSE = bytes.fromhex("0f110b" "0f115310" "0f115b20")       # movups [ebx..], xmm1..3


def gen_sse():
    k = rnd.randrange(10)
    ss = rnd.random() < 0.4
    pre = b"\xf3" if ss else b""
    src = rnd.choice([b"\xca", b"\xcb", b"\x4e\x30"])  # xmm1, xmm2 / xmm1, xmm3 / xmm1, [esi+0x30]
    if k <= 3:
        op = rnd.choice([0x58, 0x59, 0x5C, 0x5E, 0x5D, 0x5F, 0x51])
        body = pre + bytes([0x0F, op]) + src
    elif k == 4:
        body = bytes([0x0F, rnd.choice([0x54, 0x55, 0x56, 0x57, 0x14, 0x15])]) + src
    elif k == 5:
        body = pre + b"\x0f\xc2" + src + bytes([rnd.randrange(8)])
    elif k == 6:
        body = b"\x0f\xc6" + src + bytes([rnd.randrange(256)])
    elif k == 7:  # comiss / ucomiss (flags) y movmskps
        body = bytes([0x0F, rnd.choice([0x2E, 0x2F])]) + src + b"\x0f\x50\xc1"
    elif k == 8:  # conversiones a entero y de entero
        body = rnd.choice([b"\xf3\x0f\x2d\xc1", b"\xf3\x0f\x2c\xc2", b"\xf3\x0f\x2a\xcf",
                           b"\xf3\x0f\x2d\x46\x08", b"\x0f\x12\xca", b"\x0f\x16\xcb", b"\x0f\x16\x4e\x08",
                           b"\xf3\x0f\x10\x4e\x04", b"\xf3\x0f\x10\xca", b"\x0f\x13\x4b\x30"])
    else:
        body = rnd.choice([b"\x0f\x28\xca", b"\x0f\x29\x4b\x30", b"\x0f\x17\x53\x30"])
    return LOAD_SSE + body + STORE_SSE, "sse"


def gen_x87():
    # fninit; fldcw [ebp]; fld qword [edi]; fld qword [edi+8]
    pre = bytes.fromhex("dbe3" "d96d00" "dd07" "dd4708")
    k = rnd.randrange(9)
    if k <= 2:  # aritmética con st(1)
        body = bytes([rnd.choice([0xD8, 0xDC, 0xDE]), 0xC1 | rnd.choice([0, 1, 4, 5, 6, 7]) << 3])
    elif k == 3:  # con memoria de 32 y 64 bits
        body = bytes([rnd.choice([0xD8, 0xDC]), 0x40 | rnd.choice([0, 1, 4, 5, 6, 7]) << 3 | 6, rnd.randrange(0, 0x20, 4)])
    elif k == 4:  # (fscale no: fuera del rango de double el x87 de 80 bits y el nuestro difieren)
        body = rnd.choice([b"\xd9\xe0", b"\xd9\xe1", b"\xd9\xfa", b"\xd9\xc9", b"\xd9\xfc",
                           b"\xd9\xe8", b"\xd9\xee", b"\xd9\xc1"])
    elif k == 5:  # comparaciones: estado y flags
        body = rnd.choice([b"\xd8\xd1", b"\xdd\xe1", b"\xd9\xe4", b"\xd8\x56\x04"]) + b"\xdf\xe0\x66\x25\x00\x45"
        body = body if rnd.random() < 0.6 else rnd.choice([b"\xdb\xf1", b"\xdb\xe9", b"\xdf\xf1"])
    elif k == 6:  # a entero con el redondeo del cw
        body = rnd.choice([b"\xdb\x5b\x10", b"\xdf\x5b\x10", b"\xdf\x7b\x10", b"\xdb\x53\x10"])
    elif k == 7:  # enteros a la pila
        body = rnd.choice([b"\xdb\x46\x00", b"\xdf\x46\x00", b"\xda\x46\x04", b"\xde\x46\x04"])
    else:  # float32 y fcmov
        body = rnd.choice([b"\xd9\x46\x08", b"\xd9\x5b\x18", b"\xda\xc1", b"\xdb\xd1", b"\xda\xc9"])
    post = bytes.fromhex("dd5b20" "dd5b28")  # fstp qword [ebx+0x20], [ebx+0x28]
    return pre + body + post, "x87"


def gen_mmx():
    pre = bytes.fromhex("0f6f0e" "0f6f5608")  # movq mm1,[esi]; movq mm2,[esi+8]
    op = rnd.choice([0xFC, 0xFD, 0xFE, 0xF8, 0xF9, 0xFA, 0xEC, 0xED, 0xE8, 0xE9, 0xDC, 0xDD, 0xD8, 0xD9,
                     0x74, 0x75, 0x76, 0x64, 0x65, 0x66, 0xD5, 0xE5, 0xE4, 0xDB, 0xDF, 0xEB, 0xEF,
                     0x60, 0x61, 0x62, 0x68, 0x69, 0x6A, 0x63, 0x67, 0x6B, 0xDA, 0xDE, 0xEA, 0xEE, 0xE0, 0xE3,
                     0xF5, 0xF6, 0xD1, 0xD2, 0xD3, 0xE1, 0xE2, 0xF1, 0xF2, 0xF3])
    src = rnd.choice([b"\xca", b"\x4e\x10"])
    if op in (0xD1, 0xD2, 0xD3, 0xE1, 0xE2, 0xF1, 0xF2, 0xF3):
        body = b"\x0f\x6e\xd1" + bytes([0x0F, op, 0xCA])  # movd mm2, ecx (cuenta)
    else:
        body = bytes([0x0F, op]) + src
    k = rnd.random()
    if k < 0.15:
        body = bytes([0x0F, rnd.choice([0x71, 0x72, 0x73]), 0xC0 | rnd.choice([2, 6]) << 3 | 1, rnd.randrange(70)])
    elif k < 0.25:
        body = rnd.choice([b"\x0f\x70\xca" + bytes([rnd.randrange(256)]), b"\x0f\xc5\xc1" + bytes([rnd.randrange(4)]),
                           b"\x0f\xc4\xc8" + bytes([rnd.randrange(4)]), b"\x0f\xd7\xc1", b"\x0f\x7e\xc8"])
    post = bytes.fromhex("0f7f0b" "0f7f5308" "0f77")
    return pre + body + post, "mmx"


def run_unicorn(code, regs, flags, mem):
    mu = Uc(UC_ARCH_X86, UC_MODE_32)
    mu.mem_map(0, 0x4000)
    mu.mem_map(MEM, 0x1000)
    mu.mem_map(STACK - 0x1000, 0x2000)
    mu.mem_write(CODE, code + b"\x90" * 16)
    mu.mem_write(MEM, mem)
    for r, v in zip(REGS, regs):
        mu.reg_write(r, v)
    mu.reg_write(UC_X86_REG_EFLAGS, flags)
    try:
        mu.emu_start(CODE, CODE + len(code), count=1000)
    except UcError as e:
        return None, str(e)
    return ([mu.reg_read(r) for r in REGS], mu.reg_read(UC_X86_REG_EFLAGS), bytes(mu.mem_read(MEM, 256))), None


def main():
    exe = sys.argv[1]
    cases = int(sys.argv[2]) if len(sys.argv) > 2 else 5000
    proc = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    fails, missing, by = 0, set(), {}
    for _ in range(cases):
        code, name = rnd.choice([gen_sse, gen_sse, gen_x87, gen_x87, gen_mmx])()
        regs = [rnd.getrandbits(32) for _ in range(8)]
        regs[1] = rnd.choice([0, 1, 3, 7, 15, 16, 31, 33, 63, 64, rnd.getrandbits(32)])
        regs[3], regs[4], regs[5], regs[6], regs[7] = MEM + 0x40, STACK, MEM + 0xF0, MEM, MEM + 0x80
        flags = 0x2 | (rnd.getrandbits(12) & ARITH)
        mem = mem_floats()
        ref, err = run_unicorn(code, regs, flags, mem)
        proc.stdin.write("%s %s %x %s %s m\n" % (code.hex(), " ".join("%x" % r for r in regs), flags, mem.hex(),
                                                 "00" * 32))
        proc.stdin.flush()
        out = proc.stdout.readline().split()
        st = int(out[0])
        if st == 4:
            missing.add(code.hex())
            continue
        if err:
            fails += 1
            print("FALLO %s %s: unicorn %s" % (name, code.hex(), err))
            continue
        got = [int(x, 16) for x in out[2:10]]
        gflags = int(out[10], 16)
        gmem = bytes.fromhex(out[11])
        rregs, rflags, rmem = ref
        bad = []
        for i in range(8):
            if got[i] != rregs[i]:
                bad.append("r%d %x/%x" % (i, got[i], rregs[i]))
        fmask = ARITH
        if name == "x87" and any(x in code.hex() for x in ("dbf1", "dbe9", "dff1")):
            fmask &= ~0x890  # fcomi: Intel pone OF SF AF a 0; QEMU los deja
        if (gflags & fmask) != (rflags & fmask):
            bad.append("flags %x/%x" % (gflags & ARITH, rflags & ARITH))
        if gmem != rmem:
            diff = [i for i in range(256) if gmem[i] != rmem[i]]
            # NaN: el valor por defecto depende del host (x86 0xFFC00000, ARM 0x7FC00000)
            bad.append("memoria en %s" % ",".join("%x" % (d & ~3) for d in sorted(set(d & ~3 for d in diff))[:6]))
        if bad:
            fails += 1
            by[name] = by.get(name, 0) + 1
            if fails <= 40:
                print("FALLO %-4s %s: %s" % (name, code.hex(), "; ".join(bad)))
    proc.stdin.close()
    proc.wait()
    print("casos %d, fallos %d %s" % (cases, fails, by))
    if missing:
        print("faltan:", sorted(missing)[:10])
    sys.exit(1 if fails or missing else 0)


if __name__ == "__main__":
    main()
