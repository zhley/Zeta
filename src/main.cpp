#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "zeta/config.h"
#include "zeta/vm/vm.h"
#include "zeta/compiler/compiler.h"
#include "zeta/compiler/bytecode.h"

namespace {

enum class Action {
    None,
    Compile,
    Run,
    Dump,
};

// VM startup parameters, corresponding to Zeta::VM::Config
struct RuntimeConfig {
    int stackSize = ZETA_DEFAULT_STACK_SIZE;       // KiB
    int initHeapSize = ZETA_DEFAULT_INIT_HEAP_SIZE; // KiB
    int maxHeapSize = ZETA_DEFAULT_MAX_HEAP_SIZE;   // KiB, -1 means unlimited
    std::vector<std::string> moduleSearchPaths;
};

void printUsage(const char* program) {
    std::cout
        << "Usage: " << program << " <options> <file>\n\n"
        << "Options:\n"
        << "  -v, --version       print version number\n"
        << "  -h, --help          print this help message\n"
        << "  -c, --compile       compile a .zt source file into a .ztc bytecode file,\n"
        << "                      use -o/--output to specify the output file name\n"
        << "  -r, --run           run. If the input is a .ztc bytecode file, parse and\n"
        << "                      run it directly; otherwise treat it as a source file,\n"
        << "                      compile and then run it, without producing any\n"
        << "                      intermediate files\n"
        << "  -d, --dump          dump a .ztc bytecode file into a readable text file.\n"
        << "                      The default output file name is the input file name\n"
        << "                      with a .dump suffix appended, which can be overridden\n"
        << "                      with -o/--output\n"
        << "  -o, --output        specify the output file name (used with -c/--compile\n"
        << "                      and -d/--dump)\n"
        << "\n"
        << "VM startup parameters (only affect -r/--run):\n"
        << "  -s, --stack-size <KiB>            set the VM stack size (default: 1024)\n"
        << "  -i, --init-heap-size <KiB>        set the initial heap size (default: 1024)\n"
        << "  -m, --max-heap-size <KiB>         set the max heap size, -1 means unlimited\n"
        << "                                    (default: -1)\n"
        << "  -p, --module-search-path <path>   add a module search path, can be given\n"
        << "                                    multiple times (default: none)\n";
}

// Replace the extension of a source path with newExt to get the default output
// path, e.g. "foo.zt" -> "foo.ztc"
std::string replaceExtension(const std::string& path, const std::string& newExt) {
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return path + newExt;
    return path.substr(0, dot) + newExt;
}

bool parseInt(const std::string& opt, const std::string& value, int& out) {
    try {
        size_t pos = 0;
        out = std::stoi(value, &pos);
        return pos == value.size();
    } catch (...) {
        std::cerr << "Option " << opt << " requires an integer value, got: " << value << std::endl;
        return false;
    }
}

int compileSource(const std::string& srcPath, const std::string& outPath) {
    std::string error;
    std::unique_ptr<Zeta::Module> module = Zeta::compileModule(srcPath, &error);
    if (!error.empty()) {
        std::cerr << "Failed to compile: " << error << std::endl;
        return 1;
    }
    Zeta::serializeModule(module.get(), outPath, &error);
    if (!error.empty()) {
        std::cerr << "Failed to serialize: " << error << std::endl;
        return 1;
    }
    return 0;
}

int runModule(const std::unique_ptr<Zeta::Module>& module, const RuntimeConfig& config) {
    Zeta::VM vm(Zeta::VM::Config{config.stackSize, config.initHeapSize, config.maxHeapSize, config.moduleSearchPaths});
    vm.loadModule(module.get());
    try {
        vm.callFunction(module->name, "main", 0, nullptr);
    } catch (const Zeta::VMException& e) {
        std::cerr << e.what() << std::endl;
        return 2;
    }
    return 0;
}

int runSource(const std::string& srcPath, const RuntimeConfig& config) {
    std::string error;
    std::unique_ptr<Zeta::Module> module = Zeta::compileModule(srcPath, &error);
    if (!error.empty()) {
        std::cerr << "Failed to compile: " << error << std::endl;
        return 1;
    }
    return runModule(module, config);
}

int runBytecode(const std::string& bcPath, const RuntimeConfig& config) {
    std::string error;
    std::unique_ptr<Zeta::Module> module = Zeta::deserializeModule(bcPath, &error);
    if (!error.empty()) {
        std::cerr << "Failed to deserialize: " << error << std::endl;
        return 1;
    }
    return runModule(module, config);
}

int dumpModule(const std::string& bcPath, const std::string& outPath) {
    std::string error;
    std::unique_ptr<Zeta::Module> module = Zeta::deserializeModule(bcPath, &error);
    if (!error.empty()) {
        std::cerr << "Failed to deserialize: " << error << std::endl;
        return 1;
    }
    Zeta::printModule(module.get(), outPath);
    return 0;
}

} // namespace

