#include "llvm/ADT/STLExtras.h"
#include <string>
#include <memory>
#include <vector>
#include <map>
#include <iostream>


enum Tokenn {
    tok_eof = -1,
    tok_def = -2,
    tok_extern = -3,
    tok_identifier = -4,
    tok_number = -5
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
        return tok_identifier;
    }

    if (isdigit(LastChar) || LastChar == '.') { //Number: [0-9.]+
        std::string NumStr;
        do {
            NumStr += LastChar;
            LastChar = getchar();
        } while (LastChar != EOF && LastChar != '\n' && LastChar != '\r');

        if (LastChar == EOF)return gettok();
        int ThisChar = LastChar;
        LastChar = getchar();
        return ThisChar;
    }
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

        Value *codegen() override;
    };

    class VariableExprAST : public ExprAST {
        std::string Name;

    public:
        VariableExprAST(const std::string &Name) : Name(Name) {}

        Value *codegen() override;
    };


    class BinaryExprAST : public ExprAST{
        char Op;
        std::unique_ptr<ExprAST> LHS,RHS;

    public:
        BinaryExprAST(char Op,std::unique_ptr<ExprAST> LHS, std::unique_ptr<ExprAST> RHS)
            : Op(Op) , LHS(std::move(LHS)) , RHS(std::move(RHS)){}

        Value * codegen() override;
    };

    class CallExprAST : public ExprAST{
        std::string Callee;
        std::vector<std::unique_ptr<ExprAST>> Args;

    public:
        CallExprAST(const std::string &Callee,std::vector<std::unique_ptr<ExprAST>> Args)
                : Callee(Callee) , Args(std::move(Args)){}

        Value *codegen() override ;
    };

    class PrototypeAST{
        std::string Name;
        std::vector<std::string> Args;

    public:
        PrototypeAST(const std::string &Name , std::vector<std::string> Args )
                : Name(Name) , Args(std::move(Args)){}

        Function *codegen();
        const std::string &getName() const {return Name;}
    };

    class FunctionAST{
        std::unique_ptr<PrototypeAST> Proto;
        std::unique_ptr<ExprAST> Body;

    public:
        FunctionAST(std::unique_ptr<PrototypeAST> Proto,std::unique_ptr<ExprAST> Body)
        : Proto (std::move(Proto)),Body(std::move(Body)){}

        Function *codegen();
    };

}

//############ Parser
static int CurTok;
static int getNextToken(){return CurTok = gettok();}

static std::map<char,int> BinopPrecendence;

static int GetTokPrecedence() {
    if(!isascii(CurTok))return -1;

    int TokPrec = BinopPrecendence[CurTok];
    if(TokPrec <= 0)return -1;
    return TokPrec;
}

std::unique_ptr<ExprAST> LogError(const char *str){
    fprintf(stderr,"Error: %s\n" , str);
    return nullptr;
}

std::unique_ptr<PrototypeAST> LogErrorP(const char *str){
    LogError(str);
    return nullptr;
}

static std::unique_ptr<ExprAST> ParseExpression();

// numberexpr ::= number
static std::unique_ptr<ExprAST> ParserNumberExpr(){
    auto Result = llvm::make_unique<NumberExprAST>(NumVal);
    getNextToken();
    return std::move(Result);
}

static std::unique_ptr<ExprAST> ParserParenExpr(){
    getNextToken(); //eat (
    auto V = ParseExpression();
    if(!V)return nullptr;

    if(CurTok != ')')return LogError("expected )");
    getNextToken(); //eat )
    return V;
}

static std::unique_ptr<ExprAST> ParseIdentifierExpr(){
    std::string IdName = IdentifierStr;

    getNextToken();

    if(CurTok != '(')return llvm::make_unique<VariableExprAST>(IdName);

    getNextToken(); //eat (
    std::vector<std::unique_ptr<ExprAST>> Args;
    if(CurTok != ')'){
        while(true){
            if(auto Arg = ParseExpression())
                Args.push_back(std::move(Arg));
            else
                return nullptr;

            if(CurTok == ')') break;
            if(CurTok != ',')return LogError("Expected ) or , in argument list");
            getNextToken();
        }
    }
    getNextToken(); //Eat )
    return llvm::make_unique<CallExprAST>(IdName, std::move(Args));
}

static std::unique_ptr<ExprAST> ParsePrimary(){
    switch(CurTok){
        default:
            return LogError("unknown token when exception an expression");
        case tok_identifier:
            return ParseIdentifierExpr();
        case tok_number:
            return ParserNumberExpr();
        case '(':
            return ParserParenExpr();
    }
}

static std::unique_ptr<ExprAST> ParseBinOpRHS(int ExprPrec,std::unique_ptr<ExprAST> LHS){
    while(true){
        int TokPrec = GetTokPrecedence();

        if(TokPrec < ExprPrec)return LHS;
        int BinOp = CurTok;
        getNextToken();

        auto RHS = ParsePrimary();
        if(!RHS)return nullptr;
        int NextPrec = GetTokPrecedence();

        if(TokPrec < NextPrec){
            RHS = ParseBinOpRHS(TokPrec + 1, std::move(RHS));
            if(!RHS)return nullptr;
        }

        LHS = llvm::make_unique<BinaryExprAST>(BinOp,std::move(LHS),std::move(RHS));

    }
}

static std::unique_ptr<ExprAST> ParseExpression(){
    auto LHS = ParsePrimary();
    if(!LHS)return nullptr;

    return ParseBinOpRHS(0,std::move(LHS));
}

static std::unique_ptr<PrototypeAST> ParsePrototype(){
    if(CurTok != tok_identifier)return LogErrorP("Excepted function name in prototype");

    std::string FnName = IdentifierStr;
    getNextToken();

    if(CurTok != '(') return LogErrorP("Expected ( in prototype");

    std::vector<std::string> ArgNames;
    while(getNextToken() == tok_identifier)
        ArgNames.push_back(IdentifierStr);
    if(CurTok != ')')return LogErrorP("Expected ) in prototype");

    getNextToken();
    return llvm::make_unique<PrototypeAST>(FnName,std::move(ArgNames));
}

static std::unique_ptr<FunctionAST> ParseDefinition(){
    getNextToken();
    auto Proto = ParsePrototype();
    if(!Proto)return nullptr;

    if(auto E =ParseExpression())
        return llvm::make_unique<FunctionAST>(std::move(Proto),std::move(E));

    return nullptr;
}

