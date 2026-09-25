// C++ interop test host (see tests/README.md, section "C++ 互操作测试").
//
// Links against zeta_core and exercises the public C++-host interop surface
// declared in src/vm/vm.h: registerFunction / registerClass / call / callMethod /
// getLocal / setLocal / newUserData / internString / temp roots / stack ops / globals.
//
// Zeta-side scripts live next to this file as *.zt and are compiled via
// compileModule; their path is resolved through the CPP_TEST_DIR macro set by
// tests/cpp/CMakeLists.txt, so the working directory does not matter.

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

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
//
// Args may be read with pop() (last-pushed first) or getLocal(i)
// (base[0] is the first pushed arg; for methods base[0] is `this`).

// add(a, b) -> a + b
void nativeAdd(Zeta::VM* vm, int argc) {
    Zeta::Value a = vm->getLocal(0);
    Zeta::Value b = vm->getLocal(1);
    vm->push(Zeta::Value(a.intValue + b.intValue));
}

// expect_eq(a, b): asserts Value equality (type + payload).
void nativeExpectEq(Zeta::VM* vm, int argc) {
    Zeta::Value a = vm->getLocal(0);
    Zeta::Value b = vm->getLocal(1);
    check(a == b, "expect_eq()");
    vm->push(Zeta::Value());
}

// instanceField(this, name): reads a field from an Instance value.
Zeta::Value instanceField(Zeta::VM* vm, const Zeta::Value& inst, const char* name) {
    if (inst.type == Zeta::Value::Type::Object &&
        inst.ptrValue->getType() == Zeta::Object::Type::Instance) {
        return static_cast<Zeta::Instance*>(inst.ptrValue)
            ->getField(vm->internString(name)).value_or(Zeta::Value::Error);
    }
    return Zeta::Value::Error;
}

// get_x(): returns the instance's "x" field. this is local 0.
void nativeGetX(Zeta::VM* vm, int argc) {
    check(argc == 1, "native method argc == 实际传参个数 (含 this)");
    Zeta::Value inst = vm->getLocal(0);
    vm->push(instanceField(vm, inst, "x"));
}

// get_y(): returns the instance's "y" field.
void nativeGetY(Zeta::VM* vm, int argc) {
    Zeta::Value inst = vm->getLocal(0);
    vm->push(instanceField(vm, inst, "y"));
}

// set_x(v): writes the instance's "x" field via setLocal-style access to this.
void nativeSetX(Zeta::VM* vm, int argc) {
    check(argc == 2, "set_x argc == 2 (this + v)");
    Zeta::Value inst = vm->getLocal(0);
    Zeta::Value v = vm->getLocal(1);
    check(inst.type == Zeta::Value::Type::Object &&
          inst.ptrValue->getType() == Zeta::Object::Type::Instance, "set_x this is an Instance");
    static_cast<Zeta::Instance*>(inst.ptrValue)->setField(vm->internString("x"), v);
    vm->push(Zeta::Value());
}

// ---- UserData / NativeType test fixtures ----

struct HostPoint {
    int64_t x = 1;
    int64_t y = 2;
    int64_t iterPos = 0; // protocol iterator cursor for _iter/_next
};

// NativeType bound to a VM; field get pushes to the operand stack, field set
// receives the value as a parameter, methods run on a native frame
// [this(UserData), args...]. Protocol names _equals/_iter/_next are invoked
// by the VM for == / for-in.
class HostPointType : public Zeta::NativeType {
public:
    explicit HostPointType(Zeta::VM* vm) : Zeta::NativeType(vm) {
        nameX = vm->internString("x");
        nameY = vm->internString("y");
        nameGetX = vm->internString("get_x");
        nameGetY = vm->internString("get_y");
        nameSetX = vm->internString("set_x");
        nameSum = vm->internString("sum");
        nameEquals = vm->internString("_equals");
        nameIter = vm->internString("_iter");
        nameNext = vm->internString("_next");
    }

    void getField(void* instance, Zeta::String* fieldName) override {
        auto* p = static_cast<HostPoint*>(instance);
        if (fieldName == nameX) {
            vm->push(Zeta::Value(p->x));
        } else if (fieldName == nameY) {
            vm->push(Zeta::Value(p->y));
        } else {
            vm->push(Zeta::Value::Error);
        }
    }

    void setField(void* instance, Zeta::String* fieldName, const Zeta::Value& value) override {
        auto* p = static_cast<HostPoint*>(instance);
        if (value.type != Zeta::Value::Type::Int) {
            vm->reportError("HostPoint.setField: value must be Int");
            return;
        }
        if (fieldName == nameX) {
            p->x = value.intValue;
        } else if (fieldName == nameY) {
            p->y = value.intValue;
        } else {
            vm->reportError("HostPoint.setField: unknown field");
        }
    }

