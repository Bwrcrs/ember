# ============================================================
#  Ember test harness
#
#  NOTE: keep this file pure ASCII. Same reasoning as build.bat:
#  Windows PowerShell 5.1 reads a .ps1 with no BOM using the
#  system ANSI codepage (GBK on this machine), so a UTF-8 Chinese
#  character gets mis-decoded and can break the parse. Everything
#  this script asserts on is ASCII on purpose -- ember's screen
#  dumps, trace lines and disassembly are all ASCII by design
#  (see trace.cpp), and the non-ASCII things it needs to check
#  (Chinese error messages) are checked by EXIT CODE plus an
#  ASCII fragment like the offending file name.
#
#  ------------------------------------------------------------------
#  READ THIS BEFORE TRUSTING A GREEN RUN
#
#  The golden files under tests\run\*.screen and *.trace are
#  REGRESSION tests, not CORRECTNESS tests. They record what the
#  emulator DID, not what it SHOULD do. Running with -Update bakes
#  the current behaviour -- bugs included -- into "expected".
#
#  So a green run means "nothing changed since last time", which is
#  exactly what you want while refactoring and exactly what you must
#  NOT mistake for "the emulator is right".
#
#  There are only two correctness oracles, and both are separated
#  out below:
#    1. tests\external\*.ch8 -- third-party ROMs whose entire
#       purpose is to put a cross on screen when you are wrong.
#       They are skipped when the files are absent.
#    2. hand-written expectations that are NEVER regenerated --
#       see $Invariants further down.
#  ------------------------------------------------------------------
#
#  Usage:
#     powershell -File tests\run_tests.ps1
#     powershell -File tests\run_tests.ps1 -Update     regenerate goldens
#     powershell -File tests\run_tests.ps1 -Quiet      only failures + summary
# ============================================================

param(
    [switch]$Update,
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'

$TestsDir = $PSScriptRoot
$Root     = Split-Path -Parent $TestsDir
$Ember    = Join-Path $Root 'ember.exe'
$RunDir   = Join-Path $TestsDir 'run'
$ExtDir   = Join-Path $TestsDir 'external'

if (-not (Test-Path $Ember)) {
    Write-Host "[FAIL] ember.exe not found at $Ember" -ForegroundColor Red
    Write-Host "       run build.bat headless first" -ForegroundColor Red
    exit 2
}

# ---- counters ----
$script:Pass   = 0
$script:Fail   = 0
$script:Skip   = 0
$script:Failed = New-Object System.Collections.ArrayList

function Report-Pass([string]$Name) {
    $script:Pass++
    if (-not $Quiet) {
        Write-Host ("  [PASS] " + $Name) -ForegroundColor Green
    }
}

function Report-Skip([string]$Name, [string]$Why) {
    $script:Skip++
    if (-not $Quiet) {
        Write-Host ("  [SKIP] " + $Name + "  (" + $Why + ")") -ForegroundColor Yellow
    }
}

function Report-Fail([string]$Name, [string]$Detail) {
    $script:Fail++
    [void]$script:Failed.Add($Name)
    Write-Host ("  [FAIL] " + $Name) -ForegroundColor Red
    if ($Detail -ne '') {
        Write-Host ("         " + $Detail) -ForegroundColor Red
    }
}

function Write-Section([string]$Title) {
    if (-not $Quiet) {
        Write-Host ""
        Write-Host ("== " + $Title) -ForegroundColor Cyan
    }
}

# ============================================================
#  Running ember
# ============================================================

# Quote one argument for ProcessStartInfo.Arguments, which is a
# single string rather than an array -- so a path with a space in it
# would otherwise silently become two arguments.
function Quote-Arg([string]$Text) {
    if ($Text -match '[\s"]') {
        return '"' + ($Text -replace '"', '\"') + '"'
    }
    return $Text
}

# Longest any single ember invocation is allowed to take.
#
# This exists because "the emulator loops forever" is a failure mode
# that a test suite must survive, not one it should hang on. A hung
# suite looks identical to a slow one, and the person watching
# eventually kills it and learns nothing about which case hung.
#
# 30s is ~1000x the slowest real case (a 50000-cycle screen dump),
# so it can only fire on a genuine hang.
$TimeoutMs = 30000

# Run ember with real argparse semantics: real exit code, real
# UTF-8 on both streams, no shell in between.
#
# Deliberately NOT `& $Ember ...` -- PowerShell would split stdout
# into an array of lines, lose the distinction between "no output"
# and "empty line", and give us $LASTEXITCODE only sometimes.
function Invoke-Ember([string[]]$Arguments) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName               = $Ember
    $psi.Arguments              = (($Arguments | ForEach-Object { Quote-Arg $_ }) -join ' ')
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError  = $true
    $psi.UseShellExecute        = $false
    $psi.CreateNoWindow         = $true
    $psi.StandardOutputEncoding = [System.Text.Encoding]::UTF8
    $psi.StandardErrorEncoding  = [System.Text.Encoding]::UTF8
    # ember writes its screen dump relative to nothing in particular,
    # so run it from the repo root and always pass absolute paths.
    $psi.WorkingDirectory       = $Root

    $proc = [System.Diagnostics.Process]::Start($psi)

    # Drain both pipes on background tasks BEFORE waiting.
    #
    # The obvious version -- ReadToEnd() then WaitForExit() -- has two
    # problems. One is the classic deadlock: the child fills its
    # stderr pipe buffer while we are blocked reading stdout, so both
    # sides wait for each other forever. The other is that it gives us
    # no place to put a timeout at all.
    $outTask = $proc.StandardOutput.ReadToEndAsync()
    $errTask = $proc.StandardError.ReadToEndAsync()

    if (-not $proc.WaitForExit($TimeoutMs)) {
        try { $proc.Kill() } catch { }
        $proc.WaitForExit()
        return @{
            Out  = ''
            Err  = 'TIMEOUT after ' + $TimeoutMs + 'ms: ' + ($Arguments -join ' ')
            Code = -1
        }
    }

    # Safe to block here: the process has exited, so both streams are
    # at EOF and these complete immediately.
    return @{
        Out  = $outTask.Result
        Err  = $errTask.Result
        Code = $proc.ExitCode
    }
}

