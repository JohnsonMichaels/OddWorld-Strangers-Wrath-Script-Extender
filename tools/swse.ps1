# Send commands to the SWSE Console inside the running game and print the reply.
#
# The console polls SWSEMods\SWSE Console\remote_in.txt about 8x/sec and
# writes results to remote_out.txt. Each request carries a sequence number; the
# console only acts when the number CHANGES, so the seq must be bumped per send.
#
# This exists because the in-game console reads the keyboard with
# GetAsyncKeyState. Driving it by synthesising keystrokes would type into
# whatever window actually has focus, which is how the cursor got trapped
# before. The mailbox needs no focus at all.
#
# USAGE
#   powershell -File tools/swse.ps1 "shaderdump"
#   powershell -File tools/swse.ps1 "hitreact off" "wind on"
# No param() block on purpose: with -File, ANY declared parameter takes
# position 0 and swallows the first command string. Everything arrives in
# $args; the reply timeout comes from SWSE_TIMEOUT if it needs overriding.
$Commands = $args
$TimeoutSec = if ($env:SWSE_TIMEOUT) { [int]$env:SWSE_TIMEOUT } else { 15 }

$dir = "C:\Program Files (x86)\Steam\steamapps\common\Stranger's Wrath\SWSEMods\SWSE Console"
$in  = Join-Path $dir 'remote_in.txt'
$out = Join-Path $dir 'remote_out.txt'

if (-not (Test-Path $dir)) { Write-Error "console mailbox not found: $dir"; exit 1 }
if (-not $Commands -or $Commands.Count -eq 0) { Write-Error "no commands given"; exit 1 }

# Next sequence number: one past whatever is already on disk.
$seq = 1
if (Test-Path $in) {
    $first = (Get-Content $in -TotalCount 1)
    $n = 0
    if ([int]::TryParse($first, [ref]$n)) { $seq = $n + 1 }
}

# Note what the reply file looked like before, so a stale one is not mistaken
# for our answer.
$prev = ''
if (Test-Path $out) { $prev = (Get-Content $out -TotalCount 1) }

$body = ($Commands -join "`r`n")
Set-Content -Path $in -Value "$seq`r`n$body" -Encoding ascii

$deadline = (Get-Date).AddSeconds($TimeoutSec)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 200
    if (-not (Test-Path $out)) { continue }
    try { $text = Get-Content $out -Raw -ErrorAction Stop } catch { continue }
    if (-not $text) { continue }
    $lines = $text -split "`r?`n"
    if ($lines[0] -ne "$seq") { continue }
    if ($text -notmatch '<<END>>') { continue }        # still being written
    # strip the seq header and the terminator
    ($lines[1..($lines.Count - 1)] | Where-Object { $_ -ne '<<END>>' }) -join "`n"
    exit 0
}

Write-Error "no reply within ${TimeoutSec}s (is the game running with SWSE loaded?)"
exit 2
