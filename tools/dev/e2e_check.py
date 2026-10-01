import importlib.util, os, shutil, subprocess, sys, tempfile
spec = importlib.util.spec_from_file_location("g", "D:/NFSMW/NFSMW-Recompiled_ff/tools/generar_parche_ff.py")
g = importlib.util.module_from_spec(spec)
spec.loader.exec_module(g)
tmp = tempfile.mkdtemp(prefix="e2e_")
try:
    base = g.linea_base(tmp)
    herr = os.path.join(tmp, "proyecto", "tools")
    shutil.copy(os.path.join(g.RAIZ, "tools", "parche_ff.py"), herr)
    shutil.copytree(os.path.join(g.RAIZ, "tools", "sdk_nuevos"), os.path.join(herr, "sdk_nuevos"))
    r = subprocess.run([sys.executable, os.path.join(herr, "parche_ff.py")], capture_output=True,
                       text=True, encoding="utf-8", errors="replace")
    if r.returncode:
        print(r.stdout[-1500:]); sys.exit(1)
    bad = 0
    for rel in g.FICHEROS + g.NUEVOS:
        a = g.lf(open(os.path.join(base, rel), encoding="utf-8").read())
        b = g.lf(open(os.path.join(g.SDK, rel), encoding="utf-8").read())
        if a != b:
            bad += 1
            print("DIFFERS:", rel)
    print("clean rebuild matches working SDK:", bad == 0)
finally:
    shutil.rmtree(tmp, ignore_errors=True)
