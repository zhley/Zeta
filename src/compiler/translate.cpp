#include "translate.h"

#include "bytecode.h"
#include "ast.h"
#include "error.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

// TODO: 需要做优化
// TODO: 计算最大操作数栈深度. 需要做数据流分析, 可以顺便把优化做了

// NOTE: 模块别名和全局符号名可能冲突, 行为: 优先模块别名

// TODO: 构造函数特殊处理, 将其变成返回一个实例的全局函数; 放到运行期处理: 遇到函数调用, 发现栈顶是类, 则调用构造函数.

// TODO: 加入警告

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
    if(std::find(module->imports.begin(), module->imports.end(), import.path) != module->imports.end()){
        REPORT_SEMANTIC_ERROR(import.line, import.column, "Redundant import '%s'", import.path.c_str());
    } else {
        module->imports.push_back(import.path);
    }
}

void Translator::visit(AST::VarDec& varDec) {
    if(curFunc->scopeMgr.empty()){
        if(module->globalSyms.find(varDec.name) != module->globalSyms.end()){
            REPORT_SEMANTIC_ERROR(varDec.line, varDec.column, "Duplicate global variable '%s'", varDec.name.c_str());
        }
        std::unique_ptr<CompileValue> value = nullptr;
        if(varDec.init){
            if(varDec.init->type == AST::Exp::ExpType::Literal){
                value = getValue(static_cast<AST::LiteralExp*>(varDec.init.get()));
            }else{
                REPORT_SEMANTIC_ERROR(varDec.line, varDec.column, "Initializer for global variable '%s' must be a constant expression", varDec.name.c_str());
            }
        }
        module->globalSyms[varDec.name] = {varDec.isMutable, std::move(*value)};
    } else {
        int index = curFunc->scopeMgr.declare(varDec.name, varDec.isMutable);
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
    uint32_t protoIdx = compileFunctionProto(funcDec.params, funcDec.body.get(), false);
    module->globalSyms[funcDec.name] = {false, CompileValue(new CompileFunction{protoIdx})};
}

void Translator::visit(AST::ClassDec& classDec) {
    if(module->globalSyms.find(classDec.name) != module->globalSyms.end()){
        REPORT_SEMANTIC_ERROR(classDec.line, classDec.column, "Duplicate global class '%s'", classDec.name.c_str());
    }
    CompileClass* compileClass = new CompileClass();
    compileClass->name = classDec.name;
    compileClass->base = classDec.base;
    for(auto& member : classDec.members){
        if(member->type == AST::Dec::DecType::Var){
            auto* varDec = static_cast<AST::VarDec*>(member.get());
            std::unique_ptr<CompileValue> value = nullptr;
            if(varDec->init){
                if(varDec->init->type == AST::Exp::ExpType::Literal){
                    value = getValue(static_cast<AST::LiteralExp*>(varDec->init.get()));
                }else{
                    REPORT_SEMANTIC_ERROR(varDec->line, varDec->column, "Initializer for instance variable '%s' in class '%s' must be a constant expression", varDec->name.c_str(), classDec.name.c_str());
                }
            }
            if(compileClass->fields.find(varDec->name) != compileClass->fields.end()){
                REPORT_SEMANTIC_ERROR(varDec->line, varDec->column, "Duplicate instance member '%s' in class '%s'", varDec->name.c_str(), classDec.name.c_str());
            }
            compileClass->fields[varDec->name] = std::move(*value);
        } else if(member->type == AST::Dec::DecType::Func){
            auto* funcDec = static_cast<AST::FuncDec*>(member.get());
            uint32_t protoIdx = compileFunctionProto(funcDec->params, funcDec->body.get(), true);
            if(compileClass->methods.find(funcDec->name) != compileClass->methods.end()){
                REPORT_SEMANTIC_ERROR(funcDec->line, funcDec->column, "Duplicate instance member '%s' in class '%s'", funcDec->name.c_str(), classDec.name.c_str());
            }
            compileClass->methods[funcDec->name] = CompileFunction{protoIdx};
        } else {
            REPORT_SEMANTIC_ERROR(member->line, member->column, "Invalid instance member in class '%s'", classDec.name.c_str());
        }
    }
    module->globalSyms[classDec.name] = {false, CompileValue(compileClass)};
}

void Translator::visit(AST::BlockStmt& blockStmt) {
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
    curFunc->breakPosStack.push({});
    curFunc->continuePosStack.push({});
    curFunc->scopeMgr.enterScope();
    uint32_t valSlot = curFunc->scopeMgr.declare(forStmt.valVarName, true);
    if(!forStmt.idxVarName.empty()){
        uint32_t idxSlot = curFunc->scopeMgr.declare(forStmt.idxVarName, true);
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
        for(uint32_t breakPos : curFunc->breakPosStack.top()){
            set4B(breakPos, loopEnd);
        }
        for(uint32_t continuePos : curFunc->continuePosStack.top()){
            set4B(continuePos, loopStart);
        }
        curFunc->breakPosStack.pop();
        curFunc->continuePosStack.pop();
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
        for(uint32_t breakPos : curFunc->breakPosStack.top()){
            set4B(breakPos, loopEnd);
        }
        for(uint32_t continuePos : curFunc->continuePosStack.top()){
            set4B(continuePos, loopStart);
        }
        curFunc->breakPosStack.pop();
        curFunc->continuePosStack.pop();
    }
    curFunc->scopeMgr.exitScope();
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
    if(curFunc->breakPosStack.empty()){
        REPORT_SEMANTIC_ERROR(breakStmt.line, breakStmt.column, "Break statement not within a loop");
    }
    pushOpcode(Opcode::Jump);
    uint32_t breakPos = skip4B();
    curFunc->breakPosStack.top().push_back(breakPos);
}

void Translator::visit(AST::ContinueStmt& continueStmt) {
    if(curFunc->continuePosStack.empty()){
        REPORT_SEMANTIC_ERROR(continueStmt.line, continueStmt.column, "Continue statement not within a loop");
    }
    pushOpcode(Opcode::Jump);
    uint32_t continuePos = skip4B();
    curFunc->continuePosStack.top().push_back(continuePos);
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
            recordGlobalSym(callExp.funcName, moduleName, pos);
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
        auto* funcSym = curFunc->scopeMgr.resolve(callExp.funcName);
        if(funcSym){
            pushOpcode(Opcode::LoadVar);
            push4B(funcSym->index);
            pushOpcode(Opcode::Call);
        } else {
            pushOpcode(Opcode::LoadGlobal);
            uint32_t pos = skip4B();
            pushOpcode(Opcode::Call);
            recordGlobalSym(callExp.funcName, "", pos);
        }
    }
}