# Flatten the line endings and drop trailing blank lines, so a
# \r\n vs \n difference or a missing final newline is not reported
# as a failure. Anything else is a real difference.
function Normalize([string]$Text) {
    if ($null -eq $Text) { return '' }
    $t = $Text -replace "`r`n", "`n"
    $t = $t -replace "`r", "`n"
    return $t.TrimEnd("`n")
}

function Show-FirstDiff([string]$Expected, [string]$Actual) {
    $e = $Expected -split "`n"
    $a = $Actual   -split "`n"
    $n = [Math]::Max($e.Count, $a.Count)
    for ($i = 0; $i -lt $n; $i++) {
        $el = if ($i -lt $e.Count) { $e[$i] } else { '<no line>' }
        $al = if ($i -lt $a.Count) { $a[$i] } else { '<no line>' }
        if ($el -ne $al) {
            Write-Host ("         first difference at line " + ($i + 1) + ":") -ForegroundColor Red
            Write-Host ("           expected: " + $el) -ForegroundColor DarkGray
            Write-Host ("           actual  : " + $al) -ForegroundColor DarkGray
            return
        }
    }
    # Same lines, so the difference was in the trailing whitespace
    # that Normalize deliberately keeps.
    Write-Host "         content matches line by line; check trailing whitespace" -ForegroundColor DarkGray
}

# ============================================================
#  Trace helpers
# ============================================================
#
# Finding a line by CONTENT, not by array index.
#
# This matters more than it looks. A trace is not one line per
# instruction: "-- TICK 60Hz --" separators are interleaved, so
# "the 15th instruction" and "line 15 of the output" are different
# things. Indexing by position gives a test that passes or fails
# depending on where the ticks happened to land -- which is how the
# first version of these invariants managed to be wrong about three
# instructions that were actually correct.
#
# Content matching has the other property you want: it still finds
# the line when the surrounding output shifts, so a failure means
# the ASSERTION is wrong, not the line numbering.

# Run `trace` on a ROM and return its stdout, normalized.
function Trace-Of([string]$Rom, [int]$Steps) {
    return Normalize (Invoke-Ember @('trace', $Rom, '--steps', $Steps.ToString())).Out
}

# The Occurrence-th (1-based) line containing Needle, or '' if absent.
#
# Uses .Contains(), NOT -like. This is not a style preference:
# PowerShell's -like treats [ ] as a character class, so the pattern
# '*LD V2, [I]*' silently means "LD V2, followed by the single
# character I" and never matches the actual text "LD V2, [I]".
# A search that can never match fails in a way that looks like the
# emulator is broken. .Contains() is an ordinal substring test with
# no wildcards, which is what "find this literal text" should be.
function Find-TraceLine([string]$Output, [string]$Needle, [int]$Occurrence = 1) {
    $seen = 0
    foreach ($line in ($Output -split "`n")) {
        if ($line.Contains($Needle)) {
            $seen++
            if ($seen -eq $Occurrence) { return $line }
        }
    }
    return ''
}

# The instruction numbers after which a TICK happened.
#
# A trace line begins with the step number, so remembering the last
# one seen and attaching it to the next TICK line gives the phase of
# the timer -- which is the thing worth asserting, since the average
# rate is right even when the phase is wrong.
function Get-TickPositions([string]$Output) {
    $positions = New-Object System.Collections.ArrayList
    $lastStep = 0
    foreach ($line in ($Output -split "`n")) {
        if ($line -like '*TICK*') {
            [void]$positions.Add($lastStep)
            continue
        }
        if ($line.Trim() -match '^(\d+)\s') {
            $lastStep = [int]$Matches[1]
        }
    }
    return $positions.ToArray()
}

# ============================================================
#  Golden-file cases
# ============================================================
#
# Each case runs one ember command and compares stdout against a
# golden file. stderr is ignored on purpose: it carries the human
# narration ("ran 400 instructions, PC=...") which must not be part
# of a byte-exact comparison. That split is enforced by main.cpp.

$romDigit8   = Join-Path $RunDir 'digit8.ch8'
$romDiagonal = Join-Path $RunDir 'diagonal.ch8'
$romFlags    = Join-Path $RunDir 'flags.ch8'
$romBcd      = Join-Path $RunDir 'bcd.ch8'
$romCollide  = Join-Path $RunDir 'collide.ch8'
$romClip     = Join-Path $RunDir 'clip.ch8'
$romWrapv    = Join-Path $RunDir 'wrapv.ch8'
$romCls      = Join-Path $RunDir 'cls.ch8'
$romTimer    = Join-Path $RunDir 'timer.ch8'
$romHalfTop  = Join-Path $RunDir 'halftop.ch8'
$romHalfBot  = Join-Path $RunDir 'halfbottom.ch8'
$romKeyEcho  = Join-Path $RunDir 'keyecho.ch8'

