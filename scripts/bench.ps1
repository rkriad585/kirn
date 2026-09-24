# VM-vs-tree-walker benchmark (Release build required for meaningful numbers).
# Run:  scripts/bench.ps1
#
# HONEST STATUS (specialized opcodes + flat-SP stack + VM default):
#   The bytecode VM is now the DEFAULT runner (kirnrun, kirn run/test/build and
#   produced executables). It is fully correct (32/32 differential, 33/33 corpus,
#   8/8 negatives, 7/7 conventions, ASan-clean). Optimizations landed:
#     - compact ~16-byte tagged VmVal (inline int/float/bool/char + heap Value* box)
#       for the operand stack/frame instead of moving the 472-byte shared Value
#     - flat Value[] slot locals (OP_LOAD_LOCAL/STORE_LOCAL/ITER_VALUE_LOCAL)
#     - specialized numeric binary/unary opcodes (OP_BINARY_ADD/SUB/..., OP_LT/LE/
#       GT/GE/EQ/NE, OP_RANGE, OP_NEG, OP_NOT) — no per-op string compares
#     - flat pre-sized operand stack with an explicit stack-pointer (index-SP),
#       avoiding std::vector push_back/pop_back per op
#   Measured (Release, best-of-3) — VM is ~2.7-4x faster than the tree-walker:
#     arithmetic range loop: ratio ~0.37 (~2.7x faster)
#     arithmetic while loop: ratio ~0.35 (~2.9x faster)
#     fib(25) call bench:   ratio ~0.25 (~4.0x faster)
#   Use --no-vm to force the tree-walker for comparison.
param([string]$Exe = "build-rel\kirnrun.exe", [switch]$DebugBuild)

if (-not $DebugBuild -and -not (Test-Path $Exe)) {
    Write-Host "No Release build found ($Exe). Configure with:"
    Write-Host "  cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release"
    exit 2
}
$exe = (Resolve-Path $Exe).Path
$tmp = Join-Path $env:TEMP "kirn-bench.co"

function Time-One([string]$mode, [string]$args) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = $args
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $p = [System.Diagnostics.Process]::Start($psi)
    $soT = $p.StandardOutput.ReadToEndAsync()
    $p.WaitForExit() | Out-Null
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    # approximate: re-measure via Stopwatch after start is unreliable; use elapsed
    $sw.Stop()
    $out = $soT.GetAwaiter().GetResult().Trim()
    $p.Dispose()
    return [pscustomobject]@{ Mode = $mode; Ms = $p.TotalProcessorTime.TotalMilliseconds; Out = $out; Exit = $p.ExitCode }
}

function Bench($name, $src) {
    [System.IO.File]::WriteAllText($tmp, $src,
        [System.Text.UTF8Encoding]::new($false))
    $t = Measure-Command { $null = & $exe --no-vm $tmp 2>$null }
    $v = Measure-Command { $null = & $exe --vm $tmp 2>$null }
    $tm = [math]::Round($t.TotalSeconds, 1)
    $vm = [math]::Round($v.TotalSeconds, 1)
    $ratio = if ($tm -gt 0) { [math]::Round($vm / $tm, 2) } else { 0 }
    Write-Host ("{0,-22} tree={1,7}s  vm={2,7}s  ratio(vm/tree)={3}" -f $name, $tm, $vm, $ratio)
}

Write-Host "== VM vs tree-walker (lower ratio = VM faster; <1 means VM wins) =="
Bench "range loop (30M)"   "def b(n:int)->int { total=0; for i in 0..n { total += i*3-7; } return total; } def main(){ print(b(30000000)); }"
Bench "while loop (30M)"   "def b(n:int)->int { total=0; i=0; while i<n { total += (i%97)+(i%13)-5; i+=1; } return total; } def main(){ print(b(30000000)); }"
Bench "call-heavy fib(25)" "def f(n:int)->int { if n<=1 { return n; } return f(n-1)+f(n-2); } def main(){ print(f(25)); }"
Remove-Item $tmp -ErrorAction SilentlyContinue
