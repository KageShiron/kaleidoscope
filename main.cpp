#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "KaleidoscopeJIT.h"
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace llvm;
using namespace llvm::orc;

static std::unique_ptr<KaleidoscopeJIT> TheJIT;
static std::unique_ptr<legacy::FunctionPassManager> TheFPM;


static void InitializeModuleAndPassManager();


enum Tokenn {
    tok_eof = -1,
    tok_def = -2,
    tok_extern = -3,
    tok_identifier = -4,
    tok_number = -5,
    tok_if = -6,
    tok_then = -7,
    tok_else = -8
};
static std::string IdentifierStr;
static double NumVal;


static int gettok() {
    static int LastChar = ' ';
    while (isspace(LastChar))
        LastChar = getchar();

    if (isalpha(LastChar)) {
        IdentifierStr = LastChar;
        while (isalnum((LastChar = getchar())))
            IdentifierStr += LastChar;

        if (IdentifierStr == "def")return tok_def;
        if (IdentifierStr == "extern")return tok_extern;
        if(IdentifierStr == "def")return tok_def;
        if(IdentifierStr == "if")return tok_if;
        if(IdentifierStr == "then") return tok_then;
        if(IdentifierStr == "else") return tok_else;
        return tok_identifier;
    }
    if (isdigit(LastChar) || LastChar == '.') { // Number: [0-9.]+
        std::string NumStr;
        do {
            NumStr += LastChar;
            LastChar = getchar();
        } while (isdigit(LastChar) || LastChar == '.');

        NumVal = strtod(NumStr.c_str(), nullptr);
        return tok_number;
    }

    if (LastChar == '#') {
        // Comment until end of line.
        do
            LastChar = getchar();
        while (LastChar != EOF && LastChar != '\n' && LastChar != '\r');

        if (LastChar != EOF)
            return gettok();
    }

    // Check for end of file.  Don't eat the EOF.
    if (LastChar == EOF)
        return tok_eof;

    // Otherwise, just return the character as its ascii value.
    int ThisChar = LastChar;
    LastChar = getchar();
    return ThisChar;
}

//############# AST
namespace {
    class ExprAST {
    public:
        virtual ~ExprAST() = default;
        virtual Value *codegen() = 0;
    };

    class NumberExprAST : public ExprAST {
        double Val;

    public:
        NumberExprAST(double Val) : Val(Val) {}

        Value *codegen() override ;
    };

    class VariableExprAST : public ExprAST {
        std::string Name;

    public:
        VariableExprAST(const std::string &Name) : Name(Name) {}

        Value *codegen() override ;
    };


    class BinaryExprAST : public ExprAST {
        char Op;
        std::unique_ptr<ExprAST> LHS, RHS;

    public:
        BinaryExprAST(char Op, std::unique_ptr<ExprAST> LHS, std::unique_ptr<ExprAST> RHS)
                : Op(Op), LHS(std::move(LHS)), RHS(std::move(RHS)) {}

        Value * codegen() override;
    };

    class CallExprAST : public ExprAST {
        std::string Callee;
        std::vector<std::unique_ptr<ExprAST>> Args;

    public:
        CallExprAST(const std::string &Callee, std::vector<std::unique_ptr<ExprAST>> Args)
                : Callee(Callee), Args(std::move(Args)) {}

        Value *codegen() override ;
    };

    class PrototypeAST {
        std::string Name;
        std::vector<std::string> Args;

    public:
        PrototypeAST(const std::string &Name, std::vector<std::string> Args)
                : Name(Name), Args(std::move(Args)) {}

        Function *codegen();
        const std::string &getName() const { return Name; }
    };

    class FunctionAST {
        std::unique_ptr<PrototypeAST> Proto;
        std::unique_ptr<ExprAST> Body;

    public:
        FunctionAST(std::unique_ptr<PrototypeAST> Proto, std::unique_ptr<ExprAST> Body)
                : Proto(std::move(Proto)), Body(std::move(Body)) {}

        Function *codegen();
    };


    class IfExprAST : public ExprAST {
        std::unique_ptr<ExprAST> Cond,Then,Else;

    public:
        IfExprAST(std::unique_ptr<ExprAST> Cond,std::unique_ptr<ExprAST> Then,
        std::unique_ptr<ExprAST> Else)
                : Cond(std::move(Cond)),Then(std::move(Then)),Else(std::move(Else)){}

        Value *codegen() override;
    };

}

