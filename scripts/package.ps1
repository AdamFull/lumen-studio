<#
.SYNOPSIS
    Builds a release of Lumen Studio and packages it, in PowerShell (the same steps as package.sh).

.DESCRIPTION
    It writes to dist\:
      LumenStudio-<version>-win64\               the deployed folder (windeployqt)
      LumenStudio-<version>-win64-portable.zip   portable: portable.txt keeps the data in userdata\
      LumenStudio-<version>-win64.exe            the NSIS installer
      LumenStudio-<version>-SHA256.txt           checksums of what to publish

    The version comes from project() in CMakeLists.txt. The installer needs NSIS
    (https://nsis.sourceforge.io, or: winget install NSIS.NSIS); it stops early when it is missing.
    Nothing is signed: a certificate is the publisher's; Windows SmartScreen warns about unsigned
    downloads.

.PARAMETER BuildDir
    The build directory (default build\package).

.PARAMETER NoInstaller
    Everything but the installer (needs no NSIS).

.PARAMETER NoRun
    Skips starting the packaged program.

.PARAMETER QtRoot
    The Qt kit (default: QT_ROOT from the environment, else C:\Qt\6.9.2\mingw_64).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\package.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\package.ps1 -NoInstaller
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)][string]$BuildDir,
    [switch]$NoInstaller,
    [switch]$NoRun,
    [string]$QtRoot
)

Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'

function Say([string]$Text) { Write-Host "`n== $Text" }
function Die([string]$Text) { throw $Text }

# Runs a program and stops when it fails. Its errors go to the console; in Windows PowerShell a
# program that merely writes to stderr must not count as a failed command.
function Invoke-Tool([string]$Exe, [string[]]$Arguments) {
    $ErrorActionPreference = 'Continue'
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { Die "$(Split-Path -Leaf $Exe) failed (exit code $LASTEXITCODE)." }
}

function Find-Tool([string]$Name) {
    $found = Get-Command $Name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $found) { Die "$Name is not on PATH: adjust the tool folders in scripts\package.ps1." }
    return $found.Source
}

$root = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $root 'dist'
if (-not $BuildDir) {
    $BuildDir = Join-Path $root 'build\package'
} elseif (-not [IO.Path]::IsPathRooted($BuildDir)) {
    $BuildDir = Join-Path (Get-Location).Path $BuildDir
}
if (-not $QtRoot) { $QtRoot = $env:QT_ROOT }
if (-not $QtRoot) { $QtRoot = 'C:\Qt\6.9.2\mingw_64' }

