#!/usr/bin/env python3
"""Bloques decodificados contra el intérprete: secuencias largas al azar
(enteras, cadenas, mul/div, pila, x87, MMX, SSE) ejecutadas de las dos formas
con el mismo estado inicial; todo debe salir igual.
Uso: block_seq.py ruta/al/cpu_diff [casos] [semilla]
"""
import os
import random
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
seed = int(sys.argv[3]) if len(sys.argv) > 3 else 1
sys.argv = sys.argv[:1] + sys.argv[1:3] + [str(seed)]
import cpu_diff  # noqa: E402
import fp_diff  # noqa: E402

rnd = random.Random(seed)
cpu_diff.rnd = rnd
fp_diff.rnd = rnd
FAULT = "5"
JUMPS = {"jcc8", "jcc32", "call", "jmp8", "loop", "ret"}


def cmp_jcc():
    """cmp/sub/test/and/or/xor y un jcc que, si salta, se brinca inc esi (el
    JIT traduce ese jcc con los flags de ARM64)."""
    k = rnd.randrange(4)
    if k == 0:
        prod = bytes([rnd.choice([0x39, 0x29, 0x85, 0x21, 0x09, 0x31, 0x3B, 0x2B, 0x23, 0x38]),
                      0xC0 | rnd.randrange(8) << 3 | rnd.choice([0, 1, 2, 3, 5, 7])])
    elif k == 1:
        prod = bytes([0x83, 0xC0 | rnd.choice([7, 5, 4, 1, 6]) << 3 | rnd.choice([0, 1, 2, 3, 5, 7]),
                      rnd.choice([0, 1, 0x7F, 0x80, 0xFF, rnd.getrandbits(8)])])
    elif k == 2:
        prod = bytes([0x3D]) + rnd.choice([0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF,
                                            rnd.getrandbits(32)]).to_bytes(4, "little")
    else:
        prod = bytes([0x39, 0x06])  # cmp [esi], eax
    return prod + bytes([0x70 | rnd.randrange(16), 0x01, 0x46])


def comiss_jcc():
    """v0.18.80: movss de la memoria, (u)comiss y un jcc (el JIT usa el NZCV
    de fcmp, y su camino lento lo rehace de los flags)."""
    a, b = rnd.randrange(8), rnd.randrange(8)
    code = bytes([0xF3, 0x0F, 0x10, 0x46 | a << 3, 4 * rnd.randrange(16)])
    if rnd.random() < 0.5:
        code += bytes([0xF3, 0x0F, 0x10, 0x46 | b << 3, 4 * rnd.randrange(16)])
    op = rnd.choice([0x2E, 0x2F])
    if rnd.random() < 0.5:
        code += bytes([0x0F, op, 0xC0 | a << 3 | b])
    else:
        code += bytes([0x0F, op, 0x46 | a << 3, 4 * rnd.randrange(16)])
    return code + bytes([0x70 | rnd.randrange(16), 0x01, 0x46])


