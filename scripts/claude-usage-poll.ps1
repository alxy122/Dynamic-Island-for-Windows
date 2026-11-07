<#
.SYNOPSIS
  Keeps the Dynamic Island "Claude usage" tab up to date, whether or not Claude Code is running.

.DESCRIPTION
  Runs Claude's own CLI (`claude --print "/cost"`), reads the two lines it prints
  ("Current session: 41% used - resets ..." and "Current week (all models): ..."),
  and writes them to %USERPROFILE%\.claude\island-usage.json, the file the mod reads.
  The numbers are the account-wide ones from your plan, so they include claude.ai
  chats, not only Claude Code.

  Nothing here touches your login: the claude CLI does its own authentication, this
  script only reads what it prints.

.PARAMETER Loop
  Keep running and refresh every -IntervalSeconds (use this for the autostart task).

.PARAMETER IntervalSeconds
  Pause between refreshes in -Loop mode. Default 120.

.PARAMETER ClaudePath
  Full path to claude / claude.exe if it is not on PATH for this script.

.PARAMETER OutFile
  Where to write. Default: $env:ISLAND_USAGE_FILE, else %USERPROFILE%\.claude\island-usage.json

.PARAMETER FromFile
  For testing: read the CLI output from this text file instead of running claude.
#>
param(
    [switch]$Loop,
    [int]$IntervalSeconds = 120,
    [string]$ClaudePath,
    [string]$OutFile,
    [string]$FromFile,
    # Optional: append one line per run here (rotated to .old at 1 MB).
    [string]$LogFile
)

function Write-Log([string]$msg) {
    if (-not $LogFile) { return }
    try {
        if ((Test-Path -LiteralPath $LogFile) -and (Get-Item -LiteralPath $LogFile).Length -gt 1MB) {
            Move-Item -LiteralPath $LogFile -Destination "$LogFile.old" -Force
        }
        $line = '{0}  {1}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $msg
        [System.IO.File]::AppendAllText($LogFile, $line + [Environment]::NewLine, (New-Object System.Text.UTF8Encoding($false)))
    } catch { }
}

$ErrorActionPreference = 'Stop'

if (-not $OutFile) {
    if ($env:ISLAND_USAGE_FILE) { $OutFile = $env:ISLAND_USAGE_FILE }
    else { $OutFile = Join-Path $env:USERPROFILE '.claude\island-usage.json' }
}

$MonthNumber = @{ jan = 1; feb = 2; mar = 3; apr = 4; may = 5; jun = 6; jul = 7; aug = 8; sep = 9; oct = 10; nov = 11; dec = 12 }

