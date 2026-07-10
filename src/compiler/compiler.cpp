#include "compiler.h"

#include "bytecode.h"
#include "translate.h"
#include "syntax.tab.hpp"
#include "error.h"

void yyrestart(FILE*);

namespace Zeta {

std::unique_ptr<Module> compileModule(const std::string& path, std::string* outError){
    FILE* file = fopen(path.c_str(), "r");
    if(!file){
        REPORT_SYSTEM_ERROR("Failed to open file '{}'", path);
        goto ERROR;
    } else {
        yyrestart(file);
        std::unique_ptr<AST::Program> root;
        Parser parser(root);
        parser.parse();
        if(CompileErrorCollector::get().hasErrors()) goto ERROR;
        Translator translator;
        std::unique_ptr<Module> module = translator.translate(root.get());
        if(CompileErrorCollector::get().hasErrors()) goto ERROR;
        fclose(file);
        return module;
    }

    ERROR:
    if(file){
        fclose(file);
    }
    if(outError){
        *outError = CompileErrorCollector::get().getAllAsString();
    }else{
        CompileErrorCollector::get().printAll();
    }
    CompileErrorCollector::get().clear();
    return nullptr;
}

}
