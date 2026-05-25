#include "translate.h"

#include "bytecode.h"
#include "ast.h"
#include "error.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

// TODO: 需要做优化

// NOTE: 模块别名和全局符号名可能冲突, 行为: 优先模块别名

// TODO: 去掉静态成员支持; 
// TODO: 构造函数特殊处理, 将其变成返回一个实例的全局函数;
// TODO: 函数调用将参数个数压栈, 以支持重载;

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

uint32_t ScopeManager::declare(const std::string& name, bool isMutable) {
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
    inGlobalScope = true;
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
        if(varDec.init){
            varDec.init->accept(*this);
            if(!isConstExpr){
                REPORT_SEMANTIC_ERROR(varDec.line, varDec.column, "Initializer for global variable '%s' must be a constant expression", varDec.name.c_str());
            }
        }
        module->globalSyms[varDec.name] = {varDec.isMutable, (varDec.init && isConstExpr) ? std::move(value) : nullptr};
    } else {
        int index = curScopeMgr->declare(varDec.name, varDec.isMutable);
        if(index == -1){
            REPORT_SEMANTIC_ERROR(varDec.line, varDec.column, "Duplicate local variable '%s'", varDec.name.c_str());
        }
        if(varDec.init){
            varDec.init->accept(*this);
            pushOpcode(Opcode::StoreVar);
            push4B(index);
        }
    }
}

void Translator::visit(AST::FuncDec& funcDec) {
    if(module->globalSyms.find(funcDec.name) != module->globalSyms.end()){
        REPORT_SEMANTIC_ERROR(funcDec.line, funcDec.column, "Duplicate global function '%s'", funcDec.name.c_str());
    }
    module->protos.push_back(std::make_unique<Proto>());
    curProto = module->protos.back().get();
    curProto->index = module->protos.size() - 1;
    curProto->arity = funcDec.params.size();
    ScopeManager scopeManager;
    curScopeMgr = &scopeManager;
    scopeManager.enterScope();
    for(size_t i = 0; i < funcDec.params.size(); ++i){
        const std::string& paramName = funcDec.params[i];
        if(scopeManager.declare(paramName, true) == -1){
            REPORT_SEMANTIC_ERROR(funcDec.line, funcDec.column, "Duplicate parameter name '%s' in function '%s'", paramName.c_str(), funcDec.name.c_str());
        }
    }
    funcDec.body->accept(*this);
    scopeManager.exitScope();
    curProto->localCount = scopeManager.getMaxIndex() + 1;
    curScopeMgr = nullptr;
    module->globalSyms[funcDec.name] = {false, std::make_unique<CompileValue>(new CompileFunction{curProto->index})};
}

