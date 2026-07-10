#include "error.h"

#include <cstdarg>
#include <cstdio>
#include <iostream>
#include <sstream>

namespace Zeta {

CompileErrorCollector& CompileErrorCollector::get() {
    static CompileErrorCollector instance;
    return instance;
}

void CompileErrorCollector::addError(const CompileError& error) {
    errors.push_back(error);
}

bool CompileErrorCollector::hasErrors() const {
    return !errors.empty();
}

const std::vector<CompileError>& CompileErrorCollector::getErrors() const {
    return errors;
}

void CompileErrorCollector::printAll() const {
    for (const auto& error : errors) {
        std::cout << std::format("{}", error);
    }
    std::cout << std::format("Total errors: {}", errors.size()) << std::endl;
}

std::string CompileErrorCollector::getAllAsString() const {
    std::ostringstream result;
    for (const auto& error : errors) {
        result << std::format("{}\n", error);
    }
    return result.str();
}

void CompileErrorCollector::clear() {
    errors.clear();
}

} // namespace Zeta
