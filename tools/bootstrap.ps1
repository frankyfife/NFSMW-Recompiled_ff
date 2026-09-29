# NFSMW Recomp - bootstrap (Windows)
# Verifica prerequisitos, clona el ReXGlue SDK y lo compila e instala.
# Uso:  .\tools\bootstrap.ps1

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

function Test-Cmd($name) {
    return [bool](Get-Command $name -ErrorAction SilentlyContinue)
}

Write-Host "== Verificando prerequisitos ==" -ForegroundColor Cyan

$missing = @()
foreach ($t in @("git", "cmake", "ninja", "clang", "python")) {
    if (Test-Cmd $t) {
        $v = (& $t --version 2>&1 | Select-Object -First 1)
        Write-Host ("  [ok] {0,-8} {1}" -f $t, $v)
    } else {
        Write-Host ("  [--] {0,-8} NO ENCONTRADO" -f $t) -ForegroundColor Red
        $missing += $t
    }
}

if ($missing.Count -gt 0) {
    Write-Host ""
    Write-Host "Faltan: $($missing -join ', ')" -ForegroundColor Red
    Write-Host "Instala Visual Studio 2022 con el workload 'Desktop development with C++'"
    Write-Host "y los componentes individuales:"
    Write-Host "  - C++ Clang Compiler for Windows (20.x o superior)"
    Write-Host "  - MSBuild support for LLVM (clang-cl) toolset"
    exit 1
}

# Clang debe ser 20+
$clangVer = (clang --version | Select-String -Pattern '(\d+)\.\d+\.\d+' | ForEach-Object { $_.Matches[0].Groups[1].Value })
if ([int]$clangVer -lt 20) {
    Write-Host "Clang $clangVer detectado; ReXGlue necesita 20 o superior." -ForegroundColor Red
    Write-Host "MSVC y GCC no estan soportados: el codigo generado depende de intrinsics de Clang."
    exit 1
}

Write-Host ""
Write-Host "== ReXGlue SDK ==" -ForegroundColor Cyan

$sdk = Join-Path (Split-Path -Parent $root) "rexglue-sdk"

# Los parches de tools\ buscan texto exacto de ESTA version del SDK (v0.10.0).
# Con un "git pull" a lo que haya en main, cualquier anclaje puede dejar de
# aparecer y CONSTRUIR.bat se para en el paso 1.
$sdkCommit = "c94f5eb"

if (-not (Test-Path $sdk)) {
    Write-Host "  Clonando en $sdk"
    git clone https://github.com/rexglue/rexglue-sdk.git $sdk
}
Push-Location $sdk
git fetch --quiet origin
git checkout --quiet $sdkCommit
git submodule update --init --recursive

# thirdparty\libmspack trae enlaces simbolicos de git. Sin permiso para crear
# symlinks (lo normal en Windows sin modo desarrollador) se quedan como
# ficheros de texto con la ruta dentro, y lzxd.c no compila:
#     lzxd.c:1:1: error: expected identifier or '('
# Se sustituyen por copias del fichero al que apuntan.
$mspack = Join-Path $sdk "thirdparty\libmspack"
if (Test-Path $mspack) {
    $links = git -C $mspack ls-files -s | Where-Object { $_ -match '^120000 ' } |
        ForEach-Object { ($_ -split "`t", 2)[1] }
    foreach ($rel in $links) {
        $f = Join-Path $mspack $rel
        if ((Test-Path $f) -and (Get-Item $f).Length -lt 256) {
            $target = Join-Path (Split-Path $f) ((Get-Content $f -Raw).Trim())
            if (Test-Path $target -PathType Leaf) {
                Copy-Item $target $f -Force
                git -C $mspack update-index --assume-unchanged $rel
            }
        }
    }
}
Pop-Location

Push-Location $sdk
Write-Host ""
Write-Host "== Compilando (win-amd64) ==" -ForegroundColor Cyan
cmake --preset win-amd64
cmake --build out/build/win-amd64 --target install
Pop-Location

Write-Host ""
Write-Host "Listo." -ForegroundColor Green
Write-Host "Comprueba que el CLI esta accesible:  rexglue --help"
Write-Host "Si no lo encuentra, anade al PATH:  $sdk\out\install\win-amd64\bin"
Write-Host ""
Write-Host "Siguiente paso: docs\01-extraccion-xex.md"
