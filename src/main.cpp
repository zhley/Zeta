#include "vm/vm.h"
#include "log.h"

#include <iostream>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <source_file>" << std::endl;
        return 1; 
    }

    Zeta::VM vm;
    vm.loadModule(argv[1]);
    Zeta::Value v = vm.callFunction("main", 0, nullptr);
    return 0;
}
