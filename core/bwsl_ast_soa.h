#pragma once
#include <vector>
#include <string>
#include <variant>
#include <cstring>
#include "bwsl_defs.h"
#include "bwsl_arena.h"
#include "bwsl_utils.h"
#include "bwsl_types.h"
#include "bwsl_stdlib.h"
#include "bwsl_ast_common.h"
#include "bwsl_compiler_types.h"

namespace BWSL {

// Forward declaration for SoA AST
struct AST;

// Node reference - 4 bytes, can be packed further if needed
// High 8 bits = type, low 24 bits = index into type-specific pool
struct NodeRef {
    u32 packed;

    NodeRef() : packed(0xFFFFFFFF) {}
    NodeRef(ASTNodeType type, u32 index) : packed((static_cast<u32>(type) << 24) | (index & 0x00FFFFFF)) {}

    ASTNodeType Type() const { return static_cast<ASTNodeType>(packed >> 24); }
    u32 Index() const { return packed & 0x00FFFFFF; }
    bool IsValid() const { return packed != 0xFFFFFFFF; }
    bool IsNull() const { return packed == 0xFFFFFFFF; }

    static NodeRef Null() { return NodeRef(); }

    bool operator==(const NodeRef& other) const { return packed == other.packed; }
    bool operator!=(const NodeRef& other) const { return packed != other.packed; }
};

// LiteralValue is defined in bwsl_ast_common.h

//==============================================================================
// Type-specific data pools (cold data - only touched when you need that node type)
//==============================================================================

// 16 bytes - very common
struct IdentifierData {
    ArenaString name;
    SpecialIdentifier identifierKind;
    u8 _pad[3];
};

// 16 bytes - very common
struct LiteralData {
    LiteralValue value;
};

// 12 bytes - very common
struct BinaryOpData {
    BinaryOpType op;
    u8 _pad[3];
    NodeRef left;
    NodeRef right;
};

// 8 bytes
struct UnaryOpData {
    UnaryOpType op;
    u8 _pad[3];
    NodeRef operand;
};

// 24 bytes
struct MemberAccessData {
    NodeRef object;
    ArenaString member;
    u32 qualifiedNameHash = 0; // Pre-computed hash of "Module::member" for module-qualified access
    bool isModuleQualified = false; // True if using :: (module access) instead of . (field access)
};

// 8 bytes
struct ArrayAccessData {
    NodeRef array;
    NodeRef index;
};

// 12 bytes - very common
struct AssignmentData {
    NodeRef target;
    NodeRef value;
    InterpolationMode interpolation;
    u8 _pad[3];
};

// Shader stage data
struct ShaderStageData {
    NodeRef body;
    ArenaString inheritsFrom;   // Pass name to inherit from (if isInherited)
    bool isInherited;           // True if this stage inherits from another pass
    bool isDeferred;            // True if this stage comes from expression (function call or ternary)
    u8 _pad[2];
    ArenaString name;           // Compute block name (if compute stage)
    u32 workgroupSizeX;
    u32 workgroupSizeY;
    u32 workgroupSizeZ;
    NodeRef deferredExpr;       // Expression to resolve at compile-time (if isDeferred)
};

// 4 bytes + ArenaArray (12 bytes) = 16 bytes typical
struct BlockData {
    ArenaArray<NodeRef> statements;
};

// ArenaArray for NodeRef children
struct NodeRefArray {
    ArenaArray<NodeRef> refs;
};

// 24 bytes
struct VariableDeclData {
    ArenaString name;
    ArenaString type;
    NodeRef initializer;
    bool isConst;
    bool isEval;
    StorageClass storageClass;
    u8 arrayDimensions;
    u32 arrayLength;
    u32 arrayElementTypeHash;
};

// Function call data (with ArenaArray overhead)
struct FunctionCallData {
    ArenaString name;
    ArenaArray<NodeRef> arguments;
    u16 intrinsicIndex;
    u8 flags;
    u8 _padding;
    u32 moduleIndex;
    u32 moduleQualifiedHash;   // Hash of "Module::function" for module-qualified calls
    NodeRef moduleObject;       // Reference to module identifier for module-qualified calls
};

// 28 bytes
struct AttributeDeclData {
    ArenaString name;
    ArenaString dataType;
    ArenaString compression;
    u8 attributeIndex;
    bool isInstance;
    u8 _pad[2];
};

// 24 bytes
struct ResourceDeclData {
    ArenaString name;
    ArenaString typeName;
    u8 resourceIndex;
    u8 _pad[3];
};

struct FragmentOutputDeclData {
    ArenaString name;
    ArenaString typeName;
    TypeInfo typeInfo;
    u8 location;
    u8 _pad[3];
};

// 24 bytes
struct ForCStyleData {
    NodeRef init;
    NodeRef condition;
    NodeRef increment;
    NodeRef body;
    bool isEval;
    bool isWhile;
    u8 _pad[2];
};

// 28 bytes
struct ForRangeData {
    NodeRef iteratorVar;
    NodeRef rangeStart;
    NodeRef rangeEnd;
    NodeRef step;
    NodeRef body;
    bool inclusive;
    bool isEval;
    u8 _pad[2];
};

// 20 bytes
struct ForCollectionData {
    NodeRef iteratorVar;
    NodeRef collection;
    NodeRef body;
    u32 length;       // Collection length (resolved at parse/type-check time)
    bool isEval;
    u8 _pad[3];
};

// 16 bytes
struct LoopData {
    NodeRef count;
    NodeRef body;
    NodeRef untilCondition;
    bool isEval;
    u8 _pad[3];
};

// Function definition in AST - 24 bytes + ArenaArray
// Named FunctionDeclData to avoid conflict with FunctionData in bwsl_symbol_table.h
struct FunctionDeclData {
    ArenaString name;
    ArenaArray<std::pair<ArenaString, ArenaString>> parameters;
    CoreType returnType;
    u32 returnTypeHash;
    u32 ownerStructTypeHash;
    NodeRef body;
    bool isEval;
    bool isStructMethod;
    bool isConstMethod;
};

// Struct field with optional array size
struct StructFieldData {
    ArenaString name;
    TypeInfo type;
    u32 arraySize;  // 0 = not an array, >0 = fixed-size array
};

// Struct declaration - 20 bytes + ArenaArray
struct StructDeclData {
    ArenaString name;
    ArenaArray<StructFieldData> fields;
    ArenaArray<NodeRef> methods;
};

// Constraint declaration - 20 bytes
struct ConstraintDeclData {
    ArenaString name;
    TypeMask allowedTypes;
};

enum class VariantRuleType : u8 {
    Require = 0,
    Conflict = 1,
};

struct PipelineVariantDeclData {
    ArenaString name;
    ArenaString typeName;
    TypeInfo typeInfo;
    u32 enumTypeHash;
    NodeRef defaultExpr;
    LiteralValue defaultValue;
    bool defaultResolved;
    u8 _pad[3];
};

struct VariantRuleData {
    VariantRuleType type;
    u8 _pad[3];
    NodeRef lhs;
    NodeRef rhs;
};

struct PassBlockBindingData {
    ArenaString localName;
    ArenaString targetName;
};

// Pass - 32 bytes + ArenaArrays
struct PassData {
    ArenaString name;
    ArenaArray<ArenaString> usedAttributes;
    ArenaArray<ArenaString> usedResources;
    ArenaArray<FragmentOutputDeclData> fragmentOutputs;
    ArenaArray<NodeRef> consts;     // Pass-scoped constants
    ArenaArray<NodeRef> functions;  // Pass-scoped functions
    ArenaArray<PassBlockBindingData> attributeBindings;
    ArenaArray<PassBlockBindingData> resourceBindings;
    ArenaArray<PassBlockBindingData> variantBindings;
    NodeRef vertexShader;
    NodeRef fragmentShader;
    NodeRef computeShader;
    NodeRef passBlockCall;
    u32 optionalAttributesMask;     // Bitmask of optional attributes (from ? syntax)
    u32 optionalResourcesMask;      // Bitmask of optional resources (from ? syntax)
    bool hasFragmentOutputs;
    bool isPassBlockInstance;
    u8 _pad[2];
};

struct GraphResourceRef {
    ArenaString name;
    ResourceAccessMode access;
    u8 _pad[3];
};

struct ComputeGraphNode {
    ArenaString passName;
    ArenaArray<GraphResourceRef> inputs;
    ArenaArray<ArenaString> outputs;
};

struct ComputeGraphData {
    ArenaArray<ComputeGraphNode> nodes;
};

// Module - 20 bytes + ArenaArrays
struct ModuleNodeData {
    ArenaString name;
    ArenaArray<ArenaString> imports;  // Module dependencies
    ArenaArray<ArenaString> usingImports;
    ArenaArray<NodeRef> functions;
    ArenaArray<NodeRef> structs;
    ArenaArray<NodeRef> enums;
    ArenaArray<NodeRef> attributes;
    ArenaArray<NodeRef> resources;
    ArenaArray<PipelineVariantDeclData> variantDecls;
    ArenaArray<VariantRuleData> variantRules;
};

// Array type info - 16 bytes
struct ArrayTypeData {
    CoreType elementType;
    u32 elementTypeHash;
    u32 length;
    NodeRef elementTypeNode;
};

// Enum declaration - complex, ~48 bytes + ArenaArrays
struct EnumDeclData {
    ArenaString name;
    CoreType underlyingType;

    struct VariantInfo {
        ArenaString name;
        ArenaArray<CoreType> associatedTypes;
        ArenaArray<u32> associatedTypeHashes;
        u32 value;
    };
    VariantInfo currentVariant; // Temporary storage during parsing

    ArenaArray<NodeRef> variants;
    ArenaArray<NodeRef> methods;
    ArenaArray<CoreType> associatedTypes;
    ArenaArray<u32> associatedTypeHashes;
};

// Pattern match - complex, ~48 bytes + ArenaArrays
struct PatternMatchData {
    ArenaString scrutinee;
    ArenaArray<NodeRef> arms;
    NodeRef defaultArm;
    NodeRef body;
    ArenaArray<std::pair<ArenaString, ArenaString>> bindings;
    ArenaArray<NodeRef> statements;
    ArenaString variantName;
    u32 variantHash;
    bool isEval;
    bool isDefault;
    u8 _pad[2];
};

// Type pattern match for generic functions - compile-time branch selection
struct TypePatternMatchData {
    ArenaArray<NodeRef> arms;    // Array of TYPE_PATTERN_ARM nodes
    NodeRef defaultArm;          // Optional default case
};

// Individual arm in a type pattern match
struct TypePatternArmData {
    CoreType matchType;          // float2, float3, float4, etc.
    NodeRef body;                // Expression or block
    bool isDefault;
    u8 _pad[2];
};

// Case arm for switch - supports multiple values
struct SwitchCaseData {
    ArenaArray<NodeRef> values;  // Multiple case values (empty for default)
    NodeRef body;                // Case body (block)
    bool isDefault;
    u8 _pad[3];
};

// Switch statement - 20 bytes + ArenaArray
struct SwitchData {
    NodeRef expression;              // Value being switched on
    ArenaArray<NodeRef> cases;       // Array of SwitchCaseData refs
    NodeRef defaultCase;             // Default case body (or null)
    bool isExhaustive;               // All cases covered (for enums)
    u8 _pad[3];
};

// Ternary expression
struct TernaryExprData {
    NodeRef condition;
    NodeRef trueExpr;
    NodeRef falseExpr;
};

// Pipeline - the big one, ~64 bytes + ArenaArrays
struct PipelineData {
    ArenaString name;
    ArenaArray<ArenaString> imports;
    ArenaArray<ArenaString> usingImports;
    ArenaArray<NodeRef> attributes;
    ArenaArray<NodeRef> resources;
    ArenaArray<PipelineVariantDeclData> variantDecls;
    ArenaArray<VariantRuleData> variantRules;
    ArenaArray<NodeRef> passes;
    ArenaArray<NodeRef> functions;
    ArenaArray<NodeRef> enums;
    ArenaArray<NodeRef> constraints;
    NodeRef computeGraph;
};

//==============================================================================
// Main AST Structure - Structure of Arrays
//==============================================================================

struct AST {
    BWSL_Arena* arena;

