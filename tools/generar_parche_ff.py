#!/usr/bin/env python3
"""
Regenerates the BLOQUES list of tools/parche_ff.py from the patched SDK.

    python tools/generar_parche_ff.py

The baseline is not the SDK's git HEAD but what the SDK looks like right before
parche_ff.py runs: a copy of HEAD with every other tools/parche_*.py applied in
CONSTRUIR.bat's order. That way files that other patches touch too (the D3D12
presenter, for one) are diffed correctly. For each changed hunk, context lines
are added until the old text occurs once in the baseline and the new text once
in the patched file. New files live in tools/sdk_nuevos/ and are copied from
the SDK here as well.
"""

import difflib
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SDK = os.path.join(os.path.dirname(RAIZ), "rexglue-sdk")

# Order of CONSTRUIR.bat, without parche_ff.py itself.
ANTES = ["parche_diagnostico.py", "parche_anillo.py", "parche_desatasco.py",
         "parche_presentador.py", "parche_gpu_fallback.py", "parche_restaurar.py",
         "parche_velocidad.py", "parche_backend.py", "parche_privilegios.py"]

FICHEROS = [
    "include/rex/platform/fpscr.h",
    "src/graphics/graphics_system.cpp",
    "src/graphics/command_processor.cpp",
    "include/rex/graphics/d3d12/command_processor.h",
    "src/graphics/d3d12/command_processor.cpp",
    "src/kernel/xboxkrnl/xboxkrnl_video.cpp",
    "include/rex/ui/presenter.h",
    "src/ui/presenter.cpp",
    "src/ui/d3d12/d3d12_presenter.cpp",
    "src/graphics/pipeline/texture/cache.cpp",
    "include/rex/graphics/d3d12/texture_cache.h",
    "src/graphics/d3d12/texture_cache.cpp",
    "include/rex/graphics/shared_memory.h",
    "src/graphics/shared_memory.cpp",
    "src/audio/audio_system.cpp",
    "src/audio/sdl/sdl_audio_driver.cpp",
    "include/rex/audio/sdl/sdl_audio_driver.h",
    "src/audio/xma_decoder.cpp",
    "src/graphics/pipeline/shader/translator_disasm.cpp",
]
NUEVOS = ["include/rex/graphics/frame_pacer.h"]


def lf(s):
    return s.replace("\r\n", "\n")


def linea_base(tmp):
    """SDK HEAD with the other patches applied, in tmp/rexglue-sdk."""
    sdk_tmp = os.path.join(tmp, "rexglue-sdk")
    os.makedirs(sdk_tmp)
    tar = os.path.join(tmp, "head.tar")
    with open(tar, "wb") as fh:
        subprocess.run(["git", "-C", SDK, "archive", "HEAD", "src", "include"], stdout=fh,
                       check=True)
    with tarfile.open(tar) as t:
        t.extractall(sdk_tmp)
    # The scripts find the SDK as rexglue-sdk next to the project folder.
    herramientas = os.path.join(tmp, "proyecto", "tools")
    os.makedirs(herramientas)
    for nombre in ANTES:
        shutil.copy(os.path.join(RAIZ, "tools", nombre), herramientas)
    for nombre in ANTES:
        r = subprocess.run([sys.executable, os.path.join(herramientas, nombre)],
                           capture_output=True, text=True, encoding="utf-8", errors="replace")
        if r.returncode != 0:
            print(r.stdout[-2000:], r.stderr[-2000:])
            raise SystemExit(f"{nombre} failed on the baseline")
    return sdk_tmp


def bloques(rel, orig, cur):
    a = orig.splitlines(keepends=True)
    b = cur.splitlines(keepends=True)
    sm = difflib.SequenceMatcher(a=a, b=b, autojunk=False)
    trozos = []
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == "equal":
            continue
        if trozos and i1 - trozos[-1][1] <= 3:
            trozos[-1] = (trozos[-1][0], i2, trozos[-1][2], j2)
        else:
            trozos.append((i1, i2, j1, j2))
    salida = []
    for n, (i1, i2, j1, j2) in enumerate(trozos, 1):
        k = 1
        while True:
            old = "".join(a[max(0, i1 - k):i1] + a[i1:i2] + a[i2:i2 + k])
            new = "".join(b[max(0, j1 - k):j1] + b[j1:j2] + b[j2:j2 + k])
            if orig.count(old) == 1 and cur.count(new) == 1 and old.strip():
                break
            k += 1
            if k > 40:
                raise SystemExit(f"cannot make {rel} hunk {n} unique")
        salida.append((rel, f"{'/'.join(rel.split('/')[-2:])} #{n}", old, new))
    return salida


def main():
    tmp = tempfile.mkdtemp(prefix="parche_ff_")
    try:
        base = linea_base(tmp)
        todos = []
        for rel in FICHEROS:
            orig = lf(open(os.path.join(base, rel), encoding="utf-8").read())
            cur = lf(open(os.path.join(SDK, rel), encoding="utf-8").read())
            todos += bloques(rel, orig, cur)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    texto = "BLOQUES = [\n" + "".join(
        f"    ({rel!r},\n     {nombre!r},\n     {old!r},\n     {new!r}),\n"
        for rel, nombre, old, new in todos) + "]"
    ruta = os.path.join(RAIZ, "tools", "parche_ff.py")
    src = open(ruta, encoding="utf-8").read()
    a = src.index("BLOQUES = [")
    b = src.index("def buscar_sdk")
    src = src[:a] + texto + "\n\n\n" + src[b:]
    open(ruta, "w", encoding="utf-8", newline="\n").write(src)

    for rel in NUEVOS:
        destino = os.path.join(RAIZ, "tools", "sdk_nuevos", rel)
        os.makedirs(os.path.dirname(destino), exist_ok=True)
        shutil.copy(os.path.join(SDK, rel), destino)
    print(f"{len(todos)} blocks, {len(NUEVOS)} new files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
