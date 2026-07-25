#include "translate.h"

#include "bytecode.h"
#include "ast.h"
#include "error.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

// TODO: 需要做优化

// NOTE: 模块别名和全局符号名可能冲突, 行为: 优先模块别名

// TODO: 构造函数就是将类名作为函数名进行调用, 返回一个实例; 放到运行期处理: 遇到函数调用, 发现栈顶是类, 则调用方法表中的构造函数.
// 函数都会返回一个值, 没有返回值的函数自动返回 Null. 构造函数显式返回this, 否则构造出的实例可能保存不下来, 编译器暂时不做检查.
// 为运行期实现的方便, 每个函数生成的字节码最后一个指令一定是Ret指令, 栈顶为Null, 保证函数返回

// TODO: 加入警告

// TODO: 检查代码生成策略是否保证了栈平衡

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

void ScopeManager::reset(){
    scopes.clear();
    savedNextIndices.clear();
    nextIndex = 0;
    maxIndex = 0;
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
    for(auto& dec : program.decs) {
        switch(dec->type) {
            case AST::Dec::DecType::Var:
                module->globalSyms[static_cast<AST::VarDec*>(dec.get())->name] = {};
                break;
            case AST::Dec::DecType::Func:
                module->globalSyms[static_cast<AST::FuncDec*>(dec.get())->name] = {};
                break;
            case AST::Dec::DecType::Class:
                module->globalSyms[static_cast<AST::ClassDec*>(dec.get())->name] = {};
                break;
            default:
                assert(false);
        }
    }
    for (auto& dec : program.decs) {
        dec->accept(*this);
    }
}

void Translator::visit(AST::Import& import) {
    if(!import.alias.empty()){
        if(aliasToModule.find(import.alias) != aliasToModule.end()){
            REPORT_SEMANTIC_ERROR(import.line, import.column, "Duplicate alias '{}'", import.alias);
        }
        aliasToModule[import.alias] = import.path;
    }
    if(std::find(module->imports.begin(), module->imports.end(), import.path) != module->imports.end()){
        REPORT_SEMANTIC_ERROR(import.line, import.column, "Redundant import '{}'", import.path);
    } else {
        module->imports.push_back(import.path);
    }
}

void Translator::visit(AST::VarDec& varDec) {
    if(!curFunc){
        auto& varSym = module->globalSyms[varDec.name];
        if(varSym.valid){
            REPORT_SEMANTIC_ERROR(varDec.line, varDec.column, "Duplicate global variable '{}'", varDec.name);
        }
        varSym.valid = true;
        varSym.isMutable = varDec.isMutable;
        std::unique_ptr<CompileValue> value = nullptr;
        if(varDec.init){
            if(varDec.init->type == AST::Exp::ExpType::Literal){
                value = getValue(static_cast<AST::LiteralExp*>(varDec.init.get()));
            }else{
                REPORT_SEMANTIC_ERROR(varDec.line, varDec.column, "Initializer for global variable '{}' must be a constant expression", varDec.name);
            }
        }
        varSym.initValue = std::move(*value);
    } else {
        int index = curFunc->scopeMgr.declare(varDec.name, varDec.isMutable);
        if(index == -1){
            REPORT_SEMANTIC_ERROR(varDec.line, varDec.column, "Duplicate local variable '{}'", varDec.name);
        }
        if(varDec.init){
            varDec.init->accept(*this);
            pushOpcode(Opcode::StoreVar);
            push4B(index);
        }
    }
}

void Translator::visit(AST::FuncDec& funcDec) {
    auto& funcSym = module->globalSyms[funcDec.name];
    if(findBuiltin(funcDec.name)){
        REPORT_SEMANTIC_ERROR(funcDec.line, funcDec.column, "Function name '{}' conflicts with a built-in function", funcDec.name);
    }
    if(funcSym.valid){
        REPORT_SEMANTIC_ERROR(funcDec.line, funcDec.column, "Duplicate global function '{}'", funcDec.name);
    }
    funcSym.valid = true;
    funcSym.isMutable = false;
    uint32_t protoIdx = compileFunctionProto(funcDec.params, funcDec.body.get(), false);
    funcSym.initValue = CompileValue(new CompileFunction{protoIdx});
}