    // Core node metadata - hot path for traversal
    // Packed: line:20 | col:12 for position
    alignas(64) u32* positions;  // Packed line/column
    u32 nodeCount;
    u32 nodeCapacity;
    u32* positionKeys;           // NodeRef::packed -> packed line/column
    u32* positionValues;
    u32 positionMapCount;
    u32 positionMapCapacity;

    // Type-specific pools (cold, only touched when you need that node type)
    ArenaArray<IdentifierData> identifiers;
    ArenaArray<LiteralData> literals;
    ArenaArray<BinaryOpData> binaryOps;
    ArenaArray<UnaryOpData> unaryOps;
    ArenaArray<TernaryExprData> ternaryExprs;
    ArenaArray<MemberAccessData> memberAccesses;
    ArenaArray<ArrayAccessData> arrayAccesses;
    ArenaArray<AssignmentData> assignments;
    ArenaArray<ShaderStageData> shaderStages;
    ArenaArray<BlockData> blocks;
    ArenaArray<VariableDeclData> variableDecls;
    ArenaArray<FunctionCallData> functionCalls;
    ArenaArray<AttributeDeclData> attributeDecls;
    ArenaArray<ResourceDeclData> resourceDecls;
    ArenaArray<ForCStyleData> forCStyles;
    ArenaArray<ForRangeData> forRanges;
    ArenaArray<ForCollectionData> forCollections;
    ArenaArray<LoopData> loops;
    ArenaArray<FunctionDeclData> functions;
    ArenaArray<StructDeclData> structDecls;
    ArenaArray<ConstraintDeclData> constraintDecls;
    ArenaArray<PassData> passes;
    ArenaArray<ModuleNodeData> modules;
    ArenaArray<ArrayTypeData> arrayTypes;
    ArenaArray<EnumDeclData> enumDecls;
    ArenaArray<PatternMatchData> patternMatches;
    ArenaArray<TypePatternMatchData> typePatternMatches;
    ArenaArray<TypePatternArmData> typePatternArms;
    ArenaArray<SwitchCaseData> switchCases;
    ArenaArray<SwitchData> switches;
    ArenaArray<PipelineData> pipelines;
    ArenaArray<ComputeGraphData> computeGraphs;

    // Return statements reuse AssignmentData (target unused, value is the return expr)
    // If statements reuse BlockData (first statement is condition, rest is body)

    //==========================================================================
    // Initialization
    //==========================================================================

    void Init(BWSL_Arena* a, u32 estimatedNodes = 256) {
        arena = a;
        nodeCount = 0;
        nodeCapacity = estimatedNodes;

        positions = (u32*)arena->Allocate(sizeof(u32) * nodeCapacity, 64);
        positionMapCapacity = NextPowerOfTwo(estimatedNodes * 2);
        positionMapCount = 0;
        positionKeys = (u32*)arena->Allocate(sizeof(u32) * positionMapCapacity, 64);
        positionValues = (u32*)arena->Allocate(sizeof(u32) * positionMapCapacity, 64);
        memset(positionKeys, 0xFF, sizeof(u32) * positionMapCapacity);

        // Initialize pools with reasonable defaults based on typical usage
        identifiers.Init(arena, 64);        // Very common
        literals.Init(arena, 32);           // Common
        binaryOps.Init(arena, 32);          // Common
        unaryOps.Init(arena, 8);            // Less common
        ternaryExprs.Init(arena, 8);        // Less common
        memberAccesses.Init(arena, 32);     // Common
        arrayAccesses.Init(arena, 8);       // Less common
        assignments.Init(arena, 16);        // Common
        shaderStages.Init(arena, 4);        // Few per pipeline
        blocks.Init(arena, 16);             // Common
        variableDecls.Init(arena, 16);      // Common
        functionCalls.Init(arena, 32);      // Very common
        attributeDecls.Init(arena, 8);      // Few per pipeline
        resourceDecls.Init(arena, 8);       // Few per pipeline
        forCStyles.Init(arena, 4);          // Less common
        forRanges.Init(arena, 4);           // Less common
        forCollections.Init(arena, 4);      // Less common
        loops.Init(arena, 4);               // Less common
        functions.Init(arena, 8);           // Few per pipeline
        structDecls.Init(arena, 4);         // Less common
        constraintDecls.Init(arena, 4);     // Less common
        passes.Init(arena, 4);              // Few per pipeline
        modules.Init(arena, 2);             // Rare
        arrayTypes.Init(arena, 4);          // Less common
        enumDecls.Init(arena, 4);           // Less common
        patternMatches.Init(arena, 4);      // Less common
        typePatternMatches.Init(arena, 4);  // Generic type patterns
        typePatternArms.Init(arena, 8);     // Type pattern arms
        switchCases.Init(arena, 8);         // Switch case arms
        switches.Init(arena, 4);            // Less common
        pipelines.Init(arena, 1);           // Usually just one
        computeGraphs.Init(arena, 1);       // Optional
    }

    //==========================================================================
    // Position helpers
    //==========================================================================

    static u32 PackPosition(u32 line, u32 column) {
        // line: 20 bits (max 1M lines), column: 12 bits (max 4K columns)
        return ((line & 0xFFFFF) << 12) | (column & 0xFFF);
    }

    static void UnpackPosition(u32 packed, u32& line, u32& column) {
        line = packed >> 12;
        column = packed & 0xFFF;
    }

    static u32 NextPowerOfTwo(u32 value) {
        if (value <= 1) return 1;
        value--;
        value |= value >> 1;
        value |= value >> 2;
        value |= value >> 4;
        value |= value >> 8;
        value |= value >> 16;
        return value + 1;
    }

    void RecordPosition(NodeRef ref, u32 line, u32 column) {
        if (nodeCount >= nodeCapacity) {
            u32 newCapacity = nodeCapacity * 2;
            u32* newPositions = (u32*)arena->Allocate(sizeof(u32) * newCapacity, 64);
            memcpy(newPositions, positions, nodeCount * sizeof(u32));
            positions = newPositions;
            nodeCapacity = newCapacity;
        }

        u32 packedPosition = PackPosition(line, column);
        positions[nodeCount++] = packedPosition;

        IndexPosition(ref, line, column);
    }

    void IndexPosition(NodeRef ref, u32 line, u32 column) {
        if ((positionMapCount + 1) * 2 >= positionMapCapacity) {
            GrowPositionMap();
        }
        InsertPosition(ref.packed, PackPosition(line, column));
    }

    NodeRef MakeNodeRef(ASTNodeType type, u32 index, u32 line, u32 column) {
        NodeRef ref(type, index);
        IndexPosition(ref, line, column);
        return ref;
    }

    u32 FindPosition(NodeRef ref) const {
        if (ref.IsNull() || positionMapCapacity == 0) return 0;
        u32 slot = ref.packed & (positionMapCapacity - 1);
        for (u32 probe = 0; probe < positionMapCapacity; probe++) {
            u32 key = positionKeys[slot];
            if (key == ref.packed) {
                return positionValues[slot];
            }
            if (key == 0xFFFFFFFFu) {
                return 0;
            }
            slot = (slot + 1) & (positionMapCapacity - 1);
        }
        return 0;
    }

    u32 GetLine(NodeRef ref) const {
        if (ref.IsNull()) return 0;
        return FindPosition(ref) >> 12;
    }

    u32 GetColumn(NodeRef ref) const {
        if (ref.IsNull()) return 0;
        return FindPosition(ref) & 0xFFF;
    }

private:
    void InsertPosition(u32 key, u32 value) {
        u32 slot = key & (positionMapCapacity - 1);
        while (positionKeys[slot] != 0xFFFFFFFFu && positionKeys[slot] != key) {
            slot = (slot + 1) & (positionMapCapacity - 1);
        }
        if (positionKeys[slot] == 0xFFFFFFFFu) {
            positionMapCount++;
        }
        positionKeys[slot] = key;
        positionValues[slot] = value;
    }

    void GrowPositionMap() {
        u32 oldCapacity = positionMapCapacity;
        u32* oldKeys = positionKeys;
        u32* oldValues = positionValues;

        positionMapCapacity *= 2;
        positionMapCount = 0;
        positionKeys = (u32*)arena->Allocate(sizeof(u32) * positionMapCapacity, 64);
        positionValues = (u32*)arena->Allocate(sizeof(u32) * positionMapCapacity, 64);
        memset(positionKeys, 0xFF, sizeof(u32) * positionMapCapacity);

        for (u32 i = 0; i < oldCapacity; i++) {
            if (oldKeys[i] != 0xFFFFFFFFu) {
                InsertPosition(oldKeys[i], oldValues[i]);
            }
        }
    }

public:

    //==========================================================================
    // Type-specific accessors
    //==========================================================================

    IdentifierData& GetIdentifier(NodeRef ref) { return identifiers[ref.Index()]; }
    const IdentifierData& GetIdentifier(NodeRef ref) const { return identifiers[ref.Index()]; }

    LiteralData& GetLiteral(NodeRef ref) { return literals[ref.Index()]; }
    const LiteralData& GetLiteral(NodeRef ref) const { return literals[ref.Index()]; }

    BinaryOpData& GetBinaryOp(NodeRef ref) { return binaryOps[ref.Index()]; }
    const BinaryOpData& GetBinaryOp(NodeRef ref) const { return binaryOps[ref.Index()]; }

    UnaryOpData& GetUnaryOp(NodeRef ref) { return unaryOps[ref.Index()]; }
    const UnaryOpData& GetUnaryOp(NodeRef ref) const { return unaryOps[ref.Index()]; }

    TernaryExprData& GetTernaryExpression(NodeRef ref) { return ternaryExprs[ref.Index()]; }
    const TernaryExprData& GetTernaryExpression(NodeRef ref) const { return ternaryExprs[ref.Index()]; }