//############ Parser
static int CurTok;
static std::map<std::string, std::unique_ptr<PrototypeAST>> FunctionProtos;


static int getNextToken() { return CurTok = gettok(); }

static std::map<char, int> BinopPrecendence;

static int GetTokPrecedence() {
    if (!isascii(CurTok))return -1;

    int TokPrec = BinopPrecendence[CurTok];
    if (TokPrec <= 0)return -1;
    return TokPrec;
}

std::unique_ptr<ExprAST> LogError(const char *str) {
    fprintf(stderr, "Error: %s\n", str);
    return nullptr;
}

std::unique_ptr<PrototypeAST> LogErrorP(const char *str) {
    LogError(str);
    return nullptr;
}

static std::unique_ptr<ExprAST> ParseExpression();

// numberexpr ::= number
static std::unique_ptr<ExprAST> ParseNumberExpr() {
    auto Result = llvm::make_unique<NumberExprAST>(NumVal);
    getNextToken(); // consume the number
    return std::move(Result);
}

static std::unique_ptr<ExprAST> ParseParenExpr() {
    getNextToken(); //eat (
    auto V = ParseExpression();
    if (!V)return nullptr;

    if (CurTok != ')')return LogError("expected )");
    getNextToken(); //eat )
    return V;
}

static std::unique_ptr<ExprAST> ParseIdentifierExpr() {
    std::string IdName = IdentifierStr;

    getNextToken();

    if (CurTok != '(')return llvm::make_unique<VariableExprAST>(IdName);

    getNextToken(); //eat (
    std::vector<std::unique_ptr<ExprAST>> Args;
    if (CurTok != ')') {
        while (true) {
            if (auto Arg = ParseExpression())
                Args.push_back(std::move(Arg));
            else
                return nullptr;

            if (CurTok == ')') break;
            if (CurTok != ',')return LogError("Expected ) or , in argument list");
            getNextToken();
        }
    }
    getNextToken(); //Eat )
    return llvm::make_unique<CallExprAST>(IdName, std::move(Args));
}


static std::unique_ptr<ExprAST> ParseIfExpr(){
    getNextToken();

    auto Cond = ParseExpression();
    if(!Cond)return nullptr;

    if(CurTok != tok_then)
        return LogError("expected then");

    auto Then = ParseExpression();
    if(!Then)return nullptr;

    if(CurTok != tok_else)
        return LogError("expected else");

    getNextToken();

    auto Else = ParseExpression();
    if(!Else)return nullptr;

    return llvm::make_unique<IfExprAST>(std::move(Cond), std::move(Then), std::move(Else));
}

static std::unique_ptr<ExprAST> ParsePrimary() {
    switch (CurTok) {
        default:
            return LogError("unknown token when exception an expression");
        case tok_identifier:
            return ParseIdentifierExpr();
        case tok_number:
            return ParseNumberExpr();
        case '(':
            return ParseParenExpr();
        case tok_if:
            return ParseIfExpr();
    }
}

static std::unique_ptr<ExprAST> ParseBinOpRHS(int ExprPrec, std::unique_ptr<ExprAST> LHS) {
    while (true) {
        int TokPrec = GetTokPrecedence();

        if (TokPrec < ExprPrec)return LHS;
        int BinOp = CurTok;
        getNextToken();

        auto RHS = ParsePrimary();
        if (!RHS)return nullptr;
        int NextPrec = GetTokPrecedence();

        if (TokPrec < NextPrec) {
            RHS = ParseBinOpRHS(TokPrec + 1, std::move(RHS));
            if (!RHS)return nullptr;
        }

        LHS = llvm::make_unique<BinaryExprAST>(BinOp, std::move(LHS), std::move(RHS));

    }
}

static std::unique_ptr<ExprAST> ParseExpression() {
    auto LHS = ParsePrimary();
    if (!LHS)return nullptr;

    return ParseBinOpRHS(0, std::move(LHS));
}

static std::unique_ptr<PrototypeAST> ParsePrototype() {
    if (CurTok != tok_identifier)return LogErrorP("Excepted function name in prototype");

    std::string FnName = IdentifierStr;
    getNextToken();

    if (CurTok != '(') return LogErrorP("Expected ( in prototype");

    std::vector<std::string> ArgNames;
    while (getNextToken() == tok_identifier)
        ArgNames.push_back(IdentifierStr);
    if (CurTok != ')')return LogErrorP("Expected ) in prototype");

    getNextToken();
    return llvm::make_unique<PrototypeAST>(FnName, std::move(ArgNames));
}