void Translator::visit(AST::ClassDec& classDec) {
    if(module->globalSyms.find(classDec.name) != module->globalSyms.end()){
        REPORT_SEMANTIC_ERROR(classDec.line, classDec.column, "Duplicate global class '%s'", classDec.name.c_str());
    }
    CompileClass* compileClass = new CompileClass();
    compileClass->name = classDec.name;
    compileClass->base = classDec.base;
    for(auto& [isStatic, member] : classDec.members){
        if(isStatic){
            if(member->type == AST::Dec::DecType::Var){
                auto* varDec = static_cast<AST::VarDec*>(member.get());
                if(varDec->init){
                    varDec->init->accept(*this);
                    if(!isConstExpr){
                        REPORT_SEMANTIC_ERROR(varDec->line, varDec->column, "Initializer for static member '%s' must be a constant expression", varDec->name.c_str());
                    }
                }
                std::string globalName = makeGlobalSymbol(classDec.name, varDec->name);
                if(module->globalSyms.find(globalName) != module->globalSyms.end()){
                    REPORT_SEMANTIC_ERROR(varDec->line, varDec->column, "Duplicate static member '%s' in class '%s'", varDec->name.c_str(), classDec.name.c_str());
                }
                module->globalSyms[globalName] = {varDec->isMutable, (varDec->init && isConstExpr) ? std::move(value) : nullptr};
            } else if(member->type == AST::Dec::DecType::Func){
                auto* funcDec = static_cast<AST::FuncDec*>(member.get());
                module->protos.push_back(std::make_unique<Proto>());
                Proto* proto = module->protos.back().get();
                proto->index = module->protos.size() - 1;
                proto->arity = funcDec->params.size();
                ScopeManager scopeManager;
                curScopeMgr = &scopeManager;
                scopeManager.enterScope();
                for(size_t i = 0; i < funcDec->params.size(); ++i){
                    const std::string& paramName = funcDec->params[i];
                    if(scopeManager.declare(paramName, true) == -1){
                        REPORT_SEMANTIC_ERROR(funcDec->line, funcDec->column, "Duplicate parameter name '%s' in static method '%s' of class '%s'", paramName.c_str(), funcDec->name.c_str(), classDec.name.c_str());
                    }
                }
                funcDec->body->accept(*this);
                scopeManager.exitScope();
                proto->localCount = scopeManager.getMaxIndex() + 1;
                curScopeMgr = nullptr;
                std::string globalName = makeGlobalSymbol(classDec.name, funcDec->name);
                if(module->globalSyms.find(globalName) != module->globalSyms.end()){
                    REPORT_SEMANTIC_ERROR(funcDec->line, funcDec->column, "Duplicate static member '%s' in class '%s'", funcDec->name.c_str(), classDec.name.c_str());
                }
                module->globalSyms[globalName] = {false, std::make_unique<CompileValue>(new CompileFunction{proto->index})};
            } else {
                REPORT_SEMANTIC_ERROR(member->line, member->column, "Invalid static member in class '%s'", classDec.name.c_str());
            }
        } else {
            if(member->type == AST::Dec::DecType::Var){
                auto* varDec = static_cast<AST::VarDec*>(member.get());
                if(varDec->init){
                    varDec->init->accept(*this);
                    if(!isConstExpr){
                        REPORT_SEMANTIC_ERROR(varDec->line, varDec->column, "Initializer for instance member '%s' must be a constant expression", varDec->name.c_str());
                    }
                }
                if(compileClass->fields.find(varDec->name) != compileClass->fields.end()){
                    REPORT_SEMANTIC_ERROR(varDec->line, varDec->column, "Duplicate instance member '%s' in class '%s'", varDec->name.c_str(), classDec.name.c_str());
                }
                compileClass->fields[varDec->name] = varDec->init && isConstExpr ? std::move(value) : nullptr;
            } else if(member->type == AST::Dec::DecType::Func){
                auto* funcDec = static_cast<AST::FuncDec*>(member.get());
                module->protos.push_back(std::make_unique<Proto>());
                curProto = module->protos.back().get();
                curProto->index = module->protos.size() - 1;
                curProto->arity = funcDec->params.size() + 1;
                ScopeManager scopeManager;
                curScopeMgr = &scopeManager;
                scopeManager.enterScope();
                scopeManager.declare("this", false);
                for(size_t i = 0; i < funcDec->params.size(); ++i){
                    const std::string& paramName = funcDec->params[i];
                    if(scopeManager.declare(paramName, true) == -1){
                        REPORT_SEMANTIC_ERROR(funcDec->line, funcDec->column, "Duplicate parameter name '%s' in method '%s' of class '%s'", paramName.c_str(), funcDec->name.c_str(), classDec.name.c_str());
                    }
                }
                funcDec->body->accept(*this);
                scopeManager.exitScope();
                curProto->localCount = scopeManager.getMaxIndex() + 1;
                curScopeMgr = nullptr;
                if(compileClass->methods.find(funcDec->name) != compileClass->methods.end()){
                    REPORT_SEMANTIC_ERROR(funcDec->line, funcDec->column, "Duplicate instance member '%s' in class '%s'", funcDec->name.c_str(), classDec.name.c_str());
                }
                compileClass->methods[funcDec->name] = CompileFunction{curProto->index};
            } else {
                REPORT_SEMANTIC_ERROR(member->line, member->column, "Invalid instance member in class '%s'", classDec.name.c_str());
            }
        }
    }
    module->globalSyms[classDec.name] = {false, std::make_unique<CompileValue>(compileClass)};
}