void Translator::visit(AST::ClassDec& classDec) {
    auto& classSym = module->globalSyms[classDec.name];
    if(classSym.valid){
        REPORT_SEMANTIC_ERROR(classDec.line, classDec.column, "Duplicate global class '{}'", classDec.name);
    }
    classSym.valid = true;
    classSym.isMutable = false;
    CompileClass* compileClass = new CompileClass();
    compileClass->name = classDec.name;
    compileClass->base.second = classDec.base.second;
    if(!classDec.base.first.empty()){
        auto it = aliasToModule.find(classDec.base.first);
        if(it == aliasToModule.end()){
            REPORT_SEMANTIC_ERROR(classDec.line, classDec.column, "Unknown module alias '{}'", classDec.base.first);
        }
        compileClass->base.first = it->second;
    }
    for(auto& member : classDec.members){
        if(member->type == AST::Dec::DecType::Var){
            auto* varDec = static_cast<AST::VarDec*>(member.get());
            std::unique_ptr<CompileValue> value = nullptr;
            if(!varDec->isMutable){
                REPORT_SEMANTIC_ERROR(varDec->line, varDec->column, "Instance variable '{}' in class '{}' must be mutable (use 'var' instead of 'let')", varDec->name, classDec.name);
            }
            if(varDec->init){
                if(varDec->init->type == AST::Exp::ExpType::Literal){
                    value = getValue(static_cast<AST::LiteralExp*>(varDec->init.get()));
                }else{
                    REPORT_SEMANTIC_ERROR(varDec->line, varDec->column, "Initializer for instance variable '{}' in class '{}' must be a constant expression", varDec->name, classDec.name);
                }
            }
            if(compileClass->fields.find(varDec->name) != compileClass->fields.end()){
                REPORT_SEMANTIC_ERROR(varDec->line, varDec->column, "Duplicate instance member '{}' in class '{}'", varDec->name, classDec.name);
            }
            compileClass->fields[varDec->name] = value ? std::move(*value) : CompileValue();
        } else if(member->type == AST::Dec::DecType::Func){
            auto* funcDec = static_cast<AST::FuncDec*>(member.get());
            uint32_t protoIdx = compileFunctionProto(funcDec->params, funcDec->body.get(), true);
            if(compileClass->methods.find(funcDec->name) != compileClass->methods.end()){
                REPORT_SEMANTIC_ERROR(funcDec->line, funcDec->column, "Duplicate instance member '{}' in class '{}'", funcDec->name, classDec.name);
            }
            compileClass->methods[funcDec->name] = CompileFunction{protoIdx};
        } else {
            REPORT_SEMANTIC_ERROR(member->line, member->column, "Invalid instance member in class '{}'", classDec.name);
        }
    }
    classSym.initValue = CompileValue(compileClass);
}

void Translator::visit(AST::BlockStmt& blockStmt) {
    recordLineno(blockStmt.line);
    if(blockStmt.isFuncBody){
        for(auto& stmt : blockStmt.stmts){
            stmt->accept(*this);
        }
    } else {
        curFunc->scopeMgr.enterScope();
        for(auto& stmt : blockStmt.stmts){
            stmt->accept(*this);
        }
        curFunc->scopeMgr.exitScope();
    }
}

void Translator::visit(AST::ExpStmt& expStmt) {
    recordLineno(expStmt.line);
    expStmt.exp->accept(*this);
    pushOpcode(Opcode::Pop);
}