    MemberAccessData& GetMemberAccess(NodeRef ref) { return memberAccesses[ref.Index()]; }
    const MemberAccessData& GetMemberAccess(NodeRef ref) const { return memberAccesses[ref.Index()]; }

    ArrayAccessData& GetArrayAccess(NodeRef ref) { return arrayAccesses[ref.Index()]; }
    const ArrayAccessData& GetArrayAccess(NodeRef ref) const { return arrayAccesses[ref.Index()]; }

    AssignmentData& GetAssignment(NodeRef ref) { return assignments[ref.Index()]; }
    const AssignmentData& GetAssignment(NodeRef ref) const { return assignments[ref.Index()]; }

    ShaderStageData& GetShaderStage(NodeRef ref) { return shaderStages[ref.Index()]; }
    const ShaderStageData& GetShaderStage(NodeRef ref) const { return shaderStages[ref.Index()]; }

    BlockData& GetBlock(NodeRef ref) { return blocks[ref.Index()]; }
    const BlockData& GetBlock(NodeRef ref) const { return blocks[ref.Index()]; }

    VariableDeclData& GetVariableDecl(NodeRef ref) { return variableDecls[ref.Index()]; }
    const VariableDeclData& GetVariableDecl(NodeRef ref) const { return variableDecls[ref.Index()]; }

    FunctionCallData& GetFunctionCall(NodeRef ref) { return functionCalls[ref.Index()]; }
    const FunctionCallData& GetFunctionCall(NodeRef ref) const { return functionCalls[ref.Index()]; }

    AttributeDeclData& GetAttributeDecl(NodeRef ref) { return attributeDecls[ref.Index()]; }
    const AttributeDeclData& GetAttributeDecl(NodeRef ref) const { return attributeDecls[ref.Index()]; }

    ResourceDeclData& GetResourceDecl(NodeRef ref) { return resourceDecls[ref.Index()]; }
    const ResourceDeclData& GetResourceDecl(NodeRef ref) const { return resourceDecls[ref.Index()]; }

    ForCStyleData& GetForCStyle(NodeRef ref) { return forCStyles[ref.Index()]; }
    const ForCStyleData& GetForCStyle(NodeRef ref) const { return forCStyles[ref.Index()]; }

    ForRangeData& GetForRange(NodeRef ref) { return forRanges[ref.Index()]; }
    const ForRangeData& GetForRange(NodeRef ref) const { return forRanges[ref.Index()]; }

    ForCollectionData& GetForCollection(NodeRef ref) { return forCollections[ref.Index()]; }
    const ForCollectionData& GetForCollection(NodeRef ref) const { return forCollections[ref.Index()]; }

    LoopData& GetLoop(NodeRef ref) { return loops[ref.Index()]; }
    const LoopData& GetLoop(NodeRef ref) const { return loops[ref.Index()]; }

    FunctionDeclData& GetFunction(NodeRef ref) { return functions[ref.Index()]; }
    const FunctionDeclData& GetFunction(NodeRef ref) const { return functions[ref.Index()]; }

    StructDeclData& GetStructDecl(NodeRef ref) { return structDecls[ref.Index()]; }
    const StructDeclData& GetStructDecl(NodeRef ref) const { return structDecls[ref.Index()]; }

    ConstraintDeclData& GetConstraintDecl(NodeRef ref) { return constraintDecls[ref.Index()]; }
    const ConstraintDeclData& GetConstraintDecl(NodeRef ref) const { return constraintDecls[ref.Index()]; }

    PassData& GetPass(NodeRef ref) { return passes[ref.Index()]; }
    const PassData& GetPass(NodeRef ref) const { return passes[ref.Index()]; }

    ModuleNodeData& GetModule(NodeRef ref) { return modules[ref.Index()]; }
    const ModuleNodeData& GetModule(NodeRef ref) const { return modules[ref.Index()]; }

    ArrayTypeData& GetArrayType(NodeRef ref) { return arrayTypes[ref.Index()]; }
    const ArrayTypeData& GetArrayType(NodeRef ref) const { return arrayTypes[ref.Index()]; }

    EnumDeclData& GetEnumDecl(NodeRef ref) { return enumDecls[ref.Index()]; }
    const EnumDeclData& GetEnumDecl(NodeRef ref) const { return enumDecls[ref.Index()]; }

    PatternMatchData& GetPatternMatch(NodeRef ref) { return patternMatches[ref.Index()]; }
    const PatternMatchData& GetPatternMatch(NodeRef ref) const { return patternMatches[ref.Index()]; }

    TypePatternMatchData& GetTypePatternMatch(NodeRef ref) { return typePatternMatches[ref.Index()]; }
    const TypePatternMatchData& GetTypePatternMatch(NodeRef ref) const { return typePatternMatches[ref.Index()]; }

    TypePatternArmData& GetTypePatternArm(NodeRef ref) { return typePatternArms[ref.Index()]; }
    const TypePatternArmData& GetTypePatternArm(NodeRef ref) const { return typePatternArms[ref.Index()]; }

    SwitchCaseData& GetSwitchCase(NodeRef ref) { return switchCases[ref.Index()]; }
    const SwitchCaseData& GetSwitchCase(NodeRef ref) const { return switchCases[ref.Index()]; }

    SwitchData& GetSwitch(NodeRef ref) { return switches[ref.Index()]; }
    const SwitchData& GetSwitch(NodeRef ref) const { return switches[ref.Index()]; }

    PipelineData& GetPipeline(NodeRef ref) { return pipelines[ref.Index()]; }
    const PipelineData& GetPipeline(NodeRef ref) const { return pipelines[ref.Index()]; }

    ComputeGraphData& GetComputeGraph(NodeRef ref) { return computeGraphs[ref.Index()]; }
    const ComputeGraphData& GetComputeGraph(NodeRef ref) const { return computeGraphs[ref.Index()]; }
};

// FunctionCallFlags is defined in bwsl_ast_common.h

//==============================================================================
// Factory functions
//==============================================================================

namespace ASTFactory {

    inline NodeRef MakeIdentifier(AST* ast, const ArenaString& name, u32 line = 0, u32 col = 0) {
        u32 index = ast->identifiers.count;
        IdentifierData data;
        data.name = name;
        data.identifierKind = SpecialIdentifier::NONE;
        ast->identifiers.Push(ast->arena, data);

        // Store position
        if (ast->nodeCount >= ast->nodeCapacity) {
            // Grow positions array
            u32 newCapacity = ast->nodeCapacity * 2;
            u32* newPositions = (u32*)ast->arena->Allocate(sizeof(u32) * newCapacity, 64);
            memcpy(newPositions, ast->positions, ast->nodeCount * sizeof(u32));
            ast->positions = newPositions;
            ast->nodeCapacity = newCapacity;
        }
        ast->positions[ast->nodeCount++] = AST::PackPosition(line, col);

        return ast->MakeNodeRef(ASTNodeType::IDENTIFIER, index, line, col);
    }

    inline NodeRef MakeIdentifier(AST* ast, const std::string& name, u32 line = 0, u32 col = 0) {
        return MakeIdentifier(ast, ArenaString::MakeHashOnly(name), line, col);
    }

    inline NodeRef MakeLiteralFloat(AST* ast, float value, u32 line = 0, u32 col = 0) {
        u32 index = ast->literals.count;
        LiteralData data;
        data.value.type = LiteralValue::FLOAT;
        data.value.floatValue = value;
        ast->literals.Push(ast->arena, data);

        if (ast->nodeCount >= ast->nodeCapacity) {
            u32 newCapacity = ast->nodeCapacity * 2;
            u32* newPositions = (u32*)ast->arena->Allocate(sizeof(u32) * newCapacity, 64);
            memcpy(newPositions, ast->positions, ast->nodeCount * sizeof(u32));
            ast->positions = newPositions;
            ast->nodeCapacity = newCapacity;
        }
        ast->positions[ast->nodeCount++] = AST::PackPosition(line, col);

        return ast->MakeNodeRef(ASTNodeType::LITERAL, index, line, col);
    }

    inline NodeRef MakeLiteralInt(AST* ast, int32_t value, u32 line = 0, u32 col = 0) {
        u32 index = ast->literals.count;
        LiteralData data;
        data.value.type = LiteralValue::INT;
        data.value.intValue = value;
        ast->literals.Push(ast->arena, data);

        if (ast->nodeCount >= ast->nodeCapacity) {
            u32 newCapacity = ast->nodeCapacity * 2;
            u32* newPositions = (u32*)ast->arena->Allocate(sizeof(u32) * newCapacity, 64);
            memcpy(newPositions, ast->positions, ast->nodeCount * sizeof(u32));
            ast->positions = newPositions;
            ast->nodeCapacity = newCapacity;
        }
        ast->positions[ast->nodeCount++] = AST::PackPosition(line, col);

        return ast->MakeNodeRef(ASTNodeType::LITERAL, index, line, col);
    }

    inline NodeRef MakeLiteralUint(AST* ast, uint32_t value, u32 line = 0, u32 col = 0) {
        u32 index = ast->literals.count;
        LiteralData data;
        data.value.type = LiteralValue::UINT;
        data.value.uintValue = value;
        ast->literals.Push(ast->arena, data);

        if (ast->nodeCount >= ast->nodeCapacity) {
            u32 newCapacity = ast->nodeCapacity * 2;
            u32* newPositions = (u32*)ast->arena->Allocate(sizeof(u32) * newCapacity, 64);
            memcpy(newPositions, ast->positions, ast->nodeCount * sizeof(u32));
            ast->positions = newPositions;
            ast->nodeCapacity = newCapacity;
        }
        ast->positions[ast->nodeCount++] = AST::PackPosition(line, col);

        return ast->MakeNodeRef(ASTNodeType::LITERAL, index, line, col);
    }

    inline NodeRef MakeLiteralBool(AST* ast, bool value, u32 line = 0, u32 col = 0) {
        u32 index = ast->literals.count;
        LiteralData data;
        data.value.type = LiteralValue::BOOL;
        data.value.boolValue = value;
        ast->literals.Push(ast->arena, data);

        if (ast->nodeCount >= ast->nodeCapacity) {
            u32 newCapacity = ast->nodeCapacity * 2;
            u32* newPositions = (u32*)ast->arena->Allocate(sizeof(u32) * newCapacity, 64);
            memcpy(newPositions, ast->positions, ast->nodeCount * sizeof(u32));
            ast->positions = newPositions;
            ast->nodeCapacity = newCapacity;
        }
        ast->positions[ast->nodeCount++] = AST::PackPosition(line, col);

        return ast->MakeNodeRef(ASTNodeType::LITERAL, index, line, col);
    }

