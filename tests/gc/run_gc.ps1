# GC black-box test runner (PowerShell; mirrors tests/gc/run_gc.sh).
# See tests/README.md, section "GC 测试", for the // ARGS: and // EXPECT:
# header conventions.

$exe = ".\build\bin\zeta.exe"
$pass = 0
$fail = 0

foreach ($f in Get-ChildItem -Path "tests/gc" -Filter "[0-9]*.zt" | Sort-Object Name) {
    $path = $f.FullName
    $header = Get-Content $path -TotalCount 5
    $argsLine = $header | Where-Object { $_ -match '^// ARGS:' } | Select-Object -First 1
    $expectLine = $header | Where-Object { $_ -match '^// EXPECT:' } | Select-Object -First 1

    $argsets = @()
    if ($argsLine -match '^// ARGS:\s*(.*)$') {
        $argsets = $Matches[1].Split(';;')
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
            $output = & $exe $s $path 2>&1 | Out-String
        }
        $code = $LASTEXITCODE
        $n = $n + 1

        $ok = $false
        switch ($expect) {
            "all_passed" {
                if ($code -eq 0 -and $output -match "all passed") { $ok = $true }
            }
            "out_of_memory" {
                if ($code -eq 0 -and $output -match "Out of memory") { $ok = $true }
            }
        }
        if (-not $ok) {
            Write-Host "FAIL  $path (args: [$s], exit=$code)"
            $allOk = $false
        }
        if ($null -eq $firstOut) {
            $firstOut = $output
        } elseif ($firstOut -ne $output) {
            Write-Host "FAIL  $path (output differs between configurations, args: [$s])"
            $allOk = $false
        }
    }
    if ($allOk) {
        Write-Host "PASS  $path ($n runs)"
        $pass = $pass + 1
    } else {
        $fail = $fail + 1
    }
}

Write-Host ""
Write-Host "gc tests: $pass passed, $fail failed"
if ($fail -gt 0) { exit 1 }
