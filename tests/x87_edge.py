#!/usr/bin/env python3
"""v0.18.83: x87 con la precisión de 24 bits cerca de los bordes de float, JIT
contra el intérprete. Uso: x87_edge.py ruta/al/cpu_diff"""
import os, struct, subprocess, sys, itertools
exe = sys.argv[1]
env_b = dict(os.environ, CPU_BLOQUES="1", CPU_BLOQUES_N="100000", CPU_JIT="1")
env_i = {k: v for k, v in os.environ.items() if not k.startswith("CPU_BLOQUES") and k != "CPU_JIT"}
pb = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, env=env_b)
pi = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, env=env_i)
vals = [3.4028234663852886e38, 3.4e38, 1.8e38, 2e19, 1.1754943508222875e-38, 1.2e-38, 1.1e-19, 6e-39, 1.5e-45,
        1e30, -1e-30, 3.4028235677973366e38, 1.17549421e-38, 1.0, 0.0, 1e-20, 1e20]
# fninit; fldcw [ebp]; fld qword [edi]; fmul/fadd/fdiv qword [edi+8]; fstp qword [ebx]; hlt
ops = [bytes.fromhex("dc4f08"), bytes.fromhex("dc4708"), bytes.fromhex("dc7708"), bytes.fromhex("dc6708")]
fails = n = 0
for a, b in itertools.product(vals, vals):
    for op in ops:
        for cw in (0x007F, 0x027F):
            code = bytes.fromhex("dbe3d96d00dd07") + op + bytes.fromhex("dd1bf4")
            mem = bytearray(256)
            struct.pack_into("<dd", mem, 0x80, a, b)
            struct.pack_into("<H", mem, 0xF0, cw)
            regs = [0, 0, 0, 0x100040, 0x200000, 0x1000F0, 0x100000, 0x100080]
            line = "%s %s %x %s %s m\n" % (code.hex(), " ".join("%x" % r for r in regs), 2, bytes(mem).hex(), "00" * 32)
            outs = []
            for p in (pb, pi):
                p.stdin.write(line); p.stdin.flush(); outs.append(p.stdout.readline().split())
            n += 1
            if outs[0] != outs[1]:
                fails += 1
                if fails <= 5:
                    print("FALLO", a, b, op.hex(), hex(cw))
print("casos", n, "fallos", fails)