void Translator::visit(AST::MemberAccessExp& memberAccessExp) {
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
    indexAccessExp.object->accept(*this);
    indexAccessExp.index->accept(*this);
    pushOpcode(Opcode::IndexGet);
}

void Translator::visit(AST::AssignExp& assignExp) { 
    if(assignExp.op == AST::AssignExp::Op::Assign){
        assignExp.value->accept(*this);
    }else{
        assignExp.target->accept(*this);
        assignExp.value->accept(*this);
        switch (assignExp.op) {
            case AST::AssignExp::Op::AddAssign: pushOpcode(Opcode::Add); break;
            case AST::AssignExp::Op::SubAssign: pushOpcode(Opcode::Sub); break;
            case AST::AssignExp::Op::MulAssign: pushOpcode(Opcode::Mul); break;
            case AST::AssignExp::Op::DivAssign: pushOpcode(Opcode::Div); break;
            case AST::AssignExp::Op::ModAssign: pushOpcode(Opcode::Mod); break;
            case AST::AssignExp::Op::BitAndAssign: pushOpcode(Opcode::BitAnd); break;
            case AST::AssignExp::Op::BitOrAssign: pushOpcode(Opcode::BitOr); break;
            case AST::AssignExp::Op::BitXorAssign: pushOpcode(Opcode::BitXor); break;
            case AST::AssignExp::Op::ShlAssign: pushOpcode(Opcode::Shl); break;
            case AST::AssignExp::Op::ShrAssign: pushOpcode(Opcode::Shr); break;
            default: break;
        }
    }
    if(assignExp.target->type == AST::Exp::ExpType::Identifier){
        auto* idExp = static_cast<AST::IdentifierExp*>(assignExp.target.get());
        auto* varSym = curFunc->scopeMgr.resolve(idExp->name);
        if(varSym){
            if(!varSym->isMutable){
                REPORT_SEMANTIC_ERROR(assignExp.line, assignExp.column, "Cannot assign to immutable variable '%s'", idExp->name.c_str());
            }
            pushOpcode(Opcode::StoreVar);
            push4B(varSym->index);
        }else{
            pushOpcode(Opcode::StoreGlobal);
            uint32_t pos = skip4B();
            recordGlobalSym(idExp->name, "", pos);
        }
    } else if(assignExp.target->type == AST::Exp::ExpType::MemberAccess){
        auto* memberAccess = static_cast<AST::MemberAccessExp*>(assignExp.target.get());
        std::string moduleName;
        if(memberAccess->object->type == AST::Exp::ExpType::Identifier){
            auto* idExp = static_cast<AST::IdentifierExp*>(memberAccess->object.get());
            auto it = aliasToModule.find(idExp->name);
            if(it != aliasToModule.end()){
                moduleName = it->second;
            }
        }
        if(!moduleName.empty()){
            pushOpcode(Opcode::StoreGlobal);
            uint32_t pos = skip4B();
            recordGlobalSym(memberAccess->member, moduleName, pos);
        } else {
            memberAccess->object->accept(*this);
            uint32_t memberIdx = makeConstIdx(CompileValue(memberAccess->member));
            pushOpcode(Opcode::SetField);
            push4B(memberIdx);
        }
    } else if(assignExp.target->type == AST::Exp::ExpType::IndexAccess){
        auto* indexAccess = static_cast<AST::IndexAccessExp*>(assignExp.target.get());
        indexAccess->object->accept(*this);
        indexAccess->index->accept(*this);
        pushOpcode(Opcode::IndexSet);
    } else {
        REPORT_SEMANTIC_ERROR(assignExp.line, assignExp.column, "Invalid assignment target");
    }
}

