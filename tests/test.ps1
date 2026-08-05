# 定义测试目录列表
$directories = @(
    "tests/lex",
    "tests/expr",
    "tests/stmt",
    "tests/func",
    "tests/class",
    "tests/builtin",
    "tests/integration"
)

# 可执行文件路径（根据实际调整，如果为zeta.exe）
$exe = ".\build\bin\zeta.exe"

# 遍历每个目录下的 .zt 文件
foreach ($dir in $directories) {
    Get-ChildItem -Path $dir -Filter "*.zt" | ForEach-Object {
        Write-Host "=== $($_.FullName) ==="
        & $exe $_.FullName
        Write-Host ""
    }
}

# 单独执行模块测试
Write-Host "=== tests/module/mod_main.zt ==="
& $exe "tests/module/mod_main.zt"
Write-Host ""

# GC 测试 (带堆参数, 见 tests/gc/run_gc.ps1)
Write-Host "=== tests/gc (run_gc.ps1) ==="
& ".\tests\gc\run_gc.ps1"
