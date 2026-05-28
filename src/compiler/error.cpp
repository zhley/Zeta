#include "error.h"
#include <cstdarg>
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
        const char* typeStr = "";
        switch (error.type) {
            case CompileError::Type::Lexical:  typeStr = "Lexical"; break;
            case CompileError::Type::Syntax:   typeStr = "Syntax"; break;
            case CompileError::Type::Semantic: typeStr = "Semantic"; break;
            case CompileError::Type::System:   typeStr = "System"; break;
        }
        if(error.type == CompileError::Type::System) {
            fprintf(stderr, "System Error: %s\n", error.message.c_str());
        } else {
            fprintf(stderr, "%s Error at line %d, column %d: %s\n", typeStr, error.line, error.column, error.message.c_str());
        }
    }
};

std::string CompileErrorCollector::getAllAsString() const {
    std::ostringstream result;
    for (const auto& error : errors) {
        std::string typeStr;
        switch (error.type) {
            case CompileError::Type::Lexical:  typeStr = "Lexical"; break;
            case CompileError::Type::Syntax:   typeStr = "Syntax"; break;
            case CompileError::Type::Semantic: typeStr = "Semantic"; break;
            case CompileError::Type::System:   typeStr = "System"; break;
        }
        if(error.type == CompileError::Type::System) {
            result << "System Error: " << error.message << "\n";
        } else {
            result << typeStr << " Error at line " << error.line << ", column " << error.column << ": " << error.message << "\n";
        }
    }
    return result.str();
}

void CompileErrorCollector::clear() {
    errors.clear();
}

std::string formatString(const char* format, ...) {
    va_list args;
    va_start(args, format);
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return std::string(buffer);
}

} // namespace Zeta