void Translator::visit(AST::BlockStmt& blockStmt) {
    if(blockStmt.isFuncBody){
        for(auto& stmt : blockStmt.stmts){
            stmt->accept(*this);
        }
    } else {
        curScopeMgr->enterScope();
        for(auto& stmt : blockStmt.stmts){
            stmt->accept(*this);
        }
        curScopeMgr->exitScope();
    }
}

void Translator::visit(AST::ExpStmt& expStmt) {
    expStmt.exp->accept(*this);
}

void Translator::visit(AST::VarDecStmt& varDecStmt) {
    for(auto& varDec : varDecStmt.varDecs){
        varDec->accept(*this);
    }
}

void Translator::visit(AST::IfStmt& ifStmt) {
    ifStmt.cond->accept(*this);
    pushOpcode(Opcode::JumpIfFalse);
    uint32_t next = skip4B();
    ifStmt.thenBranch->accept(*this);
    if(ifStmt.elseBranch){
        pushOpcode(Opcode::Jump);
        uint32_t elseJump = skip4B();
        set4B(next, getLabel());
        ifStmt.elseBranch->accept(*this);
        set4B(elseJump, getLabel());
    }else{
        set4B(next, getLabel());
    }
}

void Translator::visit(AST::WhileStmt& whileStmt) {
    breakPosStack.push({});
    continuePosStack.push({});
    uint32_t loopStart = getLabel();
    whileStmt.cond->accept(*this);
    pushOpcode(Opcode::JumpIfFalse);
    uint32_t loopEndPos = skip4B();
    whileStmt.body->accept(*this);
    pushOpcode(Opcode::Jump);
    push4B(loopStart);
    uint32_t loopEnd = getLabel();
    set4B(loopEndPos, loopEnd);
    for(uint32_t breakPos : breakPosStack.top()){
        set4B(breakPos, loopEnd);
    }
    for(uint32_t continuePos : continuePosStack.top()){
        set4B(continuePos, loopStart);
    }
    breakPosStack.pop();
    continuePosStack.pop();
}

void Translator::visit(AST::ForStmt& forStmt) {
    breakPosStack.push({});
    continuePosStack.push({});
    curScopeMgr->enterScope();
    uint32_t valSlot = curScopeMgr->declare(forStmt.valVarName, true);
    if(!forStmt.idxVarName.empty()){
        uint32_t idxSlot = curScopeMgr->declare(forStmt.idxVarName, true);
        if(idxSlot == -1){
            REPORT_SEMANTIC_ERROR(forStmt.line, forStmt.column, "Duplicate index variable '%s' in for loop", forStmt.idxVarName.c_str());
        }
        forStmt.iterable->accept(*this); // NOTE: iterable 已经是可迭代对象
        uint32_t loopStart = getLabel();
        pushOpcode(Opcode::Dup);
        callBuiltin(Builtin::IterNext);
        pushOpcode(Opcode::Dup);
        pushOpcode(Opcode::JumpIfFalse);
        uint32_t loopEndPos = skip4B();
        pushOpcode(Opcode::Dup);
        pushOpcode(Opcode::LoadConst);
        push4B(makeConstIdx(CompileValue(0L)));
        pushOpcode(Opcode::IndexGet);
        pushOpcode(Opcode::StoreVar);
        push4B(idxSlot);
        pushOpcode(Opcode::LoadConst);
        push4B(makeConstIdx(CompileValue(1L)));
        pushOpcode(Opcode::IndexGet);
        pushOpcode(Opcode::StoreVar);
        push4B(valSlot);
        forStmt.body->accept(*this);
        pushOpcode(Opcode::Jump);
        push4B(loopStart);
        uint32_t loopEnd = getLabel();
        pushOpcode(Opcode::Pop);
        set4B(loopEndPos, loopEnd);
        for(uint32_t breakPos : breakPosStack.top()){
            set4B(breakPos, loopEnd);
        }
        for(uint32_t continuePos : continuePosStack.top()){
            set4B(continuePos, loopStart);
        }
        breakPosStack.pop();
        continuePosStack.pop();
    } else {
        forStmt.iterable->accept(*this);
        uint32_t loopStart = getLabel();
        pushOpcode(Opcode::Dup);
        callBuiltin(Builtin::IterNext);
        pushOpcode(Opcode::Dup);
        pushOpcode(Opcode::JumpIfFalse);
        uint32_t loopEndPos = skip4B();
        pushOpcode(Opcode::StoreVar);
        push4B(valSlot);
        forStmt.body->accept(*this);
        pushOpcode(Opcode::Jump);
        push4B(loopStart);
        uint32_t loopEnd = getLabel();
        pushOpcode(Opcode::Pop);
        set4B(loopEndPos, loopEnd);
        for(uint32_t breakPos : breakPosStack.top()){
            set4B(breakPos, loopEnd);
        }
        for(uint32_t continuePos : continuePosStack.top()){
            set4B(continuePos, loopStart);
        }
        breakPosStack.pop();
        continuePosStack.pop();
    }
    curScopeMgr->exitScope();
}