def jcc_mix():
    """v0.18.86: test/cmp de 8 y 16 bits, inc/dec, add y alu con inmediatos
    lógicos antes de un jcc; a veces un mov en medio y una alu después (los
    flags ya no se leen tras el jcc: el JIT los deja solo en NZCV)."""
    k = rnd.randrange(8)
    if k >= 6:  # con memoria: [esi + d] (32 bits)
        d = 4 * rnd.randrange(16)
        c = rnd.randrange(4)
        if c == 0:
            prod = bytes([rnd.choice([0x39, 0x01, 0x29, 0x09, 0x21, 0x31, 0x85]), 0x46 | rnd.randrange(4) << 3, d])
        elif c == 1:
            prod = bytes([0x83, 0x46 | rnd.choice([0, 1, 4, 5, 6, 7]) << 3, d, rnd.getrandbits(8)])
        elif c == 2:
            prod = bytes([0x81, 0x46 | rnd.choice([0, 1, 4, 5, 6, 7]) << 3, d]) + \
                rnd.choice([1, 0xFF, 0xFFFF0000, rnd.getrandbits(32)]).to_bytes(4, "little")
        else:
            prod = bytes([0xF7, 0x46, d]) + rnd.getrandbits(32).to_bytes(4, "little")  # test [esi+d], imm
    elif k == 0:
        r = rnd.randrange(8)
        if rnd.random() < 0.5:
            prod = bytes([0xF6, 0xC0 | r, rnd.choice([1, 0x10, 0x80, 0xFF, 0x05, rnd.getrandbits(8)])])
        else:
            prod = bytes([0x80, 0xF8 | r, rnd.choice([0, 1, 0x7F, 0x80, 0xFF, rnd.getrandbits(8)])])
    elif k == 1:
        prod = bytes([rnd.choice([0x84, 0x38, 0x3A]), 0xC0 | rnd.randrange(64)])
    elif k == 2:
        c = rnd.randrange(3)
        if c == 0:
            prod = bytes([0x66, rnd.choice([0x85, 0x39, 0x3B]), 0xC0 | rnd.randrange(64)])
        elif c == 1:
            prod = bytes([0x66, 0x81, 0xF8 | rnd.randrange(8)]) + rnd.choice(
                [0, 1, 0x7FFF, 0x8000, 0xFFFF, rnd.getrandbits(16)]).to_bytes(2, "little")
        else:
            prod = bytes([0x66, 0xF7, 0xC0 | rnd.randrange(8)]) + rnd.getrandbits(16).to_bytes(2, "little")
    elif k == 3:
        prod = bytes([rnd.choice([0x40, 0x48]) + rnd.choice([0, 1, 2, 3, 5, 7])])
    elif k == 4:
        imm = rnd.choice([1, 0xFF, 0xFFFF, 0xFF00FF00, 0x0F0F0F0F, 0xFFFFFFF0, 0x7FFFFFFF, 0x80000000,
                          0xFFFFF001, 0x00FFF000, 0xFFF, 0xFFFFFF01, rnd.getrandbits(32)])
        prod = bytes([0x81, 0xC0 | rnd.choice([0, 1, 4, 5, 6, 7]) << 3 | rnd.choice([0, 1, 2, 3])]) + \
            imm.to_bytes(4, "little")
    else:
        prod = bytes([rnd.choice([0x01, 0x29, 0x39]), 0xC0 | rnd.randrange(4) << 3 | rnd.randrange(4)])
    code = prod
    if rnd.random() < 0.3:
        code += bytes([0x89, 0xC0 | rnd.randrange(4) << 3 | rnd.randrange(4)])  # mov: NZCV sigue
    u = rnd.random()
    if u < 0.2:  # setcc r8
        code += bytes([0x0F, 0x90 | rnd.randrange(16), 0xC0 | rnd.randrange(8)])
    elif u < 0.35:  # cmovcc r32, r32
        code += bytes([0x0F, 0x40 | rnd.randrange(16), 0xC0 | rnd.randrange(4) << 3 | rnd.randrange(4)])
    else:
        code += bytes([0x70 | rnd.randrange(16), 0x01, 0x46])
    if rnd.random() < 0.7:
        code += bytes([0x83, 0xC7, 0x00])  # add edi, 0: pisa todos
    return code


def abs_mem():
    """v0.18.87: operandos de dirección fija ([0x100000 + d]): el JIT sabe la
    página y el offset al traducir. También al borde de la página (lento)."""
    d = rnd.choice([0, 4, 8, 0x40, 0x7C, 0x100, 0xFF8, 0xFFC, 0xFFD, 0xFFF, 4 * rnd.randrange(64)])
    a = (0x100000 + d).to_bytes(4, "little")
    r = rnd.randrange(4)
    k = rnd.randrange(12)
    if k == 0:
        return bytes([0xA1]) + a  # mov eax, [a]
    if k == 1:
        return bytes([0xA3]) + a  # mov [a], eax
    if k == 2:
        return bytes([0x8B, 0x05 | r << 3]) + a
    if k == 3:
        return bytes([0x89, 0x05 | r << 3]) + a
    if k == 4:  # cmp / add / sub / and / or / xor reg, [a] y un jcc
        return bytes([rnd.choice([0x3B, 0x03, 0x2B, 0x23, 0x0B, 0x33]), 0x05 | r << 3]) + a + \
            bytes([0x70 | rnd.randrange(16), 0x01, 0x46])
    if k == 5:  # op [a], reg y un jcc
        return bytes([rnd.choice([0x39, 0x01, 0x29, 0x21, 0x09, 0x31, 0x85]), 0x05 | r << 3]) + a + \
            bytes([0x70 | rnd.randrange(16), 0x01, 0x46])
    if k == 6:
        return bytes([0x83, 0x05 | rnd.choice([0, 1, 4, 5, 6, 7]) << 3]) + a + bytes([rnd.getrandbits(8)])
    if k == 7:
        return bytes([0x0F, rnd.choice([0xB6, 0xB7, 0xBE, 0xBF]), 0x05 | r << 3]) + a
    if k == 8:
        return bytes([rnd.choice([0x8A, 0x88]), 0x05 | r << 3]) + a
    if k == 9:
        return bytes([0xFF, 0x05 | rnd.choice([0, 1]) << 3]) + a  # inc / dec dword [a]
    if k == 10:
        return bytes([0xC7, 0x05]) + a + rnd.getrandbits(32).to_bytes(4, "little")
    return bytes([0x66, rnd.choice([0x8B, 0x89]), 0x05 | r << 3]) + a


