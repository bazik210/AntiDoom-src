$ErrorActionPreference = 'Stop'
$engine = Join-Path $PSScriptRoot '..\linuxdoom-1.10'
$out = Join-Path ([System.IO.Path]::GetTempPath()) ('antidoom-plane-test-' + [guid]::NewGuid().ToString('N') + '.exe')
$sources = Get-ChildItem $engine -Filter '*.c' | Where-Object { $_.Name -notin @('i_video.c', 'i_system.c', 'i_sound.c', 'i_net.c', 'z_zone.c') }
$compilerArgs = @('-O2', '-g', '-DNORMALUNIX', '-D_WIN32', '-D_CRT_SECURE_NO_WARNINGS', '-Dmain=antidoom_main', '-I', $engine, '-include', (Join-Path $engine 'win32_compat.h'))
$compilerArgs += $sources.FullName
$compilerArgs += @((Join-Path $PSScriptRoot 'plane_detail_regression.c'), '-o', $out, '-luser32', '-lgdi32', '-lwinmm', '-lws2_32', '-ldbghelp', '-lm')
try {
    & gcc @compilerArgs
    if ($LASTEXITCODE -ne 0) { throw 'Regression test compilation failed' }
    & $out
    if ($LASTEXITCODE -ne 0) { throw 'Plane detail regression failed' }
} finally {
    if (Test-Path -LiteralPath $out) { Remove-Item -LiteralPath $out }
}