$GoldenCases = @(
    # ---- D: drawing ----
    @{ N='digit8-screen';   R=$romDigit8;   C='screen'; A=@('--cycles','5');  X='screen' }
    @{ N='diagonal-screen'; R=$romDiagonal; C='screen'; A=@('--cycles','20'); X='screen' }
    @{ N='clip-screen';     R=$romClip;     C='screen'; A=@('--cycles','4');  X='screen' }
    @{ N='collide-screen';  R=$romCollide;  C='screen'; A=@('--cycles','4');  X='screen' }
    @{ N='bcd-screen';      R=$romBcd;      C='screen'; A=@('--cycles','14'); X='screen' }
    # CLS is only proven by a PAIR: drawn at cycle 4, gone at cycle 5.
    # A blank screen on its own proves nothing -- blank is also the
    # initial state, so it would pass even if CLS were a no-op.
    @{ N='cls-before';      R=$romCls;      C='screen'; A=@('--cycles','4');  X='screen' }
    @{ N='cls-after';       R=$romCls;      C='screen'; A=@('--cycles','5');  X='screen' }

    # ---- C: instruction semantics, via trace ----
    @{ N='digit8-trace';    R=$romDigit8;   C='trace';  A=@('--steps','5');   X='trace' }
    @{ N='diagonal-trace';  R=$romDiagonal; C='trace';  A=@('--steps','20');  X='trace' }
    @{ N='collide-trace';   R=$romCollide;  C='trace';  A=@('--steps','5');   X='trace' }
    # flags is the mini 4-flags: every 8xy_ op with x = 0xF, which is
    # the one shape where "write Vx, then write VF" can be observed.
    @{ N='flags-trace';     R=$romFlags;    C='trace';  A=@('--steps','27');  X='trace' }

    # ---- E: timing ----
    # 600/60 is exactly 10:1, so the TICK line lands after instruction
    # 10, 20, 30, ... and the delay timer counts down by hand.
    @{ N='timer-ratio-10to1'; R=$romTimer;  C='trace';  A=@('--ips','600','--hz','60','--steps','40'); X='trace' }
    # The default 700/60 is 11.667:1 -- no exact boundary, so this case
    # is what catches fractional-credit drift.
    @{ N='timer-ratio-default'; R=$romTimer; C='trace'; A=@('--steps','30'); X='trace' }

    # ---- G: quirk switches, same ROM under two presets ----
    @{ N='wrapv-modern';      R=$romWrapv;  C='screen'; A=@('--cycles','4'); X='screen' }
    @{ N='wrapv-schip-legacy';R=$romWrapv;  C='screen'; A=@('--cycles','4','--quirks','schip-legacy'); X='screen' }
    # vip flips shiftVxInPlace and vfResetOnLogic, so the same flags
    # ROM must produce different VF values than flags-trace does.
    @{ N='flags-vip';         R=$romFlags;  C='trace';  A=@('--steps','27','--quirks','vip'); X='trace' }
)

function Run-GoldenCase($Case) {
    # NOT $args -- that is a PowerShell automatic variable, and
    # shadowing it inside a function works until the day it doesn't.
    $argv = @($Case.C, $Case.R) + $Case.A
    $result = Invoke-Ember $argv

    if ($result.Code -ne 0) {
        Report-Fail $Case.N ("exit code " + $result.Code + ", expected 0")
        return
    }

    $actual     = Normalize $result.Out
    $goldenPath = Join-Path $RunDir ($Case.N + '.' + $Case.X)

    if ($Update) {
        [System.IO.File]::WriteAllText($goldenPath, $actual + "`n",
            (New-Object System.Text.UTF8Encoding($false)))
        Report-Pass ($Case.N + '  (regenerated)')
        return
    }

    if (-not (Test-Path $goldenPath)) {
        Report-Fail $Case.N ("no golden file " + (Split-Path -Leaf $goldenPath) + " -- run with -Update to create it")
        return
    }

    $expected = Normalize ([System.IO.File]::ReadAllText($goldenPath, [System.Text.Encoding]::UTF8))
    if ($actual -eq $expected) {
        Report-Pass $Case.N
    } else {
        Report-Fail $Case.N "stdout does not match the golden file"
        Show-FirstDiff $expected $actual
    }
}

# ============================================================
#  CLI cases
# ============================================================
#
# Exit code plus ASCII substring assertions. The substrings are the
# parts of ember's messages that are ASCII (file names, flag names,
# hex values); the Chinese half of the message is deliberately not
# asserted on, because this file must stay pure ASCII.

