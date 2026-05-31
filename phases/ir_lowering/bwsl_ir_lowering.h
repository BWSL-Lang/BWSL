#pragma once

#include "bwsl_ast_soa.h"
#include "bwsl_custom_type_registry.h"
#include "bwsl_defs.h"
#include "bwsl_ir_analysis.h" // For OutputSlot constants
#include "bwsl_ir_gen.h"
#include "bwsl_mem_pool.h"
#include "bwsl_resource_reflection.h"
#include "bwsl_symbol_table.h"
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace BWSL::IR {

// Maximum number of registers supported for a single shader pass
// Increased to 4096 to handle deep function inlining
static constexpr u32 MAX_REGISTERS = 4096;
using BWSL::IsArray;
using BWSL::MakeOverloadMask;
using BWSL::MakeOverloadMaskFromTypeHash;
using BWSL::OverloadMaskMatches;
using BWSL::OverloadTypeMask;

// =============================================================================
// PassVaryingContext - Tracks vertex outputs for fragment input resolution
// =============================================================================
// During pass compilation, this context collects all output.xxx assignments
// from the vertex shader and assigns them sequential slot indices. The fragment
// shader then uses this same context to resolve input.xxx to the correct slots.

struct VaryingInfo {
  u32 nameHash;  // Hash of the varying name (e.g., "worldPos")
  u32 slot;      // Assigned slot index (0, 1, 2, ...)
  CoreType type; // Type of the varying (FLOAT3, FLOAT4, etc.)
  InterpolationMode interpolation;
  char name[32]; // Actual name string for GLES output
};

struct PassVaryingContext {
  VaryingInfo varyings[16]; // Max 16 user varyings
  u32 count = 0;

  // Add a new varying or return existing slot
  u32 AddOrGetSlot(u32 nameHash, CoreType type, const char *nameStr = nullptr,
                   InterpolationMode interpolation = InterpolationMode::Default,
                   bool *conflict = nullptr) {
    if (conflict) {
      *conflict = false;
    }
    // Check if already exists
    for (u32 i = 0; i < count; i++) {
      if (varyings[i].nameHash == nameHash) {
        if (interpolation != InterpolationMode::Default) {
          if (varyings[i].interpolation == InterpolationMode::Default) {
            varyings[i].interpolation = interpolation;
          } else if (varyings[i].interpolation != interpolation && conflict) {
            *conflict = true;
          }
        }
        return varyings[i].slot;
      }
    }
    // Add new varying
    if (count >= 16) {
      fprintf(stderr, "Warning: Maximum 16 user varyings exceeded\n");
      return 0;
    }
    u32 slot = count;
    varyings[count].nameHash = nameHash;
    varyings[count].slot = slot;
    varyings[count].type = type;
    varyings[count].interpolation = interpolation;
    // Store actual name for GLES output
    if (nameStr) {
      strncpy(varyings[count].name, nameStr, 31);
      varyings[count].name[31] = '\0';
    } else {
      varyings[count].name[0] = '\0';
    }
    count++;
    return slot;
  }

  // Get slot for an existing varying (returns -1 if not found)
  s32 GetSlot(u32 nameHash) const {
    for (u32 i = 0; i < count; i++) {
      if (varyings[i].nameHash == nameHash) {
        return (s32)varyings[i].slot;
      }
    }
    return -1;
  }

  CoreType GetType(u32 slot) const {
    if (slot < count)
      return varyings[slot].type;
    return CoreType::FLOAT3;
  }

  InterpolationMode GetInterpolation(u32 slot) const {
    if (slot < count)
      return varyings[slot].interpolation;
    return InterpolationMode::Default;
  }
};

struct IRLowering {
  IRBuilder builder;
  IRProgram program;
  IRMemoryPool *pool;
  const SymbolTableData *symbols;
  AST *ast; // SoA AST - required for all data access

  // Track current context
  ShaderStage currentStage;
  u32 currentFunction;

  // LowerExpression recursion depth — pathological AST (deeply nested
  // binary/function-call trees from fuzzer inputs) blows the thread stack
  // without a cap. Guarded in LowerExpression with MAX_LOWER_DEPTH.
  u32 lowerDepth = 0;

  // Map AST node refs to registers (keyed by packed NodeRef value)
  std::unordered_map<u32, u16> nodeRegisters;

  // Map variable name hash to register
  std::unordered_map<u32, u16> variableRegisters;
  std::unordered_set<u32> initializedVariables;

  // Array base registers (var hash -> base reg)
  std::unordered_map<u32, u32> arrayBaseRegisters;

  // Map struct type name hash -> IR struct type index
  std::unordered_map<u32, u32> structTypeMap;

  // Map variable name hash -> struct type hash (for struct variables)
  std::unordered_map<u32, u32> variableStructTypes;

  // Inlining state
  u16 inlineReturnReg =
      0xFFFF; // Register to store return value during inlining
  u16 inlineReturnFlagReg = 0xFFFF; // Bool register for early-return tracking
  u32 inlineReturnCounter = 0;    // Counts returns seen during current inlining
  u32 inlineReturnGuardDepth = 0; // Suppress nested return guards
  u32 inlineDepth = 0; // Current inlining depth (for recursion detection)
  u32 inlineModuleIndex =
      0xFFFFFFFF; // Module index during inlining (for unqualified calls)
  static constexpr u32 MAX_INLINE_DEPTH = 8;

  // Stack of currently-inlining function AST nodes, for direct /
  // indirect recursion detection. Packed NodeRefs are 32-bit, so a
  // small static stack (sized to MAX_INLINE_DEPTH) is enough.
  u32 inlineStackPacked[MAX_INLINE_DEPTH] = {0};
  u32 inlineStackDepth = 0;
  // Set on the first recursion-detection event. bwslc checks this
  // before attempting SPIR-V validation and cross-compilation.
  bool recursionDiagnosed = false;
  // Tracks lowering-stage diagnostics that should fail the compile even
  // when external SPIR-V validation tooling is unavailable.
  bool hadError = false;

  // Loop nesting depth - used to ensure selection merges don't coincide with
  // continue targets
  u32 loopDepth = 0;

  // Loop break/continue target stack (for break/skip statements)
  // These are instruction indices that will be patched when the loop ends
  static constexpr u32 MAX_LOOP_NESTING = 16;
  u32 breakTargetStack[MAX_LOOP_NESTING]; // Stack of break target slots (to be
                                          // patched)
  u32 continueTargetStack[MAX_LOOP_NESTING]; // Stack of continue target
                                             // instruction indices
  u32 loopStackDepth = 0;

  // Lists of break/skip jumps that need patching when loop ends
  static constexpr u32 MAX_BREAKS_PER_LOOP = 32;
  u32 pendingBreaks[MAX_LOOP_NESTING][MAX_BREAKS_PER_LOOP];
  u32 pendingBreakCounts[MAX_LOOP_NESTING] = {0};
  u32 pendingSkips[MAX_LOOP_NESTING][MAX_BREAKS_PER_LOOP];
  u32 pendingSkipCounts[MAX_LOOP_NESTING] = {0};

  // Current pipeline for attribute lookup
  NodeRef currentPipeline;

  // Current pass for pass-scoped function lookup
  NodeRef currentPass;
  const PassData *currentPassData = nullptr;

  // Current pass varying context for vertex-to-fragment data flow
  PassVaryingContext *currentPassVaryings = nullptr;

  // Source base for string extraction (used for varying names in GLES output)
  const char *sourceBase = nullptr;

  void ReportError(const char *message);

  void Initialize(IRMemoryPool *memPool, const SymbolTableData *symTable,
                  AST *astData, const char *srcBase = nullptr);

  //==========================================================================
  // Top-level lowering entry points
  //==========================================================================

  IRProgram *LowerPipeline(NodeRef pipelineRef);

  void LowerPassConstants(const PassData &pass);

  void LowerPass(NodeRef passRef);

  void LowerShaderStage(NodeRef stageRef);

  void LowerBlock(NodeRef blockRef);

  //==========================================================================
  // Statement lowering
  //==========================================================================

  void LowerStatement(NodeRef ref);

  void LowerStatementWithReturnGuard(NodeRef ref);

  void LowerBreak();

  void LowerSkip();

  void LowerDiscard();

  void PushLoopContext();

  void PopLoopContext(u32 continueTarget, u32 breakTarget);

  void LowerForCStyle(NodeRef ref);

  void LowerForRange(NodeRef ref);

  void LowerForCollection(NodeRef ref);

  void LowerLoop(NodeRef ref);

  void LowerSwitch(NodeRef ref);

  //==========================================================================
  // Expression lowering
  //==========================================================================

  struct LowerDepthGuard {
    u32 &d;
    LowerDepthGuard(u32 &d_) : d(d_) { ++d; }
    ~LowerDepthGuard() { --d; }
  };

  u16 LowerExpression(NodeRef ref);

  // Resolves `^struct_var.field` to an OP_LOCAL_FIELD_PTR instruction.
  // Returns the destination register (non-zero) on success, or 0 if the
  // access pattern isn't a simple local-struct field (e.g. a swizzle,
  // a chained access, or an access through a non-local / non-struct
  // object). The caller falls back to the legacy ADDRESS_OF path on 0.
  u16 TryLowerLocalFieldAddressOf(NodeRef memberRef);

  u16 LowerUnaryOp(NodeRef ref);

  u16 LowerIdentifier(NodeRef ref);

  void LowerVariableDecl(NodeRef ref);

  u16 EmitZeroStruct(u32 structTypeHash);

  // Emit a zero constant for a given type
  u16 EmitZeroConstant(CoreType type);

  // Look up or register a struct type in the IR program
  u32 LookupOrRegisterStructType(u32 typeNameHash);

  u32 RegisterStructTypeFromSymbol(u32 typeNameHash,
                                   const StructData &structData);

  u32 RegisterStructTypeFromData(u32 typeNameHash, StructData *structData);

  u32 RegisterBuiltinStructType(const char *typeName,
                                const char *const *fieldNames,
                                const CoreType *fieldTypes,
                                u32 fieldCount);

  bool IsSumEnumTypeHash(u32 typeHash);

  StructData *LookupStructDataByHash(u32 typeHash);

  u32 ScalarComponentCount(CoreType type);

  void AppendEnumPayloadLayout(CoreType type, u32 typeHash,
                               CoreType *fieldTypes, u32 *fieldTypeHashes,
                               u32 &fieldCount, u32 maxFields);

  u32 GetEnumPayloadFieldCount(CoreType type, u32 typeHash);

  // Register an enum sum type as a struct in the IR.
  // Layout: { int tag; <flattened payload fields for the widest variant> }.
  // Nested sum-type enums are represented as aggregate struct fields so method
  // dispatch on pattern-bound payloads keeps the nested enum's type identity.
  u32 RegisterEnumAsStructType(u32 enumNameHash, u32 enumIndex);

  // Infer resource type from naming conventions
  // This is used when type information isn't explicitly available
  CoreType InferResourceType(u32 nameHash);

  // std140 type sizes
  u32 GetTypeSize(CoreType type);

  // std140 alignment rules
  u32 GetTypeAlignment(CoreType type);

  void LowerAssignment(NodeRef ref);

  void LowerIfStatement(NodeRef ref);

  void LowerReturn(NodeRef ref);

  u16 LowerLiteral(NodeRef ref);

  u16 LowerBinaryOp(NodeRef ref);

  u16 LowerArrayAccess(NodeRef ref);

  u16 LowerStoragePointerForAtomic(NodeRef ref);

  u16 LowerMemberAccess(NodeRef ref);

  u16 LowerFunctionCall(NodeRef ref);

  // Try to inline a function call, returns the result register or 0xFFFF if
  // inlining failed
  u16 TryInlineFunction(const FunctionCallData &call, u16 *args, u32 argCount);

  // Get type name hash for a CoreType using TypeHashes constants
  static u32 GetCoreTypeNameHash(CoreType type);

  // Try to resolve a generic function call by finding or instantiating a
  // specialization
  NodeRef TryResolveGenericFunction(const FunctionCallData &call, u16 *args,
                                    u32 argCount);

  u16 LowerTextureSample(NodeRef ref);

  //==========================================================================
  // Helper functions
  //==========================================================================

  u16 EnsureBoolCondition(u16 condReg);

  u16 CombineLoopCondition(u16 condReg);

  u16 AllocateRegister();

  u16 EmitConstantInt(u32 value);

  u16 EmitConstantUint(u32 value);

  u16 ConvertRegisterToType(u16 reg, CoreType targetType);

  u16 GetOrAllocateVariable(u32 nameHash);

  // ==========================================================================
  // Output slot helpers for varying management
  // ==========================================================================

  // Check if an output name is a builtin (stage-aware).
  // - Position is always a builtin output.
  // - Color/depth are builtin outputs only in fragment stage.
  bool IsBuiltinOutput(u32 nameHash);

  // Get the output slot for builtin outputs
  u32 GetBuiltinOutputSlot(u32 nameHash);

  CoreType GetBuiltinOutputType(u32 nameHash);

  const FragmentOutputDeclData *FindFragmentOutput(u32 nameHash) const;
  bool IsAllowedFragmentOutput(u32 nameHash) const;
  CoreType GetFragmentOutputType(u32 nameHash) const;

  u32 ResolveOutputSlotForStore(u32 nameHash, CoreType valueType,
                                const char *nameStr,
                                InterpolationMode interpolation);

  u32 ResolveOutputSlotForLoad(u32 nameHash);

  CoreType ResolveOutputTypeForLoad(u32 nameHash);

  u32 GetAttributeIndex(u32 nameHash);

  u32 GetInputSlotIndex(u32 nameHash);

  // Get the type of a fragment shader input (varying) from the pipeline
  // attributes
  CoreType GetInputTypeFromAttribute(u32 nameHash);

  // Check if an attribute is compressed and return appropriate type
  // Returns UINT for compressed attributes (raw packed data), otherwise INVALID
  CompressionFormat GetAttributeCompression(u32 attrIndex);

  CoreType GetRegisterType(u16 reg);

  void SetRegisterType(u16 reg, CoreType type);

  void AddUndefRegister(u16 reg, CoreType type);

  // Get the scalar component type of a vector type
  // FLOAT2/3/4 -> FLOAT, INT2/3/4 -> INT, UINT2/3/4 -> UINT
  // For non-vector types, returns the type itself
  CoreType GetScalarComponentType(CoreType type);

  // Get a vector type from scalar type and component count
  // e.g., (FLOAT, 3) -> FLOAT3, (UINT, 2) -> UINT2
  CoreType GetVectorType(CoreType scalarType, int componentCount);

  // Infer the type of an expression from the AST without lowering it
  // Used for determining loop iterator types before the loop header
  CoreType InferExpressionType(NodeRef ref);

  // Check if an expression would yield an unsigned integer type
  bool IsExpressionUnsigned(NodeRef ref);

  // Get the number of components for a vector type (1 for scalars, 2-4 for
  // vectors)
  u32 GetVectorDimension(CoreType type);

  // Get the vector type with a specific dimension for a base scalar/vector type
  CoreType GetVectorTypeWithDimension(CoreType baseType, u32 dim);

  CoreType LookupCoreType(u32 typeHash);

  CoreType ResolveCoreTypeFromHash(u32 typeHash, u32 *outCustomHash = nullptr);

  OverloadTypeMask MakeOverloadMaskFromResolvedTypeHash(u32 typeHash);

  OpCode IntrinsicToOpcode(StdLib::Intrinsic intrinsic);
  // IRLowering is header-only: the .inl shards below contain member
  // definitions only. Keep lowering state, arena ownership, and parallel-array
  // storage in this struct rather than introducing separate owner objects.
};

#include "bwsl_ir_lowering_core.inl"
#include "bwsl_ir_lowering_control.inl"
#include "bwsl_ir_lowering_types.inl"
#include "bwsl_ir_lowering_expr.inl"
#include "bwsl_ir_lowering_lvalues.inl"
#include "bwsl_ir_lowering_calls.inl"


} // namespace BWSL::IR
