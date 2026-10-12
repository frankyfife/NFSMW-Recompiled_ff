#!/usr/bin/env python3
"""
Direct calls between recompiled game functions, so the compiler can inline them.

    python tools/direct_calls.py            apply (after every codegen)
    python tools/direct_calls.py --undo     plain sub_X calls everywhere again
    python tools/direct_calls.py --status

The idea comes from the NFS Carbon recompilation (its tools/direct_calls.py,
measured there on Android together with ThinLTO: 142,521 calls made direct);
this is a separate implementation for this project's layout.

WHY. The code generator defines every game function twice: __imp__sub_X with
the body, and sub_X as a weak, noinline alias of it (DEFINE_REX_FUNC in
app/generated/default/nfsmw_pch.h). A hook in app/src (REX_FUNC(sub_X)) is a
strong definition that wins at link time, which is how freecam.cpp, car_lod.cpp
and the native renderer replace game functions. The price: every call goes
through the alias, and no call between game functions can be inlined, even in
the same file. For a function nobody hooks, calling __imp__sub_X runs the same
code and the compiler is free to inline it (within a file; across files with
NFSMW_THINLTO, see app/CMakeLists.txt).

WHAT COUNTS AS HOOKED. Every sub_8XXXXXXX named anywhere in app/src (hooks,
calls to __imp__ originals, comments: better one direct call too few than a
hook that is silently skipped) and in the .toml files of app/. Those keep their
sub_X calls. Calls to a name the generated files do not define are left alone.

WHAT IS NOT TOUCHED. Only app/generated/default/nfsmw_recomp.*.cpp. The
indirect-call table (nfsmw_init.cpp) keeps pointing at sub_X, so indirect calls
still reach hooks.

ADDING A HOOK LATER. A run also turns __imp__sub_X calls back into sub_X calls
for every function that is hooked now. So after adding a hook, run this again
(CONSTRUIR.bat does) and the hook is reached; the generated files contain no
__imp__sub_X calls of their own, only the ones this script writes.

Run it between the codegen and the compile; CONSTRUIR.bat does that when
NFSMW_DIRECT_CALLS=1 is set and runs --undo otherwise, so switching it off
gives the plain build back. Measured on this game 2026-10-12 (CHANGELOG.md):
no measurable gain on Windows, with or without ThinLTO, so it stays off. The Carbon numbers
are for ARM phones, not for this game on x86-64.
"""

import argparse
import pathlib
import re
import sys

RAIZ = pathlib.Path(__file__).resolve().parent.parent
APP = RAIZ / "app"
GEN = APP / "generated" / "default"
HOOKS = APP / "src"

NOMBRE = re.compile(r"\bsub_(8[0-9A-Fa-f]{7})\b")
# Game functions and the PowerPC register save/restore helpers. The codegen
# writes every call as "<name>(ctx, base)".
FUNC = r"(sub_8[0-9A-Fa-f]{7}|__(?:save|rest)(?:gprlr|fpr|vmx)_\d+)"
DEFINIDA = re.compile(r"DEFINE_REX_FUNC\(" + FUNC + r"\)")
LLAMADA = re.compile(r"(?<![\w&])" + FUNC + r"\(ctx, base\)")
DIRECTA = re.compile(r"(?<![\w&])__imp__" + FUNC + r"\(ctx, base\)")


def clave(nombre):
    """sub_ addresses compare without case; the helpers as they are."""
    return "sub_" + nombre[4:].upper() if nombre.startswith("sub_") else nombre


def enganchadas():
    ficheros = [p for p in HOOKS.rglob("*") if p.suffix in (".cpp", ".cc", ".h", ".hpp")]
    ficheros += list(APP.glob("*.toml"))
    nombres = set()
    for p in ficheros:
        texto = p.read_text(encoding="utf-8", errors="ignore")
        nombres.update("sub_" + m.upper() for m in NOMBRE.findall(texto))
    return nombres


def generados():
    return sorted(GEN.glob("nfsmw_recomp.*.cpp"))


def leer(p):
    return p.read_text(encoding="utf-8")


def escribir(p, texto):
    p.write_text(texto, encoding="utf-8", newline="")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[1])
    ap.add_argument("--undo", action="store_true", help="plain sub_X calls everywhere")
    ap.add_argument("--status", action="store_true", help="count, change nothing")
    args = ap.parse_args()

    ficheros = generados()
    if not ficheros:
        print(f"[direct_calls] no generated sources in {GEN} (run the codegen first)")
        return 0 if args.undo else 1

    if args.status:
        directas = sum(len(DIRECTA.findall(leer(p))) for p in ficheros)
        normales = sum(len(LLAMADA.findall(leer(p))) for p in ficheros)
        print(f"[direct_calls] direct: {directas}, through the alias: {normales}")
        return 0

    if args.undo:
        cambiados = 0
        for p in ficheros:
            texto = leer(p)
            nuevo = DIRECTA.sub(r"\1(ctx, base)", texto)
            if nuevo != texto:
                escribir(p, nuevo)
                cambiados += 1
        print(f"[direct_calls] plain calls restored in {cambiados} files")
        return 0

    hooks = enganchadas()
    definidas = set()
    for p in ficheros:
        definidas.update(clave(m) for m in DEFINIDA.findall(leer(p)))

    directas = mantenidas = devueltas = cambiados = 0
    for p in ficheros:
        texto = leer(p)

        def devolver(m):
            # A direct call to a function that is hooked now: back through
            # the alias, or the hook would be skipped.
            nonlocal devueltas
            if clave(m.group(1)) in hooks:
                devueltas += 1
                return f"{m.group(1)}(ctx, base)"
            return m.group(0)

        def directa(m):
            nonlocal directas, mantenidas
            nombre = clave(m.group(1))
            if nombre in hooks or nombre not in definidas:
                mantenidas += 1
                return m.group(0)
            directas += 1
            return f"__imp__{m.group(1)}(ctx, base)"

        nuevo = LLAMADA.sub(directa, DIRECTA.sub(devolver, texto))
        if nuevo != texto:
            escribir(p, nuevo)
            cambiados += 1

    print(f"[direct_calls] hooked: {len(hooks)}; made direct: {directas}; kept: {mantenidas}; "
          f"returned to hooks: {devueltas}; files written: {cambiados}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
