<#
.SYNOPSIS
    Prepares a release of Lumen Studio: tests, packages, and a DRAFT release on GitHub.

.DESCRIPTION
    Nothing reaches users until you press Publish on the draft: the app only sees published
    releases, and the tag is created when you publish.

    The notes come from the version's section of CHANGELOG.md. When it has none, the first run
    writes one from the commits since the last release and stops so that you can edit it; run
    the same command again to go on.

      1. Checks: on main, in step with origin, nothing uncommitted (but the notes), gh logged in,
         the version newer than the last release.
      2. Sets the version in CMakeLists.txt.
      3. Builds and runs the tests (-SkipTests leaves that out).
      4. Commits the version and the notes, and packages that commit (package.ps1).
      5. Pushes main and creates the draft with the installer, the ZIP and the checksums.

.PARAMETER Version
    The new version, like 0.2.0.

.PARAMETER DryRun
    Only the checks and the notes: changes nothing, here or on GitHub.

.PARAMETER SkipTests
    Leaves out step 3.

.PARAMETER Replace
    Replaces a draft of this version. A published release is never replaced.

.PARAMETER TestBuildDir
    The build folder for the tests (default build\dev).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\release.ps1 -Version 0.2.0 -DryRun

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\release.ps1 -Version 0.2.0
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)][string]$Version,
    [switch]$DryRun,
    [switch]$SkipTests,
    [switch]$Replace,
    [string]$TestBuildDir,
    [string]$QtRoot
)

Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'toolchain.ps1')

$repo = 'AdamFull/lumen-studio'
$root = Split-Path -Parent $PSScriptRoot
$cmakeLists = Join-Path $root 'CMakeLists.txt'
$changelog = Join-Path $root 'CHANGELOG.md'
$utf8 = New-Object System.Text.UTF8Encoding($false)

# git and gh, with their output as lines. They report on stderr without failing.
function Get-Output([string]$Exe, [string[]]$Arguments) {
    $ErrorActionPreference = 'Continue'
    $lines = & $Exe @Arguments 2>$null
    $script:lastExit = $LASTEXITCODE
    return $lines
}

function Get-NotesSection([string]$Text, [string]$Ver) {
    $match = [regex]::Match($Text, "(?ms)^## $([regex]::Escape($Ver))\b[^\r\n]*\r?\n(.*?)(?=^## |\z)")
    if (-not $match.Success) { return $null }
    return $match.Groups[1].Value.Trim()
}

# What users would read, from the commits since the last release. A starting point to edit.
function New-NotesDraft([string]$Since) {
    if (-not $Since) { return '- First release.' }
    $subjects = Get-Output 'git' @('-C', $root, 'log', '--no-merges', '--format=%s', "$Since..HEAD")
    $notes = foreach ($subject in @($subjects)) {
        if (-not $subject) { continue }
        if ($subject -match '^(Translations|Merge|Version \d|Lock file)\b') { continue }
        $text = $subject -replace '^M\d+[a-z]?( step \w+)?:\s*', ''
        '- ' + $text.Substring(0, 1).ToUpperInvariant() + $text.Substring(1)
    }
    if (-not $notes) { return '- Small fixes.' }
    return ($notes -join "`n")
}

