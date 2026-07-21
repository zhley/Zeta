#pragma once

#include <memory>
#include <string>

namespace Zeta {

struct Module;

std::unique_ptr<Module> compileModule(const std::string& path, std::string* outError = nullptr);

void printModule(const Module* module, const std::string& path);

}
