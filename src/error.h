#pragma once

#include <string>
#include <vector>

namespace Zeta {

struct Error {
    enum class Type {
        Lexical,
        Syntax,
        Semantic,
        Runtime
    } type;
    int line;
    int column;
    std::string message;

    Error(Type type, int line, int column, const std::string& message)
        : type(type), line(line), column(column), message(message) {}
};

class ErrorCollector {
public:
    static ErrorCollector& get();

    void addError(const Error& error);
    bool hasErrors() const;
    const std::vector<Error>& getErrors() const;
    void printAll() const;

private:
    ErrorCollector() = default;
    ~ErrorCollector() = default;

    std::vector<Error> errors;
};

std::string formatString(const char* format, ...);

}

#define REPORT_LEXICAL_ERROR(line, column, msg, ...) Zeta::ErrorCollector::get().addError(Zeta::Error(Zeta::Error::Type::Lexical, line, column, formatString(msg, ##__VA_ARGS__)))
#define REPORT_SYNTAX_ERROR(line, column, msg, ...) Zeta::ErrorCollector::get().addError(Zeta::Error(Zeta::Error::Type::Syntax, line, column, formatString(msg, ##__VA_ARGS__)))
#define REPORT_SEMANTIC_ERROR(line, column, msg, ...) Zeta::ErrorCollector::get().addError(Zeta::Error(Zeta::Error::Type::Semantic, line, column, formatString(msg, ##__VA_ARGS__)))
#define REPORT_RUNTIME_ERROR(line, column, msg, ...) Zeta::ErrorCollector::get().addError(Zeta::Error(Zeta::Error::Type::Runtime, line, column, formatString(msg, ##__VA_ARGS__)))