    inline NodeRef MakeBinaryOp(AST* ast, BinaryOpType op, NodeRef left, NodeRef right, u32 line = 0, u32 col = 0) {
        u32 index = ast->binaryOps.count;
        BinaryOpData data;
        data.op = op;
        data.left = left;
        data.right = right;
        ast->binaryOps.Push(ast->arena, data);

        if (ast->nodeCount >= ast->nodeCapacity) {
            u32 newCapacity = ast->nodeCapacity * 2;
            u32* newPositions = (u32*)ast->arena->Allocate(sizeof(u32) * newCapacity, 64);
            memcpy(newPositions, ast->positions, ast->nodeCount * sizeof(u32));
            ast->positions = newPositions;
            ast->nodeCapacity = newCapacity;
        }
        ast->positions[ast->nodeCount++] = AST::PackPosition(line, col);

        return ast->MakeNodeRef(ASTNodeType::BINARY_OP, index, line, col);
    }

    inline NodeRef MakeUnaryOp(AST* ast, UnaryOpType op, NodeRef operand, u32 line = 0, u32 col = 0) {
        u32 index = ast->unaryOps.count;
        UnaryOpData data;
        data.op = op;
        data.operand = operand;
        ast->unaryOps.Push(ast->arena, data);

        if (ast->nodeCount >= ast->nodeCapacity) {
            u32 newCapacity = ast->nodeCapacity * 2;
            u32* newPositions = (u32*)ast->arena->Allocate(sizeof(u32) * newCapacity, 64);
            memcpy(newPositions, ast->positions, ast->nodeCount * sizeof(u32));
            ast->positions = newPositions;
            ast->nodeCapacity = newCapacity;
        }
        ast->positions[ast->nodeCount++] = AST::PackPosition(line, col);

        return ast->MakeNodeRef(ASTNodeType::UNARY_OP, index, line, col);
    }

    inline NodeRef MakeTernaryExpr(AST* ast, NodeRef condition, NodeRef trueExpr, NodeRef falseExpr, u32 line = 0, u32 col = 0) {
        u32 index = ast->ternaryExprs.count;
        TernaryExprData data;
        data.condition = condition;
        data.trueExpr = trueExpr;
        data.falseExpr = falseExpr;
        ast->ternaryExprs.Push(ast->arena, data);

        if (ast->nodeCount >= ast->nodeCapacity) {
            u32 newCapacity = ast->nodeCapacity * 2;
            u32* newPositions = (u32*)ast->arena->Allocate(sizeof(u32) * newCapacity, 64);
            memcpy(newPositions, ast->positions, ast->nodeCount * sizeof(u32));
            ast->positions = newPositions;
            ast->nodeCapacity = newCapacity;
        }
        ast->positions[ast->nodeCount++] = AST::PackPosition(line, col);

        return ast->MakeNodeRef(ASTNodeType::TERNARY_EXPRESSION, index, line, col);
    }

    inline NodeRef MakeMemberAccess(AST* ast, NodeRef object, const ArenaString& member, u32 line = 0, u32 col = 0) {
        u32 index = ast->memberAccesses.count;
        MemberAccessData data;
        data.object = object;
        data.member = member;
        ast->memberAccesses.Push(ast->arena, data);

        if (ast->nodeCount >= ast->nodeCapacity) {
            u32 newCapacity = ast->nodeCapacity * 2;
            u32* newPositions = (u32*)ast->arena->Allocate(sizeof(u32) * newCapacity, 64);
            memcpy(newPositions, ast->positions, ast->nodeCount * sizeof(u32));
            ast->positions = newPositions;
            ast->nodeCapacity = newCapacity;
        }
        ast->positions[ast->nodeCount++] = AST::PackPosition(line, col);

        return ast->MakeNodeRef(ASTNodeType::MEMBER_ACCESS, index, line, col);
    }

    inline NodeRef MakeArrayAccess(AST* ast, NodeRef array, NodeRef index, u32 line = 0, u32 col = 0) {
        u32 idx = ast->arrayAccesses.count;
        ArrayAccessData data;
        data.array = array;
        data.index = index;
        ast->arrayAccesses.Push(ast->arena, data);

        if (ast->nodeCount >= ast->nodeCapacity) {
            u32 newCapacity = ast->nodeCapacity * 2;
            u32* newPositions = (u32*)ast->arena->Allocate(sizeof(u32) * newCapacity, 64);
            memcpy(newPositions, ast->positions, ast->nodeCount * sizeof(u32));
            ast->positions = newPositions;
            ast->nodeCapacity = newCapacity;
        }
        ast->positions[ast->nodeCount++] = AST::PackPosition(line, col);

        return ast->MakeNodeRef(ASTNodeType::ARRAY_ACCESS, idx, line, col);
    }

    inline NodeRef MakeAssignment(AST* ast, NodeRef target, NodeRef value, u32 line = 0, u32 col = 0,
                                  InterpolationMode interpolation = InterpolationMode::Default) {
        u32 index = ast->assignments.count;
        AssignmentData data;
        data.target = target;
        data.value = value;
        data.interpolation = interpolation;
        data._pad[0] = data._pad[1] = data._pad[2] = 0;
        ast->assignments.Push(ast->arena, data);

        if (ast->nodeCount >= ast->nodeCapacity) {
            u32 newCapacity = ast->nodeCapacity * 2;
            u32* newPositions = (u32*)ast->arena->Allocate(sizeof(u32) * newCapacity, 64);
            memcpy(newPositions, ast->positions, ast->nodeCount * sizeof(u32));
            ast->positions = newPositions;
            ast->nodeCapacity = newCapacity;
        }
        ast->positions[ast->nodeCount++] = AST::PackPosition(line, col);

        return ast->MakeNodeRef(ASTNodeType::ASSIGNMENT, index, line, col);
    }

    inline NodeRef MakeReturn(AST* ast, NodeRef value, u32 line = 0, u32 col = 0) {
        // Reuse assignment data - target is unused for returns
        u32 index = ast->assignments.count;
        AssignmentData data;
        data.target = NodeRef::Null();
        data.value = value;
        data.interpolation = InterpolationMode::Default;
        data._pad[0] = data._pad[1] = data._pad[2] = 0;
        ast->assignments.Push(ast->arena, data);

        if (ast->nodeCount >= ast->nodeCapacity) {
            u32 newCapacity = ast->nodeCapacity * 2;
            u32* newPositions = (u32*)ast->arena->Allocate(sizeof(u32) * newCapacity, 64);
            memcpy(newPositions, ast->positions, ast->nodeCount * sizeof(u32));
            ast->positions = newPositions;
            ast->nodeCapacity = newCapacity;
        }
        ast->positions[ast->nodeCount++] = AST::PackPosition(line, col);

        return ast->MakeNodeRef(ASTNodeType::RETURN, index, line, col);
    }

    inline NodeRef MakeBlock(AST* ast, u32 line = 0, u32 col = 0) {
        u32 index = ast->blocks.count;
        BlockData data;
        data.statements.Init(ast->arena, 16);
        ast->blocks.Push(ast->arena, data);

        if (ast->nodeCount >= ast->nodeCapacity) {
            u32 newCapacity = ast->nodeCapacity * 2;
            u32* newPositions = (u32*)ast->arena->Allocate(sizeof(u32) * newCapacity, 64);
            memcpy(newPositions, ast->positions, ast->nodeCount * sizeof(u32));
            ast->positions = newPositions;
            ast->nodeCapacity = newCapacity;
        }
        ast->positions[ast->nodeCount++] = AST::PackPosition(line, col);

        return ast->MakeNodeRef(ASTNodeType::BLOCK, index, line, col);
    }

    inline NodeRef MakeEvalBlock(AST* ast, u32 line = 0, u32 col = 0) {
        u32 index = ast->blocks.count;
        BlockData data;
        data.statements.Init(ast->arena, 8);
        ast->blocks.Push(ast->arena, data);

        if (ast->nodeCount >= ast->nodeCapacity) {
            u32 newCapacity = ast->nodeCapacity * 2;
            u32* newPositions = (u32*)ast->arena->Allocate(sizeof(u32) * newCapacity, 64);
            memcpy(newPositions, ast->positions, ast->nodeCount * sizeof(u32));
            ast->positions = newPositions;
            ast->nodeCapacity = newCapacity;
        }
        ast->positions[ast->nodeCount++] = AST::PackPosition(line, col);

        return ast->MakeNodeRef(ASTNodeType::EVAL_BLOCK, index, line, col);
    }

    // If statement - uses same BlockData storage but with IF_STATEMENT type
    // statements[0] = condition, statements[1] = then-body, statements[2] = else-body (optional)
    inline NodeRef MakeIfStatement(AST* ast, u32 line = 0, u32 col = 0) {
        u32 index = ast->blocks.count;
        BlockData data;
        data.statements.Init(ast->arena, 3);
        ast->blocks.Push(ast->arena, data);

        if (ast->nodeCount >= ast->nodeCapacity) {
            u32 newCapacity = ast->nodeCapacity * 2;
            u32* newPositions = (u32*)ast->arena->Allocate(sizeof(u32) * newCapacity, 64);
            memcpy(newPositions, ast->positions, ast->nodeCount * sizeof(u32));
            ast->positions = newPositions;
            ast->nodeCapacity = newCapacity;
        }
        ast->positions[ast->nodeCount++] = AST::PackPosition(line, col);

        return ast->MakeNodeRef(ASTNodeType::IF_STATEMENT, index, line, col);
    }

    inline NodeRef MakeEvalIfStatement(AST* ast, u32 line = 0, u32 col = 0) {
        u32 index = ast->blocks.count;
        BlockData data;
        data.statements.Init(ast->arena, 3);
        ast->blocks.Push(ast->arena, data);

        if (ast->nodeCount >= ast->nodeCapacity) {
            u32 newCapacity = ast->nodeCapacity * 2;
            u32* newPositions = (u32*)ast->arena->Allocate(sizeof(u32) * newCapacity, 64);
            memcpy(newPositions, ast->positions, ast->nodeCount * sizeof(u32));
            ast->positions = newPositions;
            ast->nodeCapacity = newCapacity;
        }
        ast->positions[ast->nodeCount++] = AST::PackPosition(line, col);

        return ast->MakeNodeRef(ASTNodeType::EVAL_IF, index, line, col);
    }

