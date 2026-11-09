#!/usr/bin/env python3
"""Prueba diferencial del intérprete x86 contra Unicorn (QEMU).

Genera instrucciones aleatorias, ejecuta cada una en el intérprete
(tests/cpu_diff.c) y en Unicorn, y compara registros, flags definidos,
memoria y pila. Uso: cpu_diff.py ruta/al/cpu_diff [casos] [semilla]
"""
import random
import subprocess
import sys

from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_32
from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
                               UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI,
                               UC_X86_REG_EFLAGS, UC_X86_REG_EIP)

CODE, MEM, STACK = 0x1000, 0x100000, 0x200000
REGS = [UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
        UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI]
CF, PF, AF, ZF, SF, OF, DF = 1, 4, 0x10, 0x40, 0x80, 0x800, 0x400
ARITH = CF | PF | AF | ZF | SF | OF

rnd = random.Random(int(sys.argv[3]) if len(sys.argv) > 3 else 1)


def r32():
    return rnd.choice([0, 1, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF, 0x7FFFFFFF, 0x80000000,
                       0xFFFFFFFF, rnd.getrandbits(32), rnd.getrandbits(32), rnd.getrandbits(8),
                       rnd.getrandbits(16)])


def modrm_reg():
    return 0xC0 | rnd.randrange(8) << 3 | rnd.randrange(8)


def modrm_mem(reg=None):
    """[ebx+disp8] / [esi] / [edi+disp8] / [ebx+esi*1+disp8] (SIB) dentro de MEM."""
    reg = rnd.randrange(8) if reg is None else reg
    k = rnd.randrange(4)
    if k == 0:
        return bytes([0x40 | reg << 3 | 3, rnd.randrange(0, 0x40)])
    if k == 1:
        return bytes([0x00 | reg << 3 | 6])
    if k == 2:
        return bytes([0x40 | reg << 3 | 7, rnd.randrange(0, 0x40)])
    return bytes([0x44 | reg << 3, 0x33, rnd.randrange(0, 0x20)])  # [ebx+esi+d8]


def rm(reg=None):
    if rnd.random() < 0.55:
        m = modrm_reg()
        if reg is not None:
            m = (m & 0xC7) | reg << 3
        return bytes([m]), False
    return modrm_mem(reg), True


def imm(n):
    return rnd.getrandbits(8 * n).to_bytes(n, "little")