def shift_lahf():
    """v0.18.81: shl/shr/sar de 8 o 32 bits (cuenta fija o CL) y lahf: AH de
    los flags perezosos del desplazamiento sin llamar a C."""
    r = rnd.choice([0, 1, 2, 3, 5, 7])
    op = rnd.choice([4, 5, 7])
    k = rnd.randrange(4)
    if k == 0:
        code = bytes([0xC1, 0xC0 | op << 3 | r, rnd.choice([1, 2, 7, 8, 15, 16, 31, rnd.randrange(32)])])
    elif k == 1:
        code = bytes([0xD3, 0xC0 | op << 3 | r])
    elif k == 2:
        code = bytes([0xC0, 0xC0 | op << 3 | r, rnd.randrange(1, 8)])
    else:
        code = bytes([0xD1, 0xC0 | op << 3 | r])
    return code + b"\x9f"


def piece():
    if rnd.random() < 0.06:
        return shift_lahf()
    if rnd.random() < 0.15:
        return cmp_jcc()
    if rnd.random() < 0.15:
        return jcc_mix()
    if rnd.random() < 0.12:
        return abs_mem()
    if rnd.random() < 0.1:
        return comiss_jcc()
    if rnd.random() < 0.3:
        code, _ = rnd.choice([fp_diff.gen_sse, fp_diff.gen_x87, fp_diff.gen_mmx, fp_diff.gen_mmx])()
        return code
    while True:
        code, _, name = cpu_diff.gen()
        if name not in JUMPS:
            return code


def main():
    exe = sys.argv[1]
    cases = int(sys.argv[2]) if len(sys.argv) > 2 else 3000
    env_b = dict(os.environ, CPU_BLOQUES="1", CPU_BLOQUES_N="100000", CPU_DIFF_XMM="1")
    env_i = {k: v for k, v in os.environ.items() if not k.startswith("CPU_BLOQUES")}
    env_i["CPU_DIFF_XMM"] = "1"
    pb = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, env=env_b)
    pi = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, env=env_i)
    fails = 0
    for _ in range(cases):
        code = b""
        for _ in range(rnd.randrange(2, 14)):
            p = piece()
            if len(code) + len(p) > 240:
                break
            code += p
        regs = [cpu_diff.r32() for _ in range(8)]
        regs[1] = rnd.choice([0, 1, 3, 7, 16, 33, regs[1] & 0xFF])
        regs[3], regs[4], regs[5], regs[6], regs[7] = 0x100040, 0x200000, 0x1000F0, 0x100000, 0x100080
        flags = 0x2 | (rnd.getrandbits(12) & 0x8D5)
        mem = fp_diff.mem_floats()
        if rnd.random() < 0.5:
            code += b"\xeb\x00"  # v0.18.80: jmp a la siguiente (el bloque sale por su salida fija)
        code += b"\xf4"  # hlt: las dos formas paran ahí
        line = "%s %s %x %s %s m\n" % (code.hex(), " ".join("%x" % r for r in regs), flags, mem.hex(), "00" * 32)
        outs = []
        for p in (pb, pi):
            p.stdin.write(line)
            p.stdin.flush()
            outs.append(p.stdout.readline().split())
        if outs[1][0] == "0":
            continue  # el código se escribió encima (rep stos sobre el hlt): el intérprete paró al final, sin hlt
        if outs[0][:2] == outs[1][:2] and outs[1][0] == FAULT:
            continue  # un fallo de memoria termina el juego: basta que paren igual
        if outs[0] != outs[1]:
            fails += 1
            if fails <= 20:
                k = None if os.environ.get("BLOCK_SEQ_TODO") else 11
                print("FALLO %s\n  bloques   %s\n  intérprete %s" % (code.hex(), " ".join(outs[0][:k]),
                                                                   " ".join(outs[1][:k])))
                if os.environ.get("BLOCK_SEQ_LINEA"):
                    print("  entrada: " + line.strip())
    for p in (pb, pi):
        p.stdin.close()
        p.wait()
    print("secuencias %d, distintas %d" % (cases, fails))
    sys.exit(1 if fails else 0)


main()
