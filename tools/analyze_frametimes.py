#!/usr/bin/env python3
"""
Frame pacing report from PresentMon CSV files (tools/measure_frametimes.bat).

    python tools/analyze_frametimes.py [csv ...]

Without arguments it takes every build/logs/frametimes/*.csv. For each file:
the intervals between presents (when the game hands a frame over) and between
display changes (what the screen actually showed), percentiles, spread,
hitches (> 1.5x the median) and early frames (< 0.5x), present mode and
dropped frames. Writes build/logs/frametimes/report.html with a chart per
recording (no third-party modules needed).
"""

import csv
import glob
import html
import math
import os
import statistics
import sys

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CARPETA = os.path.join(RAIZ, "build", "logs", "frametimes")


def col(fila, *nombres):
    """Column by any of its PresentMon spellings (1.x/2.x, any case)."""
    bajas = {k.lower(): v for k, v in fila.items()}
    for n in nombres:
        v = bajas.get(n.lower())
        if v not in (None, "", "NA"):
            return v
    return None


def pct(ordenados, p):
    if not ordenados:
        return float("nan")
    k = (len(ordenados) - 1) * p / 100.0
    lo, hi = math.floor(k), math.ceil(k)
    return ordenados[lo] + (ordenados[hi] - ordenados[lo]) * (k - lo)