// exit code:
// 0: success
// 1: local error
// 2: zeta vm error
int main(int argc, char* argv[]) {
    Action action = Action::None;
    std::string inputPath;
    std::string outputPath;
    RuntimeConfig runtimeConfig;
    bool printVersion = false;
    bool printHelp = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "-v" || arg == "--version") {
            printVersion = true;
        } else if (arg == "-h" || arg == "--help") {
            printHelp = true;
        } else if (arg == "-c" || arg == "--compile") {
            action = Action::Compile;
        } else if (arg == "-r" || arg == "--run") {
            action = Action::Run;
        } else if (arg == "-d" || arg == "--dump") {
            action = Action::Dump;
        } else if (arg == "-o" || arg == "--output") {
            if (i + 1 >= argc) {
                std::cerr << "Option " << arg << " requires a file name" << std::endl;
                return 1;
            }
            outputPath = argv[++i];
        } else if (arg == "-s" || arg == "--stack-size") {
            if (i + 1 >= argc) {
                std::cerr << "Option " << arg << " requires an integer value (KiB)" << std::endl;
                return 1;
            }
            if (!parseInt(arg, argv[++i], runtimeConfig.stackSize)) return 1;
        } else if (arg == "-i" || arg == "--init-heap-size") {
            if (i + 1 >= argc) {
                std::cerr << "Option " << arg << " requires an integer value (KiB)" << std::endl;
                return 1;
            }
            if (!parseInt(arg, argv[++i], runtimeConfig.initHeapSize)) return 1;
        } else if (arg == "-m" || arg == "--max-heap-size") {
            if (i + 1 >= argc) {
                std::cerr << "Option " << arg << " requires an integer value (KiB)" << std::endl;
                return 1;
            }
            if (!parseInt(arg, argv[++i], runtimeConfig.maxHeapSize)) return 1;
        } else if (arg == "-p" || arg == "--module-search-path") {
            if (i + 1 >= argc) {
                std::cerr << "Option " << arg << " requires a path" << std::endl;
                return 1;
            }
            runtimeConfig.moduleSearchPaths.push_back(argv[++i]);
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "Unknown option: " << arg << std::endl;
            return 1;
        } else {
            inputPath = arg;
        }
    }

    if (printVersion) {
        std::cout << ZETA_VERSION_STR << std::endl;
        return 0;
    }
    if (printHelp) {
        printUsage(argv[0]);
        return 0;
    }
    if (inputPath.empty()) {
        std::cerr << "Error: no input file" << std::endl << std::endl;
        printUsage(argv[0]);
        return 1;
    }

    switch (action) {
        case Action::Compile: {
            std::string outPath = outputPath.empty() ? replaceExtension(inputPath, ZETA_BC_EXT) : outputPath;
            return compileSource(inputPath, outPath);
        }
        case Action::Run: {
            if (inputPath.ends_with(ZETA_BC_EXT)) {
                return runBytecode(inputPath, runtimeConfig);
            }
            return runSource(inputPath, runtimeConfig);
        }
        case Action::Dump: {
            std::string outPath = outputPath.empty() ? inputPath + ".dump" : outputPath;
            return dumpModule(inputPath, outPath);
        }
        default: {
            // No action specified, run by default
            return runSource(inputPath, runtimeConfig);
        }
    }
}