    void callMethod(void* instance, Zeta::String* methodName, int argc) override {
        auto* p = static_cast<HostPoint*>(instance);
        if (methodName == nameGetX) {
            if (argc != 0) {
                vm->reportError("HostPoint.get_x: argc must be 0");
                vm->push(Zeta::Value::Error);
                return;
            }
            vm->push(Zeta::Value(p->x));
        } else if (methodName == nameGetY) {
            if (argc != 0) {
                vm->reportError("HostPoint.get_y: argc must be 0");
                vm->push(Zeta::Value::Error);
                return;
            }
            vm->push(Zeta::Value(p->y));
        } else if (methodName == nameSetX) {
            if (argc != 1) {
                vm->reportError("HostPoint.set_x: argc must be 1 (v, excluding this)");
                vm->push(Zeta::Value::Error);
                return;
            }
            Zeta::Value v = vm->getLocal(1);
            if (v.type != Zeta::Value::Type::Int) {
                vm->reportError("HostPoint.set_x: v must be Int");
                vm->push(Zeta::Value::Error);
                return;
            }
            p->x = v.intValue;
            vm->push(Zeta::Value::Null);
        } else if (methodName == nameSum) {
            if (argc != 0) {
                vm->reportError("HostPoint.sum: argc must be 0");
                vm->push(Zeta::Value::Error);
                return;
            }
            vm->push(Zeta::Value(p->x + p->y));
        } else if (methodName == nameEquals) {
            // _equals(other) -> Bool; used by == / !=
            if (argc != 1) {
                vm->reportError("HostPoint._equals: argc must be 1 (other, excluding this)");
                vm->push(Zeta::Value::Error);
                return;
            }
            Zeta::Value other = vm->getLocal(1);
            bool eq = false;
            if (other.isUserData()) {
                auto* oud = static_cast<Zeta::UserData*>(other.ptrValue);
                if (oud->getNativeType() == this) {
                    auto* op = static_cast<HostPoint*>(oud->getData());
                    eq = (p->x == op->x && p->y == op->y);
                }
            }
            vm->push(Zeta::Value(eq));
        } else if (methodName == nameIter) {
            // _iter() -> this (iterates x then y)
            if (argc != 0) {
                vm->reportError("HostPoint._iter: argc must be 0");
                vm->push(Zeta::Value::Error);
                return;
            }
            p->iterPos = 0;
            vm->push(vm->getLocal(0));
        } else if (methodName == nameNext) {
            // _next() -> Int or Error when exhausted
            if (argc != 0) {
                vm->reportError("HostPoint._next: argc must be 0");
                vm->push(Zeta::Value::Error);
                return;
            }
            if (p->iterPos == 0) {
                p->iterPos = 1;
                vm->push(Zeta::Value(p->x));
            } else if (p->iterPos == 1) {
                p->iterPos = 2;
                vm->push(Zeta::Value(p->y));
            } else {
                vm->push(Zeta::Value::Error);
            }
        } else {
            vm->reportError("HostPoint: unknown method");
            vm->push(Zeta::Value::Error);
        }
    }

private:
    Zeta::String* nameX = nullptr;
    Zeta::String* nameY = nullptr;
    Zeta::String* nameGetX = nullptr;
    Zeta::String* nameGetY = nullptr;
    Zeta::String* nameSetX = nullptr;
    Zeta::String* nameSum = nullptr;
    Zeta::String* nameEquals = nullptr;
    Zeta::String* nameIter = nullptr;
    Zeta::String* nameNext = nullptr;
};

// Shared with make_point() so Zeta scripts can allocate UserData instances.
// The NativeType must outlive every UserData that references it.
HostPointType* g_hostPointType = nullptr;
std::vector<std::unique_ptr<HostPoint>> g_hostPoints;

// make_point() -> UserData wrapping a fresh HostPoint{x=1,y=2}
void nativeMakePoint(Zeta::VM* vm, int argc) {
    auto pt = std::make_unique<HostPoint>();
    HostPoint* raw = pt.get();
    g_hostPoints.push_back(std::move(pt));
    if (g_hostPointType == nullptr) {
        vm->reportError("make_point: HostPointType not bound");
        vm->push(Zeta::Value::Error);
        return;
    }
    vm->newUserData(raw, g_hostPointType);
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
        {{"get_x", nativeGetX}, {"get_y", nativeGetY}, {"set_x", nativeSetX}});
    check(idx >= 0, "registerClass 返回有效索引");

    vm.push(vm.getGlobal(idx));
    vm.newInstance(0);
    vm.callMethod("get_x", 0);
    Zeta::Value x = vm.pop();
    check(x.type == Zeta::Value::Type::Int && x.intValue == 1, "callMethod get_x == 1");

    vm.push(vm.getGlobal(idx));
    vm.newInstance(0);
    Zeta::Value inst = vm.pop();
    vm.push(Zeta::Value(int64_t(99)));
    vm.push(inst);
    vm.callMethod("set_x", 1);
    vm.pop(); // discard null return
    vm.push(inst);
    vm.callMethod("get_x", 0);
    Zeta::Value x2 = vm.pop();
    check(x2.type == Zeta::Value::Type::Int && x2.intValue == 99, "set_x 后 get_x == 99");

    vm.push(vm.getGlobal(idx));
    vm.newInstance(0);
    vm.callMethod("get_y", 0);
    Zeta::Value y = vm.pop();
    check(y.type == Zeta::Value::Type::Int && y.intValue == 2, "callMethod get_y == 2");
}

