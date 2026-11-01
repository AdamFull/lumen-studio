# Shared by package.ps1 and release.ps1 (dot-sourced). Like scripts/env.sh: adjust to your machine.

function Die([string]$Text) { throw $Text }

function Say([string]$Text) { Write-Host "`n== $Text" }

# Runs a program and stops when it fails. Its errors go to the console; in Windows PowerShell a
# program that merely writes to stderr must not count as a failed command.
function Invoke-Tool([string]$Exe, [string[]]$Arguments) {
    $ErrorActionPreference = 'Continue'
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { Die "$(Split-Path -Leaf $Exe) failed (exit code $LASTEXITCODE)." }
}

function Find-Tool([string]$Name) {
    $found = Get-Command $Name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $found) { Die "$Name is not on PATH: adjust the tool folders in scripts\toolchain.ps1." }
    return $found.Source
}

# Where a kit is unless told otherwise: -QtRoot, then QT_ROOT, then the default.
function Get-QtRoot([string]$QtRoot) {
    if ($QtRoot) { return $QtRoot }
    if ($env:QT_ROOT) { return $env:QT_ROOT }
    return 'C:\Qt\6.9.2\mingw_64'
}

# Puts MinGW, Qt, CMake and Ninja on PATH for this process. The caller restores PATH.
function Use-Toolchain([string]$QtRoot) {
    $vsTools = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake'
    $toolDirs = @('C:\Qt\Tools\mingw1310_64\bin', (Join-Path $QtRoot 'bin'),
                  "$vsTools\CMake\bin", "$vsTools\Ninja", 'C:\Qt\Tools\CMake_64\bin', 'C:\Qt\Tools\Ninja')
    $env:PATH = (@($toolDirs | Where-Object { Test-Path $_ }) + $env:PATH) -join ';'
}
