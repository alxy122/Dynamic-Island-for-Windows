<#
.SYNOPSIS
    Builds dynamic-island-for-windows.wh.cpp from the parts in src/.

.DESCRIPTION
    Windhawk wants a mod as one single .wh.cpp file. For development the code
    lives in src/, split into many files. This script joins them in the order
    given by src/manifest.txt. The result is plain text concatenation, so the
    compiler sees exactly the same file as before the split.

    Edit the files in src/, then run this script. Never edit the generated
    .wh.cpp by hand: the next build overwrites it.

.PARAMETER Check
    Do not write anything. Exit with code 1 if the .wh.cpp is out of date
    (useful before a commit or in CI).

.PARAMETER Out
    Output path. Defaults to dynamic-island-for-windows.wh.cpp next to this script.

.EXAMPLE
    ./build.ps1
    ./build.ps1 -Check
#>
param(
    [switch]$Check,
    [string]$Out
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$src = Join-Path $root 'src'
if (-not $Out) { $Out = Join-Path $root 'dynamic-island-for-windows.wh.cpp' }

$utf8 = New-Object System.Text.UTF8Encoding($false)

function Read-Part([string]$rel) {
    $path = Join-Path $src $rel
    if (-not (Test-Path -LiteralPath $path)) { throw "Missing part: src/$rel (listed in src/manifest.txt)" }
    $text = [System.IO.File]::ReadAllText($path, $utf8).Replace("`r`n", "`n")
    if ($text.Length -gt 0 -and -not $text.EndsWith("`n")) { $text += "`n" }
    return $text
}

function Wrap-Block([string]$name, [string]$body) {
    return "// ==$name==`n/*`n$body*/`n// ==/$name==`n`n"
}

$sb = New-Object System.Text.StringBuilder
$listed = @{}
foreach ($raw in [System.IO.File]::ReadAllLines((Join-Path $src 'manifest.txt'), $utf8)) {
    $line = $raw.Trim()
    if ($line -eq '' -or $line.StartsWith('#')) { continue }
    $parts = $line -split '\s+', 2
    switch ($parts[0]) {
        '@header'          { [void]$sb.Append((Read-Part $parts[1]) + "`n"); $listed[$parts[1]] = $true }
        '@readme'          { [void]$sb.Append((Wrap-Block 'WindhawkModReadme' (Read-Part $parts[1]))); $listed[$parts[1]] = $true }
        '@settings'        { [void]$sb.Append((Wrap-Block 'WindhawkModSettings' (Read-Part $parts[1]))); $listed[$parts[1]] = $true }
        '@namespace-begin' { [void]$sb.Append("namespace {`n`n") }
        '@namespace-end'   { [void]$sb.Append("}  // namespace`n`n") }
        default {
            if ($line.StartsWith('@')) { throw "Unknown directive in manifest: $line" }
            [void]$sb.Append((Read-Part $line)); $listed[$line] = $true
        }
    }
}

# A part that exists but is not in the manifest would silently be left out.
$unlisted = Get-ChildItem -LiteralPath $src -Recurse -File |
    ForEach-Object { $_.FullName.Substring($src.Length + 1).Replace('\', '/') } |
    Where-Object { $_ -ne 'manifest.txt' -and -not $listed.ContainsKey($_) }
if ($unlisted) { throw "Files in src/ not listed in src/manifest.txt:`n  " + ($unlisted -join "`n  ") }

$result = $sb.ToString()

if ($Check) {
    $current = ''
    if (Test-Path -LiteralPath $Out) { $current = [System.IO.File]::ReadAllText($Out, $utf8).Replace("`r`n", "`n") }
    if ($current -ne $result) {
        Write-Host "OUT OF DATE: $Out does not match src/. Run ./build.ps1" -ForegroundColor Red
        exit 1
    }
    Write-Host "Up to date: $Out"
    exit 0
}

[System.IO.File]::WriteAllText($Out, $result, $utf8)
$lineCount = ($result.Length - $result.Replace("`n", '').Length)
Write-Host "Built $Out ($lineCount lines from $($listed.Count) parts)"