// swap_locals(a, b): reads both args via getLocal, writes them back with setLocal, returns a-b.
void nativeSwapLocals(Zeta::VM* vm, int argc) {
    check(argc == 2, "swap_locals argc == 2");
    Zeta::Value a = vm->getLocal(0);
    Zeta::Value b = vm->getLocal(1);
    vm->setLocal(0, b);
    vm->setLocal(1, a);
    vm->push(Zeta::Value(a.intValue - b.intValue));
}

void testGetSetLocal() {
    beginTest("getLocal / setLocal");
    Zeta::VM vm;
    vm.setErrorHandler(silentErrorHandler);

    int idx = vm.registerFunction("swap_locals", nativeSwapLocals);
    check(idx >= 0, "registerFunction swap_locals");

    vm.push(Zeta::Value(int64_t(3)));
    vm.push(Zeta::Value(int64_t(10)));
    vm.push(vm.getGlobal(idx));
    vm.call(2);
    Zeta::Value result = vm.pop();
    check(result.type == Zeta::Value::Type::Int && result.intValue == -7,
          "swap_locals(3,10) 返回 a-b == -7");

    // After the call the native frame is gone; caller frame is intact.
    vm.push(Zeta::Value(int64_t(1)));
    check(vm.peek(-1)->intValue == 1, "原生帧弹出后调用方栈帧仍可用");
    vm.pop();
}

void testUserDataCpp() {
    beginTest("UserData / NativeType (C++ 侧)");
    Zeta::VM vm;
    vm.setErrorHandler(silentErrorHandler);
    HostPointType type(&vm);
    HostPoint pt{3, 4};

    vm.newUserData(&pt, &type);
    Zeta::Value udVal = vm.pop();
    check(udVal.isUserData(), "newUserData 结果 isUserData()");
    check(udVal.type == Zeta::Value::Type::Object &&
              udVal.ptrValue->getType() == Zeta::Object::Type::UserData,
          "newUserData 类型为 UserData");
    check(static_cast<bool>(udVal), "UserData 真值为 true");

    auto udOpt = udVal.as<Zeta::UserData*>();
    check(udOpt.has_value(), "Value::as<UserData*>()");
    check(udOpt && (*udOpt)->getData() == &pt, "getData() 返回宿主指针");
    check(udOpt && (*udOpt)->getNativeType() == &type, "getNativeType() 返回类型");

    // Field get via UserData wrapper (result pushed on current frame stack).
    if (udOpt) {
        (*udOpt)->getField(vm.internString("x"));
        Zeta::Value x = vm.pop();
        check(x.type == Zeta::Value::Type::Int && x.intValue == 3, "getField(x) == 3");

        (*udOpt)->setField(vm.internString("x"), Zeta::Value(int64_t(30)));
        check(pt.x == 30, "setField 直接写入宿主数据");
        (*udOpt)->getField(vm.internString("x"));
        x = vm.pop();
        check(x.intValue == 30, "getField 读回 30");
    }

    // callMethod via VM (independent native frame).
    vm.push(udVal);
    vm.callMethod("get_y", 0);
    Zeta::Value y = vm.pop();
    check(y.type == Zeta::Value::Type::Int && y.intValue == 4, "callMethod get_y == 4");

    vm.push(udVal);
    vm.callMethod("sum", 0);
    Zeta::Value sum = vm.pop();
    check(sum.type == Zeta::Value::Type::Int && sum.intValue == 34, "callMethod sum == 30+4");

    vm.push(Zeta::Value(int64_t(99)));
    vm.push(udVal);
    vm.callMethod("set_x", 1);
    vm.pop();
    check(pt.x == 99, "callMethod set_x 写入宿主数据");

    // Negative: null data / null type rejected.
    HostPoint dummy{0, 0};
    vm.newUserData(nullptr, &type);
    Zeta::Value badData = vm.pop();
    check(badData.type == Zeta::Value::Type::Error, "newUserData(nullptr, type) -> Error");

    vm.newUserData(&dummy, nullptr);
    Zeta::Value badType = vm.pop();
    check(badType.type == Zeta::Value::Type::Error, "newUserData(data, nullptr) -> Error");
}

void testUserDataZeta() {
    beginTest("UserData / NativeType (Zeta 调用)");
    Zeta::VM vm;
    vm.setErrorHandler(silentErrorHandler);

    HostPointType type(&vm);
    g_hostPointType = &type;
    g_hostPoints.clear();

    vm.registerFunction("expect_eq", nativeExpectEq);
    vm.registerFunction("make_point", nativeMakePoint);

    auto module = loadScript(vm, "userdata.zt");
    if (!module) {
        g_hostPointType = nullptr;
        return;
    }
    callMain(vm, *module);
    vm.pop();
    g_hostPointType = nullptr;
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
        {{"get_x", nativeGetX}, {"get_y", nativeGetY}, {"set_x", nativeSetX}});

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
    testGetSetLocal();
    testUserDataCpp();
    testUserDataZeta();
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