static std::unique_ptr<FunctionAST> ParseDefinition() {
    getNextToken();
    auto Proto = ParsePrototype();
    if (!Proto)return nullptr;

    if (auto E = ParseExpression())
        return llvm::make_unique<FunctionAST>(std::move(Proto), std::move(E));

    return nullptr;
}

static std::unique_ptr<FunctionAST> ParseTopLevelExpr() {
    if (auto E = ParseExpression()) {
        auto Proto = llvm::make_unique<PrototypeAST>("__anon_expr", std::vector<std::string>());
        return llvm::make_unique<FunctionAST>(std::move(Proto), std::move(E));
    }
    return nullptr;
}


static std::unique_ptr<PrototypeAST> ParseExtern() {
    getNextToken();
    return ParsePrototype();
}




////////////////////////
/// Code gen

static LLVMContext TheContext;
static IRBuilder<> Builder(TheContext);
static std::unique_ptr<Module> TheModule;
static std::map<std::string,Value *> NamedValues;


Function *getFunction(std::string Name){
    if(auto *F = TheModule->getFunction(Name))return F;

    auto Fl = FunctionProtos.find(Name);
    if(Fl != FunctionProtos.end())
        return Fl->second->codegen();

    return nullptr;
}


Value *LogErrorV(const char *Str){
    LogError(Str);
    return nullptr;
}

Value * NumberExprAST::codegen() {
    return ConstantFP::get(TheContext, APFloat(Val));
}

Value *VariableExprAST::codegen(){
    Value *V = NamedValues[Name];
    if(!V)return LogErrorV("Unknown variable name");
    return V;
}

Value *BinaryExprAST::codegen() {
    Value *L = LHS->codegen();
    Value *R = RHS->codegen();

    if(!L || !R)return nullptr;

    switch(Op){
        case '+':
            return Builder.CreateFAdd(L, R, "addtmp");
        case '-':
            return Builder.CreateFSub(L, R, "subtmp");
        case '*':
            return Builder.CreateFMul(L,R,"multmp");
        case '<':
            L = Builder.CreateFCmpULT(L, R, "cmptmp");
            //Convert bool 0/1 to double 0.0 or 1.0
            return Builder.CreateUIToFP(L, Type::getDoubleTy(TheContext), "booltmp");
        default:
            return LogErrorV("invalid bainary operator");
    }
}


Value *CallExprAST::codegen() {
    Function *CalleeF = getFunction(Callee);
    if(!CalleeF)return LogErrorV("Unknown function referencecd");

    if(CalleeF->arg_size() != Args.size())return LogErrorV("incorrect # arguments passed");

    std::vector<Value *> ArgsV;
    for (unsigned long i = 0,e = Args.size(); i != e ; ++i) {
        ArgsV.push_back(Args[i]->codegen());
        if(!ArgsV.back())return nullptr; //codegenの戻り値がnullptrなら
    }
    return Builder.CreateCall(CalleeF, ArgsV, "calltmp");
}

Function *PrototypeAST::codegen() {
    std::vector<Type *> Doubles(Args.size(), Type::getDoubleTy(TheContext));
    FunctionType *FT = FunctionType::get(Type::getDoubleTy(TheContext), Doubles, false);

    Function *F = Function::Create(FT, Function::ExternalLinkage, Name, TheModule.get());

    unsigned idx = 0;
    for(auto &Arg: F->args())Arg.setName(Args[idx++]);

    return F;

}


Function *FunctionAST::codegen(){

    auto &P = *Proto;
    FunctionProtos[Proto->getName()] = std::move(Proto);


    Function *TheFunction = getFunction(P.getName());

    if(!TheFunction)TheFunction = Proto->codegen();
    if(!TheFunction)return nullptr;

    BasicBlock *BB = BasicBlock::Create(TheContext, "entry", TheFunction);
    Builder.SetInsertPoint(BB);

    NamedValues.clear();

    for (auto &Arg : TheFunction->args()) {
        NamedValues[Arg.getName()] = &Arg;
    }

    if (Value *RetVal = Body->codegen()) {
        Builder.CreateRet(RetVal);
        verifyFunction(*TheFunction);
        TheFPM->run(*TheFunction);
        return TheFunction;
    }
    //Error reading body
    TheFunction->eraseFromParent();
    return nullptr;

}

