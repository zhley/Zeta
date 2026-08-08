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

# The loop below passes full paths (FileInfo.FullName), so compare against
# resolved full paths instead of bare relative strings like
# "tests/builtin/07_input.zt" (which never match and cause 07_input.zt to run
# without piped stdin, hanging on input() in an interactive console).
$printTest = (Resolve-Path "tests/builtin/01_print.zt").Path
$inputTest = (Resolve-Path "tests/builtin/07_input.zt").Path
$printExpected = (Resolve-Path "tests/builtin/01_print.expected").Path

function Run-Test {
    param([string]$path)
    if ($path -eq $printTest) {
        # exact output check against the expected file
        $output = & $exe $path 2>&1 | Out-String
        $code = $LASTEXITCODE
        $expected = Get-Content $printExpected | Out-String
        if ($verbose) {
            Write-Host "=== $path ==="
            Write-Host $output
        }
        if ($code -eq 0 -and $output -eq $expected) {
            Write-Host "PASS" -ForegroundColor Green -NoNewline
            Write-Host "  $path"
            $script:pass++
        } else {
            Write-Host "FAIL" -ForegroundColor Red -NoNewline
            Write-Host "  $path (exit=$code, output differs from 01_print.expected)"
            Compare-Object ($output -split "`n") ($expected -split "`n") |
                Select-Object -First 8 |
                ForEach-Object { Write-Host $_.ToString() }
            $script:fail++
        }
        return
    }
    if ($path -eq $inputTest) {
        # feed one line of stdin (see tests/README.md)
        $output = "hello`n" | & $exe $path 2>&1 | Out-String
    } else {
        $output = & $exe $path 2>&1 | Out-String
    }
    $code = $LASTEXITCODE
    if ($verbose) {
        Write-Host "=== $path ==="
        Write-Host $output
    }
    if ($code -eq 0 -and $output -match "all passed") {
        Write-Host "PASS" -ForegroundColor Green -NoNewline
        Write-Host "  $path"
        $script:pass++
    } else {
        Write-Host "FAIL" -ForegroundColor Red -NoNewline
        Write-Host "  $path (exit=$code)"
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
Write-Host "zeta tests: $pass " -NoNewline
Write-Host "passed" -ForegroundColor Green -NoNewline
Write-Host ", $fail " -NoNewline
Write-Host "failed" -ForegroundColor Red

Write-Host ""
Write-Host "=== tests/gc (run_gc.ps1) ==="
& powershell -NoProfile -ExecutionPolicy Bypass -File ".\tests\gc\run_gc.ps1"
$gcOk = $LASTEXITCODE
if ($fail -gt 0 -or $gcOk -ne 0) { exit 1 }