    inline NodeRef MakeFunctionCall(AST* ast, const ArenaString& name, u32 line = 0, u32 col = 0) {
        u32 index = ast->functionCalls.count;
        FunctionCallData data;
        data.name = name;
        data.arguments.Init(ast->arena, 4);
        data.intrinsicIndex = 0xFFFF;
        data.flags = 0;
        data._padding = 0;
        data.moduleIndex = 0xFFFFFFFF;
        data.moduleQualifiedHash = 0;
        data.moduleObject = NodeRef::Null();

        // Check if it's an intrinsic at creation
        if (const auto* intrinsic = StdLib::IntrinsicLookup::Find(name.nameHash)) {
            data.flags |= FunctionCallFlags::IS_INTRINSIC;
            data.intrinsicIndex = (u16)(intrinsic - StdLib::INTRINSICS);

            if ((intrinsic->flags & StdLib::IntrinsicFlags::CUSTOM_METAL) ||
                (intrinsic->flags & StdLib::IntrinsicFlags::CUSTOM_HLSL) ||
                (intrinsic->flags & StdLib::IntrinsicFlags::CUSTOM_GLSL)) {
                data.flags |= FunctionCallFlags::NEEDS_CUSTOM_IMPL;
            }

            if (intrinsic->flags & StdLib::IntrinsicFlags::TEXTURE_OP) {
                data.flags |= FunctionCallFlags::IS_TEXTURE_OP;
            }

            if (intrinsic->flags & StdLib::IntrinsicFlags::WAVE_OP) {
                data.flags |= FunctionCallFlags::IS_WAVE_OP;
            }
        }

        ast->functionCalls.Push(ast->arena, data);

        if (ast->nodeCount >= ast->nodeCapacity) {
            u32 newCapacity = ast->nodeCapacity * 2;
            u32* newPositions = (u32*)ast->arena->Allocate(sizeof(u32) * newCapacity, 64);
            memcpy(newPositions, ast->positions, ast->nodeCount * sizeof(u32));
            ast->positions = newPositions;
            ast->nodeCapacity = newCapacity;
        }
        ast->positions[ast->nodeCount++] = AST::PackPosition(line, col);

        return ast->MakeNodeRef(ASTNodeType::FUNCTION_CALL, index, line, col);
    }

    inline NodeRef MakeFunctionCall(AST* ast, const std::string& name, u32 line = 0, u32 col = 0) {
        return MakeFunctionCall(ast, ArenaString::MakeHashOnly(name), line, col);
    }

    inline NodeRef MakeVariableDecl(AST* ast, const ArenaString& name, const ArenaString& type,
                                    NodeRef initializer, bool isConst, u32 line = 0, u32 col = 0,
                                    StorageClass storageClass = StorageClass::Default,
                                    u8 arrayDimensions = 0, u32 arrayLength = 0,
                                    u32 arrayElementTypeHash = 0) {
        u32 index = ast->variableDecls.count;
        VariableDeclData data;
        data.name = name;
        data.type = type;
        data.initializer = initializer;
        data.isConst = isConst;
        data.isEval = false;
        data.storageClass = storageClass;
        data.arrayDimensions = arrayDimensions;
        data.arrayLength = arrayLength;
        data.arrayElementTypeHash = arrayElementTypeHash;
        ast->variableDecls.Push(ast->arena, data);

        if (ast->nodeCount >= ast->nodeCapacity) {
            u32 newCapacity = ast->nodeCapacity * 2;
            u32* newPositions = (u32*)ast->arena->Allocate(sizeof(u32) * newCapacity, 64);
            memcpy(newPositions, ast->positions, ast->nodeCount * sizeof(u32));
            ast->positions = newPositions;
            ast->nodeCapacity = newCapacity;
        }
        ast->positions[ast->nodeCount++] = AST::PackPosition(line, col);

        return ast->MakeNodeRef(ASTNodeType::VARIABLE_DECL, index, line, col);
    }

    inline NodeRef MakeShaderStage(AST* ast, ASTNodeType stageType, NodeRef body, u32 line = 0, u32 col = 0) {
        u32 index = ast->shaderStages.count;
        ShaderStageData data;
        data.body = body;
        data.inheritsFrom = ArenaString::MakeHashOnly(0u);
        data.isInherited = false;
        data.isDeferred = false;
        data.name = ArenaString::MakeHashOnly(0u);
        data.workgroupSizeX = 1;
        data.workgroupSizeY = 1;
        data.workgroupSizeZ = 1;
        data.deferredExpr = NodeRef::Null();
        ast->shaderStages.Push(ast->arena, data);

        if (ast->nodeCount >= ast->nodeCapacity) {
            u32 newCapacity = ast->nodeCapacity * 2;
            u32* newPositions = (u32*)ast->arena->Allocate(sizeof(u32) * newCapacity, 64);
            memcpy(newPositions, ast->positions, ast->nodeCount * sizeof(u32));
            ast->positions = newPositions;
            ast->nodeCapacity = newCapacity;
        }
        ast->positions[ast->nodeCount++] = AST::PackPosition(line, col);

        return ast->MakeNodeRef(stageType, index, line, col);
    }

    inline NodeRef MakeAttributeDecl(AST* ast, const std::string& name, const std::string& type,
                                     u32 line = 0, u32 col = 0) {
        u32 index = ast->attributeDecls.count;
        AttributeDeclData data;
        data.name = ArenaString::MakeHashOnly(name);
        data.dataType = ArenaString::MakeHashOnly(type);
        ReverseLookup::Register(data.name.nameHash, name.c_str());
        ReverseLookup::Register(data.dataType.nameHash, type.c_str());
        data.compression = ArenaString::MakeHashOnly(0u);
        data.attributeIndex = 0xFF;
        data.isInstance = false;
        ast->attributeDecls.Push(ast->arena, data);

        if (ast->nodeCount >= ast->nodeCapacity) {
            u32 newCapacity = ast->nodeCapacity * 2;
            u32* newPositions = (u32*)ast->arena->Allocate(sizeof(u32) * newCapacity, 64);
            memcpy(newPositions, ast->positions, ast->nodeCount * sizeof(u32));
            ast->positions = newPositions;
            ast->nodeCapacity = newCapacity;
        }
        ast->positions[ast->nodeCount++] = AST::PackPosition(line, col);

        return ast->MakeNodeRef(ASTNodeType::ATTRIBUTE_DECL, index, line, col);
    }

    inline NodeRef MakeResourceDecl(AST* ast, const std::string& name, const std::string& type,
                                    u32 line = 0, u32 col = 0) {
        u32 index = ast->resourceDecls.count;
        ResourceDeclData data;
        data.name = ArenaString::MakeHashOnly(name);
        data.typeName = ArenaString::MakeHashOnly(type);
        data.resourceIndex = 0xFF;
        ast->resourceDecls.Push(ast->arena, data);

        if (ast->nodeCount >= ast->nodeCapacity) {
            u32 newCapacity = ast->nodeCapacity * 2;
            u32* newPositions = (u32*)ast->arena->Allocate(sizeof(u32) * newCapacity, 64);
            memcpy(newPositions, ast->positions, ast->nodeCount * sizeof(u32));
            ast->positions = newPositions;
            ast->nodeCapacity = newCapacity;
        }
        ast->positions[ast->nodeCount++] = AST::PackPosition(line, col);

        return ast->MakeNodeRef(ASTNodeType::RESOURCE_DECL, index, line, col);
    }

    inline NodeRef MakePass(AST* ast, const std::string& name, u32 line = 0, u32 col = 0) {
        u32 index = ast->passes.count;
        PassData data;
        data.name = ArenaString::MakeHashOnly(name);
        ReverseLookup::Register(data.name.nameHash, name.c_str());
        data.usedAttributes.Init(ast->arena, 8);
        data.usedResources.Init(ast->arena, 8);
        data.fragmentOutputs.Init(ast->arena, 4);
        data.consts.Init(ast->arena, 4);
        data.functions.Init(ast->arena, 8);  // Pass-scoped functions
        data.attributeBindings.Init(ast->arena, 4);
        data.resourceBindings.Init(ast->arena, 4);
        data.variantBindings.Init(ast->arena, 4);
        data.vertexShader = NodeRef::Null();
        data.fragmentShader = NodeRef::Null();
        data.computeShader = NodeRef::Null();
        data.passBlockCall = NodeRef::Null();
        data.optionalAttributesMask = 0;
        data.optionalResourcesMask = 0;
        data.hasFragmentOutputs = false;
        data.isPassBlockInstance = false;
        ast->passes.Push(ast->arena, data);

        if (ast->nodeCount >= ast->nodeCapacity) {
            u32 newCapacity = ast->nodeCapacity * 2;
            u32* newPositions = (u32*)ast->arena->Allocate(sizeof(u32) * newCapacity, 64);
            memcpy(newPositions, ast->positions, ast->nodeCount * sizeof(u32));
            ast->positions = newPositions;
            ast->nodeCapacity = newCapacity;
        }
        ast->positions[ast->nodeCount++] = AST::PackPosition(line, col);

        return ast->MakeNodeRef(ASTNodeType::PASS, index, line, col);
    }

    inline NodeRef MakePipeline(AST* ast, const std::string& name, u32 line = 0, u32 col = 0) {
        u32 index = ast->pipelines.count;
        PipelineData data;
        data.name = ArenaString::MakeHashOnly(name);
        data.imports.Init(ast->arena, 4);
        data.usingImports.Init(ast->arena, 4);
        data.attributes.Init(ast->arena, 16);
        data.resources.Init(ast->arena, 16);
        data.variantDecls.Init(ast->arena, 4);
        data.variantRules.Init(ast->arena, 4);
        data.passes.Init(ast->arena, 8);
        data.functions.Init(ast->arena, 16);
        data.enums.Init(ast->arena, 8);
        data.constraints.Init(ast->arena, 8);
        data.computeGraph = NodeRef::Null();
        ast->pipelines.Push(ast->arena, data);

        if (ast->nodeCount >= ast->nodeCapacity) {
            u32 newCapacity = ast->nodeCapacity * 2;
            u32* newPositions = (u32*)ast->arena->Allocate(sizeof(u32) * newCapacity, 64);
            memcpy(newPositions, ast->positions, ast->nodeCount * sizeof(u32));
            ast->positions = newPositions;
            ast->nodeCapacity = newCapacity;
        }
        ast->positions[ast->nodeCount++] = AST::PackPosition(line, col);

        return ast->MakeNodeRef(ASTNodeType::PIPELINE, index, line, col);
    }

    inline NodeRef MakeComputeGraph(AST* ast, u32 line = 0, u32 col = 0) {
        u32 index = ast->computeGraphs.count;
        ComputeGraphData data;
        data.nodes.Init(ast->arena, 4);
        ast->computeGraphs.Push(ast->arena, data);

        if (ast->nodeCount >= ast->nodeCapacity) {
            u32 newCapacity = ast->nodeCapacity * 2;
            u32* newPositions = (u32*)ast->arena->Allocate(sizeof(u32) * newCapacity, 64);
            memcpy(newPositions, ast->positions, ast->nodeCount * sizeof(u32));
            ast->positions = newPositions;
            ast->nodeCapacity = newCapacity;
        }
        ast->positions[ast->nodeCount++] = AST::PackPosition(line, col);

        return ast->MakeNodeRef(ASTNodeType::COMPUTE_GRAPH, index, line, col);
    }

