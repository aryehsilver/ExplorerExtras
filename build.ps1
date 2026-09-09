# Builds Explorer Extras. Usage: .\build.ps1 [-Configuration Release] [-Run]
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$Run
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found - is Visual Studio installed?" }

function Find-MSBuild {
    # Preferred: a complete, launchable instance that declares MSBuild.
    $p = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
    if ($p -and (Test-Path (Join-Path $p 'MSBuild\Current\Bin\MSBuild.exe'))) {
        return (Join-Path $p 'MSBuild\Current\Bin\MSBuild.exe')
    }
    # An instance mid-install or mid-modify reports isComplete=false and is
    # filtered out by default, so fall back to whatever has MSBuild on disk.
    foreach ($cand in (& $vswhere -all -prerelease -products * -property installationPath)) {
        $exe = Join-Path $cand 'MSBuild\Current\Bin\MSBuild.exe'
        if (Test-Path $exe) { return $exe }
    }
    return $null
}

$msbuild = Find-MSBuild
if (-not $msbuild) { throw "MSBuild.exe not found in any Visual Studio installation." }

$exe = Join-Path $root "build\x64\$Configuration\ExplorerExtras.exe"

# A running instance holds a lock on the output file.
#
# Give it time to close properly. It holds a reference to an out-of-process
# preview handler, and killing it outright orphans that process - which then
# lingers and can leave later previews stuck on their loading screen.
Get-Process ExplorerExtras -ErrorAction SilentlyContinue | ForEach-Object {
    Write-Host "Stopping running instance (pid $($_.Id))..." -ForegroundColor Yellow
    $_.CloseMainWindow() | Out-Null
    if (-not $_.WaitForExit(6000)) {
        Write-Host "  did not exit in time; killing" -ForegroundColor Red
        $_.Kill()
        $_.WaitForExit(5000) | Out-Null
    }
}

& $msbuild (Join-Path $root 'ExplorerExtras.sln') `
    /nologo `
    /v:minimal `
    /p:Configuration=$Configuration `
    /p:Platform=x64 `
    /m

if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE" }

Write-Host "`nBuilt: $exe" -ForegroundColor Green

if ($Run) {
    Start-Process $exe
    Write-Host "Started. Look for the folder icon in the notification area." -ForegroundColor Green
}