void Translator::visit(AST::AssignStmt& assignStmt) {
    recordLineno(assignStmt.line); 
    auto pushVal = [&](){
        if(assignStmt.op == AST::AssignStmt::Op::Assign){
            assignStmt.value->accept(*this);
        }else{
            assignStmt.target->accept(*this);
            assignStmt.value->accept(*this);
            switch (assignStmt.op) {
                case AST::AssignStmt::Op::AddAssign: pushOpcode(Opcode::Add); break;
                case AST::AssignStmt::Op::SubAssign: pushOpcode(Opcode::Sub); break;
                case AST::AssignStmt::Op::MulAssign: pushOpcode(Opcode::Mul); break;
                case AST::AssignStmt::Op::DivAssign: pushOpcode(Opcode::Div); break;
                case AST::AssignStmt::Op::ModAssign: pushOpcode(Opcode::Mod); break;
                case AST::AssignStmt::Op::BitAndAssign: pushOpcode(Opcode::BitAnd); break;
                case AST::AssignStmt::Op::BitOrAssign: pushOpcode(Opcode::BitOr); break;
                case AST::AssignStmt::Op::BitXorAssign: pushOpcode(Opcode::BitXor); break;
                case AST::AssignStmt::Op::ShlAssign: pushOpcode(Opcode::Shl); break;
                case AST::AssignStmt::Op::ShrAssign: pushOpcode(Opcode::Shr); break;
                default: break;
            }
        }
    };
    
    if(assignStmt.target->type == AST::Exp::ExpType::Identifier){
        pushVal();
        auto* idExp = static_cast<AST::IdentifierExp*>(assignStmt.target.get());
        auto* varSym = curFunc->scopeMgr.resolve(idExp->name);
        if(varSym){
            if(!varSym->isMutable){
                REPORT_SEMANTIC_ERROR(assignStmt.line, assignStmt.column, "Cannot assign to immutable variable '{}'", idExp->name);
            }
            pushOpcode(Opcode::StoreVar);
            push4B(varSym->index);
        }else{
            pushOpcode(Opcode::StoreGlobal);
            uint32_t pos = skip4B();
            recordGlobalSym(idExp->name, "", pos);
        }
    } else if(assignStmt.target->type == AST::Exp::ExpType::MemberAccess){
        auto* memberAccess = static_cast<AST::MemberAccessExp*>(assignStmt.target.get());
        std::string moduleName;
        if(memberAccess->object->type == AST::Exp::ExpType::Identifier){
            auto* idExp = static_cast<AST::IdentifierExp*>(memberAccess->object.get());
            auto it = aliasToModule.find(idExp->name);
            if(it != aliasToModule.end()){
                moduleName = it->second;
            }
        }
        if(!moduleName.empty()){
            pushVal();
            pushOpcode(Opcode::StoreGlobal);
            uint32_t pos = skip4B();
            recordGlobalSym(memberAccess->member, moduleName, pos);
        } else {
            memberAccess->object->accept(*this);
            pushVal();
            uint32_t memberIdx = makeConstIdx(CompileValue(memberAccess->member));
            pushOpcode(Opcode::SetField);
            push4B(memberIdx);
        }
    } else if(assignStmt.target->type == AST::Exp::ExpType::IndexAccess){
        auto* indexAccess = static_cast<AST::IndexAccessExp*>(assignStmt.target.get());
        indexAccess->object->accept(*this);
        indexAccess->index->accept(*this);
        pushVal();
        pushOpcode(Opcode::IndexSet);
    } else {
        REPORT_SEMANTIC_ERROR(assignStmt.line, assignStmt.column, "Invalid assignment target");
    }
}

void Translator::visit(AST::VarDecStmt& varDecStmt) {
    recordLineno(varDecStmt.line);
    for(auto& varDec : varDecStmt.varDecs){
        varDec->accept(*this);
    }
}

void Translator::visit(AST::IfStmt& ifStmt) {
    recordLineno(ifStmt.line);
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
    recordLineno(whileStmt.line);
    curFunc->breakPosStack.push({});
    curFunc->continuePosStack.push({});
    uint32_t loopStart = getLabel();
    whileStmt.cond->accept(*this);
    pushOpcode(Opcode::JumpIfFalse);
    uint32_t loopEndPos = skip4B();
    whileStmt.body->accept(*this);
    pushOpcode(Opcode::Jump);
    push4B(loopStart);
    uint32_t loopEnd = getLabel();
    set4B(loopEndPos, loopEnd);
    for(uint32_t breakPos : curFunc->breakPosStack.top()){
        set4B(breakPos, loopEnd);
    }
    for(uint32_t continuePos : curFunc->continuePosStack.top()){
        set4B(continuePos, loopStart);
    }
    curFunc->breakPosStack.pop();
    curFunc->continuePosStack.pop();
}

