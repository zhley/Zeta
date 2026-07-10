#pragma once

#include <string>
#include <vector>
#include <format>

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

}

template <>
struct std::formatter<Zeta::CompileError> {
    constexpr auto parse(std::format_parse_context& ctx) {
        return ctx.begin(); 
    }
    template <typename FormatContext>
    auto format(const Zeta::CompileError& error, FormatContext& ctx) const {
        std::string typeStr;
        switch (error.type) {
            case Zeta::CompileError::Type::Lexical: typeStr = "Lexical"; break;
            case Zeta::CompileError::Type::Syntax: typeStr = "Syntax"; break;
            case Zeta::CompileError::Type::Semantic: typeStr = "Semantic"; break;
            case Zeta::CompileError::Type::System: typeStr = "System"; break;
        }
        return std::format_to(ctx.out(), "[{} Error] Line {}, Column {}: {}", typeStr, error.line, error.column, error.message);
    }
};

#define REPORT_LEXICAL_ERROR(line, column, msg, ...) Zeta::CompileErrorCollector::get().addError(Zeta::CompileError(Zeta::CompileError::Type::Lexical, line, column, std::format(msg, ##__VA_ARGS__)))
#define REPORT_SYNTAX_ERROR(line, column, msg, ...) Zeta::CompileErrorCollector::get().addError(Zeta::CompileError(Zeta::CompileError::Type::Syntax, line, column, std::format(msg, ##__VA_ARGS__)))
#define REPORT_SEMANTIC_ERROR(line, column, msg, ...) Zeta::CompileErrorCollector::get().addError(Zeta::CompileError(Zeta::CompileError::Type::Semantic, line, column, std::format(msg, ##__VA_ARGS__)))
#define REPORT_SYSTEM_ERROR(msg, ...) Zeta::CompileErrorCollector::get().addError(Zeta::CompileError(Zeta::CompileError::Type::System, 0, 0, std::format(msg, ##__VA_ARGS__)))