$CliCases = @(
    @{ N='help';           A=@('--help');                  C=0; Out=@('ember screen','--quirks') }
    @{ N='no-args';        A=@();                          C=2; Out=@('ember trace') }
    # `run` needs the GUI build; the headless build must say so
    # clearly instead of pretending to work.
    @{ N='headless-run';   A=@('run', $romDigit8);         C=2; Err=@('screen') }

    @{ N='selftest';       A=@('selftest');                C=0 }
    @{ N='font-checksum';  A=@('selftest','--font');       C=0; Out=@('0x32D0','0xC43CE435','F0 90 90 90 F0') }
    @{ N='quirks-table';   A=@('selftest','--quirks-table'); C=0; Out=@('shiftVxInPlace','0x000','schip-legacy') }

    @{ N='bad-quirks';     A=@('selftest','--quirks','nope'); C=1; Err=@('nope','schip-legacy') }
    @{ N='bad-subcommand'; A=@('bogus');                    C=1; Err=@('bogus','selftest') }
    @{ N='unknown-flag';   A=@('screen', $romDigit8, '--nope'); C=1; Err=@('--nope') }
    @{ N='missing-rom';    A=@('screen', (Join-Path $RunDir 'no-such-file.ch8')); C=1; Err=@('no-such-file.ch8') }
    @{ N='bad-poke';       A=@('screen', $romDigit8, '--poke', '1FF'); C=1; Err=@('1FF') }
    # The message must echo what the user typed. It used to print the
    # address after two rounds of masking, so 2000=01 reported "0x000"
    # and the number that caused the error was nowhere in the output.
    @{ N='poke-address-out-of-range'; A=@('screen', $romDigit8, '--poke', '2000=01'); C=1; Err=@('2000') }
    @{ N='poke-value-out-of-range';   A=@('screen', $romDigit8, '--poke', '1FF=100'); C=1; Err=@('100') }
    @{ N='bad-cycles';     A=@('screen', $romDigit8, '--cycles', 'abc'); C=1; Err=@('abc') }
    @{ N='cycles-missing-value'; A=@('screen', $romDigit8, '--cycles'); C=1; Err=@('--cycles') }

    # Both flag spellings must work. --cycles=5 exists because scripts
    # read better with it, and a form that is accepted but broken is
    # worse than one that is rejected.
    @{ N='cycles-space-form'; A=@('screen', $romDigit8, '--cycles', '5');  C=0; Out=@('####') }
    @{ N='cycles-equals-form'; A=@('screen', $romDigit8, '--cycles=5');    C=0; Out=@('####') }

    # The disassembler's own output, checked for shape rather than as
    # a golden: it is also covered indirectly by every trace golden.
    @{ N='dis-bytes';      A=@('dis', $romDigit8, '--bytes', '4'); C=0; Out=@('0x200','0x6004','LD V0, 0x04') }
)

function Run-CliCase($Case) {
    $result = Invoke-Ember $Case.A

    if ($result.Code -ne $Case.C) {
        Report-Fail $Case.N ("exit code " + $result.Code + ", expected " + $Case.C)
        return
    }

    foreach ($needle in $Case.Out) {
        if (-not $result.Out.Contains($needle)) {
            Report-Fail $Case.N ("stdout does not contain: " + $needle)
            return
        }
    }
    foreach ($needle in $Case.Err) {
        if (-not $result.Err.Contains($needle)) {
            Report-Fail $Case.N ("stderr does not contain: " + $needle)
            return
        }
    }

    Report-Pass $Case.N
}

# ============================================================
#  Renderer checks (half-block terminal output)
# ============================================================
#
# The terminal player (`ember play`) needs a real console, a real
# keyboard and a real clock, so it cannot be automated. What CAN be
# automated is the part that turns the 64x32 framebuffer into text --
# and that part is where the interesting mistakes live.
#
# `screen --ansi` runs exactly the same renderHalfBlock() that `play`
# uses, so these assertions are about the real thing, not a copy.
#
# The mistake this is built to catch: the half-block renderer pairs
# pixel rows (0,1), (2,3), (4,5)... and puts the FIRST row of each
# pair in the foreground and the SECOND in the background. Get that
# pairing off by one, or swap foreground and background, and the
# picture still looks like "a shape" -- you would only notice by
# squinting at a game and wondering why everything is a row too high.
#
# So the two ROMs here draw ONE pixel, and differ by exactly one byte:
# halftop puts it at (0,0), halfbottom at (0,1). Same cell either way.
# The colour codes in front of the block character are what tells the
# two apart -- which is why this asserts on escapes and not on the
# stripped text (stripped, the two outputs are identical).

function Get-AnsiLines([string]$Rom, [string]$Cycles) {
    $out = Normalize (Invoke-Ember @('screen', $Rom, '--cycles', $Cycles, '--ansi')).Out
    return ,@($out -split "`n")
}

# Everything before the first half-block char on a line: the escape
# codes that were in effect when the first pixel got drawn.
function Get-PrefixBeforeBlock([string]$Line) {
    $idx = $Line.IndexOf([char]0x2580)
    if ($idx -lt 0) { return '' }
    return $Line.Substring(0, $idx)
}

