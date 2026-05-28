#pragma once

#include <string>
#include <vector>

namespace Zeta {

struct CompileError {
    enum class Type {
        Lexical,
        Syntax,
        Semantic,
        System
    } type;
    int line;
    int column;
    std::string message;

    CompileError(Type type, int line, int column, const std::string& message)
        : type(type), line(line), column(column), message(message) {}
};

class CompileErrorCollector {
public:
    static CompileErrorCollector& get();

    void addError(const CompileError& error);
    bool hasErrors() const;
    const std::vector<CompileError>& getErrors() const;
    void printAll() const;
    std::string getAllAsString() const;
    void clear();

private:
    CompileErrorCollector() = default;
    ~CompileErrorCollector() = default;

    std::vector<CompileError> errors;
};

std::string formatString(const char* format, ...);

}

#define REPORT_LEXICAL_ERROR(line, column, msg, ...) Zeta::CompileErrorCollector::get().addError(Zeta::CompileError(Zeta::CompileError::Type::Lexical, line, column, Zeta::formatString(msg, ##__VA_ARGS__)))
#define REPORT_SYNTAX_ERROR(line, column, msg, ...) Zeta::CompileErrorCollector::get().addError(Zeta::CompileError(Zeta::CompileError::Type::Syntax, line, column, Zeta::formatString(msg, ##__VA_ARGS__)))
#define REPORT_SEMANTIC_ERROR(line, column, msg, ...) Zeta::CompileErrorCollector::get().addError(Zeta::CompileError(Zeta::CompileError::Type::Semantic, line, column, Zeta::formatString(msg, ##__VA_ARGS__)))
#define REPORT_SYSTEM_ERROR(msg, ...) Zeta::CompileErrorCollector::get().addError(Zeta::CompileError(Zeta::CompileError::Type::System, 0, 0, Zeta::formatString(msg, ##__VA_ARGS__)))