Value *IfExprAST::codegen() {
    Value *CondV = Cond->codegen();
    if (!CondV)return nullptr;

    CondV = Builder.CreateFCmpONE(
            CondV, ConstantFP::get(TheContext, APFloat(0, 0)), "ifcond");

    Function *TheFunction = Builder.GetInsertBlock()->getParent;

    BasicBlock *ThenBB = BasicBlock::Create(TheContext, "then", TheFunction);
    BasicBlock *ElseBB = BasicBlock::Create(TheContext, "else");
    BasicBlock *MergeBB = BasicBlock::Create(TheContext, "ifcont");

    Builder.CreateCondBr(CondV, ThenBB, ElseBB);
    Builder.SetInsertPoint(ThenBB);

    Value *ThenV = Then->codegen();
    if(!ThenV)return nullptr;
    Builder.CreateBr(MergeBB);

    ThenBB = Builder.GetInsertBlock();

}


static void HandleDefinition() {
    if (auto FnAST = ParseDefinition()) {
        if(auto *FnIR = FnAST->codegen()) {
            fprintf(stderr, "Parsed a function definition.\n");
            FnIR->print(errs());
            fprintf(stderr, "\n");
            TheJIT->addModule(std::move(TheModule));
            InitializeModuleAndPassManager();
        }
    } else {
        getNextToken();
    }
}


static void HandleExtern() {
    if (auto ProtoAST = ParseExtern()) {
        if (auto *FnIR = ProtoAST->codegen()) {
            fprintf(stderr, "Read extern: ");
            FnIR->print(errs());
            fprintf(stderr, "\n");
            FunctionProtos[ProtoAST->getName()] = std::move(ProtoAST);
        }
    } else {
        // Skip token for error recovery.
        getNextToken();
    }
}
static void HandleTopLevelExpression() {
    // Evaluate a top-level expression into an anonymous function.
    if (auto FnAST = ParseTopLevelExpr()) {
        if (FnAST->codegen()) {
            // JIT the module containing the anonymous expression, keeping a handle so
            // we can free it later.
            auto H = TheJIT->addModule(std::move(TheModule));
            InitializeModuleAndPassManager();

            // Search the JIT for the __anon_expr symbol.
            auto ExprSymbol = TheJIT->findSymbol("__anon_expr");
            assert(ExprSymbol && "Function not found");

            // Get the symbol's address and cast it to the right type (takes no
            // arguments, returns a double) so we can call it as a native function.
            double (*FP)() = (double (*)())(intptr_t)cantFail(ExprSymbol.getAddress());
            fprintf(stderr, "Evaluated to %f\n", FP());

            // Delete the anonymous expression module from the JIT.
            TheJIT->removeModule(H);
        }
    } else {
        // Skip token for error recovery.
        getNextToken();
    }
}

static void MainLoop() {
    while (true) {
        fprintf(stderr, "ready> ");
        switch (CurTok) {
            case tok_eof:
                return;
            case ';':
                getNextToken();
                break;
            case tok_def:
                HandleDefinition();
                break;
            case tok_extern:
                HandleExtern();
                break;
            default:
                HandleTopLevelExpression();
                break;
        }
    }
}

///////////////////
// JIT

static void InitializeModuleAndPassManager(){
    TheModule = llvm::make_unique<Module>("my cool jit", TheContext);
    TheModule->setDataLayout(TheJIT->getTargetMachine().createDataLayout());

    TheFPM = llvm::make_unique<legacy::FunctionPassManager>(TheModule.get());

    TheFPM->add(createInstructionCombiningPass());
    TheFPM->add(createReassociatePass());
    TheFPM->add(createGVNPass());
    TheFPM->add(createCFGSimplificationPass());

    TheFPM->doInitialization();

}

#ifdef LLVM_ON_WIN32
#define DLLEXPORT __declspec(dllexport)
#else
#define DLLEXPORT
#endif

extern "C" DLLEXPORT double putchard(double X)
{
    fputc(char(X), stderr);
    return 0;
}

extern "C" DLLEXPORT double printd(double X){
    fprintf(stderr, "%f\n", X);
    return 0;
}

int main() {
    InitializeNativeTarget();;
    InitializeNativeTargetAsmPrinter();
    InitializeNativeTargetAsmParser();

    BinopPrecendence['<'] = 10;
    BinopPrecendence['+'] = 20;
    BinopPrecendence['-'] = 30;
    BinopPrecendence['*'] = 40;

    fprintf(stderr, "ready> ");
    getNextToken();

    TheJIT = llvm::make_unique<KaleidoscopeJIT>();

    InitializeModuleAndPassManager();

    MainLoop();


    return 0;
}