# C++ 互操作测试子项目（见 tests/README.md）。
#
# 编译一个链接 zeta_core 的宿主程序，用于测试 src/vm/vm.h 中面向
# C++ 宿主的公共互操作接口（registerFunction/registerClass/call/wrapPointer 等）。
# Zeta 侧测试代码位于同目录下的 *.zt 文件，由宿主程序通过 compileModule 编译。

add_executable(cpp_interop_test cpp_interop_test.cpp)
target_link_libraries(cpp_interop_test zeta_core)

# 将本目录的绝对路径传入测试程序，用于定位配套的 .zt 文件，
# 使测试与运行时的工作目录无关。
target_compile_definitions(cpp_interop_test PRIVATE CPP_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