    inline NodeRef MakeFunction(AST* ast, const std::string& name, CoreType returnType, u32 line = 0, u32 col = 0) {
        u32 index = ast->functions.count;
        FunctionDeclData data;
        data.name = ArenaString::MakeHashOnly(name);
        data.returnType = returnType;
        data.returnTypeHash = 0;
        data.ownerStructTypeHash = 0;
        data.parameters.Init(ast->arena, 4);
        data.body = NodeRef::Null();
        data.isEval = false;
        data.isStructMethod = false;
        data.isConstMethod = false;
        ast->functions.Push(ast->arena, data);

        if (ast->nodeCount >= ast->nodeCapacity) {
            u32 newCapacity = ast->nodeCapacity * 2;
            u32* newPositions = (u32*)ast->arena->Allocate(sizeof(u32) * newCapacity, 64);
            memcpy(newPositions, ast->positions, ast->nodeCount * sizeof(u32));
            ast->positions = newPositions;
            ast->nodeCapacity = newCapacity;
        }
        ast->positions[ast->nodeCount++] = AST::PackPosition(line, col);

        return ast->MakeNodeRef(ASTNodeType::FUNCTION, index, line, col);
    }

    inline NodeRef MakeStructDecl(AST* ast, const std::string& name, u32 line = 0, u32 col = 0) {
        u32 index = ast->structDecls.count;
        StructDeclData data;
        data.name = ArenaString::MakeHashOnly(name);
        data.fields.Init(ast->arena, 8);
        data.methods.Init(ast->arena, 4);
        ast->structDecls.Push(ast->arena, data);

        if (ast->nodeCount >= ast->nodeCapacity) {
            u32 newCapacity = ast->nodeCapacity * 2;
            u32* newPositions = (u32*)ast->arena->Allocate(sizeof(u32) * newCapacity, 64);
            memcpy(newPositions, ast->positions, ast->nodeCount * sizeof(u32));
            ast->positions = newPositions;
            ast->nodeCapacity = newCapacity;
        }
        ast->positions[ast->nodeCount++] = AST::PackPosition(line, col);

        return ast->MakeNodeRef(ASTNodeType::STRUCT_DECL, index, line, col);
    }

    inline NodeRef MakeForCStyle(AST* ast, NodeRef init, NodeRef condition, NodeRef increment,
                                  NodeRef body, bool isEval, u32 line = 0, u32 col = 0,
                                  bool isWhile = false) {
        u32 index = ast->forCStyles.count;
        ForCStyleData data;
        data.init = init;
        data.condition = condition;
        data.increment = increment;
        data.body = body;
        data.isEval = isEval;
        data.isWhile = isWhile;
        ast->forCStyles.Push(ast->arena, data);

        if (ast->nodeCount >= ast->nodeCapacity) {
            u32 newCapacity = ast->nodeCapacity * 2;
            u32* newPositions = (u32*)ast->arena->Allocate(sizeof(u32) * newCapacity, 64);
            memcpy(newPositions, ast->positions, ast->nodeCount * sizeof(u32));
            ast->positions = newPositions;
            ast->nodeCapacity = newCapacity;
        }
        ast->positions[ast->nodeCount++] = AST::PackPosition(line, col);

        return ast->MakeNodeRef(ASTNodeType::FOR_CSTYLE, index, line, col);
    }

    inline NodeRef MakeForRange(AST* ast, NodeRef iteratorVar, NodeRef rangeStart, NodeRef rangeEnd,
                                 NodeRef step, NodeRef body, bool inclusive, bool isEval, u32 line = 0, u32 col = 0) {
        u32 index = ast->forRanges.count;
        ForRangeData data;
        data.iteratorVar = iteratorVar;
        data.rangeStart = rangeStart;
        data.rangeEnd = rangeEnd;
        data.step = step;
        data.body = body;
        data.inclusive = inclusive;
        data.isEval = isEval;
        ast->forRanges.Push(ast->arena, data);

        if (ast->nodeCount >= ast->nodeCapacity) {
            u32 newCapacity = ast->nodeCapacity * 2;
            u32* newPositions = (u32*)ast->arena->Allocate(sizeof(u32) * newCapacity, 64);
            memcpy(newPositions, ast->positions, ast->nodeCount * sizeof(u32));
            ast->positions = newPositions;
            ast->nodeCapacity = newCapacity;
        }
        ast->positions[ast->nodeCount++] = AST::PackPosition(line, col);

        return ast->MakeNodeRef(ASTNodeType::FOR_RANGE, index, line, col);
    }

    inline NodeRef MakeForCollection(AST* ast, NodeRef iteratorVar, NodeRef collection,
                                      NodeRef body, bool isEval, u32 length, u32 line = 0, u32 col = 0) {
        u32 index = ast->forCollections.count;
        ForCollectionData data;
        data.iteratorVar = iteratorVar;
        data.collection = collection;
        data.body = body;
        data.isEval = isEval;
        data.length = length;
        ast->forCollections.Push(ast->arena, data);

        if (ast->nodeCount >= ast->nodeCapacity) {
            u32 newCapacity = ast->nodeCapacity * 2;
            u32* newPositions = (u32*)ast->arena->Allocate(sizeof(u32) * newCapacity, 64);
            memcpy(newPositions, ast->positions, ast->nodeCount * sizeof(u32));
            ast->positions = newPositions;
            ast->nodeCapacity = newCapacity;
        }
        ast->positions[ast->nodeCount++] = AST::PackPosition(line, col);

        return ast->MakeNodeRef(ASTNodeType::FOR_COLLECTION, index, line, col);
    }

    inline NodeRef MakeLoop(AST* ast, NodeRef count, NodeRef body, NodeRef untilCondition,
                            bool isEval, u32 line = 0, u32 col = 0) {
        u32 index = ast->loops.count;
        LoopData data;
        data.count = count;
        data.body = body;
        data.untilCondition = untilCondition;
        data.isEval = isEval;
        ast->loops.Push(ast->arena, data);

        if (ast->nodeCount >= ast->nodeCapacity) {
            u32 newCapacity = ast->nodeCapacity * 2;
            u32* newPositions = (u32*)ast->arena->Allocate(sizeof(u32) * newCapacity, 64);
            memcpy(newPositions, ast->positions, ast->nodeCount * sizeof(u32));
            ast->positions = newPositions;
            ast->nodeCapacity = newCapacity;
        }
        ast->positions[ast->nodeCount++] = AST::PackPosition(line, col);

        return ast->MakeNodeRef(ASTNodeType::LOOP, index, line, col);
    }

    inline NodeRef MakeConstraint(AST* ast, const ArenaString& name, TypeMask allowedTypes,
                                   u32 line = 0, u32 col = 0) {
        u32 index = ast->constraintDecls.count;
        ConstraintDeclData data;
        data.name = name;
        data.allowedTypes = allowedTypes;
        ast->constraintDecls.Push(ast->arena, data);

        if (ast->nodeCount >= ast->nodeCapacity) {
            u32 newCapacity = ast->nodeCapacity * 2;
            u32* newPositions = (u32*)ast->arena->Allocate(sizeof(u32) * newCapacity, 64);
            memcpy(newPositions, ast->positions, ast->nodeCount * sizeof(u32));
            ast->positions = newPositions;
            ast->nodeCapacity = newCapacity;
        }
        ast->positions[ast->nodeCount++] = AST::PackPosition(line, col);

        return ast->MakeNodeRef(ASTNodeType::CONSTRAINT_DECL, index, line, col);
    }

    inline NodeRef MakeEnumDecl(AST* ast, const ArenaString& name, CoreType underlyingType,
                                 u32 line = 0, u32 col = 0) {
        u32 index = ast->enumDecls.count;
        EnumDeclData data;
        data.name = name;
        data.underlyingType = underlyingType;
        data.currentVariant.associatedTypes.Init(ast->arena, 4);
        data.currentVariant.associatedTypeHashes.Init(ast->arena, 4);
        data.variants.Init(ast->arena, 8);
        data.methods.Init(ast->arena, 4);
        data.associatedTypes.Init(ast->arena, 0);
        data.associatedTypeHashes.Init(ast->arena, 0);
        ast->enumDecls.Push(ast->arena, data);

        if (ast->nodeCount >= ast->nodeCapacity) {
            u32 newCapacity = ast->nodeCapacity * 2;
            u32* newPositions = (u32*)ast->arena->Allocate(sizeof(u32) * newCapacity, 64);
            memcpy(newPositions, ast->positions, ast->nodeCount * sizeof(u32));
            ast->positions = newPositions;
            ast->nodeCapacity = newCapacity;
        }
        ast->positions[ast->nodeCount++] = AST::PackPosition(line, col);

        return ast->MakeNodeRef(ASTNodeType::ENUM_DECL, index, line, col);
    }

    inline NodeRef MakeVariantDecl(AST* ast, const ArenaString& name, u32 value, u32 line = 0, u32 col = 0) {
        u32 index = ast->enumDecls.count;
        EnumDeclData data;
        data.name = name;
        data.underlyingType = CoreType::INVALID;
        data.currentVariant.name = name;
        data.currentVariant.value = value;
        data.currentVariant.associatedTypes.Init(ast->arena, 4);
        data.currentVariant.associatedTypeHashes.Init(ast->arena, 4);
        data.variants.Init(ast->arena, 0);
        data.methods.Init(ast->arena, 0);
        data.associatedTypes.Init(ast->arena, 4);
        data.associatedTypeHashes.Init(ast->arena, 4);
        ast->enumDecls.Push(ast->arena, data);

        if (ast->nodeCount >= ast->nodeCapacity) {
            u32 newCapacity = ast->nodeCapacity * 2;
            u32* newPositions = (u32*)ast->arena->Allocate(sizeof(u32) * newCapacity, 64);
            memcpy(newPositions, ast->positions, ast->nodeCount * sizeof(u32));
            ast->positions = newPositions;
            ast->nodeCapacity = newCapacity;
        }
        ast->positions[ast->nodeCount++] = AST::PackPosition(line, col);

        return ast->MakeNodeRef(ASTNodeType::VARIANT_DECL, index, line, col);
    }

    inline NodeRef MakePatternMatch(AST* ast, const ArenaString& scrutinee, u32 line = 0, u32 col = 0) {
        u32 index = ast->patternMatches.count;
        PatternMatchData data;
        data.scrutinee = scrutinee;
        data.arms.Init(ast->arena, 8);
        data.defaultArm = NodeRef::Null();
        data.body = NodeRef::Null();
        data.bindings.Init(ast->arena, 0);
        data.statements.Init(ast->arena, 0);
        data.variantName = ArenaString::MakeHashOnly(0u);
        data.variantHash = 0;
        data.isEval = false;
        data.isDefault = false;
        ast->patternMatches.Push(ast->arena, data);

        if (ast->nodeCount >= ast->nodeCapacity) {
            u32 newCapacity = ast->nodeCapacity * 2;
            u32* newPositions = (u32*)ast->arena->Allocate(sizeof(u32) * newCapacity, 64);
            memcpy(newPositions, ast->positions, ast->nodeCount * sizeof(u32));
            ast->positions = newPositions;
            ast->nodeCapacity = newCapacity;
        }
        ast->positions[ast->nodeCount++] = AST::PackPosition(line, col);

        return ast->MakeNodeRef(ASTNodeType::PATTERN_MATCH, index, line, col);
    }