void Translator::visit(AST::IdentifierExp& identifierExp) {
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
    pushOpcode(Opcode::LoadVar);
    push4B(0); // "this" is always at slot 0
    if(!curFunc->inMethod){
        REPORT_SEMANTIC_ERROR(thisExp.line, thisExp.column, "'this' can only be used in methods");
    }
}

void Translator::visit(AST::ArrayExp& arrayExp) {
    for(auto& elem : arrayExp.elements){
        elem->accept(*this);
    }
    pushOpcode(Opcode::LoadConst);
    push4B(makeConstIdx(CompileValue((int64_t)arrayExp.elements.size())));
    callBuiltin(Builtin::NewArray);
}

void Translator::visit(AST::MapExp& mapExp) {
    for(auto& pair : mapExp.entries){
        pushOpcode(Opcode::LoadConst);
        push4B(makeConstIdx(CompileValue(pair.first)));
        pair.second->accept(*this);
    }
    pushOpcode(Opcode::LoadConst);
    push4B(makeConstIdx(CompileValue((int64_t)mapExp.entries.size())));
    callBuiltin(Builtin::NewMap);
}

void Translator::visit(AST::IntLitExp& intLitExp) {
    pushOpcode(Opcode::LoadConst);
    push4B(makeConstIdx(CompileValue(intLitExp.value)));
}

void Translator::visit(AST::FloatLitExp& floatLitExp) {
    pushOpcode(Opcode::LoadConst);
    push4B(makeConstIdx(CompileValue(floatLitExp.value)));
}

void Translator::visit(AST::StrLitExp& strLitExp) {
    pushOpcode(Opcode::LoadConst);
    push4B(makeConstIdx(CompileValue(strLitExp.value)));
}

void Translator::visit(AST::BoolLitExp& boolLitExp) {
    pushOpcode(Opcode::LoadConst);
    push4B(makeConstIdx(CompileValue(boolLitExp.value)));
}

void Translator::visit(AST::NullLitExp& nullLitExp) {
    pushOpcode(Opcode::LoadConst);
    push4B(makeConstIdx(CompileValue()));
}

void Translator::visit(AST::ArrayLitExp& arrayLitExp) {
    std::vector<CompileValue>* arr = new std::vector<CompileValue>();
    for(auto& elem : arrayLitExp.elements){
        auto elemVal = getValue(elem.get());
        arr->emplace_back(std::move(*elemVal));
    }
    pushOpcode(Opcode::LoadConst);
    push4B(makeConstIdx(CompileValue(arr)));
}

void Translator::visit(AST::MapLitExp& mapLitExp) {
    std::vector<std::pair<std::string, CompileValue>>* m = new std::vector<std::pair<std::string, CompileValue>>();
    for(auto& pair : mapLitExp.entries){
        auto val = getValue(pair.second.get());
        m->emplace_back(pair.first, std::move(*val));
    }
    pushOpcode(Opcode::LoadConst);
    push4B(makeConstIdx(CompileValue(m)));
}

void Translator::visit(AST::FuncLitExp& funcLitExp) {
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
            REPORT_SEMANTIC_ERROR(body->line, body->column, "Duplicate parameter name '%s' in function literal", paramName.c_str());
        }
    }
    body->accept(*this);
    curFunc->scopeMgr.exitScope();
    curFunc->proto->localCount = curFunc->scopeMgr.getMaxIndex() + 1;
    uint32_t protoIdx = curFunc->proto->index;
    curFunc = std::move(savedFunc);
    return protoIdx;
}

} // namespace Zeta
