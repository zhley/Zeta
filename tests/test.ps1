# Zeta test runner (PowerShell; mirrors tests/test.sh).
# See tests/README.md for the conventions.
#
# Usage:
#   powershell -File tests/test.ps1           run all tests and check pass/fail
#   powershell -File tests/test.ps1 v         also print each test's raw output
#   powershell -File tests/test.ps1 clear     delete every *.ztc / *.dump under tests/

param([string]$action = "")

$exe = ".\build\bin\zeta.exe"

if ($action -eq "clear") {
    Get-ChildItem -Path "tests" -Recurse -Include "*.ztc", "*.dump" | Remove-Item
    Write-Host "cleared all *.ztc and *.dump files under tests/"
    exit 0
}

$verbose = ($action -eq "v")
$pass = 0
$fail = 0

function Run-Test {
    param([string]$path)
    if ($path -eq "tests/builtin/01_print.zt") {
        # exact output check against the expected file
        $output = & $exe $path 2>&1 | Out-String
        $expected = Get-Content "tests/builtin/01_print.expected" | Out-String
        if ($verbose) {
            Write-Host "=== $path ==="
            Write-Host $output
        }
        if ($output -eq $expected) {
            Write-Host "PASS  $path" -ForegroundColor Green
            $script:pass++
        } else {
            Write-Host "FAIL  $path (output differs from 01_print.expected)" -ForegroundColor Red
            Compare-Object ($output -split "`n") ($expected -split "`n") |
                Select-Object -First 8 |
                ForEach-Object { Write-Host $_.ToString() }
            $script:fail++
        }
        return
    }
    if ($path -eq "tests/builtin/07_input.zt") {
        # feed one line of stdin (see tests/README.md)
        $output = "hello`n" | & $exe $path 2>&1 | Out-String
    } else {
        $output = & $exe $path 2>&1 | Out-String
    }
    if ($verbose) {
        Write-Host "=== $path ==="
        Write-Host $output
    }
    if ($output -match "all passed") {
        Write-Host "PASS  $path" -ForegroundColor Green
        $script:pass++
    } else {
        Write-Host "FAIL  $path" -ForegroundColor Red
        ($output -split "`n") | Select-Object -Last 5
        $script:fail++
    }
}

foreach ($dir in @("tests/lex", "tests/expr", "tests/stmt", "tests/func", "tests/class", "tests/builtin", "tests/integration")) {
    Get-ChildItem -Path $dir -Filter "*.zt" | Sort-Object Name | ForEach-Object {
        Run-Test $_.FullName
    }
}

Run-Test "tests/module/mod_main.zt"

Write-Host ""
if ($fail -gt 0) {
    Write-Host "zeta tests: $pass passed, $fail failed" -ForegroundColor Red
} else {
    Write-Host "zeta tests: $pass passed, $fail failed" -ForegroundColor Green
}

Write-Host ""
Write-Host "=== tests/gc (run_gc.ps1) ==="
& ".\tests\gc\run_gc.ps1"
$gcOk = $LASTEXITCODE
if ($fail -gt 0 -or $gcOk -ne 0) { exit 1 }