function Run-RendererChecks {
    Write-Section "Renderer (half-block)"

    $esc = [char]27
    $stripAnsi = "$esc" + '\[[0-9;]*m'
    $block = [char]0x2580

    $top = Get-AnsiLines $romHalfTop '4'
    $bot = Get-AnsiLines $romHalfBot '4'

    # 1. 64x32 pixels become 16 text lines. 32 would mean the renderer
    #    is emitting one line per pixel row (which is what renderAscii
    #    does, and it does not fit in a 30-row console).
    if ($top.Count -eq 16) {
        Report-Pass "half-block output is 16 lines"
    } else {
        Report-Fail "half-block output is 16 lines" ("got " + $top.Count)
    }

    # 2. Every line is 64 cells wide, and every cell is either a block
    #    or a space. A 65-char line would mean an off-by-one in the
    #    column loop; any other character means an escape leaked.
    $badWidth = 0
    $badChar  = 0
    foreach ($l in $top) {
        $plain = $l -replace $stripAnsi, ''
        if ($plain.Length -ne 64) { $badWidth++ }
        foreach ($c in $plain.ToCharArray()) {
            if ($c -ne $block -and $c -ne ' ') { $badChar++ }
        }
    }
    if ($badWidth -eq 0) {
        Report-Pass "every half-block line is 64 cells"
    } else {
        Report-Fail "every half-block line is 64 cells" ($badWidth.ToString() + " line(s) had the wrong width")
    }
    if ($badChar -eq 0) {
        Report-Pass "half-block cells are only block or space"
    } else {
        Report-Fail "half-block cells are only block or space" ($badChar.ToString() + " stray char(s)")
    }

    # 3. One pixel in, one block out. More than one would mean the
    #    renderer drew a pixel on more than one line.
    $blocks = 0
    foreach ($l in $top) { $blocks += ($l -replace $stripAnsi, '').Split($block).Count - 1 }
    if ($blocks -eq 1) {
        Report-Pass "one lit pixel -> exactly one block"
    } else {
        Report-Fail "one lit pixel -> exactly one block" ("got " + $blocks)
    }

    # 4. THE ONE THAT MATTERS. Pixel at (0,0) must land in the TOP half
    #    of the cell: foreground bright white, background black.
    #    97 = bright white foreground, 40 = black background,
    #    30 = black foreground, 107 = bright white background.
    $fgOn  = $esc + '[97m'
    $bgOn  = $esc + '[107m'
    $fgOff = $esc + '[30m'
    $bgOff = $esc + '[40m'

    $prefixTop = Get-PrefixBeforeBlock $top[0]
    if ($prefixTop.Contains($fgOn) -and $prefixTop.Contains($bgOff)) {
        Report-Pass "pixel at (0,0) is drawn in the cell's TOP half (fg on, bg off)"
    } else {
        Report-Fail "pixel at (0,0) is drawn in the cell's TOP half (fg on, bg off)" ("prefix was " + ($prefixTop -replace $esc, '<ESC>'))
    }

    # 5. And pixel at (0,1) must land in the BOTTOM half of the SAME
    #    cell (line 1 of the output, not line 2).
    $prefixBot = Get-PrefixBeforeBlock $bot[0]
    if ($prefixBot.Contains($fgOff) -and $prefixBot.Contains($bgOn)) {
        Report-Pass "pixel at (0,1) is drawn in the cell's BOTTOM half (fg off, bg on)"
    } else {
        Report-Fail "pixel at (0,1) is drawn in the cell's BOTTOM half (fg off, bg on)" ("prefix was " + ($prefixBot -replace $esc, '<ESC>'))
    }

    # 6. Both pixels are in row 0 of the output. If a renderer paired
    #    rows as (1,2), (3,4)... the second pixel would appear on
    #    line 2 instead. This is the off-by-one guard.
    $botBlockLine = -1
    for ($i = 0; $i -lt $bot.Count; $i++) {
        if (($bot[$i] -replace $stripAnsi, '').Contains($block)) { $botBlockLine = $i; break }
    }
    if ($botBlockLine -eq 0) {
        Report-Pass "pixel at (0,1) still lands on output line 1"
    } else {
        Report-Fail "pixel at (0,1) still lands on output line 1" ("landed on line " + ($botBlockLine + 1))
    }

    # 7. The two renders must actually differ. Stripped they are
    #    identical, so if the whole outputs matched, the renderer is
    #    ignoring the vertical position entirely.
    if (($top -join "`n") -ne ($bot -join "`n")) {
        Report-Pass "top-half and bottom-half pixels render differently"
    } else {
        Report-Fail "top-half and bottom-half pixels render differently" "both produced identical bytes"
    }

    # 8. Every line ends with a reset. Without it the last cell's
    #    colours bleed past the newline and paint the start of the
    #    next line -- which reads as "there is a stray column on the
    #    left of the picture".
    $noReset = 0
    foreach ($l in $top) { if (-not $l.EndsWith($esc + '[0m')) { $noReset++ } }
    if ($noReset -eq 0) {
        Report-Pass "every half-block line ends with a colour reset"
    } else {
        Report-Fail "every half-block line ends with a colour reset" ($noReset.ToString() + " line(s) did not")
    }

    # 9. A blank screen is all spaces: no block characters at all.
    #    This catches a renderer that draws the "both pixels off" case
    #    as a block instead of a space.
    $blank = Get-AnsiLines $romHalfTop '0'
    $blankBlocks = 0
    foreach ($l in $blank) { $blankBlocks += ($l -replace $stripAnsi, '').Split($block).Count - 1 }
    if ($blankBlocks -eq 0) {
        Report-Pass "blank display renders no blocks at all"
    } else {
        Report-Fail "blank display renders no blocks at all" ("got " + $blankBlocks)
    }

    # 10. Fx0A (wait for key) with no key held must NOT wedge the
    #     emulator: it burns cycles and returns, so the run still
    #     terminates. keyecho waits for a key as its very first act,
    #     so if the blocking path could hang, this command would
    #     never return and the whole suite would stall here.
    $r = Invoke-Ember @('screen', $romKeyEcho, '--cycles', '200')
    if ($r.Code -eq 0) {
        Report-Pass "Fx0A with no key held still terminates (200 cycles)"
    } else {
        Report-Fail "Fx0A with no key held still terminates (200 cycles)" ("exit " + $r.Code)
    }

    # 11. `play` must REFUSE, not hang, when there is no console.
    #
    #     Under this harness stdout is always a pipe, so play can never
    #     start here -- which makes this the one play behaviour that
    #     IS automatable, and it is the important one: getting it wrong
    #     means taking over the screen and then waiting forever for a
    #     keypress that can never arrive. That failure would look like
    #     a hung test suite, not a failed test.
    #
    #     The match is on 'ember screen' (the ASCII hint line at the
    #     end of the message) rather than the Chinese text, because
    #     this script has to stay pure ASCII -- see the file header.
    $r = Invoke-Ember @('play', $romKeyEcho)
    if ($r.Code -eq 2 -and $r.Out -eq '' -and $r.Err.Contains('ember screen')) {
        Report-Pass "play refuses (exit 2) instead of hanging when stdout is not a console"
    } else {
        Report-Fail "play refuses (exit 2) instead of hanging when stdout is not a console" `
                    ("exit " + $r.Code + ", stderr starts: " + ($r.Err -split "`n")[0])
    }
}

