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


def piece():
    if rnd.random() < 0.15:
        return cmp_jcc()
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
    env_b = dict(os.environ, CPU_BLOQUES="1", CPU_BLOQUES_N="100000")
    env_i = {k: v for k, v in os.environ.items() if not k.startswith("CPU_BLOQUES")}
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
                print("FALLO %s\n  bloques   %s\n  intérprete %s" % (code.hex(), " ".join(outs[0][:11]),
                                                                   " ".join(outs[1][:11])))
                if os.environ.get("BLOCK_SEQ_LINEA"):
                    print("  entrada: " + line.strip())
    for p in (pb, pi):
        p.stdin.close()
        p.wait()
    print("secuencias %d, distintas %d" % (cases, fails))
    sys.exit(1 if fails else 0)


main()