void Translator::visit(AST::ForStmt& forStmt) {
    recordLineno(forStmt.line);
    curFunc->breakPosStack.push({});
    curFunc->continuePosStack.push({});
    curFunc->scopeMgr.enterScope();
    uint32_t valSlot = curFunc->scopeMgr.declare(forStmt.valVarName, true);
    
    forStmt.iterable->accept(*this);
    callBuiltin(Builtin::GetIter);
    uint32_t loopStart = getLabel();
    pushOpcode(Opcode::Dup);
    callBuiltin(Builtin::IterNext);
    pushOpcode(Opcode::Dup);
    pushOpcode(Opcode::StoreVar);
    push4B(valSlot);
    callBuiltin(Builtin::Check);
    pushOpcode(Opcode::JumpIfFalse);
    uint32_t loopEndPos = skip4B();
    forStmt.body->accept(*this);
    pushOpcode(Opcode::Jump);
    push4B(loopStart);
    uint32_t loopEnd = getLabel();
    pushOpcode(Opcode::Pop);
    set4B(loopEndPos, loopEnd);
    for(uint32_t breakPos : curFunc->breakPosStack.top()){
        set4B(breakPos, loopEnd);
    }
    for(uint32_t continuePos : curFunc->continuePosStack.top()){
        set4B(continuePos, loopStart);
    }
    curFunc->breakPosStack.pop();
    curFunc->continuePosStack.pop();

    curFunc->scopeMgr.exitScope();
}

void Translator::visit(AST::ReturnStmt& returnStmt) {
    recordLineno(returnStmt.line);
    if(returnStmt.value){
        returnStmt.value->accept(*this);
    } else {
        pushOpcode(Opcode::LoadConst);
        push4B(makeConstIdx(CompileValue()));
    }
    pushOpcode(Opcode::Ret);
}

void Translator::visit(AST::BreakStmt& breakStmt) {
    recordLineno(breakStmt.line);
    if(curFunc->breakPosStack.empty()){
        REPORT_SEMANTIC_ERROR(breakStmt.line, breakStmt.column, "Break statement not within a loop");
    }
    pushOpcode(Opcode::Jump);
    uint32_t breakPos = skip4B();
    curFunc->breakPosStack.top().push_back(breakPos);
}

void Translator::visit(AST::ContinueStmt& continueStmt) {
    recordLineno(continueStmt.line);
    if(curFunc->continuePosStack.empty()){
        REPORT_SEMANTIC_ERROR(continueStmt.line, continueStmt.column, "Continue statement not within a loop");
    }
    pushOpcode(Opcode::Jump);
    uint32_t continuePos = skip4B();
    curFunc->continuePosStack.top().push_back(continuePos);
}

void Translator::visit(AST::ConditionalExp& conditionalExp) {
    recordLineno(conditionalExp.line);
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
    recordLineno(binaryExp.line);
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
            case AST::BinaryExp::Op::Is: pushOpcode(Opcode::Is); break;
            case AST::BinaryExp::Op::BitAnd: pushOpcode(Opcode::BitAnd); break;
            case AST::BinaryExp::Op::BitOr: pushOpcode(Opcode::BitOr); break;
            case AST::BinaryExp::Op::BitXor: pushOpcode(Opcode::BitXor); break;
            case AST::BinaryExp::Op::Shl: pushOpcode(Opcode::Shl); break;
            case AST::BinaryExp::Op ::Shr: pushOpcode(Opcode::Shr); break;
            default: assert(false); break;
        }
    }
}

void Translator::visit(AST::UnaryExp& unaryExp) {
    recordLineno(unaryExp.line);
    unaryExp.operand->accept(*this);
    switch (unaryExp.op) {
        case AST::UnaryExp::Op::Neg: pushOpcode(Opcode::Neg); break;
        case AST::UnaryExp::Op::Not: pushOpcode(Opcode::Not); break;
        case AST::UnaryExp::Op::BitNot: pushOpcode(Opcode::BitNot); break;
        default: break;
    }
}

