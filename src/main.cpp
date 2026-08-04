#include <iostream>
#include <memory>

#include "zeta/vm/vm.h"
#include "zeta/compiler/compiler.h"
#include "zeta/compiler/bytecode.h"

// TODO: 提供更多的参数选项

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <source_file>" << std::endl;
        return 1; 
    }

    std::string srcPath(argv[1]);
    std::string bcPath = srcPath.substr(0, srcPath.find_last_of('.')) + ZETA_BC_EXT;

    std::string compileError;
    std::unique_ptr<Zeta::Module> module = Zeta::compileModule(srcPath, &compileError);
    if (!compileError.empty()) {
        std::cerr << "Failed to compile: " << compileError << std::endl;
        return 1;
    }

    Zeta::serializeModule(module.get(), bcPath, &compileError);
    if (!compileError.empty()) {
        std::cerr << "Failed to serialize: " << compileError << std::endl;
        return 1;
    }
    module = Zeta::deserializeModule(bcPath, &compileError);
    if (!compileError.empty()) {
        std::cerr << "Failed to deserialize: " << compileError << std::endl;
        return 1;
    }

    Zeta::VM vm;
    vm.loadModule(module.get());
    Zeta::Value v = vm.callFunction(module->name, "main", 0, nullptr);
    return 0;
}