# ============================================================
#  Structure checks
# ============================================================
#
# Things that are properties of the output's SHAPE, not of its
# content. These are the ones that catch a whole class of
# "the golden was regenerated and nobody noticed" accidents.

function Run-StructureChecks {
    Write-Section "Structure"

    # 1. The screen dump is exactly 32 lines of exactly 64 characters.
    #    A resized display would keep every golden "consistent" if the
    #    goldens were regenerated, but this catches it independently.
    $r = Invoke-Ember @('screen', $romDiagonal, '--cycles', '20')
    $lines = (Normalize $r.Out) -split "`n"
    if ($lines.Count -eq 32) {
        Report-Pass "screen dump is 32 lines"
    } else {
        Report-Fail "screen dump is 32 lines" ("got " + $lines.Count)
    }
    $badWidth = 0
    foreach ($l in $lines) { if ($l.Length -ne 64) { $badWidth++ } }
    if ($badWidth -eq 0) {
        Report-Pass "every screen line is 64 chars"
    } else {
        Report-Fail "every screen line is 64 chars" ($badWidth.ToString() + " line(s) had the wrong width")
    }

    # 2. stdout carries data only. If a stray banner ever leaks into
    #    stdout, every golden becomes useless at once -- so assert the
    #    screen dump contains nothing but '#' and '.'.
    $onlyArt = $true
    foreach ($l in $lines) {
        if ($l -notmatch '^[#.]*$') { $onlyArt = $false }
    }
    if ($onlyArt) {
        Report-Pass "screen stdout is only '#' and '.'"
    } else {
        Report-Fail "screen stdout is only '#' and '.'" "something non-ASCII-art leaked into stdout"
    }

    # 3. selftest must actually be running checks. Without this, a
    #    refactor that accidentally makes checkRunAll() return early
    #    would leave every test green while nothing was verified.
    $r = Invoke-Ember @('selftest')
    $count = ((Normalize $r.Out) -split "`n").Count
    if ($r.Code -eq 0 -and $count -ge 50) {
        Report-Pass ("selftest reports " + $count + " lines and exits 0")
    } else {
        Report-Fail "selftest reports >= 50 lines and exits 0" ("exit " + $r.Code + ", " + $count + " lines")
    }

    # 4. The two screen dumps that must differ must actually differ.
    #    If they ever became equal, the quirk switch under test is
    #    doing nothing -- and both goldens would still "pass".
    $a = Normalize (Invoke-Ember @('screen', $romWrapv, '--cycles', '4')).Out
    $b = Normalize (Invoke-Ember @('screen', $romWrapv, '--cycles', '4', '--quirks', 'schip-legacy')).Out
    if ($a -ne $b) {
        Report-Pass "spriteVerticalWrap actually changes the picture"
    } else {
        Report-Fail "spriteVerticalWrap actually changes the picture" "modern and schip-legacy produced identical screens"
    }

    # 5. Same for the vip preset on the flags ROM.
    $a = Normalize (Invoke-Ember @('trace', $romFlags, '--steps', '27')).Out
    $b = Normalize (Invoke-Ember @('trace', $romFlags, '--steps', '27', '--quirks', 'vip')).Out
    if ($a -ne $b) {
        Report-Pass "vip preset actually changes shift/logic behaviour"
    } else {
        Report-Fail "vip preset actually changes shift/logic behaviour" "modern and vip produced identical traces"
    }
}

# ============================================================
#  Invariants -- hand-written, NEVER regenerated
# ============================================================
#
# These are the closest thing this harness has to a correctness
# oracle short of the third-party ROMs. They are written out by
# hand from the CHIP-8 specification and -Update must never touch
# them. If one goes red, the emulator is wrong -- not the test.