void Translator::visit(AST::CallExp& callExp) {
    recordLineno(callExp.line);
    if(callExp.caller){
        std::string moduleName;
        if(callExp.caller->type == AST::Exp::ExpType::Identifier){
            auto* idExp = static_cast<AST::IdentifierExp*>(callExp.caller.get());
            auto it = aliasToModule.find(idExp->name);
            if(it != aliasToModule.end()){
                moduleName = it->second;
            }
        }
        if(!moduleName.empty()){
            for(auto& arg : callExp.args){
                arg->accept(*this);
            }
            pushOpcode(Opcode::LoadGlobal);
            uint32_t pos = skip4B();
            pushOpcode(Opcode::Call);
            push1B(static_cast<uint8_t>(callExp.args.size()));
            recordGlobalSym(callExp.funcName, moduleName, pos);
        }else{
            // method call
            for(auto& arg : callExp.args){
                arg->accept(*this);
            }
            callExp.caller->accept(*this);
            uint32_t funcIdx = makeConstIdx(CompileValue(callExp.funcName));
            pushOpcode(Opcode::CallMethod);
            push1B(static_cast<uint8_t>(callExp.args.size()));
            push4B(funcIdx);
        }
    } else {
        // normal function call
        for(auto& arg : callExp.args){
            arg->accept(*this);
        }
        if(const BuiltinDesc* builtin = findBuiltin(callExp.funcName)){
            // built-in function call
            pushOpcode(Opcode::CallBuiltin);
            push1B(static_cast<uint8_t>(builtin->id));
            return;
        }
        auto* funcSym = curFunc->scopeMgr.resolve(callExp.funcName);
        if(funcSym){
            pushOpcode(Opcode::LoadVar);
            push4B(funcSym->index);
            pushOpcode(Opcode::Call);
            push1B(static_cast<uint8_t>(callExp.args.size()));
        } else {
            pushOpcode(Opcode::LoadGlobal);
            uint32_t pos = skip4B();
            pushOpcode(Opcode::Call);
            push1B(static_cast<uint8_t>(callExp.args.size()));
            recordGlobalSym(callExp.funcName, "", pos);
        }
    }
}

void Translator::visit(AST::SuperCallExp& superCallExp) {
    recordLineno(superCallExp.line);
    if(!curFunc->inMethod){
        REPORT_SEMANTIC_ERROR(superCallExp.line, superCallExp.column, "super() can only be used in methods");
    }
    for(auto& arg : superCallExp.args){
        arg->accept(*this);
    }
    pushOpcode(Opcode::LoadVar);
    push4B(0); // "this" is always at slot 0
    uint32_t funcIdx = makeConstIdx(CompileValue(superCallExp.methodName));
    pushOpcode(Opcode::SuperCall);
    push1B(static_cast<uint8_t>(superCallExp.args.size()));
    push4B(funcIdx);
}

void Translator::visit(AST::MemberAccessExp& memberAccessExp) {
    recordLineno(memberAccessExp.line);
    std::string moduleName;
    if(memberAccessExp.object->type == AST::Exp::ExpType::Identifier){
        auto* idExp = static_cast<AST::IdentifierExp*>(memberAccessExp.object.get());
        auto it = aliasToModule.find(idExp->name);
        if(it != aliasToModule.end()){
            moduleName = it->second;
        }
    }
    if(!moduleName.empty()){
        pushOpcode(Opcode::LoadGlobal);
        uint32_t pos = skip4B();
        recordGlobalSym(memberAccessExp.member, moduleName, pos);
    } else {
        memberAccessExp.object->accept(*this);
        uint32_t memberIdx = makeConstIdx(CompileValue(memberAccessExp.member));
        pushOpcode(Opcode::GetField);
        push4B(memberIdx);
    }
}

void Translator::visit(AST::IndexAccessExp& indexAccessExp) {
    recordLineno(indexAccessExp.line);
    indexAccessExp.object->accept(*this);
    indexAccessExp.index->accept(*this);
    pushOpcode(Opcode::IndexGet);
}

void Translator::visit(AST::IdentifierExp& identifierExp) {
    recordLineno(identifierExp.line);
    auto* varSym = curFunc->scopeMgr.resolve(identifierExp.name);
    if(varSym){
        pushOpcode(Opcode::LoadVar);
        push4B(varSym->index);
    }else{
        pushOpcode(Opcode::LoadGlobal);
        uint32_t pos = skip4B();
        recordGlobalSym(identifierExp.name, "", pos);
    }
}

