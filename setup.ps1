# setup.ps1 - copy your Call of Duty: Black Ops install next to the built game (one time, about 11 GB).
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File setup.ps1 [-GameDir "D:\Games\Call of Duty Black Ops"] [-Build <folder>]
#
# The game runs from its build folder and reads the original game's files (zone\, main\*.iwd, videos, DLLs) from
# there: build\Release for the Visual Studio project, build-windows-x64 or build-windows-x86 for build-windows.sh
# (-Build picks one; without it the first of those that holds BO1Zombies.exe). Your Steam install is only read, never
# changed. BlackOps.exe and BlackOpsMP.exe are not copied.
param(
    [string]$GameDir = 'C:\Program Files (x86)\Steam\steamapps\common\Call of Duty Black Ops',
    [string]$Build = ''
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path (Join-Path $GameDir 'zone'))) {
    throw "No Black Ops install at '$GameDir'. Pass -GameDir with the folder that holds BlackOps.exe."
}
if (-not $Build) {
    $Build = 'build\Release'
    foreach ($b in 'build\Release', 'build-windows-x64', 'build-windows-x86') {
        if (Test-Path (Join-Path (Join-Path $PSScriptRoot $b) 'BO1Zombies.exe')) { $Build = $b; break }
    }
}
$dst = if ([System.IO.Path]::IsPathRooted($Build)) { $Build } else { Join-Path $PSScriptRoot $Build }
New-Item -ItemType Directory -Force $dst | Out-Null

# which game is built there: 64-bit or 32-bit (the executable's PE machine), and whether it is the Visual Studio one
# (which needs the Visual C++ runtime; build-windows.sh's has its runtime linked in)
$exe = Join-Path $dst 'BO1Zombies.exe'
$x64 = $false
$msvc = $true
if (Test-Path $exe) {
    $bytes = [System.IO.File]::ReadAllBytes($exe)
    $pe = [System.BitConverter]::ToInt32($bytes, 0x3C)
    $x64 = [System.BitConverter]::ToUInt16($bytes, $pe + 4) -eq 0x8664
    $msvc = [System.Text.Encoding]::ASCII.GetString($bytes).Contains('VCRUNTIME140')
}

# players\ (your own profile and settings) is left out: the game makes a fresh one
robocopy $GameDir $dst *.* /S /E /XF BlackOps.exe BlackOpsMP.exe /XD (Join-Path $GameDir 'players') /NDL /NFL /NP /R:1 /W:1
if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }

$dx = Join-Path $dst 'Redist\DirectX'
function Expand-Redist([string]$cab, [string]$file) {
    $path = Join-Path $dx $cab
    if (Test-Path $path) { expand.exe $path "-F:$file" $dst | Out-Null }
    if (-not (Test-Path (Join-Path $dst $file))) {
        throw "could not extract $($file) from $($path): install the DirectX End-User Runtime (June 2010), or put $($file) next to BO1Zombies.exe"
    }
}
if ($x64) {
    # 64-bit: steam_api64.dll is the build's own; D3DX9 (d3dx9_43) out of the game's Redist\DirectX (XInput is
    # Windows' own xinput1_4)
    Expand-Redist 'Jun2010_d3dx9_43_x64.cab' 'd3dx9_43.dll'
} else {
    # the engine's own DLLs replace the install's older ones (the exe is built against this Steam SDK and Bink)
    Copy-Item (Join-Path $PSScriptRoot 'src\binklib\binkw32.dll') $dst -Force
    Copy-Item (Join-Path $PSScriptRoot 'src\steam\redistributable_bin\*.dll') $dst -Force
    # runtimes the exe needs that a fresh Windows lacks, put next to the exe so nothing has to be installed:
    # DirectX 9 (d3dx9_43, xinput1_3) out of the game's own Redist\DirectX
    Expand-Redist 'Jun2010_d3dx9_43_x86.cab' 'd3dx9_43.dll'
    Expand-Redist 'APR2007_xinput_x86.cab' 'xinput1_3.dll'
}
if ($msvc -and -not $x64) {
    # Visual C++ 2015-2022 (x86) out of Visual Studio, for the Visual Studio project's build
    $crt = Get-ChildItem 'C:\Program Files*\Microsoft Visual Studio\*\*\VC\Redist\MSVC\*\x86\Microsoft.VC14*.CRT' -Directory -ErrorAction SilentlyContinue |
        Sort-Object FullName | Select-Object -Last 1
    if ($crt) { Copy-Item (Join-Path $crt.FullName 'msvcp140.dll'), (Join-Path $crt.FullName 'vcruntime140.dll') $dst -Force }
    else { 'Visual C++ runtime not found next to Visual Studio: whoever plays needs the "Visual C++ Redistributable 2015-2022 (x86)"' }
}
"game files ready in $dst"