void Translator::visit(AST::ReturnStmt& returnStmt) {
    if(returnStmt.value){
        returnStmt.value->accept(*this);
    } else {
        pushOpcode(Opcode::LoadConst);
        push4B(makeConstIdx(CompileValue()));
    }
    pushOpcode(Opcode::Ret);
}

void Translator::visit(AST::BreakStmt& breakStmt) {
    if(breakPosStack.empty()){
        REPORT_SEMANTIC_ERROR(breakStmt.line, breakStmt.column, "Break statement not within a loop");
    }
    pushOpcode(Opcode::Jump);
    uint32_t breakPos = skip4B();
    breakPosStack.top().push_back(breakPos);
}

void Translator::visit(AST::ContinueStmt& continueStmt) {
    if(continuePosStack.empty()){
        REPORT_SEMANTIC_ERROR(continueStmt.line, continueStmt.column, "Continue statement not within a loop");
    }
    pushOpcode(Opcode::Jump);
    uint32_t continuePos = skip4B();
    continuePosStack.top().push_back(continuePos);
}

void Translator::visit(AST::ConditionalExp& conditionalExp) {
    conditionalExp.cond->accept(*this);
    pushOpcode(Opcode::JumpIfFalse);
    uint32_t elsePos = skip4B();
    conditionalExp.thenBranch->accept(*this);
    pushOpcode(Opcode::Jump);
    uint32_t endPos = skip4B();
    set4B(elsePos, getLabel());
    conditionalExp.elseBranch->accept(*this);
    set4B(endPos, getLabel());
}