$pathBefore = $env:PATH
$exitCode = 0
try {
    # The toolchain: where it is on this machine (adjust to yours, like scripts/env.sh).
    $vsTools = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake'
    $toolDirs = @('C:\Qt\Tools\mingw1310_64\bin', (Join-Path $QtRoot 'bin'),
                  "$vsTools\CMake\bin", "$vsTools\Ninja", 'C:\Qt\Tools\CMake_64\bin', 'C:\Qt\Tools\Ninja')
    $env:PATH = (@($toolDirs | Where-Object { Test-Path $_ }) + $env:PATH) -join ';'

    $cmake = Find-Tool 'cmake'
    # Next to cmake: a "cpack" found on PATH can be Chocolatey's, another program.
    $cpack = Join-Path (Split-Path -Parent $cmake) 'cpack.exe'
    if (-not (Test-Path $cpack)) { Die "cpack was not found next to $cmake." }
    $objdump = Find-Tool 'objdump'
    $python = Find-Tool 'python'
    Find-Tool 'ninja' | Out-Null
    Find-Tool 'g++' | Out-Null
    if (-not (Test-Path $QtRoot)) { Die "Qt is not at ${QtRoot}: use -QtRoot or set QT_ROOT." }

    $makensis = $null
    if (-not $NoInstaller) {
        $found = Get-Command 'makensis' -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($found) { $makensis = $found.Source }
        foreach ($candidate in @("${env:ProgramFiles(x86)}\NSIS\makensis.exe", "$env:ProgramFiles\NSIS\makensis.exe")) {
            if (-not $makensis -and (Test-Path $candidate)) { $makensis = $candidate }
        }
        if (-not $makensis) { Die 'makensis was not found. Install NSIS (winget install NSIS.NSIS), or use -NoInstaller.' }
    }

    Say 'Checks before building'
    if (Get-Command 'git' -CommandType Application -ErrorAction SilentlyContinue) {
        $ErrorActionPreference = 'Continue'
        $changes = & git -C $root status --porcelain
        $commit = & git -C $root rev-parse --short HEAD
        $ErrorActionPreference = 'Stop'
        if ($changes) { Write-Host 'WARNING: the working tree has uncommitted changes; they go into this package.' }
        Write-Host "Commit: $commit"
    }
    Invoke-Tool $python @('-I', (Join-Path $root 'scripts\check-translations.py'))

    Say 'Building the release'
    Invoke-Tool $cmake @('-S', $root, '-B', $BuildDir, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DLUMEN_BUILD_TESTS=OFF',
                         '-DLUMEN_DEPLOY_ON_BUILD=OFF', "-DCMAKE_PREFIX_PATH=$($QtRoot.Replace('\', '/'))")
    Invoke-Tool $cmake @('--build', $BuildDir, '--parallel')

    $versionLine = Select-String -Path (Join-Path $BuildDir 'CMakeCache.txt') -Pattern '^CMAKE_PROJECT_VERSION:STATIC=(.+)$' | Select-Object -First 1
    if (-not $versionLine) { Die 'The version was not found in CMakeCache.txt.' }
    $version = $versionLine.Matches[0].Groups[1].Value
    $name = "LumenStudio-$version-win64"
    $stage = Join-Path $dist $name
    $zip = Join-Path $dist "$name-portable.zip"
    $setup = Join-Path $dist "$name.exe"
    $sums = Join-Path $dist "LumenStudio-$version-SHA256.txt"

    Say "Deploying version $version to dist\$name"
    foreach ($old in @($stage, $zip, $setup, $sums)) {
        if (Test-Path $old) { Remove-Item -LiteralPath $old -Recurse -Force }
    }
    New-Item -ItemType Directory -Force -Path $dist | Out-Null
    Invoke-Tool $cmake @('--install', $BuildDir, '--prefix', $stage)

    Say 'Checking the deployed folder'
    foreach ($file in @('LumenStudio.exe', 'qt.conf', 'plugins\platforms\qwindows.dll', 'plugins\sqldrivers\qsqlite.dll',
                        'plugins\tls\qschannelbackend.dll', 'plugins\imageformats\qjpeg.dll', 'data\workflows',
                        'docs\USER_GUIDE.md', 'docs\THIRD_PARTY_NOTICES.md')) {
        if (-not (Test-Path (Join-Path $stage $file))) { Die "$file is missing from the deployed folder." }
    }
    # Every DLL the program and its plugins load must be in the folder or part of Windows.
    $system32 = Join-Path $env:SystemRoot 'System32'
    $missing = 0
    foreach ($binary in Get-ChildItem -LiteralPath $stage -Recurse -File | Where-Object { $_.Extension -in '.exe', '.dll' }) {
        $ErrorActionPreference = 'Continue'
        $headers = & $objdump -p $binary.FullName
        $ErrorActionPreference = 'Stop'
        foreach ($line in $headers) {
            if ($line -notmatch '^\s*DLL Name: (.+)$') { continue }
            $dll = $Matches[1].Trim()
            if ($dll -like 'api-ms-win-*' -or $dll -like 'ext-ms-win-*') { continue } # resolved by Windows at load time
            if (-not (Test-Path (Join-Path $stage $dll)) -and -not (Test-Path (Join-Path $system32 $dll))) {
                Write-Host "  $($binary.FullName.Substring($stage.Length + 1)) needs $dll"
                $missing++
            }
        }
    }
    if ($missing) { Die 'The folder lacks DLLs (above): it would not start on another computer.' }
    Write-Host 'All DLLs are there.'

    if (-not $NoRun) {
        Say 'Starting the packaged program, without Qt or MinGW on PATH'
        $scratch = Join-Path ([IO.Path]::GetTempPath()) ('lumen-package-' + [guid]::NewGuid().ToString('N'))
        $data = Join-Path $scratch 'data'
        $shot = Join-Path $scratch 'start.png'
        New-Item -ItemType Directory -Force -Path $scratch | Out-Null

        # It renders its window to a PNG and quits; its data goes to the temporary folder.
        # An empty LUMEN_DATA_DIR would mean the real settings and projects of whoever runs this.
        if (-not $data -or -not (Test-Path $scratch)) { Die 'Could not prepare the temporary folder for the start check.' }
        $start = New-Object System.Diagnostics.ProcessStartInfo
        $start.FileName = Join-Path $stage 'LumenStudio.exe'
        $start.Arguments = "--screenshot `"$shot`" --size 1280x720"
        $start.WorkingDirectory = $scratch
        $start.UseShellExecute = $false
        $start.RedirectStandardOutput = $true
        $start.RedirectStandardError = $true
        $start.EnvironmentVariables['PATH'] = "$system32;$env:SystemRoot"
        $start.EnvironmentVariables['LUMEN_DATA_DIR'] = $data
        $program = [System.Diagnostics.Process]::Start($start)
        $program.BeginOutputReadLine()
        $program.BeginErrorReadLine()
        if (-not $program.WaitForExit(180000)) {
            $program.Kill()
            Die 'The packaged program did not quit by itself within 3 minutes.'
        }
        if ($program.ExitCode -ne 0) { Die "The packaged program did not start and quit by itself (exit code $($program.ExitCode))." }

        if (-not (Test-Path $shot) -or (Get-Item $shot).Length -eq 0) { Die 'The packaged program started but rendered nothing.' }
        $log = Join-Path $data 'logs\lumen.log'
        if (-not (Test-Path $log)) { Die "The packaged program wrote no log to the temporary folder: $log" }
        if (Select-String -Path $log -CaseSensitive -Pattern 'crash|FATAL' -Quiet) { Die "The packaged program logged a crash: $log" }
        if (Select-String -Path $log -CaseSensitive -Pattern 'Could not parse' -Quiet) { Die "The packaged program lost its stylesheet: $log" }
        Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
        Write-Host 'It starts and renders its window.'
    }

    Say 'Portable ZIP'
    # The marker keeps settings, cache and projects next to the executable.
    $marker = Join-Path $stage 'portable.txt'
    [IO.File]::WriteAllText($marker, "Portable mode: settings, logs, cache and projects are stored in the userdata folder.`nDelete this file to use the per-user locations instead.`n")
    Push-Location $dist
    try {
        Invoke-Tool $cmake @('-E', 'tar', 'cf', "$name-portable.zip", '--format=zip', $name)
    } finally {
        Pop-Location
    }
    Remove-Item -LiteralPath $marker
    Write-Host $zip

    $published = @($zip)
    if (-not $NoInstaller) {
        Say 'Installer'
        $env:PATH = (Split-Path -Parent $makensis) + ';' + $env:PATH
        Push-Location $BuildDir
        try {
            Invoke-Tool $cpack @('-G', 'NSIS', '-B', $dist)
        } finally {
            Pop-Location
        }
        $packages = Join-Path $dist '_CPack_Packages'
        if (Test-Path $packages) { Remove-Item -LiteralPath $packages -Recurse -Force }
        if (-not (Test-Path $setup) -or (Get-Item $setup).Length -eq 0) { Die "cpack did not produce $name.exe." }
        Write-Host $setup
        $published += $setup
    }

    Say 'Checksums'
    # In the format of sha256sum, so that "sha256sum -c" reads the file.
    $lines = @($published | ForEach-Object {
        '{0} *{1}' -f (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash.ToLowerInvariant(), (Split-Path -Leaf $_)
    })
    $lines | ForEach-Object { Write-Host $_ }
    [IO.File]::WriteAllText($sums, (($lines -join "`n") + "`n"))

    $bytes = (Get-ChildItem -LiteralPath $stage -Recurse -File | Measure-Object -Property Length -Sum).Sum
    Write-Host ("`nDone: {0:N0} MB deployed in dist\{1}" -f ($bytes / 1MB), $name)
} catch {
    [Console]::Error.WriteLine("ERROR: $($_.Exception.Message)")
    $exitCode = 1
} finally {
    $env:PATH = $pathBefore
}
exit $exitCode