void Translator::visit(AST::ThisExp& thisExp) {
    recordLineno(thisExp.line);
    pushOpcode(Opcode::LoadVar);
    push4B(0); // "this" is always at slot 0
    if(!curFunc->inMethod){
        REPORT_SEMANTIC_ERROR(thisExp.line, thisExp.column, "'this' can only be used in methods");
    }
}

void Translator::visit(AST::ArrayExp& arrayExp) {
    recordLineno(arrayExp.line);
    pushOpcode(Opcode::LoadConst);
    push4B(makeConstIdx(CompileValue((int64_t)arrayExp.elements.size())));
    callBuiltin(Builtin::NewArray);
    for(int i = 0; i < arrayExp.elements.size(); ++i){
        pushOpcode(Opcode::Dup);
        pushOpcode(Opcode::LoadConst);
        push4B(makeConstIdx(CompileValue((int64_t)i)));
        arrayExp.elements[i]->accept(*this);
        pushOpcode(Opcode::IndexSet);
    }
}

void Translator::visit(AST::MapExp& mapExp) {
    recordLineno(mapExp.line);
    pushOpcode(Opcode::LoadConst);
    push4B(makeConstIdx(CompileValue((int64_t)mapExp.entries.size())));
    callBuiltin(Builtin::NewMap);
    for(auto& pair : mapExp.entries){
        pushOpcode(Opcode::Dup);
        pushOpcode(Opcode::LoadConst);
        push4B(makeConstIdx(CompileValue(pair.first)));
        pair.second->accept(*this);
        pushOpcode(Opcode::IndexSet);
    }
}

void Translator::visit(AST::IntLitExp& intLitExp) {
    recordLineno(intLitExp.line);
    pushOpcode(Opcode::LoadConst);
    push4B(makeConstIdx(CompileValue(intLitExp.value)));
}

void Translator::visit(AST::FloatLitExp& floatLitExp) {
    recordLineno(floatLitExp.line);
    pushOpcode(Opcode::LoadConst);
    push4B(makeConstIdx(CompileValue(floatLitExp.value)));
}

void Translator::visit(AST::StrLitExp& strLitExp) {
    recordLineno(strLitExp.line);
    pushOpcode(Opcode::LoadConst);
    push4B(makeConstIdx(CompileValue(strLitExp.value)));
}

void Translator::visit(AST::BoolLitExp& boolLitExp) {
    recordLineno(boolLitExp.line);
    pushOpcode(Opcode::LoadConst);
    push4B(makeConstIdx(CompileValue(boolLitExp.value)));
}

void Translator::visit(AST::NullLitExp& nullLitExp) {
    recordLineno(nullLitExp.line);
    pushOpcode(Opcode::LoadConst);
    push4B(makeConstIdx(CompileValue()));
}

void Translator::visit(AST::ArrayLitExp& arrayLitExp) {
    recordLineno(arrayLitExp.line);
    std::vector<CompileValue>* arr = new std::vector<CompileValue>();
    for(auto& elem : arrayLitExp.elements){
        auto elemVal = getValue(elem.get());
        arr->emplace_back(std::move(*elemVal));
    }
    pushOpcode(Opcode::LoadConst);
    push4B(makeConstIdx(CompileValue(arr)));
}

void Translator::visit(AST::MapLitExp& mapLitExp) {
    recordLineno(mapLitExp.line);
    std::vector<std::pair<std::string, CompileValue>>* m = new std::vector<std::pair<std::string, CompileValue>>();
    for(auto& pair : mapLitExp.entries){
        auto val = getValue(pair.second.get());
        m->emplace_back(pair.first, std::move(*val));
    }
    pushOpcode(Opcode::LoadConst);
    push4B(makeConstIdx(CompileValue(m)));
}

void Translator::visit(AST::FuncLitExp& funcLitExp) {
    recordLineno(funcLitExp.line);
    uint32_t protoIdx = compileFunctionProto(funcLitExp.params, funcLitExp.body.get(), false);
    pushOpcode(Opcode::LoadConst);
    push4B(makeConstIdx(CompileValue(new CompileFunction{protoIdx})));
}

