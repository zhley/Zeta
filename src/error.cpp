#include "error.h"
#include <cstdarg>

namespace Zeta {

ErrorCollector& ErrorCollector::get() {
    static ErrorCollector instance;
    return instance;
}

void ErrorCollector::addError(const Error& error) {
    errors.push_back(error);
}

bool ErrorCollector::hasErrors() const {
    return !errors.empty();
}

const std::vector<Error>& ErrorCollector::getErrors() const {
    return errors;
}

void ErrorCollector::printAll() const {
    for (const auto& error : errors) {
        const char* typeStr = "";
        switch (error.type) {
            case Error::Type::Lexical:  typeStr = "Lexical"; break;
            case Error::Type::Syntax:   typeStr = "Syntax"; break;
            case Error::Type::Semantic: typeStr = "Semantic"; break;
            case Error::Type::Runtime:  typeStr = "Runtime"; break;
        }
        fprintf(stderr, "%s Error at line %d, column %d: %s\n", typeStr, error.line, error.column, error.message.c_str());
    }
};

std::string formatString(const char* format, ...) {
    va_list args;
    va_start(args, format);
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return std::string(buffer);
}

} // namespace Zeta
