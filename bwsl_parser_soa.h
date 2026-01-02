#pragma once
#include "bwsl_lexer.h"
#include "bwsl_ast_soa.h"
#include "bwsl_utils.h"
#include "bwsl_arena.h"
#include "bwsl_symbol_table.h"
#include "bwsl_eval_soa.h"
#include <cstddef>
#include <unordered_map>
#include <string>
#include <vector>

namespace BWSL {

// Module search path configuration
// External tools (like bwslc) can add additional paths where modules should be searched
void AddModuleSearchPath(const std::string& path);
void ClearModuleSearchPaths();

struct ParseError {
    const char* message;
    u32 line;
    u32 column;
    TokenRef token;
};

struct CompilationContext {
    static constexpr size_t DEFAULT_ARENA_SIZE = 16 * 1024 * 1024; // 16MB for complex module hierarchies

    void* memory;
    BWSL_Arena arena;
    AST ast;                    // New SoA AST structure
    NodeRef root;               // Root node reference (usually pipeline)
    EvalCache evalCache;

    CompilationContext() : root(NodeRef::Null()) {
        memory = malloc(DEFAULT_ARENA_SIZE);
        arena.Initialize(memory, DEFAULT_ARENA_SIZE);
        ast.Init(&arena, 2048);  // Larger initial capacity for modules
        evalCache.Init();
    }

    ~CompilationContext() {
        if (memory) {
            free(memory);
        }
    }

    void Reset() {
        arena.Reset();
        ast.Init(&arena, 256);
        root = NodeRef::Null();
    }
};

struct Parser {
    Lexer* lexer;
    TokenStream* stream;  // TokenStream for SoA token access
    CompilationContext* context;
    BWSL_Arena* arena;
    AST* ast;  // Quick access to context->ast

    const char* sourceBase() const { return stream->GetSourceBase(); }
    SourceLocation getLocation(u32 offset) const { return lexer->GetSourceLocation(offset); }

    TokenRef current;
    TokenRef previous;

    std::string_view CurrentValue() const { return stream->GetValue(current); }
    std::string_view PreviousValue() const { return stream->GetValue(previous); }
    TokenType CurrentTokenType() const { return stream->GetType(current); }
    TokenType PreviousTokenType() const { return stream->GetType(previous); }

    bool hadError;
    bool panicMode;
    bool inShaderStage;
    bool hasLookahead;
    bool has3TokenLookahead;

    ArenaArray<ParseError> errors;
    u32 loopDepth;
    u32 functionDepth;

    TokenRef lookahead;
    TokenRef lookahead3;  // Third token for 3-token lookahead (IDENTIFIER :: ?)

    SymbolTableData symbolTable;

    ShaderStage currentShaderStage;

    u32 currentModuleIndex = 0xFFFFFFFF;

    NodeRef currentPass;
    NodeRef currentPipeline;
    std::unordered_map<std::string, u16> variableRegisters;
    std::unordered_map<u32, std::vector<u32>> multiDimArrayDims;

    struct TypeCache {
        static constexpr u32 CACHE_SIZE = 128;

        struct Entry {
            u32 nameHash;
            TypeInfo info;
            bool valid;
        };

        Entry entries[CACHE_SIZE];

        void Init() { memset(entries, 0, sizeof(entries)); }

        TypeInfo* Find(u32 hash) {
            u32 slot = hash & (CACHE_SIZE - 1);
            if (entries[slot].valid && entries[slot].nameHash == hash) {
                return &entries[slot].info;
            }
            return nullptr;
        }

        void Insert(u32 hash, const TypeInfo& info) {
            u32 slot = hash & (CACHE_SIZE - 1);
            entries[slot].nameHash = hash;
            entries[slot].info = info;
            entries[slot].valid = true;
        }

        void Clear() { memset(entries, 0, sizeof(entries)); }
    };

    TypeCache typeCache;

    void Init(Lexer* lex, TokenStream* ts, CompilationContext* ctx) {
        lexer = lex;
        stream = ts;
        context = ctx;
        arena = &ctx->arena;
        ast = &ctx->ast;
        hadError = false;
        panicMode = false;
        loopDepth = 0;
        functionDepth = 0;
        errors.Init(arena, 16);
        hasLookahead = false;
        has3TokenLookahead = false;
        multiDimArrayDims.clear();
        SymbolTable::Init(&symbolTable, arena);
        currentShaderStage = ShaderStage::Fragment;
        inShaderStage = false;
        currentPass = NodeRef::Null();
        currentPipeline = NodeRef::Null();
        typeCache.Init();
        // Start at first token (pre-tokenized stream)
        current = 0;
        previous = 0;
        lookahead = INVALID_TOKEN;
        lookahead3 = INVALID_TOKEN;
    }

    // Main parse functions
    NodeRef ParsePipeline();

    // Parse a standalone module file
    // Note: This is public to allow the CLI compiler to parse module files directly
    NodeRef ParseModuleFile() {
        // MODULE token should be current after Init()
        if (stream->GetType(current) == TokenType::MODULE) {
            Advance();  // Consume MODULE
            return ParseModule();
        }
        ErrorAtCurrent("Expected 'module' keyword");
        return NodeRef::Null();
    }

    TypeInfo GetExpressionType(NodeRef expr);

private:
    //----------------- Token management ---------------------------//
    void Advance();
    bool Check(TokenType type) const { return stream->GetType(current) == type; }
    bool Match(TokenType type);
    bool Consume(TokenType type, const char* message);
    void Synchronize();
    TokenRef PeekNext();
    TokenRef Peek3();  // Look 3 tokens ahead (for distinguishing IDENTIFIER :: ( from IDENTIFIER :: IDENTIFIER)
    bool IsFunctionDeclStart();  // Check if current position starts a function declaration