std::unique_ptr<CompileValue> Translator::getValue(const AST::LiteralExp* exp) {
    switch(exp->type) {
        case AST::LiteralExp::LiteralType::Int: return std::make_unique<CompileValue>(static_cast<const AST::IntLitExp*>(exp)->value);
        case AST::LiteralExp::LiteralType::Float: return std::make_unique<CompileValue>(static_cast<const AST::FloatLitExp*>(exp)->value);
        case AST::LiteralExp::LiteralType::Str: return std::make_unique<CompileValue>(static_cast<const AST::StrLitExp*>(exp)->value);
        case AST::LiteralExp::LiteralType::Bool: return std::make_unique<CompileValue>(static_cast<const AST::BoolLitExp*>(exp)->value);
        case AST::LiteralExp::LiteralType::Null: return std::make_unique<CompileValue>();
        case AST::LiteralExp::LiteralType::Array: {
            auto arrExp = static_cast<const AST::ArrayLitExp*>(exp);
            std::vector<CompileValue>* arr = new std::vector<CompileValue>();
            for(auto& elem : arrExp->elements){
                auto elemVal = getValue(elem.get());
                arr->emplace_back(std::move(*elemVal));
            }
            return std::make_unique<CompileValue>(arr);
        }
        case AST::LiteralExp::LiteralType::Map: {
            auto mapExp = static_cast<const AST::MapLitExp*>(exp);
            std::vector<std::pair<std::string, CompileValue>>* m = new std::vector<std::pair<std::string, CompileValue>>();
            for(auto& pair : mapExp->entries){
                auto val = getValue(pair.second.get());
                m->emplace_back(pair.first, std::move(*val));
            }
            return std::make_unique<CompileValue>(m);
        }
        case AST::LiteralExp::LiteralType::Func: {
            auto funcExp = static_cast<const AST::FuncLitExp*>(exp);
            uint32_t protoIdx = compileFunctionProto(funcExp->params, funcExp->body.get(), false);
            return std::make_unique<CompileValue>(new CompileFunction{protoIdx});
        }
        default: return nullptr;
    }
}

uint32_t Translator::compileFunctionProto(const std::vector<std::string>& params, AST::BlockStmt* body, bool isMethod) {
    std::unique_ptr<FuncState> savedFunc = std::move(curFunc);
    curFunc = std::make_unique<FuncState>();
    module->protos.push_back(std::make_unique<Proto>());
    curFunc->proto = module->protos.back().get();
    curFunc->proto->index = module->protos.size() - 1;
    curFunc->proto->arity = params.size() + (isMethod ? 1 : 0);
    curFunc->inMethod = isMethod;
    curFunc->scopeMgr.reset();
    curFunc->scopeMgr.enterScope();
    if(isMethod){
        curFunc->scopeMgr.declare("this", false);
    }
    for(auto& paramName : params){
        if(curFunc->scopeMgr.declare(paramName, true) == -1){
            REPORT_SEMANTIC_ERROR(body->line, body->column, "Duplicate parameter name '{}' in function literal", paramName);
        }
    }
    body->accept(*this);
    pushOpcode(Opcode::LoadConst);
    push4B(makeConstIdx(CompileValue()));
    pushOpcode(Opcode::Ret);
    curFunc->scopeMgr.exitScope();
    curFunc->proto->localCount = curFunc->scopeMgr.getMaxIndex() + 1;
    curFunc->proto->maxStackSize = calcMaxStackSize(curFunc->proto->bytecode);
    uint32_t protoIdx = curFunc->proto->index;
    curFunc = std::move(savedFunc);
    return protoIdx;
}