$pathBefore = $env:PATH
$exitCode = 0
$testData = $null
$dataBefore = $env:LUMEN_DATA_DIR
$platformBefore = $env:QT_QPA_PLATFORM
try {
    if ($Version -notmatch '^\d+\.\d+\.\d+$') { Die "The version must look like 0.2.0, not '$Version'." }
    $tag = "v$Version"
    $QtRoot = Get-QtRoot $QtRoot
    Use-Toolchain $QtRoot
    Find-Tool 'git' | Out-Null
    Find-Tool 'gh' | Out-Null
    $cmake = Find-Tool 'cmake'

    Say 'Checks'
    Get-Output 'gh' @('auth', 'status') | Out-Null
    if ($lastExit -ne 0) { Die 'gh is not logged in: run "gh auth login".' }
    $branch = Get-Output 'git' @('-C', $root, 'rev-parse', '--abbrev-ref', 'HEAD')
    if ($branch -ne 'main') { Die "Releases are made from main, this is $branch." }
    Get-Output 'git' @('-C', $root, 'fetch', '--tags', 'origin') | Out-Null
    if ($lastExit -ne 0) { Die 'Could not reach origin.' }
    $behind = Get-Output 'git' @('-C', $root, 'rev-list', '--count', 'HEAD..origin/main')
    if ([int]$behind -ne 0) { Die "main is $behind commit(s) behind origin: pull first." }

    # Only the files this script edits itself may be uncommitted: the notes of a first run, or
    # the version of a run that stopped later on.
    $changes = @(Get-Output 'git' @('-C', $root, 'status', '--porcelain')) | Where-Object { $_ }
    $others = @($changes | Where-Object { $_ -notmatch '^.. (CHANGELOG\.md|CMakeLists\.txt)$' })
    if ($others.Count -gt 0) { Die "Commit these first:`n$($others -join "`n")" }

    # Published releases only: a draft has no tag yet.
    $last = Get-Output 'git' @('-C', $root, 'tag', '--list', 'v*', '--sort=-v:refname') | Select-Object -First 1
    if ($last -and [version]$Version -le [version]$last.Substring(1)) { Die "$tag is not newer than the last release, $last." }
    $existing = Get-Output 'gh' @('release', 'view', $tag, '--repo', $repo, '--json', 'isDraft', '--jq', '.isDraft')
    $hasDraft = $lastExit -eq 0
    if ($hasDraft -and $existing -ne 'true') { Die "$tag is already published." }
    if ($hasDraft -and -not $Replace) { Die "A draft of $tag exists: use -Replace, or delete it on GitHub." }
    Write-Host "On main, in step with origin. Last release: $(if ($last) { $last } else { 'none' })."

    Say 'Release notes'
    $text = if (Test-Path $changelog) { [IO.File]::ReadAllText($changelog) } else { "# Changelog`n" }
    $notes = Get-NotesSection $text $Version
    if (-not $notes) {
        $draft = New-NotesDraft $last
        if ($DryRun) {
            Write-Host "CHANGELOG.md has no section for $Version. A run without -DryRun would write this and stop:`n"
            Write-Host $draft
            Write-Host "`nDry run: nothing was changed."
            exit 0
        }
        $section = "## $Version - $(Get-Date -Format 'yyyy-MM-dd')`n`n$draft`n`n"
        $heading = [regex]::Match($text, '(?m)^# [^\r\n]*\r?\n+')
        $at = if ($heading.Success) { $heading.Index + $heading.Length } else { 0 }
        [IO.File]::WriteAllText($changelog, $text.Insert($at, $section), $utf8)
        Write-Host "CHANGELOG.md has a new section for ${Version}:`n"
        Write-Host $draft
        Write-Host "`nEdit it for the people who will read it, then run the same command again."
        exit 0
    }
    Write-Host $notes
    if ($DryRun) {
        Write-Host "`nDry run: nothing was changed."
        exit 0
    }

    Say "Setting the version to $Version"
    $lists = [IO.File]::ReadAllText($cmakeLists)
    $pattern = [regex]'(?m)^(\s*VERSION )\d+\.\d+\.\d+'
    if (-not $pattern.IsMatch($lists)) { Die 'The version was not found in CMakeLists.txt.' }
    [IO.File]::WriteAllText($cmakeLists, $pattern.Replace($lists, "`${1}$Version", 1), $utf8)

    if (-not $SkipTests) {
        Say 'Tests'
        if (-not $TestBuildDir) { $TestBuildDir = Join-Path $root 'build\dev' }
        if (-not (Test-Path (Join-Path $TestBuildDir 'CMakeCache.txt'))) { Die "No build in ${TestBuildDir}: use -TestBuildDir or -SkipTests." }
        Invoke-Tool $cmake @('--build', $TestBuildDir, '--parallel')
        # Their own data folder: without one they would use the real settings and projects.
        $testData = Join-Path ([IO.Path]::GetTempPath()) ('lumen-release-' + [guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Force -Path $testData | Out-Null
        if (-not (Test-Path $testData)) { Die 'Could not prepare the data folder of the tests.' }
        $env:LUMEN_DATA_DIR = $testData
        $env:QT_QPA_PLATFORM = 'offscreen'
        $failed = @()
        foreach ($test in Get-ChildItem (Join-Path $TestBuildDir 'bin\tst_*.exe')) {
            $report = Join-Path $testData "$($test.BaseName).txt"
            $ErrorActionPreference = 'Continue'
            & $test.FullName -o "$report,txt" | Out-Null
            $code = $LASTEXITCODE
            $ErrorActionPreference = 'Stop'
            $totals = if (Test-Path $report) { Select-String -Path $report -Pattern '^Totals:' | Select-Object -First 1 } else { $null }
            Write-Host "$($test.BaseName): $(if ($totals) { $totals.Line } else { "no report (exit code $code)" })"
            if ($code -ne 0) { $failed += $test.BaseName }
        }
        if ($failed.Count -gt 0) { Die "Tests failed: $($failed -join ', '). Reports: $testData" }
        $env:LUMEN_DATA_DIR = $dataBefore
        $env:QT_QPA_PLATFORM = $platformBefore
        Remove-Item -LiteralPath $testData -Recurse -Force -ErrorAction SilentlyContinue
    }

    Say 'Committing'
    Invoke-Tool 'git' @('-C', $root, 'add', 'CMakeLists.txt', 'CHANGELOG.md')
    Get-Output 'git' @('-C', $root, 'diff', '--cached', '--quiet') | Out-Null
    if ($lastExit -ne 0) {
        Invoke-Tool 'git' @('-C', $root, 'commit', '-q', '-m', "Version $Version")
    } else {
        Write-Host 'Nothing to commit: the version and the notes are in.'
    }

    # The package comes from the commit that goes out.
    Say 'Packaging'
    Invoke-Tool 'powershell' @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $root 'scripts\package.ps1'), '-QtRoot', $QtRoot)
    $dist = Join-Path $root 'dist'
    $assets = @((Join-Path $dist "LumenStudio-$Version-win64.exe"),
                (Join-Path $dist "LumenStudio-$Version-win64-portable.zip"),
                (Join-Path $dist "LumenStudio-$Version-SHA256.txt"))
    foreach ($asset in $assets) {
        if (-not (Test-Path $asset)) { Die "$asset was not packaged." }
    }

    Say 'Publishing a draft'
    Invoke-Tool 'git' @('-C', $root, 'push', 'origin', 'main')
    $commit = Get-Output 'git' @('-C', $root, 'rev-parse', 'HEAD')
    if ($hasDraft) { Invoke-Tool 'gh' @('release', 'delete', $tag, '--repo', $repo, '--yes') }
    $notesFile = Join-Path ([IO.Path]::GetTempPath()) ('lumen-notes-' + [guid]::NewGuid().ToString('N') + '.md')
    [IO.File]::WriteAllText($notesFile, $notes + "`n", $utf8)
    try {
        Invoke-Tool 'gh' (@('release', 'create', $tag, '--repo', $repo, '--draft', '--target', $commit,
                            '--title', "Lumen Studio $Version", '--notes-file', $notesFile) + $assets)
    } finally {
        Remove-Item -LiteralPath $notesFile -Force -ErrorAction SilentlyContinue
    }
    Write-Host "`nThe draft is ready. Try the installer, then press Publish on GitHub; only then do users get it."
} catch {
    [Console]::Error.WriteLine("ERROR: $($_.Exception.Message)")
    $exitCode = 1
} finally {
    $env:PATH = $pathBefore
    $env:LUMEN_DATA_DIR = $dataBefore
    $env:QT_QPA_PLATFORM = $platformBefore
}
exit $exitCode
