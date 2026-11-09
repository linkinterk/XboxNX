#!/usr/bin/env python3
"""Pares de instrucciones contra Unicorn: una que pone flags (ALU, inc,
dec) y otra que los lee (jcc, setcc, cmovcc, adc, sbb, pushf, lahf, rcl).
Prueba los flags perezosos del intérprete entre instrucciones.
Uso: flags_pair.py ruta/al/cpu_diff [casos] [semilla]"""
import random
import subprocess
import sys

from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
                               UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI,
                               UC_X86_REG_EFLAGS, UC_X86_REG_EIP)

CODE, MEM, STACK = 0x1000, 0x100000, 0x200000
REGS = [UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
        UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI]
AF = 0x10
MASK = 0x8D5 & ~AF  # CF PF ZF SF OF (AF: indefinido tras las lógicas)
rnd = random.Random(int(sys.argv[3]) if len(sys.argv) > 3 else 1)


def r32():
    return rnd.choice([0, 1, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF, 0x7FFFFFFF, 0x80000000,
                       0xFFFFFFFF, rnd.getrandbits(32), rnd.getrandbits(8)])


def reg_rm():
    # sin esp (el 4) como destino
    return 0xC0 | rnd.choice([0, 1, 2, 3, 5, 6, 7]) << 3 | rnd.choice([0, 1, 2, 3, 5, 6, 7])


def producer():
    k = rnd.randrange(4)
    if k == 0:
        op = rnd.choice([0x00, 0x01, 0x08, 0x09, 0x10, 0x11, 0x18, 0x19, 0x20, 0x21, 0x28, 0x29, 0x30, 0x31,
                         0x38, 0x39])
        return bytes([op, reg_rm()])
    if k == 1:
        return bytes([rnd.choice([0x40, 0x41, 0x42, 0x43, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4D, 0x4E,
                                  0x4F])])
    if k == 2:
        return bytes([0x83, 0xC0 | rnd.randrange(8) << 3 | rnd.choice([0, 1, 2, 3, 5, 6, 7]), rnd.getrandbits(8)])
    return bytes([0x66, rnd.choice([0x01, 0x29, 0x39, 0x21]), reg_rm()])


def consumer():
    k = rnd.randrange(8)
    if k == 0:
        return bytes([0x70 | rnd.randrange(16), 0x00])
    if k == 1:
        return bytes([0x0F, 0x90 | rnd.randrange(16), 0xC0 | rnd.choice([0, 1, 2, 3])])
    if k == 2:
        return bytes([0x0F, 0x40 | rnd.randrange(16), reg_rm()])
    if k == 3:
        return bytes([rnd.choice([0x11, 0x13, 0x19, 0x1B]), reg_rm()])
    if k == 4:
        return b"\x9c"
    if k == 5:
        return b"\x9f"
    if k == 6:
        return bytes([0xD1, 0xD0 | rnd.choice([0, 1, 2, 3])])
    return bytes([rnd.choice([0x40, 0x48]) | rnd.choice([0, 1, 2, 3])])


def main():
    proc = subprocess.Popen([sys.argv[1]], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    cases = int(sys.argv[2]) if len(sys.argv) > 2 else 5000
    fails = 0
    for _ in range(cases):
        code = producer() + consumer()
        regs = [r32() for _ in range(8)]
        regs[4] = STACK
        flags = 0x2 | (rnd.getrandbits(12) & 0x8D5)
        mem = bytes(256)
        stack = bytes(0x2000)
        mu = Uc(UC_ARCH_X86, UC_MODE_32)
        mu.mem_map(0, 0x4000)
        mu.mem_map(MEM, 0x1000)
        mu.mem_map(STACK - 0x1000, 0x2000)
        mu.mem_write(CODE, code + b"\x90" * 16)
        for r, v in zip(REGS, regs):
            mu.reg_write(r, v)
        mu.reg_write(UC_X86_REG_EFLAGS, flags)
        mu.emu_start(CODE, 0xFFFFFFFF, count=2)
        ref = [mu.reg_read(r) for r in REGS]
        rflags = mu.reg_read(UC_X86_REG_EFLAGS)
        rstk = bytes(mu.mem_read(STACK - 16, 32))
        line = "%s %s %x %s %s m\n" % (code.hex(), " ".join("%x" % r for r in regs), flags, mem.hex(),
                                       stack[0x1000 - 16:0x1000 + 16].hex())
        proc.stdin.write(line)
        proc.stdin.flush()
        out = proc.stdout.readline().split()
        got = [int(x, 16) for x in out[2:10]]
        gflags = int(out[10], 16)
        gstk = bytes.fromhex(out[12])
        bad = []
        # lahf deja AF en AH: se compara sin ese bit
        if code[-1] == 0x9F:
            ref[0] &= ~(AF << 8)
            got[0] &= ~(AF << 8)
        if got != ref:
            bad.append("regs %s != %s" % ([hex(x) for x in got], [hex(x) for x in ref]))
        if (gflags ^ rflags) & MASK:
            bad.append("flags %x != %x" % (gflags, rflags))
        if code[-1] == 0x9C:
            g = int.from_bytes(gstk[12:16], "little") & ~AF
            r = int.from_bytes(rstk[12:16], "little") & ~AF
            if g != r:
                bad.append("pushf %x != %x" % (g, r))
        if bad:
            fails += 1
            if fails <= 20:
                print("FALLO %s: %s" % (code.hex(), "; ".join(bad)))
    print("pares %d, fallos %d" % (cases, fails))
    sys.exit(1 if fails else 0)


main()
