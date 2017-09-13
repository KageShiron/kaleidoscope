
#include <string>
#include <c++/iostream>
#include <c++/memory>
#include <c++/vector>
#include <c++/map>

enum Token {
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
    fprintf(stderr,"Error: %s\n" , Str);
    return nullptr;
}

std::unique_ptr<PrototypeAST> LogErrorP(const char *Str){
    LogError(str);
    return nullptr;
}

static std::unique_ptr<ExprAST> ParserExpression();

// numberexpr ::= number
static std::unique_ptr<ExprAST> ParserNumberExpr(){
    auto Result = llvm::make_unique<NumberExprAST>(NumVal);
    getNextToken();
    return std::move(Result);
}