function Run-Invariants {
    Write-Section "Invariants (hand-written, never regenerated)"

    # 8xy4 with x == 0xF: the addend 0xFF + 0x02 overflows, so VF must
    # end up 1. If the implementation writes VF before Vx, VF would be
    # left holding the truncated sum 0x01 instead.
    $line = Find-TraceLine (Trace-Of $romFlags 3) 'ADD VF, V1'
    if ($line -like '*VF=01*') {
        Report-Pass "8xy4 x=0xF -> VF holds the carry, not the sum"
    } else {
        Report-Fail "8xy4 x=0xF -> VF holds the carry, not the sum" ("got: " + $line)
    }

    # 8xy5 with 5-3: no borrow, so VF = 1. The direction is the
    # classic mix-up (1 means NO borrow, not borrow).
    $line = (Find-TraceLine (Trace-Of $romFlags 9) 'SUB VF, V1' 1)
    if ($line -like '*VF=01*') {
        Report-Pass "8xy5 5-3 -> VF=1 (no borrow)"
    } else {
        Report-Fail "8xy5 5-3 -> VF=1 (no borrow)" ("got: " + $line)
    }

    # 8xy5 with 3-5: borrow, so VF = 0.
    $line = (Find-TraceLine (Trace-Of $romFlags 9) 'SUB VF, V1' 2)
    if ($line -like '*VF=00*') {
        Report-Pass "8xy5 3-5 -> VF=0 (borrow)"
    } else {
        Report-Fail "8xy5 3-5 -> VF=0 (borrow)" ("got: " + $line)
    }

    # 8xy6 shifts the LSB out into VF: 0x05 >> 1 -> VF = 1.
    $line = Find-TraceLine (Trace-Of $romFlags 16) 'SHR VF'
    if ($line -like '*VF=01*') {
        Report-Pass "8xy6 0x05 >> 1 -> VF=1 (LSB out)"
    } else {
        Report-Fail "8xy6 0x05 >> 1 -> VF=1 (LSB out)" ("got: " + $line)
    }

    # 8xyE shifts the MSB out into VF: 0x81 << 1 -> VF = 1.
    $line = Find-TraceLine (Trace-Of $romFlags 19) 'SHL VF'
    if ($line -like '*VF=01*') {
        Report-Pass "8xyE 0x81 << 1 -> VF=1 (MSB out)"
    } else {
        Report-Fail "8xyE 0x81 << 1 -> VF=1 (MSB out)" ("got: " + $line)
    }

    # Fx29 must compute fontBase + 5 * Vx -- the 5 is the glyph size and
    # is the thing a wrong implementation gets wrong. At the modern
    # 0x050 base: Vx=2 -> 0x05A, Vx=5 -> 0x069.
    #
    # Note which LINES these land on. Steps 9 and 12 are both Fx29 with
    # value 5 (V1 and V2 are both 5 after Fx33), so step 12 sets I to
    # the value it already had and the tracer prints NO delta for it.
    # That is the tracer being right -- it reports changes, not
    # assignments -- but it means "the 12th instruction's line" is the
    # wrong place to look for the 12th instruction's result.
    $line = Find-TraceLine (Trace-Of $romBcd 12) 'LD F, V0'
    if ($line.Contains('I=05A')) {
        Report-Pass "Fx29 with V0=2 -> I = 0x050 + 5*2 = 0x05A"
    } else {
        Report-Fail "Fx29 with V0=2 -> I = 0x050 + 5*2 = 0x05A" ("got: " + $line)
    }
    $line = Find-TraceLine (Trace-Of $romBcd 12) 'LD F, V1'
    if ($line.Contains('I=069')) {
        Report-Pass "Fx29 with V1=5 -> I = 0x050 + 5*5 = 0x069"
    } else {
        Report-Fail "Fx29 with V1=5 -> I = 0x050 + 5*5 = 0x069" ("got: " + $line)
    }

    # Fx33 splits 255 into 2, 5, 5 -- readable straight off the trace,
    # hand-derived from the spec (hundreds / tens / units).
    $line = Find-TraceLine (Trace-Of $romBcd 12) 'LD V2, [I]'
    if ($line.Contains('V0=02') -and $line.Contains('V1=05') -and $line.Contains('V2=05')) {
        Report-Pass "Fx33 turns 255 into memory 2,5,5"
    } else {
        Report-Fail "Fx33 turns 255 into memory 2,5,5" ("got: " + $line)
    }

    # ...and the same three digits must come out ON SCREEN as "255",
    # which is what proves the whole chain (Fx33 -> Fx65 -> Fx29 -> Dxyn)
    # hangs together.
    #
    # The expected rows are the glyph shapes from the font table (which
    # is separately pinned by the --font checksum) placed at the x
    # coordinates the ROM chose. A wrong digit here is a wrong digit,
    # not a rendering difference -- 2 and 5 differ in row 1 and row 3.
    $lines = (Normalize (Invoke-Ember @('screen', $romBcd, '--cycles', '14')).Out) -split "`n"
    $expected = @(
        '####....####....####',
        '...#....#.......#...',
        '####....####....####',
        '#..........#.......#',
        '####....####....####'
    )
    $ok = $true
    $badRow = -1
    for ($i = 0; $i -lt 5; $i++) {
        if ($lines[$i].Substring(0, 20) -ne $expected[$i]) { $ok = $false; $badRow = $i }
    }
    if ($ok) {
        Report-Pass "the screen reads 255 (all three digits)"
    } else {
        Report-Fail "the screen reads 255 (all three digits)" ("row " + ($badRow + 1) + " is wrong")
        Write-Host ("           actual  : " + $lines[$badRow].Substring(0, 20)) -ForegroundColor DarkGray
        Write-Host ("           expected: " + $expected[$badRow]) -ForegroundColor DarkGray
    }

    # Dxyn collision: drawing the same sprite twice must blank it and
    # set VF. This is the definition of collision -- "a pixel went from
    # on to off" -- not "the sprites overlapped".
    #
    # Occurrence 2: the FIRST Dxyn just draws, and VF stays 0. Only the
    # second one, landing on pixels that are already lit, sets VF.
    # Asserting on occurrence 1 would pass on an emulator that always
    # set VF -- which is the bug this is here to catch.
    $line = Find-TraceLine (Trace-Of $romCollide 5) 'DRW V0, V1, 5' 2
    if ($line.Contains('VF=01')) {
        Report-Pass "Dxyn redraw on the same pixels -> VF=1"
    } else {
        Report-Fail "Dxyn redraw on the same pixels -> VF=1" ("got: " + $line)
    }

    # ---- timing ----
    #
    # These pin the PHASE of the timer, not just its rate. An
    # implementation that accumulates hz/ips as a double has the right
    # average rate but lands one instruction late forever -- which is
    # exactly what this project shipped until these numbers were
    # measured and the accumulator made integral.

    # 600/60 is exactly 10:1, so the ticks are at 10, 20, 30, 40.
    $r = Invoke-Ember @('trace', $romTimer, '--ips', '600', '--hz', '60', '--steps', '45')
    $pos = @(Get-TickPositions (Normalize $r.Out))
    if ($pos.Count -eq 4 -and $pos[0] -eq 10 -and $pos[1] -eq 20 -and $pos[2] -eq 30 -and $pos[3] -eq 40) {
        Report-Pass "600/60 ticks after exactly instructions 10,20,30,40"
    } else {
        Report-Fail "600/60 ticks after exactly instructions 10,20,30,40" ("got: " + ($pos -join ', '))
    }

    # The delay timer starts at 10 and loses 1 per tick, so after the
    # four ticks above it must read 6 -- and must never wrap to 255.
    if ($r.Err -like '*DT=0x06*') {
        Report-Pass "delay timer 10 minus 4 ticks = 6 (no underflow)"
    } else {
        $foot = (Normalize $r.Err) -split "`n"
        Report-Fail "delay timer 10 minus 4 ticks = 6 (no underflow)" ("footer was: " + $foot[$foot.Count - 1])
    }

    # The default 700/60 is 35/3 instructions per tick, so tick k lands
    # after instruction ceil(35k/3): 12, 24, 35, 47, 59, 70, 82, 94.
    # Round numbers nowhere -- which is why this one catches drift that
    # 600/60 cannot. The pattern is +12,+11,+12,+12,+11,+12,+12 --
    # three ticks per 35 instructions, exactly.
    $r = Invoke-Ember @('trace', $romTimer, '--steps', '100')
    $pos = @(Get-TickPositions (Normalize $r.Out))
    $want = @(12, 24, 35, 47, 59, 70, 82, 94)
    if (($pos -join ',') -eq ($want -join ',')) {
        Report-Pass "700/60 ticks at 12,24,35,47,59,70,82,94 (no fractional drift)"
    } else {
        Report-Fail "700/60 ticks at 12,24,35,47,59,70,82,94 (no fractional drift)" ("got: " + ($pos -join ', '))
    }
}

