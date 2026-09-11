param([string]$Test = 'bot_run')
$ErrorActionPreference = 'Stop'
$engine = (Resolve-Path (Join-Path $PSScriptRoot '..\linuxdoom-1.10')).Path
$outdir = Join-Path $PSScriptRoot '..\..\scratch\headless'
New-Item -ItemType Directory -Force $outdir | Out-Null
$outdir = (Resolve-Path $outdir).Path
$sources = Get-ChildItem $engine -Filter '*.c' | Where-Object { $_.Name -notin @('i_video.c','i_system.c','i_sound.c','i_net.c','z_zone.c','s_sound.c') }
$headers = (Get-ChildItem $engine -Filter '*.h' | Sort-Object LastWriteTime -Descending | Select-Object -First 1).LastWriteTime
$flags = @('-O2','-g','-DNORMALUNIX','-D_WIN32','-D_CRT_SECURE_NO_WARNINGS','-Dmain=antidoom_main','-I',$engine,'-include',(Join-Path $engine 'win32_compat.h'))
$objects = @()
foreach ($source in $sources) {
    $object = Join-Path $outdir ($source.BaseName + '.o')
    if (!(Test-Path $object) -or (Get-Item $object).LastWriteTime -lt $source.LastWriteTime -or (Get-Item $object).LastWriteTime -lt $headers -or (Get-Item $object).LastWriteTime -lt (Get-Item $PSCommandPath).LastWriteTime) {
        $extra = @()
        if ($source.Name -eq 'i_video_win32.c') { $extra += '-DI_CaptureMouse=I_UnusedTestCaptureMouse' }
        & gcc @flags @extra -c $source.FullName -o $object
        if ($LASTEXITCODE) { throw "Compilation failed: $($source.Name)" }
    }
    if (!($Test -eq 'bot_run' -and $source.Name -eq 'p_bot.c')) { $objects += $object }
}
$out = Join-Path $outdir ($Test + '.exe')
& gcc @flags (Join-Path $PSScriptRoot ($Test + '.c')) (Join-Path $PSScriptRoot 'headless_platform.c') @objects -o $out -luser32 -lgdi32 -lwinmm -lws2_32 -ldbghelp -lm
if ($LASTEXITCODE) { throw 'Test link failed' }
Write-Output $out