    inline NodeRef MakePatternMatchArm(AST* ast, const ArenaString& variantName, bool isDefault, u32 line = 0, u32 col = 0) {
        u32 index = ast->patternMatches.count;
        PatternMatchData data;
        data.scrutinee = ArenaString::MakeHashOnly(0u);
        data.arms.Init(ast->arena, 0);
        data.defaultArm = NodeRef::Null();
        data.body = NodeRef::Null();
        data.bindings.Init(ast->arena, 4);
        data.statements.Init(ast->arena, 0);
        data.variantName = variantName;
        data.variantHash = variantName.nameHash;
        data.isEval = false;
        data.isDefault = isDefault;
        ast->patternMatches.Push(ast->arena, data);

        if (ast->nodeCount >= ast->nodeCapacity) {
            u32 newCapacity = ast->nodeCapacity * 2;
            u32* newPositions = (u32*)ast->arena->Allocate(sizeof(u32) * newCapacity, 64);
            memcpy(newPositions, ast->positions, ast->nodeCount * sizeof(u32));
            ast->positions = newPositions;
            ast->nodeCapacity = newCapacity;
        }
        ast->positions[ast->nodeCount++] = AST::PackPosition(line, col);

        return ast->MakeNodeRef(ASTNodeType::PATTERN_MATCH_ARM, index, line, col);
    }

    inline NodeRef MakeTypePatternMatch(AST* ast, u32 line = 0, u32 col = 0) {
        u32 index = ast->typePatternMatches.count;
        TypePatternMatchData data;
        data.arms.Init(ast->arena, 4);
        data.defaultArm = NodeRef::Null();
        ast->typePatternMatches.Push(ast->arena, data);

        if (ast->nodeCount >= ast->nodeCapacity) {
            u32 newCapacity = ast->nodeCapacity * 2;
            u32* newPositions = (u32*)ast->arena->Allocate(sizeof(u32) * newCapacity, 64);
            memcpy(newPositions, ast->positions, ast->nodeCount * sizeof(u32));
            ast->positions = newPositions;
            ast->nodeCapacity = newCapacity;
        }
        ast->positions[ast->nodeCount++] = AST::PackPosition(line, col);

        return ast->MakeNodeRef(ASTNodeType::TYPE_PATTERN_MATCH, index, line, col);
    }

    inline NodeRef MakeTypePatternArm(AST* ast, CoreType matchType, bool isDefault, NodeRef body, u32 line = 0, u32 col = 0) {
        u32 index = ast->typePatternArms.count;
        TypePatternArmData data;
        data.matchType = matchType;
        data.body = body;
        data.isDefault = isDefault;
        ast->typePatternArms.Push(ast->arena, data);

        if (ast->nodeCount >= ast->nodeCapacity) {
            u32 newCapacity = ast->nodeCapacity * 2;
            u32* newPositions = (u32*)ast->arena->Allocate(sizeof(u32) * newCapacity, 64);
            memcpy(newPositions, ast->positions, ast->nodeCount * sizeof(u32));
            ast->positions = newPositions;
            ast->nodeCapacity = newCapacity;
        }
        ast->positions[ast->nodeCount++] = AST::PackPosition(line, col);

        return ast->MakeNodeRef(ASTNodeType::TYPE_PATTERN_ARM, index, line, col);
    }

    inline NodeRef MakeModule(AST* ast, const std::string& name, u32 line = 0, u32 col = 0) {
        u32 index = ast->modules.count;
        ModuleNodeData data;
        data.name = ArenaString::MakeHashOnly(name);
        data.imports.Init(ast->arena, 4);
        data.usingImports.Init(ast->arena, 4);
        data.functions.Init(ast->arena, 16);
        data.structs.Init(ast->arena, 8);
        data.enums.Init(ast->arena, 4);
        data.attributes.Init(ast->arena, 8);
        data.resources.Init(ast->arena, 8);
        data.variantDecls.Init(ast->arena, 4);
        data.variantRules.Init(ast->arena, 4);
        ast->modules.Push(ast->arena, data);

        if (ast->nodeCount >= ast->nodeCapacity) {
            u32 newCapacity = ast->nodeCapacity * 2;
            u32* newPositions = (u32*)ast->arena->Allocate(sizeof(u32) * newCapacity, 64);
            memcpy(newPositions, ast->positions, ast->nodeCount * sizeof(u32));
            ast->positions = newPositions;
            ast->nodeCapacity = newCapacity;
        }
        ast->positions[ast->nodeCount++] = AST::PackPosition(line, col);

        return ast->MakeNodeRef(ASTNodeType::MODULE, index, line, col);
    }

    inline NodeRef MakeSwitchCase(AST* ast, NodeRef body, bool isDefault, u32 line = 0, u32 col = 0) {
        u32 index = ast->switchCases.count;
        SwitchCaseData data;
        data.values.Init(ast->arena, isDefault ? 0 : 4);  // Empty for default, small initial size for cases
        data.body = body;
        data.isDefault = isDefault;
        ast->switchCases.Push(ast->arena, data);

        if (ast->nodeCount >= ast->nodeCapacity) {
            u32 newCapacity = ast->nodeCapacity * 2;
            u32* newPositions = (u32*)ast->arena->Allocate(sizeof(u32) * newCapacity, 64);
            memcpy(newPositions, ast->positions, ast->nodeCount * sizeof(u32));
            ast->positions = newPositions;
            ast->nodeCapacity = newCapacity;
        }
        ast->positions[ast->nodeCount++] = AST::PackPosition(line, col);

        return ast->MakeNodeRef(ASTNodeType::SWITCH_CASE, index, line, col);
    }

    inline NodeRef MakeSwitch(AST* ast, NodeRef expression, u32 line = 0, u32 col = 0) {
        u32 index = ast->switches.count;
        SwitchData data;
        data.expression = expression;
        data.cases.Init(ast->arena, 8);
        data.defaultCase = NodeRef::Null();
        data.isExhaustive = false;
        ast->switches.Push(ast->arena, data);

        if (ast->nodeCount >= ast->nodeCapacity) {
            u32 newCapacity = ast->nodeCapacity * 2;
            u32* newPositions = (u32*)ast->arena->Allocate(sizeof(u32) * newCapacity, 64);
            memcpy(newPositions, ast->positions, ast->nodeCount * sizeof(u32));
            ast->positions = newPositions;
            ast->nodeCapacity = newCapacity;
        }
        ast->positions[ast->nodeCount++] = AST::PackPosition(line, col);

        return ast->MakeNodeRef(ASTNodeType::SWITCH, index, line, col);
    }

} // namespace ASTFactory

//==============================================================================
// AST Cloning for Generic Function Instantiation
//==============================================================================

namespace ASTClone {

    // Type substitution map: maps constraint type hash -> concrete type name
    struct TypeSubstitution {
        u32 constraintHash;     // Hash of constraint type name (e.g., "FloatVectors")
        ArenaString concreteType; // Concrete type name (e.g., "float3")
        CoreType coreType;      // CoreType for the concrete type
    };

    struct CloneContext {
        AST* ast;
        BWSL_Arena* arena;
        const TypeSubstitution* substitutions;
        u32 substitutionCount;

        // Check if a type name should be substituted
        inline const TypeSubstitution* FindSubstitution(u32 typeHash) const {
            for (u32 i = 0; i < substitutionCount; i++) {
                if (substitutions[i].constraintHash == typeHash) {
                    return &substitutions[i];
                }
            }
            return nullptr;
        }

        // Substitute type if needed, returns original if no substitution found
        inline ArenaString SubstituteType(const ArenaString& typeName) const {
            const TypeSubstitution* sub = FindSubstitution(typeName.nameHash);
            return sub ? sub->concreteType : typeName;
        }
    };

    // Forward declaration
    NodeRef CloneNode(CloneContext& ctx, NodeRef node);

    // Clone a block and all its statements
    inline NodeRef CloneBlock(CloneContext& ctx, NodeRef blockRef) {
        if (blockRef.IsNull()) return NodeRef::Null();

        const BlockData& src = ctx.ast->GetBlock(blockRef);
        u32 line = ctx.ast->GetLine(blockRef);
        u32 col = ctx.ast->GetColumn(blockRef);
        std::vector<NodeRef> statements;
        statements.reserve(src.statements.count);
        for (u32 i = 0; i < src.statements.count; i++) {
            statements.push_back(src.statements[i]);
        }

        NodeRef newBlock = ASTFactory::MakeBlock(ctx.ast, line, col);

        for (NodeRef stmt : statements) {
            NodeRef cloned = CloneNode(ctx, stmt);
            if (cloned.IsValid()) {
                ctx.ast->GetBlock(newBlock).statements.Push(ctx.arena, cloned);
            }
        }

        return newBlock;
    }

    // Clone a function call
    inline NodeRef CloneFunctionCall(CloneContext& ctx, NodeRef callRef) {
        const FunctionCallData& src = ctx.ast->GetFunctionCall(callRef);
        u32 line = ctx.ast->GetLine(callRef);
        u32 col = ctx.ast->GetColumn(callRef);
        ArenaString name = src.name;
        u16 intrinsicIndex = src.intrinsicIndex;
        u8 flags = src.flags;
        u32 moduleIndex = src.moduleIndex;
        u32 moduleQualifiedHash = src.moduleQualifiedHash;
        NodeRef moduleObject = src.moduleObject;
        std::vector<NodeRef> args;
        args.reserve(src.arguments.count);
        for (u32 i = 0; i < src.arguments.count; i++) {
            args.push_back(src.arguments[i]);
        }
        NodeRef clonedModuleObject = moduleObject.IsValid()
            ? CloneNode(ctx, moduleObject)
            : NodeRef::Null();

        NodeRef newCall = ASTFactory::MakeFunctionCall(ctx.ast, name, line, col);
        FunctionCallData& dst = ctx.ast->GetFunctionCall(newCall);

        dst.intrinsicIndex = intrinsicIndex;
        dst.flags = flags;
        dst.moduleIndex = moduleIndex;
        dst.moduleQualifiedHash = moduleQualifiedHash;
        dst.moduleObject = clonedModuleObject;

        for (NodeRef arg : args) {
            NodeRef clonedArg = CloneNode(ctx, arg);
            ctx.ast->GetFunctionCall(newCall).arguments.Push(ctx.arena, clonedArg);
        }

        return newCall;
    }

    // Clone a variable declaration with type substitution
    inline NodeRef CloneVariableDecl(CloneContext& ctx, NodeRef varRef) {
        const VariableDeclData& src = ctx.ast->GetVariableDecl(varRef);
        u32 line = ctx.ast->GetLine(varRef);
        u32 col = ctx.ast->GetColumn(varRef);

        // Substitute type if it's a constrained type
        ArenaString newType = ctx.SubstituteType(src.type);

        // Clone initializer first
        NodeRef clonedInit = src.initializer.IsValid() ? CloneNode(ctx, src.initializer) : NodeRef::Null();

        NodeRef newVar = ASTFactory::MakeVariableDecl(ctx.ast, src.name, newType, clonedInit, src.isConst,
                                                      line, col, src.storageClass, src.arrayDimensions,
                                                      src.arrayLength, src.arrayElementTypeHash);

        return newVar;
    }