# ============================================================
#  Third-party ROMs
# ============================================================
#
# Timendus/chip8-test-suite is GPLv3, so the ROMs are NOT committed:
# tests\external\ is gitignored and the README there records where to
# download them. Keeping the capability on this machine without
# making a licence decision the project never intended to make.

function Run-ExternalCases {
    Write-Section "Third-party ROMs (skipped when absent)"

    $external = @(
        @{ N='3-corax+';   File='3-corax+.ch8';   Cycles='5000' }
        @{ N='4-flags';    File='4-flags.ch8';    Cycles='5000' }
        @{ N='1-chip8-logo'; File='1-chip8-logo.ch8'; Cycles='3000' }
        @{ N='2-ibm-logo'; File='2-ibm-logo.ch8'; Cycles='3000' }
    )

    foreach ($case in $external) {
        $rom = Join-Path $ExtDir $case.File
        if (-not (Test-Path $rom)) {
            Report-Skip $case.N "file not downloaded"
            continue
        }
        $result = Invoke-Ember @('screen', $rom, '--cycles', $case.Cycles)
        if ($result.Code -ne 0) {
            Report-Fail $case.N ("exit code " + $result.Code)
            continue
        }
        # These are visual oracles: a human reads the screen. The
        # harness only asserts they ran and drew something -- claiming
        # to judge a cross from a tick automatically would be a lie.
        $art = Normalize $result.Out
        if ($art -like '*#*') {
            Report-Pass ($case.N + " ran and drew something (read the screen yourself)")
        } else {
            Report-Fail $case.N "the screen is completely blank"
        }
    }
}

# ============================================================
#  Main
# ============================================================

Write-Host ""
Write-Host "Ember tests" -ForegroundColor White
if ($Update) {
    Write-Host "  *** -Update: regenerating every golden from CURRENT behaviour ***" -ForegroundColor Yellow
    Write-Host "  *** that includes any bugs. Only do this on purpose.        ***" -ForegroundColor Yellow
}

Write-Section "Golden files"
foreach ($case in $GoldenCases) { Run-GoldenCase $case }

Write-Section "Command line"
foreach ($case in $CliCases) { Run-CliCase $case }

Run-StructureChecks
Run-RendererChecks
Run-Invariants
Run-ExternalCases

Write-Host ""
$total = $script:Pass + $script:Fail + $script:Skip
Write-Host ("Total " + $total + ": " + $script:Pass + " pass, " + $script:Fail + " fail, " + $script:Skip + " skip") -ForegroundColor White

if ($script:Fail -gt 0) {
    Write-Host ""
    Write-Host "Failed:" -ForegroundColor Red
    foreach ($name in $script:Failed) { Write-Host ("  - " + $name) -ForegroundColor Red }
    exit 1
}

exit 0
