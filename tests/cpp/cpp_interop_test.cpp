// C++ interop test host (see tests/README.md, section "C++ 互操作测试").
//
// Links against zeta_core and exercises the public C++-host interop surface
// declared in src/vm/vm.h: registerFunction / registerClass / call / callMethod /
// wrapPointer / unwrapPointer / internString / temp roots / stack ops / globals.
//
// Zeta-side scripts live next to this file as *.zt and are compiled via
// compileModule; their path is resolved through the CPP_TEST_DIR macro set by
// tests/cpp/CMakeLists.txt, so the working directory does not matter.

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

#include "zeta/vm/vm.h"
#include "zeta/compiler/compiler.h"
#include "zeta/compiler/bytecode.h"

namespace {

int g_passed = 0;
int g_failed = 0;

void check(bool cond, const char* what) {
    if (cond) {
        ++g_passed;
    } else {
        ++g_failed;
        std::cout << "  FAIL: " << what << "\n";
    }
}

void beginTest(const char* name) {
    std::cout << "== " << name << " ==\n";
}

// Errors are deliberately silenced: each test verifies outcomes through
// check()/return values, and negative-path cases trigger errors on purpose.
void silentErrorHandler(const Zeta::VM::Error&) {}

std::string scriptPath(const std::string& file) {
    return std::string(CPP_TEST_DIR) + "/" + file;
}

std::unique_ptr<Zeta::Module> loadScript(Zeta::VM& vm, const std::string& file) {
    std::string error;
    std::unique_ptr<Zeta::Module> module = Zeta::compileModule(scriptPath(file), &error);
    if (!module) {
        check(false, ("compile " + file + ": " + error).c_str());
        return nullptr;
    }
    vm.loadModule(module.get());
    return module;
}

// Call the module's main() (0 arguments).
void callMain(Zeta::VM& vm, const Zeta::Module& module) {
    int mainIdx = vm.findGlobal(module.name, "main");
    vm.push(vm.getGlobal(mainIdx));
    vm.call(0);
    vm.pop();
}

// ---- native functions registered into the VM by the tests ----

// add(a, b) -> a + b
void nativeAdd(Zeta::VM* vm, int argc) {
    Zeta::Value b = vm->pop();
    Zeta::Value a = vm->pop();
    vm->push(Zeta::Value(a.intValue + b.intValue));
}

// expect_eq(a, b): asserts strict value equality (test data is Int only).
void nativeExpectEq(Zeta::VM* vm, int argc) {
    Zeta::Value b = vm->pop();
    Zeta::Value a = vm->pop();
    check(a == b, "expect_eq()");
    vm->push(Zeta::Value());
}

// get_x(): returns the instance's "x" field.
void nativeGetX(Zeta::VM* vm, int argc) {
    check(argc == 1, "native method argc == 形参个数 + 1");
    Zeta::Value inst = vm->pop(); // instance is the topmost argument
    vm->push(inst[vm->internString("x")]);
}

// get_y(): returns the instance's "y" field.
void nativeGetY(Zeta::VM* vm, int argc) {
    Zeta::Value inst = vm->pop();
    vm->push(inst[vm->internString("y")]);
}

// ---- test cases ----

void testStackOps() {
    beginTest("stack ops (push/peek/pop)");
    Zeta::VM vm;
    vm.setErrorHandler(silentErrorHandler);

    vm.push(Zeta::Value(int64_t(10)));
    vm.push(Zeta::Value(int64_t(20)));
    vm.push(Zeta::Value(int64_t(30)));
    vm.push(Zeta::Value(int64_t(40)));
    vm.push(Zeta::Value(int64_t(50)));

    check(vm.peek(-1)->intValue == 50, "peek(-1) == 50");
    check(vm.peek(-2)->intValue == 40, "peek(-2) == 40");
    check(vm.peek(-3)->intValue == 30, "peek(-3) == 30");

    check(vm.pop().intValue == 50, "pop() == 50");

    // pop(count) pops multiple values at once.
    vm.pop(2); // 弹出 40、30
    check(vm.peek(-1)->intValue == 20, "pop(2) 后 peek(-1) == 20");

    // push() returns the slot; it can be written in place.
    Zeta::Value* slot = vm.push(Zeta::Value(int64_t(5)));
    *slot = Zeta::Value(int64_t(7));
    check(vm.peek(-1)->intValue == 7, "push() 返回槽位可原地改写");
    vm.pop(3);
}

void testInternString() {
    beginTest("internString");
    Zeta::VM vm;
    vm.setErrorHandler(silentErrorHandler);

    Zeta::String* a = vm.internString("abc");
    Zeta::String* b = vm.internString("abc");
    check(a == b, "internString 相同内容返回同一指针");
    check(std::string(a->getData(), a->getLength()) == "abc", "internString 内容正确");

    vm.newStrObj("abc");
    Zeta::Value strObjVal = vm.pop();
    auto* strObj = static_cast<Zeta::StrObj*>(strObjVal.ptrValue);
    Zeta::String* c = vm.internString(strObj);
    check(c == a, "internString(StrObj*) 与 internString(str) 返回同一指针");
}

void testNewObjects() {
    beginTest("newArray / newMap / newStrObj");
    Zeta::VM vm;
    vm.setErrorHandler(silentErrorHandler);

    vm.newArray();
    Zeta::Value arr = vm.pop();
    check(arr.type == Zeta::Value::Type::Object &&
              arr.ptrValue->getType() == Zeta::Object::Type::Array,
          "newArray() 类型为 Array");
    check(static_cast<Zeta::Array*>(arr.ptrValue)->getSize() == 0, "newArray() 大小为 0");

    vm.newArray(5);
    Zeta::Value arr5 = vm.pop();
    check(arr5.type == Zeta::Value::Type::Object &&
              arr5.ptrValue->getType() == Zeta::Object::Type::Array,
          "newArray(5) 类型为 Array");
    check(static_cast<Zeta::Array*>(arr5.ptrValue)->getSize() == 5, "newArray(5) 大小为 5");

    vm.newMap();
    Zeta::Value map = vm.pop();
    check(map.type == Zeta::Value::Type::Object &&
              map.ptrValue->getType() == Zeta::Object::Type::Map,
          "newMap() 类型为 Map");

    vm.newStrObj("hello");
    Zeta::Value str = vm.pop();
    check(str.type == Zeta::Value::Type::Object &&
              str.ptrValue->getType() == Zeta::Object::Type::StrObj,
          "newStrObj() 类型为 StrObj");
    auto* strObj = static_cast<Zeta::StrObj*>(str.ptrValue);
    check(std::string(strObj->getData(), strObj->getLength()) == "hello",
          "newStrObj() 内容正确");
}

void testRegisterFunctionCppCall() {
    beginTest("registerFunction + C++ 调用");
    Zeta::VM vm;
    vm.setErrorHandler(silentErrorHandler);

    int idx = vm.registerFunction("add", nativeAdd);
    check(idx >= 0, "registerFunction 返回有效索引");
    check(vm.findGlobal("", "add") == idx, "findGlobal 返回相同索引");

    Zeta::Value fn = vm.getGlobal(idx);
    check(fn.type == Zeta::Value::Type::NativeFunc, "getGlobal 返回 NativeFunc");

    vm.push(Zeta::Value(int64_t(2)));
    vm.push(Zeta::Value(int64_t(3)));
    vm.push(fn);
    vm.call(2);
    Zeta::Value result = vm.pop();
    check(result.type == Zeta::Value::Type::Int && result.intValue == 5,
          "C++ 调用 add(2,3) == 5");
}

void testRegisterClassCppCall() {
    beginTest("registerClass + C++ 调用");
    Zeta::VM vm;
    vm.setErrorHandler(silentErrorHandler);

    int idx = vm.registerClass("NativePoint",
        {{"x", Zeta::Value(int64_t(1))}, {"y", Zeta::Value(int64_t(2))}},
        {{"get_x", nativeGetX}, {"get_y", nativeGetY}});
    check(idx >= 0, "registerClass 返回有效索引");

    vm.push(vm.getGlobal(idx));
    vm.newInstance(0);
    vm.callMethod("get_x", 0);
    Zeta::Value x = vm.pop();
    check(x.type == Zeta::Value::Type::Int && x.intValue == 1, "callMethod get_x == 1");

    vm.push(vm.getGlobal(idx));
    vm.newInstance(0);
    vm.callMethod("get_y", 0);
    Zeta::Value y = vm.pop();
    check(y.type == Zeta::Value::Type::Int && y.intValue == 2, "callMethod get_y == 2");
}

void testWrapUnwrapPointer() {
    beginTest("wrapPointer / unwrapPointer");
    Zeta::VM vm;
    vm.setErrorHandler(silentErrorHandler);

    int clsIdx = vm.registerClass("Wrapper", {}, {});
    check(clsIdx >= 0, "registerClass Wrapper");

    int sentinel = 12345;
    vm.wrapPointer(&sentinel, vm.getGlobal(clsIdx));
    void* unwrapped = vm.unwrapPointer();
    check(unwrapped == &sentinel, "unwrapPointer 返回同一地址");

    // Negative path: top of stack is not an instance -> error + nullptr.
    vm.push(Zeta::Value(int64_t(0)));
    void* bad = vm.unwrapPointer();
    check(bad == nullptr, "unwrapPointer 非实例返回 nullptr");
}

void testTempRoot() {
    beginTest("pushTempRoot / popTempRoot");
    Zeta::VM vm;
    vm.setErrorHandler(silentErrorHandler);

    vm.newStrObj("persistent");
    Zeta::Value* root = vm.pushTempRoot();
    check(root != nullptr, "pushTempRoot 返回指针");

    // Allocate heavily to trigger GC; the temp root must survive.
    for (int i = 0; i < 100000; ++i) {
        vm.newStrObj("garbage");
        vm.pop();
    }

    check(root->type == Zeta::Value::Type::Object &&
              root->ptrValue->getType() == Zeta::Object::Type::StrObj,
          "临时根类型仍为 StrObj");
    auto* strObj = static_cast<Zeta::StrObj*>(root->ptrValue);
    check(std::string(strObj->getData(), strObj->getLength()) == "persistent",
          "临时根对象跨 GC 存活");

    vm.popTempRoot(root);
}

void testRegisterFunctionZetaCall() {
    beginTest("registerFunction + Zeta 调用");
    Zeta::VM vm;
    vm.setErrorHandler(silentErrorHandler);
    vm.registerFunction("add", nativeAdd);
    vm.registerFunction("expect_eq", nativeExpectEq);

    auto module = loadScript(vm, "native_function.zt");
    if (!module) return;
    callMain(vm, *module);
    vm.pop();
}

void testRegisterClassZetaCall() {
    beginTest("registerClass + Zeta 调用");
    Zeta::VM vm;
    vm.setErrorHandler(silentErrorHandler);
    vm.registerFunction("expect_eq", nativeExpectEq);
    vm.registerClass("NativePoint",
        {{"x", Zeta::Value(int64_t(1))}, {"y", Zeta::Value(int64_t(2))}},
        {{"get_x", nativeGetX}, {"get_y", nativeGetY}});

    auto module = loadScript(vm, "native_class.zt");
    if (!module) return;
    callMain(vm, *module);
    vm.pop();
}

void testCppCallZetaFunction() {
    beginTest("C++ 调用 Zeta 函数");
    Zeta::VM vm;
    vm.setErrorHandler(silentErrorHandler);
    auto module = loadScript(vm, "zeta_function.zt");
    if (!module) return;

    int idx = vm.findGlobal(module->name, "multiply");
    check(idx >= 0, "findGlobal multiply");

    vm.push(Zeta::Value(int64_t(6)));
    vm.push(Zeta::Value(int64_t(7)));
    vm.push(vm.getGlobal(idx));
    vm.call(2);
    Zeta::Value result = vm.pop();
    check(result.type == Zeta::Value::Type::Int && result.intValue == 42,
          "multiply(6,7) == 42");
}

void testCppCallZetaMethod() {
    beginTest("C++ 调用 Zeta 方法");
    Zeta::VM vm;
    vm.setErrorHandler(silentErrorHandler);
    auto module = loadScript(vm, "zeta_class.zt");
    if (!module) return;

    int clsIdx = vm.findGlobal(module->name, "Calc");
    check(clsIdx >= 0, "findGlobal Calc");

    vm.push(vm.getGlobal(clsIdx));
    vm.newInstance(0);
    Zeta::Value instance = vm.pop();

    vm.push(Zeta::Value(int64_t(10)));
    vm.push(Zeta::Value(int64_t(32)));
    vm.push(instance);
    vm.callMethod("add", 2);
    Zeta::Value result = vm.pop();
    check(result.type == Zeta::Value::Type::Int && result.intValue == 42,
          "Calc.add(10,32) == 42");
}

void testGetSetGlobal() {
    beginTest("getGlobal / setGlobal 跨 C++/Zeta");
    Zeta::VM vm;
    vm.setErrorHandler(silentErrorHandler);
    vm.registerFunction("expect_eq", nativeExpectEq);
    auto module = loadScript(vm, "zeta_global.zt");
    if (!module) return;

    int gIdx = vm.findGlobal(module->name, "g");
    check(gIdx >= 0, "findGlobal g");

    Zeta::Value init = vm.getGlobal(gIdx);
    check(init.type == Zeta::Value::Type::Int && init.intValue == 0, "g 初始为 0");

    vm.setGlobal(gIdx, Zeta::Value(int64_t(42)));
    Zeta::Value updated = vm.getGlobal(gIdx);
    check(updated.type == Zeta::Value::Type::Int && updated.intValue == 42,
          "setGlobal 后 getGlobal 读到 42");

    // The script's main() reads g and asserts it equals 42.
    callMain(vm, *module);
}

} // namespace

int main() {
    testStackOps();
    testInternString();
    testNewObjects();
    testRegisterFunctionCppCall();
    testRegisterClassCppCall();
    testWrapUnwrapPointer();
    testTempRoot();
    testRegisterFunctionZetaCall();
    testRegisterClassZetaCall();
    testCppCallZetaFunction();
    testCppCallZetaMethod();
    testGetSetGlobal();

    int total = g_passed + g_failed;
    if (g_failed == 0) {
        std::cout << "cpp interop: all passed (" << g_passed << "/" << total << ")\n";
        return 0;
    }
    std::cout << "cpp interop: " << g_passed << "/" << total << " passed, "
              << g_failed << " failed\n";
    return 1;
}
