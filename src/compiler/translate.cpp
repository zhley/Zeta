#include "translate.h"

#include "bytecode.h"
#include "ast.h"
#include "error.h"

#include <memory>

namespace Zeta {

void ScopeManager::enterScope() {
    scopes.push_back(std::make_unique<Scope>());
    savedNextIndices.push_back(nextIndex);
}

void ScopeManager::exitScope() {
    if (!scopes.empty()) {
        scopes.pop_back();
        nextIndex = savedNextIndices.back();
        savedNextIndices.pop_back();
    }
}

int ScopeManager::declare(const std::string& name, bool isMutable) {
    if (scopes.empty()) {
        return -1; // No scope to declare in
    }
    Scope* currentScope = scopes.back().get();
    if (currentScope->symbols.find(name) != currentScope->symbols.end()) {
        return -1; // Symbol already declared in this scope
    }
    uint32_t index = nextIndex++;
    currentScope->symbols[name] = {index, isMutable};
    if (index > maxIndex) {
        maxIndex = index;
    }
    return index;
}

const ScopeManager::Scope::Sym* ScopeManager::resolve(const std::string& name) const {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
        const Scope* scope = it->get();
        auto symIt = scope->symbols.find(name);
        if (symIt != scope->symbols.end()) {
            return &symIt->second;
        }
    }
    return nullptr; // Not found
}

std::unique_ptr<Module> Translator::translate(AST::Program* program) {
    module = new Module();
    program->accept(*this);
    return std::unique_ptr<Module>(module);
}

void Translator::visit(AST::Program& program) {
    for (auto& import : program.imports) {
        import->accept(*this);
    }
    for (auto& dec : program.decs) {
        dec->accept(*this);
    }
}

void Translator::visit(AST::Import& import) {
    if(!import.alias.empty()){
        if(aliasToModule.find(import.alias) != aliasToModule.end()){
            REPORT_SEMANTIC_ERROR(import.line, import.column, "Duplicate alias '%s'", import.alias.c_str());
        }
        aliasToModule[import.alias] = import.path;
    }
    module->imports.push_back(import.path);
}

void Translator::visit(AST::VarDec& varDec) {
    if(inGlobalScope){
        if(module->globalSyms.find(varDec.name) != module->globalSyms.end()){
            REPORT_SEMANTIC_ERROR(varDec.line, varDec.column, "Duplicate global variable '%s'", varDec.name.c_str());
        }
        varDec.init->accept(*this);
        if(!isConstExpr){
            REPORT_SEMANTIC_ERROR(varDec.line, varDec.column, "Initializer for global variable '%s' must be a constant expression", varDec.name.c_str());
        }
        module->globalSyms[varDec.name] = {false, varDec.isMutable, isConstExpr ? value : Value(), ""};
    }
    // TODO:
}

void Translator::visit(AST::FuncDec& funcDec) {
    if(inGlobalScope){
        if(module->globalSyms.find(funcDec.name) != module->globalSyms.end()){
            REPORT_SEMANTIC_ERROR(funcDec.line, funcDec.column, "Duplicate global function '%s'", funcDec.name.c_str());
        }
        module->protos.push_back(std::make_unique<Proto>());
    }
}

} // namespace Zeta