def gen():
    """Devuelve (bytes, flags_indefinidos, nombre)."""
    p66 = b"\x66" if rnd.random() < 0.15 else b""
    osz = 2 if p66 else 4
    k = rnd.randrange(30)
    if k <= 3:  # ALU 00-3D
        op = rnd.randrange(8) << 3 | rnd.randrange(6)
        und = AF if (op >> 3) in (1, 4, 6) else 0
        if (op & 7) < 4:
            m, _ = rm()
            return p66 + bytes([op]) + m, und, "alu"
        return p66 + bytes([op]) + imm(1 if (op & 1) == 0 else osz), und, "alu-imm"
    if k == 4:  # grupo 1
        op = rnd.choice([0x80, 0x81, 0x83])
        sub = rnd.randrange(8)
        m, _ = rm(sub)
        n = 1 if op != 0x81 else osz
        und = AF if sub in (1, 4, 6) else 0
        return p66 + bytes([op]) + m + imm(n), und, "grp1"
    if k == 5:  # inc/dec
        if rnd.random() < 0.5:
            return p66 + bytes([0x40 + rnd.randrange(16)]), 0, "incdec"
        op = rnd.choice([0xFE, 0xFF])
        m, _ = rm(rnd.randrange(2))
        return p66 + bytes([op]) + m, 0, "incdec-m"
    if k == 6:  # test
        op = rnd.choice([0x84, 0x85, 0xA8, 0xA9, 0xF6, 0xF7])
        if op in (0x84, 0x85):
            m, _ = rm()
            return p66 + bytes([op]) + m, AF, "test"
        if op in (0xA8, 0xA9):
            return p66 + bytes([op]) + imm(1 if op == 0xA8 else osz), AF, "test"
        m, _ = rm(0)
        return p66 + bytes([op]) + m + imm(1 if op == 0xF6 else osz), AF, "test"
    if k in (7, 8):  # grupo 2
        op = rnd.choice([0xC0, 0xC1, 0xD0, 0xD1, 0xD2, 0xD3])
        sub = rnd.choice([0, 1, 2, 3, 4, 5, 7])
        m, _ = rm(sub)
        tail = bytes([rnd.choice([0, 1, 2, 3, 7, 8, 9, 15, 16, 17, 31, 32, 33, rnd.randrange(256)])]) \
            if op in (0xC0, 0xC1) else b""
        return p66 + bytes([op]) + m + tail, ("shift", op, sub, tail), "grp2"
    if k == 9:  # grupo 3
        op = rnd.choice([0xF6, 0xF7])
        sub = rnd.randrange(2, 8)
        m, _ = rm(sub)
        und = {2: 0, 3: 0, 4: SF | ZF | AF | PF, 5: SF | ZF | AF | PF}.get(sub, ARITH)
        return p66 + bytes([op]) + m, und, "grp3"
    if k == 10:  # imul
        c = rnd.randrange(3)
        if c == 0:
            m, _ = rm()
            return p66 + b"\x0f\xaf" + m, SF | ZF | AF | PF, "imul2"
        m, _ = rm()
        if c == 1:
            return p66 + b"\x6b" + m + imm(1), SF | ZF | AF | PF, "imul3"
        return p66 + b"\x69" + m + imm(osz), SF | ZF | AF | PF, "imul3"
    if k == 11:  # mov
        op = rnd.choice([0x88, 0x89, 0x8A, 0x8B])
        m, _ = rm()
        return p66 + bytes([op]) + m, 0, "mov"
    if k == 12:
        op = rnd.choice([0xC6, 0xC7, 0xB0 + rnd.randrange(8), 0xB8 + rnd.randrange(8)])
        if op in (0xC6, 0xC7):
            m, _ = rm(0)
            return p66 + bytes([op]) + m + imm(1 if op == 0xC6 else osz), 0, "mov-imm"
        return p66 + bytes([op]) + imm(1 if op < 0xB8 else osz), 0, "mov-imm"
    if k == 13:  # moffs
        op = rnd.choice([0xA0, 0xA1, 0xA2, 0xA3])
        return p66 + bytes([op]) + (MEM + rnd.randrange(0, 0x80)).to_bytes(4, "little"), 0, "moffs"
    if k == 14:  # movzx/movsx
        op = rnd.choice([0xB6, 0xB7, 0xBE, 0xBF])
        m, _ = rm()
        return p66 + bytes([0x0F, op]) + m, 0, "movx"
    if k == 15:  # lea con cualquier modrm/sib (no toca memoria)
        mod = rnd.randrange(3)
        reg = rnd.randrange(8)
        r_m = rnd.randrange(8)
        b = bytes([mod << 6 | reg << 3 | r_m])
        if r_m == 4:
            b += bytes([rnd.randrange(256)])
        if mod == 1:
            b += imm(1)
        elif mod == 2 or (mod == 0 and (r_m == 5 or (r_m == 4 and (b[1] & 7) == 5))):
            b += imm(4)
        return b"\x8d" + b, 0, "lea"
    if k == 16:  # xchg / cmpxchg / xadd
        c = rnd.randrange(4)
        if c == 0:
            m, _ = rm()
            return p66 + bytes([rnd.choice([0x86, 0x87])]) + m, 0, "xchg"
        if c == 1:
            return p66 + bytes([0x91 + rnd.randrange(7)]), 0, "xchg-a"
        m, _ = rm()
        return p66 + bytes([0x0F, rnd.choice([0xB0, 0xB1, 0xC0, 0xC1])]) + m, 0, "cmpxchg/xadd"
    if k == 17:  # setcc / cmovcc / bswap
        c = rnd.randrange(3)
        m, _ = rm()
        if c == 0:
            return bytes([0x0F, 0x90 + rnd.randrange(16)]) + m, 0, "setcc"
        if c == 1:
            return p66 + bytes([0x0F, 0x40 + rnd.randrange(16)]) + m, 0, "cmovcc"
        return bytes([0x0F, 0xC8 + rnd.randrange(8)]), 0, "bswap"
    if k == 18:  # bt*
        c = rnd.randrange(2)
        if c == 0:
            op = rnd.choice([0xA3, 0xAB, 0xB3, 0xBB])
            m, mem = rm()
            return p66 + bytes([0x0F, op]) + m, ("bt", mem), "bt-r"
        sub = rnd.randrange(4, 8)
        m, _ = rm(sub)
        return p66 + b"\x0f\xba" + m + imm(1), OF | SF | AF | PF, "bt-i"
    if k == 19:  # bsf/bsr
        m, _ = rm()
        return p66 + bytes([0x0F, rnd.choice([0xBC, 0xBD])]) + m, ("bsf",), "bsf"
    if k == 20:  # shld/shrd
        op = rnd.choice([0xA4, 0xA5, 0xAC, 0xAD])
        m, _ = rm()
        tail = bytes([rnd.randrange(32)]) if op in (0xA4, 0xAC) else b""
        return bytes([0x0F, op]) + m + tail, ("shd", op, tail), "shld"
    if k == 21:  # varios de un byte
        return p66 + bytes([rnd.choice([0x98, 0x99, 0x9E, 0x9F, 0xF5, 0xF8, 0xF9, 0xFC, 0xFD, 0x90])]), 0, "misc"
    if k == 22:  # push/pop
        c = rnd.randrange(6)
        if c == 0:
            return bytes([0x50 + rnd.choice([0, 1, 2, 3, 5, 6, 7])]), 0, "push"
        if c == 1:
            return bytes([0x58 + rnd.choice([0, 1, 2, 3, 5, 6, 7])]), 0, "pop"
        if c == 2:
            return b"\x6a" + imm(1), 0, "push"
        if c == 3:
            return b"\x68" + imm(4), 0, "push"
        if c == 4:
            return b"\x9c", 0, "pushf"
        return bytes([0xFF]) + modrm_mem(6), 0, "push-m"
    if k == 23:  # saltos
        c = rnd.randrange(5)
        if c == 0:
            return bytes([0x70 + rnd.randrange(16)]) + imm(1), 0, "jcc8"
        if c == 1:
            return bytes([0x0F, 0x80 + rnd.randrange(16)]) + rnd.randrange(-0x800, 0x800).to_bytes(4, "little", signed=True), 0, "jcc32"
        if c == 2:
            return b"\xe8" + rnd.randrange(-0x800, 0x800).to_bytes(4, "little", signed=True), 0, "call"
        if c == 3:
            return b"\xeb" + imm(1), 0, "jmp8"
        return bytes([rnd.choice([0xE2, 0xE3])]) + imm(1), 0, "loop"
    if k == 24:  # cadenas
        op = rnd.choice([0xA4, 0xA5, 0xA6, 0xA7, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF])
        rep = rnd.choice([b"", b"\xf3", b"\xf2"]) if op in (0xA6, 0xA7, 0xAE, 0xAF) else rnd.choice([b"", b"\xf3"])
        return rep + p66 + bytes([op]), ("str",), "string"
    if k == 25:  # ret / leave
        return rnd.choice([b"\xc3", b"\xc2\x08\x00", b"\xc9"]), 0, "ret"
    if k == 26:  # neg/not y cmpxchg8b
        if rnd.random() < 0.5:
            return b"\x0f\xc7" + modrm_mem(1), 0, "cmpxchg8b"
        m, _ = rm(rnd.choice([2, 3]))
        return p66 + bytes([rnd.choice([0xF6, 0xF7])]) + m, 0, "negnot"
    if k == 27:  # popf
        return b"\x9d", 0, "popf"
    if k == 28:  # cdq tras mul para valores grandes
        return p66 + b"\xf7" + bytes([0xE0 | rnd.randrange(8)]), SF | ZF | AF | PF, "mul-r"
    m, _ = rm()
    return p66 + bytes([rnd.choice([0x00, 0x29, 0x31, 0x19, 0x11])]) + m, 0, "alu2"