    // Clone a binary operation
    inline NodeRef CloneBinaryOp(CloneContext& ctx, NodeRef binRef) {
        const BinaryOpData& src = ctx.ast->GetBinaryOp(binRef);
        u32 line = ctx.ast->GetLine(binRef);
        u32 col = ctx.ast->GetColumn(binRef);

        NodeRef left = CloneNode(ctx, src.left);
        NodeRef right = CloneNode(ctx, src.right);

        return ASTFactory::MakeBinaryOp(ctx.ast, src.op, left, right, line, col);
    }

    // Clone a unary operation
    inline NodeRef CloneUnaryOp(CloneContext& ctx, NodeRef unaryRef) {
        const UnaryOpData& src = ctx.ast->GetUnaryOp(unaryRef);
        u32 line = ctx.ast->GetLine(unaryRef);
        u32 col = ctx.ast->GetColumn(unaryRef);

        NodeRef operand = CloneNode(ctx, src.operand);

        return ASTFactory::MakeUnaryOp(ctx.ast, src.op, operand, line, col);
    }

    // Clone an assignment
    inline NodeRef CloneAssignment(CloneContext& ctx, NodeRef assignRef) {
        const AssignmentData& src = ctx.ast->GetAssignment(assignRef);
        u32 line = ctx.ast->GetLine(assignRef);
        u32 col = ctx.ast->GetColumn(assignRef);

        NodeRef target = CloneNode(ctx, src.target);
        NodeRef value = CloneNode(ctx, src.value);

        return ASTFactory::MakeAssignment(ctx.ast, target, value, line, col, src.interpolation);
    }

    // Clone a return statement (returns reuse AssignmentData with null target)
    inline NodeRef CloneReturn(CloneContext& ctx, NodeRef retRef) {
        const AssignmentData& src = ctx.ast->GetAssignment(retRef);
        u32 line = ctx.ast->GetLine(retRef);
        u32 col = ctx.ast->GetColumn(retRef);

        NodeRef value = src.value.IsValid() ? CloneNode(ctx, src.value) : NodeRef::Null();

        return ASTFactory::MakeReturn(ctx.ast, value, line, col);
    }

    // Clone an if statement (if statements reuse BlockData: [condition, thenBranch, elseBranch?])
    inline NodeRef CloneIfStatement(CloneContext& ctx, NodeRef ifRef) {
        const BlockData& src = ctx.ast->GetBlock(ifRef);
        u32 line = ctx.ast->GetLine(ifRef);
        u32 col = ctx.ast->GetColumn(ifRef);
        std::vector<NodeRef> statements;
        statements.reserve(src.statements.count);
        for (u32 i = 0; i < src.statements.count; i++) {
            statements.push_back(src.statements[i]);
        }

        NodeRef newIf = ASTFactory::MakeIfStatement(ctx.ast, line, col);

        // Clone all statements (condition, then branch, optional else branch)
        for (NodeRef stmt : statements) {
            NodeRef cloned = CloneNode(ctx, stmt);
            if (cloned.IsValid()) {
                ctx.ast->GetBlock(newIf).statements.Push(ctx.arena, cloned);
            }
        }

        return newIf;
    }

    // Clone a member access
    inline NodeRef CloneMemberAccess(CloneContext& ctx, NodeRef memberRef) {
        const MemberAccessData& src = ctx.ast->GetMemberAccess(memberRef);
        u32 line = ctx.ast->GetLine(memberRef);
        u32 col = ctx.ast->GetColumn(memberRef);

        NodeRef object = CloneNode(ctx, src.object);

        NodeRef newMember = ASTFactory::MakeMemberAccess(ctx.ast, object, src.member, line, col);
        MemberAccessData& dst = ctx.ast->GetMemberAccess(newMember);
        dst.qualifiedNameHash = src.qualifiedNameHash;
        dst.isModuleQualified = src.isModuleQualified;

        return newMember;
    }

    // Clone an array access
    inline NodeRef CloneArrayAccess(CloneContext& ctx, NodeRef arrayRef) {
        const ArrayAccessData& src = ctx.ast->GetArrayAccess(arrayRef);
        u32 line = ctx.ast->GetLine(arrayRef);
        u32 col = ctx.ast->GetColumn(arrayRef);

        NodeRef array = CloneNode(ctx, src.array);
        NodeRef index = CloneNode(ctx, src.index);

        return ASTFactory::MakeArrayAccess(ctx.ast, array, index, line, col);
    }

    // Clone a ternary expression
    inline NodeRef CloneTernary(CloneContext& ctx, NodeRef ternRef) {
        const TernaryExprData& src = ctx.ast->GetTernaryExpression(ternRef);
        u32 line = ctx.ast->GetLine(ternRef);
        u32 col = ctx.ast->GetColumn(ternRef);

        NodeRef condition = CloneNode(ctx, src.condition);
        NodeRef trueExpr = CloneNode(ctx, src.trueExpr);
        NodeRef falseExpr = CloneNode(ctx, src.falseExpr);

        return ASTFactory::MakeTernaryExpr(ctx.ast, condition, trueExpr, falseExpr, line, col);
    }

    // Main recursive clone function
    inline NodeRef CloneNode(CloneContext& ctx, NodeRef node) {
        if (node.IsNull()) return NodeRef::Null();

        switch (node.Type()) {
            case ASTNodeType::IDENTIFIER: {
                const IdentifierData& src = ctx.ast->GetIdentifier(node);
                u32 line = ctx.ast->GetLine(node);
                u32 col = ctx.ast->GetColumn(node);
                return ASTFactory::MakeIdentifier(ctx.ast, src.name, line, col);
            }

            case ASTNodeType::LITERAL: {
                const LiteralData& src = ctx.ast->GetLiteral(node);
                u32 line = ctx.ast->GetLine(node);
                u32 col = ctx.ast->GetColumn(node);
                // Clone based on literal type
                switch (src.value.type) {
                    case LiteralValue::FLOAT:
                        return ASTFactory::MakeLiteralFloat(ctx.ast, src.value.floatValue, line, col);
                    case LiteralValue::INT:
                        return ASTFactory::MakeLiteralInt(ctx.ast, src.value.intValue, line, col);
                    case LiteralValue::UINT:
                        return ASTFactory::MakeLiteralUint(ctx.ast, src.value.uintValue, line, col);
                    case LiteralValue::BOOL:
                        return ASTFactory::MakeLiteralBool(ctx.ast, src.value.boolValue, line, col);
                    default:
                        return NodeRef::Null();
                }
            }

            case ASTNodeType::BINARY_OP:
                return CloneBinaryOp(ctx, node);

            case ASTNodeType::UNARY_OP:
                return CloneUnaryOp(ctx, node);

            case ASTNodeType::TERNARY_EXPRESSION:
                return CloneTernary(ctx, node);

            case ASTNodeType::MEMBER_ACCESS:
                return CloneMemberAccess(ctx, node);

            case ASTNodeType::ARRAY_ACCESS:
                return CloneArrayAccess(ctx, node);

            case ASTNodeType::ASSIGNMENT:
                return CloneAssignment(ctx, node);

            case ASTNodeType::BLOCK:
                return CloneBlock(ctx, node);

            case ASTNodeType::VARIABLE_DECL:
                return CloneVariableDecl(ctx, node);

            case ASTNodeType::FUNCTION_CALL:
                return CloneFunctionCall(ctx, node);

            case ASTNodeType::RETURN:
                return CloneReturn(ctx, node);

            case ASTNodeType::IF_STATEMENT:
                return CloneIfStatement(ctx, node);

            case ASTNodeType::TYPE_PATTERN_MATCH: {
                // Compile-time type pattern matching for generics
                // Find which arm matches the concrete type and clone only that body
                const TypePatternMatchData& pm = ctx.ast->GetTypePatternMatch(node);

                NodeRef matchingArm = NodeRef::Null();
                NodeRef defaultArm = pm.defaultArm;

                // Find which arm matches based on substituted types
                for (u32 i = 0; i < pm.arms.count; i++) {
                    NodeRef armRef = pm.arms[i];
                    const TypePatternArmData& arm = ctx.ast->GetTypePatternArm(armRef);

                    if (arm.isDefault) {
                        defaultArm = armRef;
                        continue;
                    }

                    // Check if this arm's type matches any of the substituted concrete types
                    for (u32 s = 0; s < ctx.substitutionCount; s++) {
                        if (ctx.substitutions[s].coreType == arm.matchType) {
                            matchingArm = armRef;
                            break;
                        }
                    }

                    if (matchingArm.IsValid()) break;
                }

                // Use matching arm, or default if no match
                if (matchingArm.IsNull() && defaultArm.IsValid()) {
                    matchingArm = defaultArm;
                }

                if (matchingArm.IsValid()) {
                    const TypePatternArmData& arm = ctx.ast->GetTypePatternArm(matchingArm);
                    return CloneNode(ctx, arm.body);
                }

                // No matching arm - this shouldn't happen if exhaustiveness is validated
                return NodeRef::Null();
            }

            // For other node types, we can add as needed
            // For now, return null for unhandled types
            default:
                // TODO: Add more node types as needed
                return NodeRef::Null();
        }
    }

    // Clone a function declaration with type substitution
    // This is the main entry point for generic function instantiation
    inline NodeRef CloneFunction(
        AST* ast,
        BWSL_Arena* arena,
        NodeRef srcFuncRef,
        const TypeSubstitution* substitutions,
        u32 substitutionCount,
        const std::string& mangledName
    ) {
        const FunctionDeclData& src = ast->GetFunction(srcFuncRef);
        u32 line = ast->GetLine(srcFuncRef);
        u32 col = ast->GetColumn(srcFuncRef);

        // Create clone context
        CloneContext ctx;
        ctx.ast = ast;
        ctx.arena = arena;
        ctx.substitutions = substitutions;
        ctx.substitutionCount = substitutionCount;

        // Determine return type - substitute if needed
        CoreType returnType = src.returnType;
        for (u32 i = 0; i < substitutionCount; i++) {
            // If return type was a constraint, use the concrete type
            // This is a simplification - we might need to check the original return type name
            // For now, we leave the return type as-is since we set it during instantiation
        }

        // Create new function
        NodeRef newFunc = ASTFactory::MakeFunction(ast, mangledName, returnType, line, col);
        FunctionDeclData& dst = ast->GetFunction(newFunc);

        dst.isEval = src.isEval;

        // Clone parameters with type substitution
        for (u32 i = 0; i < src.parameters.count; i++) {
            ArenaString paramName = src.parameters[i].first;
            ArenaString paramType = ctx.SubstituteType(src.parameters[i].second);
            dst.parameters.Push(arena, std::make_pair(paramName, paramType));
        }

        // Clone body
        if (src.body.IsValid()) {
            dst.body = CloneNode(ctx, src.body);
        }

        return newFunc;
    }

} // namespace ASTClone

} // namespace BWSL
