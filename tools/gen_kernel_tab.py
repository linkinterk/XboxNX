#!/usr/bin/env python3
"""Genera source/kexports.h desde una copia de hakuX.
Uso: gen_kernel_tab.py <ruta a hakuX>/native/kernel > source/kexports.h"""
import glob, os, re, sys

kdir = sys.argv[1]
exp = re.findall(r'NC_KEXPORT\((\d+), "([A-Za-z0-9_]+)", NC_KABI_(\w+), (\d+)\)',
                 open(os.path.join(kdir, 'kexports_table.h')).read())
impl = set()
for f in glob.glob(os.path.join(kdir, '*.c')):
    impl |= set(re.findall(r'nc_kernel_impl\(k, "([A-Za-z0-9_]+)"', open(f).read()))
print('/*\n * XboxNX — exportaciones del kernel de Xbox: ordinal, nombre y estado en hakuX.\n *')
print(' * Generado por tools/gen_kernel_tab.py desde hakuX (native/kernel/kexports_table.h,')
print(' * que viene de xboxkrnl.exe.def de nxdk, CC0-1.0) y las funciones que el kernel')
print(' * de hakuX implementa (nc_kernel_impl). No editar a mano.\n */')
print('/* { ordinal, nombre, tipo: KX_DATA dato, KX_IMPL función que hakuX ya tiene, KX_TODO función que falta } */')
for o, n, abi, _ in exp:
    k = 'DATA' if abi == 'DATA' else ('IMPL' if n in impl else 'TODO')
    print(f'KX({o}, "{n}", KX_{k})')
