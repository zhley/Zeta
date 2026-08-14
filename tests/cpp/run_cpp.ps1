# C++ interop test runner (PowerShell; mirrors tests/cpp/run_cpp.sh).
# See tests/README.md, section "C++ 互操作测试".
#
# Runs the cpp_interop_test host program (built via `cmake --build build`)
# and checks that it prints "all passed" and exits 0.

$exe = ".\build\bin\cpp_interop_test.exe"

if (-not (Test-Path $exe)) {
    Write-Host "FAIL" -ForegroundColor Red -NoNewline
    Write-Host "  $exe not found. Build it first: cmake --build build"
    exit 1
}

$output = & $exe 2>&1 | Out-String
$code = $LASTEXITCODE

if ($code -eq 0 -and $output -match "all passed") {
    Write-Host "PASS" -ForegroundColor Green -NoNewline
    Write-Host "  cpp_interop_test"
    exit 0
} else {
    Write-Host "FAIL" -ForegroundColor Red -NoNewline
    Write-Host "  cpp_interop_test (exit=$code)"
    ($output -split "`n") | Select-Object -Last 15
    exit 1
}