# "Nov 2, 2:40pm (Europe/Berlin)", "2:40pm (Europe/Berlin)" or "Nov 8, 9am" -> epoch seconds (UTC)
function Get-ResetEpoch([string]$text) {
    $pattern = '(?:(?<mon>[A-Za-z]{3})[a-z]*\.?\s+(?<day>\d{1,2}),?\s+)?(?<h>\d{1,2})(?::(?<min>\d{2}))?\s*(?<ap>[ap]m)\b(?:\s*\((?<tz>[^)]+)\))?'
    $m = [regex]::Match($text, $pattern, [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
    if (-not $m.Success) { return $null }

    $hour = [int]$m.Groups['h'].Value
    $minute = if ($m.Groups['min'].Success) { [int]$m.Groups['min'].Value } else { 0 }
    $pm = $m.Groups['ap'].Value.ToLowerInvariant() -eq 'pm'
    if ($pm -and $hour -lt 12) { $hour += 12 }
    if (-not $pm -and $hour -eq 12) { $hour = 0 }

    # The CLI prints the time in your configured zone; use it if this runtime knows it
    # (PowerShell 7), otherwise the machine's own zone (Windows PowerShell 5.1).
    $tz = [TimeZoneInfo]::Local
    if ($m.Groups['tz'].Success) {
        try { $tz = [TimeZoneInfo]::FindSystemTimeZoneById($m.Groups['tz'].Value.Trim()) } catch { }
    }
    $now = [TimeZoneInfo]::ConvertTimeFromUtc([DateTime]::UtcNow, $tz)

    if ($m.Groups['mon'].Success) {
        $mon = $MonthNumber[$m.Groups['mon'].Value.ToLowerInvariant()]
        if (-not $mon) { return $null }
        $local = [DateTime]::new($now.Year, $mon, [int]$m.Groups['day'].Value, $hour, $minute, 0)
        if ($local -lt $now.AddDays(-2)) { $local = $local.AddYears(1) }   # December -> January
    } else {
        $local = $now.Date.AddHours($hour).AddMinutes($minute)
        if ($local -lt $now.AddMinutes(-1)) { $local = $local.AddDays(1) }   # "today" already passed: tomorrow
    }

    try { $utc = [TimeZoneInfo]::ConvertTimeToUtc($local, $tz) } catch { return $null }
    return [DateTimeOffset]::new($utc).ToUnixTimeSeconds()
}

function ConvertFrom-ClaudeCost([string]$text) {
    $out = [ordered]@{ updated_at = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds() }
    $rules = @(
        @('five_hour', '^\s*Current session:\s*(?<pct>\d+(?:\.\d+)?)\s*%\s*used(?<rest>.*)$'),
        @('seven_day', '^\s*Current week \(all models\):\s*(?<pct>\d+(?:\.\d+)?)\s*%\s*used(?<rest>.*)$')
    )
    foreach ($line in ($text -split "\r?\n")) {
        foreach ($rule in $rules) {
            $m = [regex]::Match($line, $rule[1], [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
            if (-not $m.Success) { continue }
            $reset = Get-ResetEpoch $m.Groups['rest'].Value
            if ($null -eq $reset) { continue }
            $out[$rule[0]] = [ordered]@{ used_percentage = [double]$m.Groups['pct'].Value; resets_at = $reset }
        }
    }
    return $out
}

function Get-ClaudeCostText {
    if ($FromFile) { return (Get-Content -LiteralPath $FromFile -Raw) }
    $exe = if ($ClaudePath) { $ClaudePath } else { (Get-Command claude -ErrorAction Stop).Source }
    # A job gives us a timeout and works for claude.exe, .cmd and .ps1 alike. It runs from
    # the profile folder so no project of yours picks up an extra session.
    $job = Start-Job -ArgumentList $exe -ScriptBlock {
        param($claude)
        Set-Location -LiteralPath $env:USERPROFILE
        & $claude --print '/cost' 2>&1 | Out-String
    }
    try {
        if (-not (Wait-Job $job -Timeout 90)) { return $null }
        return ((Receive-Job $job) -join "`n")
    } finally {
        Remove-Job $job -Force -ErrorAction SilentlyContinue
    }
}

# True if the file was updated.
function Update-UsageFile {
    $text = Get-ClaudeCostText
    if ([string]::IsNullOrWhiteSpace($text)) { return $false }
    $data = ConvertFrom-ClaudeCost $text
    if ($data.Count -le 1) { return $false }   # nothing recognised: keep the old file

    $dir = Split-Path -Parent $OutFile
    if ($dir -and -not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
    $json = $data | ConvertTo-Json -Depth 4 -Compress
    $tmp = "$OutFile.tmp"
    # UTF-8 without BOM, via a temp file so the mod never reads half a file.
    [System.IO.File]::WriteAllText($tmp, $json, (New-Object System.Text.UTF8Encoding($false)))
    Move-Item -LiteralPath $tmp -Destination $OutFile -Force
    return $true
}

if ($Loop) {
    while ($true) {
        try { [void](Update-UsageFile) } catch { }
        Start-Sleep -Seconds $IntervalSeconds
    }
} else {
    try {
        if (Update-UsageFile) {
            $d = Get-Content -LiteralPath $OutFile -Raw | ConvertFrom-Json
            Write-Log ("OK    session {0}%  week {1}%" -f $d.five_hour.used_percentage, $d.seven_day.used_percentage)
            Write-Output "Updated $OutFile"
        } else {
            Write-Log 'WARN  no usage data found (CLI timeout or unrecognised output)'
            Write-Output 'No usage data found.'; exit 1
        }
    } catch {
        Write-Log "ERROR $($_.Exception.Message)"
        throw
    }
}