int Translator::calcMaxStackSize(const std::vector<uint8_t>& bytecode) {
    // {
    //     std::cout << "====================\n";
    //     printBytecode(bytecode);
    //     std::cout << "====================\n";
    // }
    
    if (bytecode.empty()) return 0;

    std::unordered_map<uint32_t, int> stackDepths;
    std::vector<std::pair<uint32_t, int>> worklist;

    auto addToWorklist = [&](uint32_t pc, int depth) {
        auto it = stackDepths.find(pc);
        if (it == stackDepths.end()) {
            stackDepths[pc] = depth;
            worklist.push_back({pc, depth});
        } else {
            assert(it->second == depth);
        }
    };

    worklist.push_back({0, 0});
    stackDepths[0] = 0;

    int maxStack = 0;

    while (!worklist.empty()) {
        auto [pc, depth] = worklist.back();
        worklist.pop_back();

        if (depth > maxStack) maxStack = depth;
        if (pc >= bytecode.size()) continue;

        Opcode op = static_cast<Opcode>(bytecode[pc]);
        int delta = 0;
        uint32_t size = 1;
        bool hasFallthrough = true;

        switch (op) {
            case Opcode::LoadConst:
            case Opcode::LoadGlobal:
            case Opcode::LoadVar:
                delta = 1;
                size = 5;
                break;
            case Opcode::StoreGlobal:
            case Opcode::StoreVar:
                delta = -1;
                size = 5;
                break;
            case Opcode::Add:
            case Opcode::Sub:
            case Opcode::Mul:
            case Opcode::Div:
            case Opcode::Mod:
            case Opcode::BitAnd:
            case Opcode::BitOr:
            case Opcode::BitXor:
            case Opcode::Shl:
            case Opcode::Shr:
            case Opcode::Eq:
            case Opcode::Neq:
            case Opcode::Lt:
            case Opcode::Gt:
            case Opcode::Le:
            case Opcode::Ge:
            case Opcode::Is:
                delta = -1;
                size = 1;
                break;
            case Opcode::Neg:
            case Opcode::Not:
            case Opcode::BitNot:
                delta = 0;
                size = 1;
                break;
            case Opcode::Jump: {
                delta = 0;
                size = 5;
                hasFallthrough = false;
                uint32_t target;
                std::memcpy(&target, &bytecode[pc + 1], 4);
                addToWorklist(target, depth);
                break;
            }
            case Opcode::JumpIfFalse:
            case Opcode::JumpIfTrue: {
                delta = -1;
                size = 5;
                uint32_t target;
                std::memcpy(&target, &bytecode[pc + 1], 4);
                int newDepth = depth + delta;
                addToWorklist(pc + size, newDepth);
                addToWorklist(target, newDepth);
                hasFallthrough = false;
                break;
            }
            case Opcode::Ret:
                delta = -1;
                size = 1;
                hasFallthrough = false;
                assert(depth + delta == 0);
                break;
            case Opcode::Call: {
                uint8_t argCount = bytecode[pc + 1];
                delta = -static_cast<int>(argCount);
                size = 2;
                break;
            }
            case Opcode::GetField:
                delta = 0;
                size = 5;
                break;
            case Opcode::SetField:
                delta = -2;
                size = 5;
                break;
            case Opcode::CallMethod: {
                uint8_t argCount = bytecode[pc + 1];
                delta = -static_cast<int>(argCount);
                size = 6;
                break;
            }
            case Opcode::SuperCall: {
                uint8_t argCount = bytecode[pc + 1];
                delta = -static_cast<int>(argCount);
                size = 6;
                break;
            }
            case Opcode::IndexGet:
                delta = -1;
                size = 1;
                break;
            case Opcode::IndexSet:
                delta = -3;
                size = 1;
                break;
            case Opcode::Pop:
                delta = -1;
                size = 1;
                break;
            case Opcode::Dup:
                delta = 1;
                size = 1;
                break;
            case Opcode::CallBuiltin: {
                uint8_t id = bytecode[pc + 1];
                delta = builtinTable[id].stackDelta;
                size = 2;
                break;
            }
            case Opcode::Nop:
                delta = 0;
                size = 1;
                break;
            case Opcode::Halt:
                delta = 0;
                size = 1;
                hasFallthrough = false;
                break;
            default:
                delta = 0;
                size = 1;
                break;
        }

        if (hasFallthrough) {
            int newDepth = depth + delta;
            addToWorklist(pc + size, newDepth);
        }
    }

    return maxStack;
}

} // namespace Zeta