    //----------------- Error handling -----------------------------//
    void Error(const char* message);
    void ErrorAt(TokenRef token, const char* message);
    void ErrorAtCurrent(const char* message) { ErrorAt(current, message); }
    void ErrorAtPrevious(const char* message) { ErrorAt(previous, message); }
    void Error(const std::string& message) { Error(message.c_str()); }
    void ErrorAt(TokenRef token, const std::string& message) { ErrorAt(token, message.c_str()); }
    void ErrorAtCurrent(const std::string& message) { ErrorAt(current, message.c_str()); }
    void ErrorAtPrevious(const std::string& message) { ErrorAt(previous, message.c_str()); }

    //------------------ Parsing functions ------------------------//
    void ParseImports(NodeRef pipeline);
    void ParseAttributes(NodeRef pipeline);
    void ParsePassBody(NodeRef pass);
    void ParseComputeBody(NodeRef compute);
    void ParseUseAttributes(NodeRef pass);
    void ParseFunctionsBlockBody(NodeRef block);
    void ParseFunctionParameters(NodeRef function);
    NodeRef ParseComputeStage();
    NodeRef ParseComputeGraph();
    ComputeGraphNode ParseComputeGraphNode();
    void ParseComputeGraphInputs(ComputeGraphNode& node);
    void ParseComputeGraphOutputs(ComputeGraphNode& node);

    NodeRef ParsePass();
    NodeRef ParseAttributeDecl();
    NodeRef ParseResourceDecl();
    NodeRef ParseBlock();
    NodeRef ParseStatement();
    NodeRef ParseCustomTypeVarDecl();
    NodeRef ParseExpression();
    NodeRef ParseAssignment();
    NodeRef ParseTernary();
    NodeRef ParseOr();
    NodeRef ParseAnd();
    NodeRef ParseBitwiseOr();
    NodeRef ParseBitwiseXor();
    NodeRef ParseBitwiseAnd();
    NodeRef ParseEquality();
    NodeRef ParseComparison();
    NodeRef ParseBitwiseShift();
    NodeRef ParseTerm();
    NodeRef ParseFactor();
    NodeRef ParseUnary();
    NodeRef ParsePostfix();
    NodeRef ParsePrimary();
    NodeRef ParseFunctionCall(NodeRef function);
    NodeRef ParseMemberAccess(NodeRef object);
    NodeRef ParseFunction();
    NodeRef ParseStruct();
    NodeRef ResolveEnumMethod(const ArenaString& enumName, const ArenaString& methodName);
    NodeRef ParseModule();
    NodeRef ParseEvalStatement();
    NodeRef ParseEvalIf();
    NodeRef ParseArrayAccess(NodeRef array);
    NodeRef ParseShaderStage(ASTNodeType stageType);
    NodeRef ParseShaderStageInheritance(ASTNodeType stageType);
    NodeRef ParseShaderStageExpression(ASTNodeType stageType);  // Parse vertex = expr or fragment = expr
    NodeRef ParseSwitch();
    NodeRef ParseForStatement(bool isEval);
    NodeRef ParseLoopStatement(bool isEval);
    NodeRef ParseEnum();
    NodeRef ParseEnumVariant();
    NodeRef ParseEnumMethod();
    NodeRef ParsePatternMatch(NodeRef scrutinee);
    NodeRef ParseTypePatternMatch();
    NodeRef ParseConstraint();
    TypeMask ParseConstraintTypeExpression();
    TypeMask ParseConstraintType();
    NodeRef ParseGenericParams();
    NodeRef ParseGenericFunction();
    NodeRef ParseWhereClause();
    NodeRef ParseArrayInitializer();
    NodeRef ParseInlineArrayConstruction();
    NodeRef ParseArrayDeclaration(CoreType elementType, StorageClass storageClass = StorageClass::Default);
    NodeRef FlattenMultiDimArrayAccess(NodeRef access);
    TypeInfo ResolveType(const std::string& typeName);
    TypeInfo GetTypeInfoFromSymbol(Symbol* sym);
    TypeInfo ParseArrayType();

    //----------------- Helper functions ------------------------//
    u8 GetAttributeIndex(const ArenaString& name);
    bool TryRegisterModuleFromDisk(const std::string& moduleName);
    ResourceType GetResourceType(TokenType type);
    CoreType TokenTypeToReturnType(TokenType type);
    BinaryOpType TokenTypeToBinaryOp(TokenType type);
    bool MatchMask(TokenMask mask);
    bool CheckMask(TokenMask mask);
    bool ValidateAttributeInUse(const ArenaString& attrName);
    bool ValidateAssignmentTarget(NodeRef target);

    //----------------- Shader stage expression resolution ------------------------//
    void ResolveShaderStageExpressions(NodeRef pipeline);
    NodeRef ResolveShaderStageExpr(NodeRef stageNode, const PassData& pass, ASTNodeType expectedType);
    NodeRef LookupShaderFunction(u32 nameHash, const PassData& pass, CoreType expectedReturnType);

    // Parameter substitution for shader functions
    struct ParamSubstitution {
        u32 nameHash;
        LiteralValue value;
    };
    NodeRef CloneShaderStageWithParams(NodeRef stageNode, const ParamSubstitution* subs, u32 subCount);
    NodeRef CloneNodeWithParams(NodeRef node, const ParamSubstitution* subs, u32 subCount);

    inline TypeInfo ResolveTypeFromToken(TokenType token);
};

} // namespace BWSL