def resumen(valores):
    v = sorted(valores)
    if len(v) < 2:
        return None
    med = statistics.median(v)
    media = statistics.fmean(v)
    return {
        "n": len(v),
        "fps": 1000.0 / media if media > 0 else 0.0,
        "median": med,
        "mean": media,
        "stdev": statistics.pstdev(v),
        "p1": pct(v, 1), "p99": pct(v, 99), "p999": pct(v, 99.9),
        "min": v[0], "max": v[-1],
        "low1": 1000.0 / statistics.fmean(v[-max(1, len(v) // 100):]),
        "hitches": sum(1 for x in v if x > 1.5 * med),
        "early": sum(1 for x in v if x < 0.5 * med),
        "within1": 100.0 * sum(1 for x in v if abs(x - med) <= 1.0) / len(v),
    }


def leer_json(ruta):
    """CapFrameX capture (Documents\\CapFrameX\\Captures\\*.json): looks for the
    arrays wherever this version puts them (usually Runs[].CaptureData)."""
    import json

    with open(ruta, encoding="utf-8-sig") as fh:
        datos = json.load(fh)
    series = []

    def buscar(nodo):
        if isinstance(nodo, dict):
            claves = {k.lower(): k for k in nodo}
            if "msbetweenpresents" in claves and isinstance(nodo[claves["msbetweenpresents"]], list):
                series.append((nodo, claves))
            for v in nodo.values():
                buscar(v)
        elif isinstance(nodo, list):
            for v in nodo:
                buscar(v)

    buscar(datos)
    presentes, pantalla, caidas = [], [], 0
    for nodo, claves in series:
        mp = nodo[claves["msbetweenpresents"]]
        md = nodo.get(claves.get("msbetweendisplaychange", ""), []) or []
        dr = nodo.get(claves.get("dropped", ""), []) or []
        presentes += [float(x) for x in mp[1:]]
        for i in range(1, len(md)):
            if i < len(dr) and dr[i]:
                caidas += 1
            elif float(md[i]) > 0:
                pantalla.append(float(md[i]))
    return presentes, pantalla, {"CapFrameX": len(presentes)}, caidas, 0


def leer_propio(ruta):
    """The game's own recording (F10, frame_times_dir): one row per present with
    the present time and DXGI's latest displayed present and its display time."""
    presentes, pantalla, modos = [], [], {}
    previo = None
    visto = {}
    with open(ruta, newline="", encoding="utf-8") as fh:
        for fila in csv.DictReader(fh):
            t = float(fila["present_ms"])
            if previo is not None:
                presentes.append(t - previo)
            previo = t
            n, d = int(fila["displayed_present"]), float(fila["display_ms"])
            if n and d:
                visto.setdefault(n, d)
            clave = f"{fila['target_fps']} fps, vsync {fila['vsync']}"
            modos[clave] = modos.get(clave, 0) + 1
    ordenados = sorted(visto.items())
    for (n0, d0), (n1, d1) in zip(ordenados, ordenados[1:]):
        if n1 == n0 + 1 and d1 > d0:
            pantalla.append(d1 - d0)
    return presentes, pantalla, modos, 0, 0


def leer(ruta):
    if ruta.lower().endswith(".json"):
        return leer_json(ruta)
    with open(ruta, encoding="utf-8", errors="replace") as fh:
        if fh.readline().startswith("present,present_ms"):
            return leer_propio(ruta)
    presentes, pantalla, modos, caidas, tearing = [], [], {}, 0, 0
    with open(ruta, newline="", encoding="utf-8", errors="replace") as fh:
        for fila in csv.DictReader(fh):
            app = (col(fila, "Application") or "").lower()
            if app and app != "nfsmw.exe":
                continue
            mp = col(fila, "MsBetweenPresents", "msBetweenPresents")
            if mp is not None:
                presentes.append(float(mp))
            dropped = col(fila, "Dropped")
            if dropped is not None and dropped.strip() not in ("0", "False", "false"):
                caidas += 1
            else:
                md = col(fila, "MsBetweenDisplayChange", "msBetweenDisplayChange")
                if md is not None and float(md) > 0:
                    pantalla.append(float(md))
            modo = col(fila, "PresentMode") or "?"
            modos[modo] = modos.get(modo, 0) + 1
            if (col(fila, "AllowsTearing") or "0") in ("1", "True", "true"):
                tearing += 1
    # The first interval of a recording is measured from before it started.
    return presentes[1:], pantalla[1:], modos, caidas, tearing


def svg(valores, objetivo, ancho=960, alto=220):
    if not valores:
        return ""
    tope = max(objetivo * 2.2, min(max(valores), objetivo * 4))
    paso = ancho / max(1, len(valores) - 1)
    y = lambda ms: alto - min(ms, tope) / tope * alto
    puntos = " ".join(f"{i * paso:.1f},{y(v):.1f}" for i, v in enumerate(valores))
    marcas = "".join(
        f'<circle cx="{i * paso:.1f}" cy="{y(v):.1f}" r="3" fill="#e5484d"/>'
        for i, v in enumerate(valores) if v > 1.5 * objetivo)
    guias = "".join(
        f'<line x1="0" x2="{ancho}" y1="{y(m):.1f}" y2="{y(m):.1f}" stroke="#8886" '
        f'stroke-dasharray="4 4"/><text x="4" y="{y(m) - 4:.1f}" font-size="11" '
        f'fill="#888">{m:.1f} ms</text>'
        for m in (objetivo, objetivo * 1.5, objetivo * 2))
    return (f'<svg viewBox="0 0 {ancho} {alto}" width="100%" preserveAspectRatio="none" '
            f'style="background:#0f1115;border-radius:6px">{guias}'
            f'<polyline points="{puntos}" fill="none" stroke="#f59e0b" stroke-width="1.2"/>'
            f'{marcas}</svg>')


def fila_tabla(nombre, r):
    if not r:
        return f"<tr><td>{nombre}</td><td colspan=10>no data</td></tr>"
    return (f"<tr><td>{nombre}</td><td>{r['n']}</td><td>{r['fps']:.1f}</td>"
            f"<td>{r['median']:.2f}</td><td>{r['stdev']:.2f}</td>"
            f"<td>{r['p1']:.2f} / {r['p99']:.2f} / {r['p999']:.2f}</td>"
            f"<td>{r['min']:.2f} / {r['max']:.2f}</td><td>{r['within1']:.1f} %</td>"
            f"<td>{r['hitches']}</td><td>{r['early']}</td><td>{r['low1']:.1f}</td></tr>")


def main():
    capframex = os.path.join(os.path.expanduser("~"), "Documents", "CapFrameX", "Captures")
    rutas = sys.argv[1:] or (sorted(glob.glob(os.path.join(CARPETA, "*.csv"))) +
                             sorted(glob.glob(os.path.join(capframex, "*nfsmw*.json"))))
    if not rutas:
        print("No CSV files. Record with tools\\measure_frametimes.bat first.")
        return 1
    partes = []
    for ruta in rutas:
        presentes, pantalla, modos, caidas, tearing = leer(ruta)
        rp, rd = resumen(presentes), resumen(pantalla)
        nombre = os.path.basename(ruta)
        print(f"\n== {nombre}")
        for etiqueta, r in (("presents", rp), ("display", rd)):
            if not r:
                print(f"  {etiqueta:8}: no data")
                continue
            print(f"  {etiqueta:8}: {r['n']} frames, {r['fps']:.1f} fps | median {r['median']:.2f} ms, "
                  f"stdev {r['stdev']:.2f} | p1/p99/p99.9 {r['p1']:.2f}/{r['p99']:.2f}/{r['p999']:.2f} | "
                  f"min/max {r['min']:.2f}/{r['max']:.2f} | within +-1 ms {r['within1']:.1f} % | "
                  f"hitches (>1.5x) {r['hitches']} | early (<0.5x) {r['early']} | 1% low {r['low1']:.1f} fps")
        print(f"  dropped {caidas} | allows tearing {tearing} | present modes {modos}")
        base = pantalla if pantalla else presentes
        objetivo = (rd or rp)["median"] if (rd or rp) else 16.67
        partes.append(
            f"<h2>{html.escape(nombre)}</h2>"
            f"<p>Present modes: {html.escape(str(modos))} · dropped: {caidas} · "
            f"frames allowing tearing: {tearing}</p>"
            "<table><tr><th></th><th>frames</th><th>fps</th><th>median ms</th><th>stdev</th>"
            "<th>p1 / p99 / p99.9</th><th>min / max</th><th>within ±1 ms</th>"
            "<th>hitches &gt;1.5×</th><th>early &lt;0.5×</th><th>1% low fps</th></tr>"
            f"{fila_tabla('presents', rp)}{fila_tabla('display', rd)}</table>"
            f"<p>{'Display changes' if pantalla else 'Presents'}, frame by frame "
            "(red: hitch):</p>" + svg(base, objetivo))
    os.makedirs(CARPETA, exist_ok=True)
    salida = os.path.join(CARPETA, "report.html")
    with open(salida, "w", encoding="utf-8") as fh:
        fh.write("<!doctype html><meta charset=utf-8><title>Frame pacing</title><style>"
                 "body{font:14px system-ui;background:#16181d;color:#ddd;margin:24px}"
                 "table{border-collapse:collapse;margin:8px 0}td,th{padding:4px 10px;"
                 "border-bottom:1px solid #333;text-align:right}td:first-child,th:first-child"
                 "{text-align:left}h2{color:#f59e0b;font-size:16px;margin-top:28px}</style>"
                 "<h1>NFSMW frame pacing</h1>" + "".join(partes))
    print(f"\nreport: {salida}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