void Translator::visit(AST::BinaryExp& binaryExp) {
    if(binaryExp.op == AST::BinaryExp::Op::And){
        // NOTE: 表达式结果可能不是布尔类型, 但应该不重要
        binaryExp.left->accept(*this);
        pushOpcode(Opcode::Dup);
        pushOpcode(Opcode::JumpIfFalse);
        uint32_t endPos = skip4B();
        pushOpcode(Opcode::Pop);
        binaryExp.right->accept(*this);
        set4B(endPos, getLabel());
        return;
    } else if(binaryExp.op == AST::BinaryExp::Op::Or){
        binaryExp.left->accept(*this);
        pushOpcode(Opcode::Dup);
        pushOpcode(Opcode::JumpIfTrue);
        uint32_t endPos = skip4B();
        pushOpcode(Opcode::Pop);
        binaryExp.right->accept(*this);
        set4B(endPos, getLabel());
        return;
    }else{
        binaryExp.left->accept(*this);
        binaryExp.right->accept(*this);
        switch (binaryExp.op) {
            case AST::BinaryExp::Op::Add: pushOpcode(Opcode::Add); break;
            case AST::BinaryExp::Op::Sub: pushOpcode(Opcode::Sub); break;
            case AST::BinaryExp::Op::Mul: pushOpcode(Opcode::Mul); break;
            case AST::BinaryExp::Op::Div: pushOpcode(Opcode::Div); break;
            case AST::BinaryExp::Op::Mod: pushOpcode(Opcode::Mod); break;
            case AST::BinaryExp::Op::Eq: pushOpcode(Opcode::Eq); break;
            case AST::BinaryExp::Op::Neq: pushOpcode(Opcode::Neq); break;
            case AST::BinaryExp::Op::Lt: pushOpcode(Opcode::Lt); break;
            case AST::BinaryExp::Op::Gt: pushOpcode(Opcode::Gt); break;
            case AST::BinaryExp::Op::Leq: pushOpcode(Opcode::Le); break;
            case AST::BinaryExp::Op::Geq: pushOpcode(Opcode::Ge); break;
            case AST::BinaryExp::Op::BitAnd: pushOpcode(Opcode::BitAnd); break;
            case AST::BinaryExp::Op::BitOr: pushOpcode(Opcode::BitOr); break;
            case AST::BinaryExp::Op::BitXor: pushOpcode(Opcode::BitXor); break;
            case AST::BinaryExp::Op::Shl: pushOpcode(Opcode::Shl); break;
            case AST::BinaryExp::Op ::Shr: pushOpcode(Opcode::Shr); break;
            default: break;
        }
    }
}

void Translator::visit(AST::UnaryExp& unaryExp) {
    unaryExp.operand->accept(*this);
    switch (unaryExp.op) {
        case AST::UnaryExp::Op::Neg: pushOpcode(Opcode::Neg); break;
        case AST::UnaryExp::Op::Not: pushOpcode(Opcode::Not); break;
        case AST::UnaryExp::Op::BitNot: pushOpcode(Opcode::BitNot); break;
        default: break;
    }
}

void Translator::visit(AST::CallExp& callExp) {
    if(callExp.caller){
        if(!nameSpace.empty()){
            for(auto& arg : callExp.args){
                arg->accept(*this);
            }
            callExp.caller->accept(*this);
            // global function
            pushOpcode(Opcode::LoadGlobal);
            uint32_t pos = skip4B();
            pushOpcode(Opcode::Call);
            // static method or global function
            std::string funcName = nameSpace.className.empty() ? callExp.funcName : makeGlobalSymbol(nameSpace.className, callExp.funcName);
            std::string moduleName = nameSpace.moduleAlias.empty() ? "" : aliasToModule[nameSpace.moduleAlias];
            recordGlobalSym(funcName, moduleName, pos);
        }else{
            // method call
            for(auto& arg : callExp.args){
                arg->accept(*this);
            }
            callExp.caller->accept(*this);
            uint32_t funcIdx = makeConstIdx(CompileValue(callExp.funcName));
            pushOpcode(Opcode::CallMethod);
            push4B(funcIdx);
        }
    } else {
        // normal function call
        for(auto& arg : callExp.args){
            arg->accept(*this);
        }
        pushOpcode(Opcode::LoadGlobal);
        uint32_t pos = skip4B();
        pushOpcode(Opcode::Call);
        recordGlobalSym(callExp.funcName, "", pos);
    }
}

void Translator::visit(AST::MemberAccessExp& memberAccessExp) {
    memberAccessExp.object->accept(*this);
    if(nameSpace.empty()){
        pushOpcode(Opcode::GetField);
        uint32_t nameIdx = makeConstIdx(CompileValue(memberAccessExp.member));
        push4B(nameIdx);
    } else {
        pushOpcode(Opcode::GetField);
        uint32_t pos = skip4B();
        std::string className = nameSpace.className.empty() ? memberAccessExp.member : makeGlobalSymbol(nameSpace.className, memberAccessExp.member);
        recordGlobalSym(className, nameSpace.moduleAlias.empty() ? "" : aliasToModule[nameSpace.moduleAlias], pos);
    }
}

} // namespace Zeta
