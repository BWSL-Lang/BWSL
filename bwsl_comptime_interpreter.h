#pragma once

#include "bwsl_ast_soa.h"
#include <string>

namespace BWSL {

struct CompilationContext;
struct Parser;

namespace Comptime {

struct ComptimeBudget {
    u32 executedStatements;
    u32 emittedStatements;
    u32 loopIterations;
    u32 cloneNodes;
    u32 cloneDepth;
    u32 maxExecutedStatements;
    u32 maxEmittedStatements;
    u32 maxLoopIterations;
    u32 maxCloneNodes;
    u32 maxCloneDepth;
};

struct ComptimeValue {
    LiteralValue value;
};

struct ComptimeBinding {
    u32 nameHash;
    TypeInfo typeInfo;
    u32 valueSlot;
    u32 scopeDepth;
    u8 flags;
};

struct ComptimeScope {
    u32 firstBinding;
    u32 bindingCount;
    u32 parentScope;
};

struct ComptimeFrame {
    NodeRef node;
    u32 scopeId;
    u32 cursor;
    u8 kind;
};

struct ComptimeEmitList {
    u32 firstStatement;
    u32 statementCount;
};

struct ComptimeDiagnostic {
    NodeRef node;
    u32 line;
    u32 column;
    u32 code;
    const char* message;
};

bool RunComptimeInterpreter(CompilationContext* context, Parser* parser,
                            NodeRef root, std::string* outError = nullptr);

} // namespace Comptime
} // namespace BWSL
