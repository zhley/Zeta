# GC black-box test runner (PowerShell; mirrors tests/gc/run_gc.sh).
# See tests/README.md, section "GC 测试", for the // ARGS: and // EXPECT:
# header conventions.
#
# // EXPECT: all_passed | out_of_memory       default: all_passed
#     all_passed    - exit code 0 and output must contain "all passed".
#                     (Assert failures and runtime errors still exit 0,
#                     so the output check is what actually matters.)
#     out_of_memory - exit code 2 and output must contain "Heap limit
#                     exceeded": when the heap hits the -m limit the VM
#                     throws VMException, main() prints the message and
#                     returns 2 (see tests/README.md).

$exe = ".\build\bin\zeta.exe"
$pass = 0
$fail = 0

foreach ($f in Get-ChildItem -Path "tests/gc" -Filter "*.zt" |
    Where-Object { $_.Name -match '^[0-9]' } | Sort-Object Name) {
    $path = $f.FullName
    $header = Get-Content $path
    $argsLine = $header | Where-Object { $_ -match '^// ARGS:' } | Select-Object -First 1
    $expectLine = $header | Where-Object { $_ -match '^// EXPECT:' } | Select-Object -First 1

    $argsets = @()
    if ($argsLine -match '^// ARGS:\s*(.*)$') {
        $argsets = $Matches[1] -split ';;'
    }
    $expect = "all_passed"
    if ($expectLine -match '^// EXPECT:\s*(\S+)') {
        $expect = $Matches[1]
    }

    if ($argsets.Count -eq 0 -or ($argsets.Count -eq 1 -and $argsets[0].Trim() -eq "")) {
        $argsets = @("")
    }

    $allOk = $true
    $firstOut = $null
    $n = 0
    foreach ($s in $argsets) {
        $s = $s.Trim()
        if ($s -eq "") {
            $output = & $exe $path 2>&1 | Out-String
        } else {
            $vmArgs = $s -split '\s+'
            $output = & $exe @vmArgs $path 2>&1 | Out-String
        }
        $code = $LASTEXITCODE
        $n = $n + 1

        $ok = $false
        switch ($expect) {
            "all_passed" {
                if ($code -eq 0 -and $output -match "all passed") { $ok = $true }
            }
            "out_of_memory" {
                if ($code -eq 2 -and $output -match "Heap limit exceeded") { $ok = $true }
            }
        }
        if (-not $ok) {
            Write-Host "FAIL" -ForegroundColor Red -NoNewline
            Write-Host "  $path (args: [$s], exit=$code)"
            $allOk = $false
        }
        if ($null -eq $firstOut) {
            $firstOut = $output
        } elseif ($firstOut -ne $output) {
            Write-Host "FAIL" -ForegroundColor Red -NoNewline
            Write-Host "  $path (output differs between configurations, args: [$s])"
            $allOk = $false
        }
    }
    if ($allOk) {
        Write-Host "PASS" -ForegroundColor Green -NoNewline
        Write-Host "  $path ($n runs)"
        $pass = $pass + 1
    } else {
        $fail = $fail + 1
    }
}

Write-Host ""
Write-Host "gc tests: $pass " -NoNewline
Write-Host "passed" -ForegroundColor Green -NoNewline
Write-Host ", $fail " -NoNewline
Write-Host "failed" -ForegroundColor Red
if ($fail -gt 0) { exit 1 }