def run_unicorn(code, regs, flags, mem, stack, until=None):
    mu = Uc(UC_ARCH_X86, UC_MODE_32)
    mu.mem_map(0, 0x4000)  # el destino de saltos y llamadas cercanas también
    mu.mem_map(MEM, 0x1000)
    mu.mem_map(STACK - 0x1000, 0x2000)
    mu.mem_write(CODE, code + b"\x90" * 16)
    mu.mem_write(MEM, mem)
    mu.mem_write(STACK - 0x1000, stack)
    for r, v in zip(REGS, regs):
        mu.reg_write(r, v)
    mu.reg_write(UC_X86_REG_EFLAGS, flags)
    try:
        if until:  # rep: Unicorn cuenta cada vuelta como una instrucción
            mu.emu_start(CODE, until, count=100000)
        else:
            mu.emu_start(CODE, 0xFFFFFFFF, count=1)
    except UcError as e:
        return None, str(e)
    out = [mu.reg_read(r) for r in REGS]
    return (mu.reg_read(UC_X86_REG_EIP), out, mu.reg_read(UC_X86_REG_EFLAGS),
            bytes(mu.mem_read(MEM, 256)), bytes(mu.mem_read(STACK - 16, 32))), None


def main():
    exe = sys.argv[1]
    cases = int(sys.argv[2]) if len(sys.argv) > 2 else 20000
    proc = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    fails, skipped, by = 0, 0, {}
    for i in range(cases):
        code, und, name = gen()
        regs = [r32() for _ in range(8)]
        regs[3] = MEM + 0x40   # ebx
        regs[6] = MEM + 0x80   # esi
        regs[7] = MEM + 0x20   # edi
        regs[4] = STACK        # esp
        if name in ("string",):
            regs[1] = rnd.randrange(0, 9)
        if name == "ret" and code == b"\xc9":
            regs[5] = STACK - 8   # ebp
        flags = 0x2 | (rnd.getrandbits(12) & (ARITH | (DF if name == "string" else 0)))
        if name == "string" and flags & DF:
            regs[6] += 0x40
            regs[7] += 0x40
        mem = bytes(rnd.getrandbits(8) for _ in range(256))
        stack = bytearray(0x2000)
        # dirección de retorno válida y un eflags razonable para ret/popf
        stack[0x1000:0x1004] = (CODE + 0x40).to_bytes(4, "little")
        if name == "popf":
            stack[0x1000:0x1004] = (0x2 | (rnd.getrandbits(12) & (ARITH | DF))).to_bytes(4, "little")
        if name == "ret" and code == b"\xc9":
            stack[0xFF8:0xFFC] = (0x12345678).to_bytes(4, "little")
            stack[0xFFC:0x1000] = (CODE + 0x40).to_bytes(4, "little")
        until = CODE + len(code) if name == "string" and code[0] in (0xF2, 0xF3) else None
        ref, err = run_unicorn(code, regs, flags, mem, bytes(stack), until)
        line = "%s %s %x %s %s\n" % (code.hex(), " ".join("%x" % r for r in regs), flags, mem.hex(),
                                     bytes(stack[0x1000 - 16:0x1000 + 16]).hex())
        proc.stdin.write(line)
        proc.stdin.flush()
        out = proc.stdout.readline().split()
        st = int(out[0])
        if err:
            if not (st == 6 and "UC_ERR_EXCEPTION" in err) and not (st == 4) and \
                    not (st == 5 and ("READ_UNMAPPED" in err or "WRITE_UNMAPPED" in err)):
                fails += 1
                print("FALLO %s %s: unicorn %s, nosotros stop %d" % (name, code.hex(), err, st))
            continue
        if st == 4:  # instrucción que falta
            by.setdefault("faltan", set()).add(code.hex())
            continue
        eip = int(out[1], 16)
        got = [int(x, 16) for x in out[2:10]]
        gflags = int(out[10], 16)
        gmem = bytes.fromhex(out[11])
        gstk = bytes.fromhex(out[12])
        reip, rregs, rflags, rmem, rstk = ref
        mask = ARITH | DF
        if isinstance(und, tuple):
            kind = und[0]
            if kind == "shift":
                _, op, sub, tail = und
                sz = 1 if op in (0xC0, 0xD0, 0xD2) else (2 if code[0] == 0x66 else 4)
                cnt = (tail[0] if tail else (1 if op in (0xD0, 0xD1) else regs[1] & 0xFF)) & 31
                mask &= ~AF
                if cnt != 1:
                    mask &= ~OF
                if sub in (0, 1, 2, 3):
                    mask &= ~(SF | ZF | PF)
                if sz < 4 and cnt >= sz * 8 and sub in (4, 5, 6):
                    mask &= ~CF
            elif kind == "bt":
                mask &= ~(OF | SF | AF | PF)
            elif kind == "bsf":
                mask &= ~(CF | OF | SF | AF | PF)
            elif kind == "shd":
                mask &= ~(AF | OF)
            elif kind == "str":
                pass
        else:
            mask &= ~und
        bad = []
        if eip != reip:
            bad.append("eip %x/%x" % (eip, reip))
        skip_regs = set()
        if isinstance(und, tuple) and und[0] == "bsf":
            skip_regs = set(range(8))  # destino indefinido si la fuente es 0
            if not (rflags & ZF):
                skip_regs = set()
        if isinstance(und, tuple) and und[0] == "shd":
            cnt = (und[2][0] if und[2] else regs[1] & 0xFF) & 31
            if code[0] == 0x66 and cnt > 16:
                continue
        for i2 in range(8):
            if i2 not in skip_regs and got[i2] != rregs[i2]:
                bad.append("r%d %x/%x" % (i2, got[i2], rregs[i2]))
        if (gflags & mask) != (rflags & mask):
            bad.append("flags %x/%x (máscara %x)" % (gflags & mask, rflags & mask, mask))
        if gmem != rmem:
            bad.append("memoria")
        if gstk[:16] != rstk[:16]:
            bad.append("pila")
        if bad:
            fails += 1
            by[name] = by.get(name, 0) + 1 if isinstance(by.get(name, 0), int) else 1
            if fails <= 40:
                print("FALLO %-12s %-24s %s" % (name, code.hex(), ", ".join(bad)))
    proc.stdin.close()
    proc.wait()
    print("casos %d, saltados %d, fallos %d" % (cases, skipped, fails))
    if "faltan" in by:
        print("instrucciones que faltan:", sorted(by["faltan"])[:20])
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
