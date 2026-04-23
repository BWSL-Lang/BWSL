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
  char name[32]; // Actual name string for GLES output
};

struct PassVaryingContext {
  VaryingInfo varyings[16]; // Max 16 user varyings
  u32 count = 0;

  // Add a new varying or return existing slot
  u32 AddOrGetSlot(u32 nameHash, CoreType type, const char *nameStr = nullptr) {
    // Check if already exists
    for (u32 i = 0; i < count; i++) {
      if (varyings[i].nameHash == nameHash) {
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

  // Current pass varying context for vertex-to-fragment data flow
  PassVaryingContext *currentPassVaryings = nullptr;

  // Source base for string extraction (used for varying names in GLES output)
  const char *sourceBase = nullptr;

  void Initialize(IRMemoryPool *memPool, const SymbolTableData *symTable,
                  AST *astData, const char *srcBase = nullptr) {
    pool = memPool;
    symbols = symTable;
    ast = astData;
    sourceBase = srcBase;
    initializedVariables.clear();
    builder.pool = pool;
    builder.program = &program;
    builder.currentInstruction = 0;
    builder.nextRegister = 0;

    // Allocate IR arrays. instructionCount must be zeroed explicitly — it's
    // a plain u32 field with no inline initializer, and the enclosing
    // IRLowering is stack-allocated in the fuzz/test harnesses, so without
    // this we read stack garbage if no instructions end up being emitted.
    u32 initialSize = 1024;
    program.instructionCount = 0;
    program.instructionCapacity = initialSize;
    program.opcodes = (u16 *)pool->Allocate(initialSize * sizeof(u16), 64);
    program.types = (u16 *)pool->Allocate(initialSize * sizeof(u16), 64);
    program.flags = (u16 *)pool->Allocate(initialSize * sizeof(u16), 64);
    program.destinations = (u16 *)pool->Allocate(initialSize * sizeof(u16), 64);
    program.operands = (u16 *)pool->Allocate(initialSize * 4 * sizeof(u16), 64);
    // Initialize unused operand slots to 0x3FFF (max register index without
    // constant flags) This value will be > registerCount so SSA will skip it,
    // and SPIR-V backend ignores it
    for (u32 i = 0; i < initialSize * 4; i++) {
      program.operands[i] = 0x3FFF;
    }
    program.metadata = (u32 *)pool->Allocate(initialSize * sizeof(u32), 64);
    memset(program.metadata, 0, initialSize * sizeof(u32));
    program.branchTrueTargets =
        (u32 *)pool->Allocate(initialSize * sizeof(u32), 64);
    program.branchFalseTargets =
        (u32 *)pool->Allocate(initialSize * sizeof(u32), 64);
    memset(program.branchTrueTargets, 0xFF, initialSize * sizeof(u32));
    memset(program.branchFalseTargets, 0xFF, initialSize * sizeof(u32));

    // Allocate structure info arrays for control flow
    program.structureInfo =
        (u32 *)pool->Allocate(initialSize * sizeof(u32), 64);
    program.continueInfo = (u32 *)pool->Allocate(initialSize * sizeof(u32), 64);
    memset(program.structureInfo, 0, initialSize * sizeof(u32));
    memset(program.continueInfo, 0xFF,
           initialSize * sizeof(u32)); // 0xFFFFFFFF = no continue target

    // Allocate switch case arrays
    u32 switchCapacity = 16; // Initial number of switches
    u32 caseCapacity = 64;   // Initial total cases across all switches
    program.switchInstructionIndices =
        (u32 *)pool->Allocate(switchCapacity * sizeof(u32), 64);
    program.switchCaseOffsets =
        (u32 *)pool->Allocate((switchCapacity + 1) * sizeof(u32), 64);
    program.switchCaseValues =
        (s32 *)pool->Allocate(caseCapacity * sizeof(s32), 64);
    program.switchCaseTargets =
        (u32 *)pool->Allocate(caseCapacity * sizeof(u32), 64);
    program.switchDefaultTargets =
        (u32 *)pool->Allocate(switchCapacity * sizeof(u32), 64);
    program.switchCount = 0;
    program.switchCaseCapacity = caseCapacity;
    program.switchCaseOffsets[0] = 0; // First switch starts at case 0

    // Initialize constant pools
    program.floatConstants = (float *)pool->Allocate(256 * sizeof(float), 64);
    program.intConstants = (u32 *)pool->Allocate(256 * sizeof(u32), 64);
    program.uintConstants = (u32 *)pool->Allocate(256 * sizeof(u32), 64);
    program.boolConstants =
        (u8 *)pool->Allocate(8 * sizeof(u8), 64); // Only need 2 (true/false)
    program.floatCount = 0;
    program.intCount = 0;
    program.uintCount = 0;
    program.boolCount = 0;

    // Allocate builder constant hash arrays for deduplication
    builder.floatConstantHashes = (u32 *)pool->Allocate(256 * sizeof(u32), 64);
    builder.intConstantHashes = (u32 *)pool->Allocate(256 * sizeof(u32), 64);
    builder.uintConstantHashes = (u32 *)pool->Allocate(256 * sizeof(u32), 64);
    builder.floatConstantCount = 0;
    builder.intConstantCount = 0;
    builder.uintConstantCount = 0;

    // Register info
    program.registerTypes =
        (u16 *)pool->Allocate(MAX_REGISTERS * sizeof(u16), 64);
    program.registerCount = 0;

    // Struct type metadata
    u32 structCapacity = 64;
    u32 fieldCapacity = 256;
    program.structTypes = (IRProgram::StructTypeInfo *)pool->Allocate(
        structCapacity * sizeof(IRProgram::StructTypeInfo), 64);
    program.structFieldTypes =
        (u16 *)pool->Allocate(fieldCapacity * sizeof(u16), 64);
    program.structFieldNameHashes =
        (u32 *)pool->Allocate(fieldCapacity * sizeof(u32), 64);
    program.structFieldTypeHashes =
        (u32 *)pool->Allocate(fieldCapacity * sizeof(u32), 64);
    program.structFieldByteOffsets =
        (u32 *)pool->Allocate(fieldCapacity * sizeof(u32), 64);
    program.structFieldArraySizes =
        (u32 *)pool->Allocate(fieldCapacity * sizeof(u32), 64);
    memset(program.structFieldArraySizes, 0, fieldCapacity * sizeof(u32));
    memset(program.structFieldTypeHashes, 0, fieldCapacity * sizeof(u32));
    program.registerStructTypes =
        (u32 *)pool->Allocate(MAX_REGISTERS * sizeof(u32), 64);
    memset(program.registerStructTypes, 0, MAX_REGISTERS * sizeof(u32));
    program.registerStorageInfo =
        (u32 *)pool->Allocate(MAX_REGISTERS * sizeof(u32), 64);
    memset(program.registerStorageInfo, 0, MAX_REGISTERS * sizeof(u32));
    program.structTypeCount = 0;
    program.structTypeCapacity = structCapacity;
    program.structFieldCapacity = fieldCapacity;

    u32 sharedCapacity = 16;
    program.sharedNameHashes =
        (u32 *)pool->Allocate(sharedCapacity * sizeof(u32), 64);
    program.sharedTypes =
        (u16 *)pool->Allocate(sharedCapacity * sizeof(u16), 64);
    program.sharedArraySizes =
        (u32 *)pool->Allocate(sharedCapacity * sizeof(u32), 64);
    program.sharedRegisters =
        (u16 *)pool->Allocate(sharedCapacity * sizeof(u16), 64);
    program.sharedVarCount = 0;
    program.sharedVarCapacity = sharedCapacity;

    // Local array tracking
    u32 localArrayCapacity = 16;
    program.localArrayNameHashes =
        (u32 *)pool->Allocate(localArrayCapacity * sizeof(u32), 64);
    program.localArrayTypes =
        (u16 *)pool->Allocate(localArrayCapacity * sizeof(u16), 64);
    program.localArrayStructTypes =
        (u32 *)pool->Allocate(localArrayCapacity * sizeof(u32), 64);
    program.localArraySizes =
        (u32 *)pool->Allocate(localArrayCapacity * sizeof(u32), 64);
    program.localArrayRegisters =
        (u16 *)pool->Allocate(localArrayCapacity * sizeof(u16), 64);
    memset(program.localArrayStructTypes, 0, localArrayCapacity * sizeof(u32));
    program.localArrayCount = 0;
    program.localArrayCapacity = localArrayCapacity;

    // Buffer element types (initialized to 0 = unknown)
    memset(program.bufferElementStructTypes, 0,
           sizeof(program.bufferElementStructTypes));
    memset(program.bufferElementCoreTypes, 0,
           sizeof(program.bufferElementCoreTypes));

    // PHI fields (will be populated by SSA if needed)
    program.phiBlockIndices = nullptr;
    program.phiResultRegs = nullptr;
    program.phiTypes = nullptr;
    program.phiOperandOffsets = nullptr;
    program.phiOperandValues = nullptr;
    program.phiOperandBlocks = nullptr;
    program.phiCount = 0;

    // Undef register tracking (populated by SSA for variables without entry
    // block defs)
    u32 undefCapacity = 64; // Initial capacity
    program.undefRegs = (u16 *)pool->Allocate(undefCapacity * sizeof(u16), 64);
    program.undefRegTypes =
        (u16 *)pool->Allocate(undefCapacity * sizeof(u16), 64);
    program.undefRegCount = 0;
    program.undefRegCapacity = undefCapacity;

    // Clear maps
    nodeRegisters.clear();
    variableRegisters.clear();
    arrayBaseRegisters.clear();
    structTypeMap.clear();
    variableStructTypes.clear();
  }

  //==========================================================================
  // Top-level lowering entry points
  //==========================================================================

  IRProgram *LowerPipeline(NodeRef pipelineRef) {
    currentPipeline = pipelineRef;
    const PipelineData &pipeline = ast->GetPipeline(pipelineRef);
    for (u32 i = 0; i < pipeline.passes.count; i++) {
      LowerPass(pipeline.passes[i]);
    }
    return &program;
  }

  void LowerPassConstants(const PassData &pass) {
    for (u32 i = 0; i < pass.consts.count; i++) {
      LowerVariableDecl(pass.consts[i]);
    }
  }

  void LowerPass(NodeRef passRef) {
    const PassData &pass = ast->GetPass(passRef);
    currentPass = passRef; // Track current pass for pass-scoped function lookup

    // Create varying context for this pass - tracks vertex outputs
    // so fragment shader can resolve input.xxx to matching slots
    PassVaryingContext varyingContext;
    currentPassVaryings = &varyingContext;

    // Phase 1: Lower vertex shader (collects output.xxx usages)
    if (!pass.vertexShader.IsNull()) {
      currentStage = ShaderStage::Vertex;
      LowerPassConstants(pass);
      LowerShaderStage(pass.vertexShader);
    }

    // Phase 2: Lower fragment shader (uses collected varyings for input
    // resolution)
    if (!pass.fragmentShader.IsNull()) {
      currentStage = ShaderStage::Fragment;
      LowerPassConstants(pass);
      LowerShaderStage(pass.fragmentShader);
    }

    if (!pass.computeShader.IsNull()) {
      currentStage = ShaderStage::Compute;
      LowerPassConstants(pass);
      LowerShaderStage(pass.computeShader);
    }

    currentPassVaryings = nullptr;
    currentPass = NodeRef::Null();
  }

  void LowerShaderStage(NodeRef stageRef) {
    const ShaderStageData &stage = ast->GetShaderStage(stageRef);
    if (!stage.body.IsNull()) {
      LowerBlock(stage.body);
    }
    // Emit return at end of shader
    builder.EmitInstruction(OP_RET, 0, 0);
  }

  void LowerBlock(NodeRef blockRef) {
    const BlockData &block = ast->GetBlock(blockRef);
    bool guardInlineReturns =
        (inlineDepth > 0 && inlineReturnFlagReg != 0xFFFF);
    bool returnSeen = false;

    // Save variable-register map so that inner-scope redeclarations
    // (`{ float x = …; }` inside an outer `int x`) don't overwrite the
    // outer binding. Without this, references to `x` after the inner
    // block resolve to the inner (now-dead) slot and get the wrong
    // type. The parser's symbol table does scope-stack pops; the
    // lowering flat-map didn't.
    auto savedVariableRegisters = variableRegisters;
    auto savedVariableStructTypes = variableStructTypes;

    for (u32 i = 0; i < block.statements.count; i++) {
      u32 returnCountBefore = inlineReturnCounter;
      if (guardInlineReturns && returnSeen) {
        LowerStatementWithReturnGuard(block.statements[i]);
      } else {
        LowerStatement(block.statements[i]);
      }
      if (guardInlineReturns && inlineReturnCounter != returnCountBefore) {
        returnSeen = true;
      }
    }

    variableRegisters = savedVariableRegisters;
    variableStructTypes = savedVariableStructTypes;
  }

  //==========================================================================
  // Statement lowering
  //==========================================================================

  void LowerStatement(NodeRef ref) {
    switch (ref.Type()) {
    case ASTNodeType::ASSIGNMENT:
      LowerAssignment(ref);
      break;
    case ASTNodeType::IF_STATEMENT:
      LowerIfStatement(ref);
      break;
    case ASTNodeType::FOR_CSTYLE:
      LowerForCStyle(ref);
      break;
    case ASTNodeType::FOR_RANGE:
      LowerForRange(ref);
      break;
    case ASTNodeType::FOR_COLLECTION:
      LowerForCollection(ref);
      break;
    case ASTNodeType::LOOP:
      LowerLoop(ref);
      break;
    case ASTNodeType::SWITCH:
      LowerSwitch(ref);
      break;
    case ASTNodeType::RETURN:
      LowerReturn(ref);
      break;
    case ASTNodeType::VARIABLE_DECL:
      LowerVariableDecl(ref);
      break;
    case ASTNodeType::BLOCK:
      LowerBlock(ref);
      break;
    case ASTNodeType::FUNCTION_CALL:
      // Function call as statement (discarding return value)
      LowerExpression(ref);
      break;
    case ASTNodeType::BREAK_STATEMENT:
      LowerBreak();
      break;
    case ASTNodeType::SKIP_STATEMENT:
      LowerSkip();
      break;
    case ASTNodeType::DISCARD_STATEMENT:
      LowerDiscard();
      break;
    default:
      // Other expression statements
      LowerExpression(ref);
      break;
    }
  }

  void LowerStatementWithReturnGuard(NodeRef ref) {
    if (inlineReturnFlagReg == 0xFFFF) {
      LowerStatement(ref);
      return;
    }

    u32 branchIdx = builder.currentInstruction;
    builder.EmitInstruction(OP_BRANCH, 0, inlineReturnFlagReg, 0, 0);

    u32 falseTarget = builder.currentInstruction;
    LowerStatement(ref);

    u32 mergePoint = builder.currentInstruction;
    builder.EmitInstruction(OP_NOP, 0, 0);

    program.SetBranchTargets(branchIdx, mergePoint, falseTarget);
    program.structureInfo[branchIdx] =
        IRProgram::PackStructure(IRProgram::STRUCT_IF_HEADER, mergePoint);
  }

  void LowerBreak() {
    // Emit jump to break target (will be patched when loop ends)
    if (loopStackDepth == 0) {
      // Error: break outside of loop - shouldn't happen if parser validates
      return;
    }
    u32 jumpIdx = builder.currentInstruction;
    builder.EmitInstruction(OP_JUMP, 0, 0);
    // Record this break for patching when loop ends
    u32 depth = loopStackDepth - 1;
    if (pendingBreakCounts[depth] < MAX_BREAKS_PER_LOOP) {
      pendingBreaks[depth][pendingBreakCounts[depth]++] = jumpIdx;
    }
  }

  void LowerSkip() {
    // Emit jump to continue target (will be patched when continue target is
    // known)
    if (loopStackDepth == 0) {
      // Error: skip outside of loop - shouldn't happen if parser validates
      return;
    }
    u32 jumpIdx = builder.currentInstruction;
    builder.EmitInstruction(OP_JUMP, 0, 0);
    // Record this skip for patching when continue target is known
    u32 depth = loopStackDepth - 1;
    if (pendingSkipCounts[depth] < MAX_BREAKS_PER_LOOP) {
      pendingSkips[depth][pendingSkipCounts[depth]++] = jumpIdx;
    }
  }

  void LowerDiscard() {
    // Emit discard instruction - terminates fragment shader execution
    // This is a terminator instruction (like return) that ends the current
    // block
    builder.EmitInstruction(OP_DISCARD, 0, 0);
  }

  void PushLoopContext() {
    if (loopStackDepth < MAX_LOOP_NESTING) {
      pendingBreakCounts[loopStackDepth] = 0;
      pendingSkipCounts[loopStackDepth] = 0;
      loopStackDepth++;
    }
  }

  void PopLoopContext(u32 continueTarget, u32 breakTarget) {
    if (loopStackDepth == 0)
      return;
    loopStackDepth--;

    // Patch all pending skip jumps to continue target
    for (u32 i = 0; i < pendingSkipCounts[loopStackDepth]; i++) {
      program.metadata[pendingSkips[loopStackDepth][i]] = continueTarget;
    }

    // Patch all pending break jumps to break target (loop end)
    for (u32 i = 0; i < pendingBreakCounts[loopStackDepth]; i++) {
      program.metadata[pendingBreaks[loopStackDepth][i]] = breakTarget;
    }
  }

  void LowerForCStyle(NodeRef ref) {
    const ForCStyleData &forLoop = ast->GetForCStyle(ref);

    // Snapshot variable→register map *before* the init clause runs so
    // the iterator binding itself (e.g. `int x = …`) doesn't leak out
    // of the loop if it shadowed an outer variable. The body gets its
    // own snapshot/restore below so body-local decls don't escape into
    // the increment clause either.
    auto savedOuterVariableRegisters = variableRegisters;

    // Initialization
    if (!forLoop.init.IsNull()) {
      LowerStatement(forLoop.init);
    }

    u32 loopHeader = builder.currentInstruction;

    // Condition check
    bool needsReturnGuard = (inlineDepth > 0 && inlineReturnFlagReg != 0xFFFF);
    bool hasBranch = (!forLoop.condition.IsNull() || needsReturnGuard);
    u32 branchIdx = 0;
    u32 bodyStart = 0;
    if (hasBranch) {
      u16 condReg = !forLoop.condition.IsNull()
                        ? LowerExpression(forLoop.condition)
                        : builder.EmitConstantBool(true);
      if (needsReturnGuard) {
        condReg = CombineLoopCondition(condReg);
      }
      branchIdx = builder.currentInstruction;
      builder.EmitInstruction(OP_BRANCH, 0, condReg);
      bodyStart = builder.currentInstruction; // True target = body
    } else {
      bodyStart = builder.currentInstruction;
    }

    // Body - track loop depth for nested if-statement merge handling
    loopDepth++;
    PushLoopContext();
    // Snapshot variable→register mapping so body-scoped declarations
    // (including shadowed iterators in nested for-loops) don't leak into
    // the increment clause.
    auto savedVariableRegisters = variableRegisters;
    if (!forLoop.body.IsNull()) {
      LowerStatement(forLoop.body);
    }
    variableRegisters = std::move(savedVariableRegisters);
    loopDepth--;

    // Continue target = start of increment
    u32 continueTarget = builder.currentInstruction;

    // Increment
    if (!forLoop.increment.IsNull()) {
      LowerStatement(forLoop.increment);
    }

    // Jump back to loop header
    u32 backEdgeIdx = builder.currentInstruction;
    builder.EmitInstruction(OP_JUMP, 0, 0);
    program.metadata[backEdgeIdx] = loopHeader;

    // Always emit a dedicated merge block to avoid conflicts with subsequent
    // headers.
    u32 loopEnd = builder.currentInstruction; // NOP is the merge point
    builder.EmitInstruction(OP_NOP, 0, 0);

    // Patch break/skip jumps
    PopLoopContext(continueTarget, loopEnd);

    // Patch branch: true = continue into body, false = exit loop
    if (hasBranch) {
      program.SetBranchTargets(branchIdx, bodyStart, loopEnd);

      // Annotate loop structure
      program.structureInfo[branchIdx] =
          IRProgram::PackStructure(IRProgram::STRUCT_LOOP_HEADER, loopEnd);
      program.continueInfo[branchIdx] = continueTarget;
    }

    // Restore outer scope — drops the iterator binding (and anything the
    // increment clause introduced) so subsequent references resolve to
    // shadowed outer variables with their original types.
    variableRegisters = std::move(savedOuterVariableRegisters);
  }

  void LowerForRange(NodeRef ref) {
    const ForRangeData &forLoop = ast->GetForRange(ref);

    // Snapshot outer scope so the iterator (and any body-scope locals)
    // don't leak out, shadowing / mis-typing later references.
    auto savedOuterVariableRegisters = variableRegisters;

    // Infer iterator type from range bounds WITHOUT lowering yet
    // This preserves the correct control flow structure for SPIR-V
    bool isUnsigned = IsExpressionUnsigned(forLoop.rangeEnd);
    CoreType iterType = isUnsigned ? CoreType::UINT : CoreType::INT;

    // Allocate iterator register and initialize to rangeStart
    u16 iterReg = AllocateRegister();
    SetRegisterType(iterReg, iterType);
    if (!forLoop.rangeStart.IsNull()) {
      // Check if rangeStart is a literal - if so, emit the right constant type
      if (forLoop.rangeStart.Type() == ASTNodeType::LITERAL) {
        const LiteralData &lit = ast->GetLiteral(forLoop.rangeStart);
        if (lit.value.type == LiteralValue::INT && isUnsigned) {
          // Convert int literal to uint constant
          u16 startReg = EmitConstantUint(static_cast<u32>(lit.value.intValue));
          builder.EmitInstruction(OP_STORE_REG, iterReg, startReg);
        } else if (lit.value.type == LiteralValue::UINT && !isUnsigned) {
          // Convert uint literal to int constant
          u16 startReg = EmitConstantInt(static_cast<int>(lit.value.uintValue));
          builder.EmitInstruction(OP_STORE_REG, iterReg, startReg);
        } else {
          u16 startReg = LowerExpression(forLoop.rangeStart);
          builder.EmitInstruction(OP_STORE_REG, iterReg, startReg);
        }
      } else {
        u16 startReg = LowerExpression(forLoop.rangeStart);
        builder.EmitInstruction(OP_STORE_REG, iterReg, startReg);
      }
    } else {
      // Default start is 0
      u16 zeroReg = isUnsigned ? EmitConstantUint(0) : EmitConstantInt(0);
      builder.EmitInstruction(OP_STORE_REG, iterReg, zeroReg);
    }

    // Store iterator variable mapping
    if (!forLoop.iteratorVar.IsNull()) {
      // iteratorVar is stored as an identifier in the AST
      const IdentifierData &iterVar = ast->GetIdentifier(forLoop.iteratorVar);
      variableRegisters[iterVar.name.nameHash] = iterReg;
    }

    u32 loopHeader = builder.currentInstruction;

    // Now lower rangeEnd inside the loop header where it belongs
    u16 endReg = LowerExpression(forLoop.rangeEnd);

    // Condition: iter < rangeEnd (or <= if inclusive)
    // Use unsigned comparison for uint, signed for int
    u16 condReg = AllocateRegister();
    SetRegisterType(condReg, CoreType::BOOL); // Comparison result is BOOL
    OpCode cmpOp;
    if (isUnsigned) {
      cmpOp = forLoop.inclusive ? OP_ULE : OP_ULT;
    } else {
      cmpOp = forLoop.inclusive ? OP_ILE : OP_ILT;
    }
    builder.EmitInstruction(cmpOp, condReg, iterReg, endReg);
    if (inlineDepth > 0 && inlineReturnFlagReg != 0xFFFF) {
      condReg = CombineLoopCondition(condReg);
    }

    u32 branchIdx = builder.currentInstruction;
    builder.EmitInstruction(OP_BRANCH, 0, condReg);

    u32 bodyStart = builder.currentInstruction;

    // Body - track loop depth for nested if-statement merge handling
    loopDepth++;
    PushLoopContext();
    if (!forLoop.body.IsNull()) {
      LowerStatement(forLoop.body);
    }
    loopDepth--;

    // Continue target = start of increment
    u32 continueTarget = builder.currentInstruction;

    // Increment by step (default 1) - use appropriate type for constant
    // Note: IADD works for both signed and unsigned (bit-identical operation)
    u16 stepReg;
    if (forLoop.step.IsNull()) {
      stepReg = isUnsigned ? EmitConstantUint(1) : EmitConstantInt(1);
    } else {
      stepReg = LowerExpression(forLoop.step);
    }
    builder.EmitInstruction(OP_IADD, iterReg, iterReg, stepReg);

    // Jump back to loop header
    u32 backEdgeIdx = builder.currentInstruction;
    builder.EmitInstruction(OP_JUMP, 0, 0);
    program.metadata[backEdgeIdx] = loopHeader;

    // If inside an outer loop, emit NOP to create dedicated merge block
    // This prevents inner merge from conflicting with outer continue target
    // The merge point must be the NOP instruction itself
    u32 loopEnd = builder.currentInstruction; // NOP is the merge point
    builder.EmitInstruction(OP_NOP, 0, 0);

    // Patch break/skip jumps
    PopLoopContext(continueTarget, loopEnd);

    // Patch branch: true = continue into body, false = exit loop
    program.SetBranchTargets(branchIdx, bodyStart, loopEnd);

    // Annotate loop structure
    program.structureInfo[branchIdx] =
        IRProgram::PackStructure(IRProgram::STRUCT_LOOP_HEADER, loopEnd);
    program.continueInfo[branchIdx] = continueTarget;

    variableRegisters = std::move(savedOuterVariableRegisters);
  }

  void LowerForCollection(NodeRef ref) {
    const ForCollectionData &forLoop = ast->GetForCollection(ref);

    auto savedOuterVariableRegisters = variableRegisters;

    // Get base register for the collection
    u16 collectionReg = LowerExpression(forLoop.collection);

    // Allocate index register, initialize to 0
    u16 indexReg = AllocateRegister();
    SetRegisterType(indexReg, CoreType::INT); // Loop index is always INT
    builder.EmitInstruction(OP_STORE_REG, indexReg, EmitConstantInt(0));

    // Allocate iterator variable register (holds current element)
    u16 iterReg = AllocateRegister();
    // Note: iterReg type depends on collection element type, left unset for now
    if (!forLoop.iteratorVar.IsNull()) {
      // iteratorVar is stored as an identifier in the AST
      const IdentifierData &iterVar = ast->GetIdentifier(forLoop.iteratorVar);
      variableRegisters[iterVar.name.nameHash] = iterReg;
    }

    // Length comes from the struct (resolved at parse time)
    u16 lengthReg = EmitConstantInt(forLoop.length);

    u32 loopHeader = builder.currentInstruction;

    // Condition: index < length
    u16 condReg = AllocateRegister();
    SetRegisterType(condReg, CoreType::BOOL); // Comparison result is BOOL
    builder.EmitInstruction(OP_ILT, condReg, indexReg, lengthReg);
    if (inlineDepth > 0 && inlineReturnFlagReg != 0xFFFF) {
      condReg = CombineLoopCondition(condReg);
    }

    u32 branchIdx = builder.currentInstruction;
    builder.EmitInstruction(OP_BRANCH, 0, condReg);

    u32 bodyStart = builder.currentInstruction;

    // Load current element: iter = collection[index]
    builder.EmitInstruction(OP_ARRAY_LOAD, iterReg, collectionReg, indexReg);

    // Body - track loop depth for nested if-statement merge handling
    loopDepth++;
    PushLoopContext();
    if (!forLoop.body.IsNull()) {
      LowerStatement(forLoop.body);
    }
    loopDepth--;

    // Continue target = start of increment
    u32 continueTarget = builder.currentInstruction;

    // Increment index
    builder.EmitInstruction(OP_IADD, indexReg, indexReg, EmitConstantInt(1));

    // Jump back to loop header
    u32 backEdgeIdx = builder.currentInstruction;
    builder.EmitInstruction(OP_JUMP, 0, 0);
    program.metadata[backEdgeIdx] = loopHeader;

    // If inside an outer loop, emit NOP to create dedicated merge block
    // This prevents inner merge from conflicting with outer continue target
    // The merge point must be the NOP instruction itself
    u32 loopEnd = builder.currentInstruction; // NOP is the merge point
    builder.EmitInstruction(OP_NOP, 0, 0);

    // Patch break/skip jumps
    PopLoopContext(continueTarget, loopEnd);

    // Patch branch: true = continue into body, false = exit loop
    program.SetBranchTargets(branchIdx, bodyStart, loopEnd);

    // Annotate loop structure
    program.structureInfo[branchIdx] =
        IRProgram::PackStructure(IRProgram::STRUCT_LOOP_HEADER, loopEnd);
    program.continueInfo[branchIdx] = continueTarget;

    variableRegisters = std::move(savedOuterVariableRegisters);
  }

  void LowerLoop(NodeRef ref) {
    const LoopData &loop = ast->GetLoop(ref);

    // If count is specified, this is a counted loop
    if (!loop.count.IsNull()) {
      u16 countReg = LowerExpression(loop.count);
      u16 iterReg = AllocateRegister();
      SetRegisterType(iterReg, CoreType::INT); // Loop iterator is always INT
      builder.EmitInstruction(OP_STORE_REG, iterReg, EmitConstantInt(0));

      u32 loopHeader = builder.currentInstruction;

      // Check iter < count
      u16 condReg = AllocateRegister();
      SetRegisterType(condReg, CoreType::BOOL); // Comparison result is BOOL
      builder.EmitInstruction(OP_ILT, condReg, iterReg, countReg);
      if (inlineDepth > 0 && inlineReturnFlagReg != 0xFFFF) {
        condReg = CombineLoopCondition(condReg);
      }

      u32 branchIdx = builder.currentInstruction;
      builder.EmitInstruction(OP_BRANCH, 0, condReg);

      u32 bodyStart = builder.currentInstruction;

      // Body - track loop depth for nested if-statement merge handling
      loopDepth++;
      PushLoopContext();
      if (!loop.body.IsNull()) {
        LowerStatement(loop.body);
      }

      // Check until condition if present (early exit)
      u32 untilBranchIdx = 0;
      if (!loop.untilCondition.IsNull()) {
        u16 untilReg = LowerExpression(loop.untilCondition);
        untilBranchIdx = builder.currentInstruction;
        builder.EmitInstruction(OP_BRANCH, 0, untilReg);
      }
      loopDepth--;

      // Continue target = start of increment
      u32 continueTarget = builder.currentInstruction;

      // Increment
      builder.EmitInstruction(OP_IADD, iterReg, iterReg, EmitConstantInt(1));

      // Jump back to loop header
      u32 backEdgeIdx = builder.currentInstruction;
      builder.EmitInstruction(OP_JUMP, 0, 0);
      program.metadata[backEdgeIdx] = loopHeader;

      // If inside an outer loop, emit NOP to create dedicated merge block
      // This prevents inner merge from conflicting with outer continue target
      // The merge point must be the NOP instruction itself
      u32 loopEnd = builder.currentInstruction; // NOP is the merge point
      builder.EmitInstruction(OP_NOP, 0, 0);

      // Patch break/skip jumps
      PopLoopContext(continueTarget, loopEnd);

      // Patch main loop branch: true = continue into body, false = exit loop
      program.SetBranchTargets(branchIdx, bodyStart, loopEnd);

      // Patch until branch if present: true = exit, false = continue
      if (!loop.untilCondition.IsNull()) {
        program.SetBranchTargets(untilBranchIdx, loopEnd, continueTarget);
      }

      // Annotate loop structure
      program.structureInfo[branchIdx] =
          IRProgram::PackStructure(IRProgram::STRUCT_LOOP_HEADER, loopEnd);
      program.continueInfo[branchIdx] = continueTarget;
    } else {
      // Infinite loop with until condition
      u32 loopHeader = builder.currentInstruction;

      // For infinite loops, we need a dummy branch at the header for SPIR-V
      // structure Use a constant true condition
      u16 trueReg = builder.EmitConstantBool(true);
      if (inlineDepth > 0 && inlineReturnFlagReg != 0xFFFF) {
        trueReg = CombineLoopCondition(trueReg);
      }
      u32 branchIdx = builder.currentInstruction;
      builder.EmitInstruction(OP_BRANCH, 0, trueReg);

      u32 bodyStart = builder.currentInstruction;

      // Body - track loop depth for nested if-statement merge handling
      loopDepth++;
      PushLoopContext();
      if (!loop.body.IsNull()) {
        LowerStatement(loop.body);
      }

      u32 untilBranchIdx = 0;
      if (!loop.untilCondition.IsNull()) {
        u16 untilReg = LowerExpression(loop.untilCondition);
        untilBranchIdx = builder.currentInstruction;
        // Branch out if until condition is true
        builder.EmitInstruction(OP_BRANCH, 0, untilReg);
      }
      loopDepth--;

      // Continue target = back-edge
      u32 continueTarget = builder.currentInstruction;

      // Jump back to loop header
      u32 backEdgeIdx = builder.currentInstruction;
      builder.EmitInstruction(OP_JUMP, 0, 0);
      program.metadata[backEdgeIdx] = loopHeader;

      // If inside an outer loop, emit NOP to create dedicated merge block
      // This prevents inner merge from conflicting with outer continue target
      // The merge point must be the NOP instruction itself
      u32 loopEnd = builder.currentInstruction; // NOP is the merge point
      builder.EmitInstruction(OP_NOP, 0, 0);

      // Patch break/skip jumps
      PopLoopContext(continueTarget, loopEnd);

      // Patch header branch: always enters body (true=body, false=exit for
      // structure)
      program.SetBranchTargets(branchIdx, bodyStart, loopEnd);

      // Patch until branch if present
      if (!loop.untilCondition.IsNull()) {
        program.SetBranchTargets(untilBranchIdx, loopEnd, continueTarget);
      }

      // Annotate loop structure
      program.structureInfo[branchIdx] =
          IRProgram::PackStructure(IRProgram::STRUCT_LOOP_HEADER, loopEnd);
      program.continueInfo[branchIdx] = continueTarget;
    }
  }

  void LowerSwitch(NodeRef ref) {
    const SwitchData &sw = ast->GetSwitch(ref);

    auto GetCaseLiteralValue = [&](NodeRef valueRef, s32 *outVal) -> bool {
      if (valueRef.Type() == ASTNodeType::LITERAL) {
        const LiteralData &lit = ast->GetLiteral(valueRef);
        switch (lit.value.type) {
        case LiteralValue::INT:
          *outVal = static_cast<s32>(lit.value.intValue);
          return true;
        case LiteralValue::UINT:
          *outVal = static_cast<s32>(lit.value.uintValue);
          return true;
        case LiteralValue::BOOL:
          *outVal = lit.value.boolValue ? 1 : 0;
          return true;
        default:
          return false;
        }
      }

      if (valueRef.Type() == ASTNodeType::IDENTIFIER) {
        const IdentifierData &ident = ast->GetIdentifier(valueRef);
        Symbol *sym = SymbolTable::LookupAny(
            const_cast<SymbolTableData *>(symbols), ident.name);
        if (sym) {
          if (sym->kind == SymbolKind::EVAL_CONSTANT) {
            const LiteralValue &val = symbols->evalConstants[sym->index];
            if (val.type == LiteralValue::INT) {
              *outVal = static_cast<s32>(val.intValue);
              return true;
            }
            if (val.type == LiteralValue::UINT) {
              *outVal = static_cast<s32>(val.uintValue);
              return true;
            }
            if (val.type == LiteralValue::BOOL) {
              *outVal = val.boolValue ? 1 : 0;
              return true;
            }
          } else if (sym->kind == SymbolKind::VARIABLE) {
            const VariableData &varData = symbols->variables[sym->index];
            if (varData.isConst) {
              const LiteralValue &val = varData.evalValue;
              if (val.type == LiteralValue::INT) {
                *outVal = static_cast<s32>(val.intValue);
                return true;
              }
              if (val.type == LiteralValue::UINT) {
                *outVal = static_cast<s32>(val.uintValue);
                return true;
              }
              if (val.type == LiteralValue::BOOL) {
                *outVal = val.boolValue ? 1 : 0;
                return true;
              }
            }
          }
        }
      }

      // Enum variant access: `EnumName.Variant` as a case label.
      if (valueRef.Type() == ASTNodeType::MEMBER_ACCESS) {
        const MemberAccessData &access = ast->GetMemberAccess(valueRef);
        if (access.object.Type() == ASTNodeType::IDENTIFIER) {
          const IdentifierData &objIdent = ast->GetIdentifier(access.object);
          Symbol *enumSym = SymbolTable::LookupAny(
              const_cast<SymbolTableData *>(symbols), objIdent.name);
          if (enumSym && (enumSym->kind == SymbolKind::CUSTOM_TYPE ||
                          enumSym->kind == SymbolKind::ENUM_SYMBOL)) {
            const EnumData &enumData = symbols->enums[enumSym->index];
            for (u32 v = 0; v < enumData.variants.count; v++) {
              if (enumData.variants[v].name.nameHash ==
                  access.member.nameHash) {
                *outVal = static_cast<s32>(enumData.variants[v].value);
                return true;
              }
            }
          }
        }
      }
      return false;
    };

    // Lower switch expression
    u16 exprReg = LowerExpression(sw.expression);

    // Emit the switch instruction
    u32 switchIdx = builder.currentInstruction;
    builder.EmitInstruction(OP_SWITCH, 0, exprReg);
    // metadata will point to switch data index
    program.metadata[switchIdx] = program.switchCount;

    // Count case arms and total values (each arm can have multiple values)
    u32 caseArmCount = sw.cases.count;
    bool hasDefault = !sw.defaultCase.IsNull();

    // Count total case values across all arms for array sizing
    u32 totalCaseValues = 0;
    for (u32 i = 0; i < caseArmCount; i++) {
      NodeRef caseRef = sw.cases[i];
      const SwitchCaseData &caseData = ast->GetSwitchCase(caseRef);
      if (!caseData.isDefault) {
        totalCaseValues += caseData.values.count;
      }
    }

    // First pass: collect all case values for jump table analysis
    s32 minCase = INT32_MAX;
    s32 maxCase = INT32_MIN;
    for (u32 i = 0; i < caseArmCount; i++) {
      NodeRef caseRef = sw.cases[i];
      const SwitchCaseData &caseData = ast->GetSwitchCase(caseRef);

      if (!caseData.isDefault) {
        for (u32 v = 0; v < caseData.values.count; v++) {
          NodeRef valueRef = caseData.values[v];
          s32 caseVal = 0;
          if (!GetCaseLiteralValue(valueRef, &caseVal)) {
            fprintf(
                stderr,
                "Error: switch case values must be compile-time literals\n");
            return;
          }
          minCase = (caseVal < minCase) ? caseVal : minCase;
          maxCase = (caseVal > maxCase) ? caseVal : maxCase;
        }
      }
    }

    // Determine if we should use a jump table
    // Jump table is efficient when density >= 50% and range is reasonable
    s32 range = (totalCaseValues > 0) ? (maxCase - minCase + 1) : 0;
    bool useJumpTable = (totalCaseValues >= 3) && (range <= 256) &&
                        (static_cast<u32>(range) <= totalCaseValues * 2);
    (void)useJumpTable; // For future optimization - currently always use linear

    // Record switch ID and reserve case data range up-front (nested switches
    // rely on this)
    u32 switchId = program.switchCount++;
    program.switchInstructionIndices[switchId] = switchIdx;

    // Get offset for case values in flattened arrays
    u32 caseOffset = program.switchCaseOffsets[switchId];
    program.switchCaseOffsets[switchId + 1] = caseOffset + totalCaseValues;

    // Emit case bodies and collect targets
    u32 *caseTargets = (u32 *)alloca(caseArmCount * sizeof(u32));
    u32 *caseJumps = (u32 *)alloca(caseArmCount * sizeof(u32));

    for (u32 i = 0; i < caseArmCount; i++) {
      NodeRef caseRef = sw.cases[i];
      const SwitchCaseData &caseData = ast->GetSwitchCase(caseRef);

      caseTargets[i] = builder.currentInstruction;

      // Lower case body
      if (!caseData.body.IsNull()) {
        LowerStatement(caseData.body);
      }

      // Emit jump to merge point (will patch later)
      caseJumps[i] = builder.currentInstruction;
      builder.EmitInstruction(OP_JUMP, 0, 0);
    }

    // Emit default case if present
    u32 defaultTarget = builder.currentInstruction;
    u32 defaultJumpIdx = 0;
    if (hasDefault) {
      const SwitchCaseData &defaultData = ast->GetSwitchCase(sw.defaultCase);
      if (!defaultData.body.IsNull()) {
        LowerStatement(defaultData.body);
      }
      // Jump to merge point
      defaultJumpIdx = builder.currentInstruction;
      builder.EmitInstruction(OP_JUMP, 0, 0);
    }

    u32 mergePoint = builder.currentInstruction;
    if (loopDepth > 0) {
      // Keep switch merge inside loop body, distinct from loop continue/merge
      // targets.
      builder.EmitInstruction(OP_NOP, 0, 0, 0);
    }

    // Patch all case jumps to merge point
    for (u32 i = 0; i < caseArmCount; i++) {
      program.metadata[caseJumps[i]] = mergePoint;
    }
    if (defaultJumpIdx != 0) {
      program.metadata[defaultJumpIdx] = mergePoint;
    }

    // Store case data in IR program - each value maps to its arm's target
    u32 valueIdx = 0;
    for (u32 i = 0; i < caseArmCount; i++) {
      NodeRef caseRef = sw.cases[i];
      const SwitchCaseData &caseData = ast->GetSwitchCase(caseRef);

      if (!caseData.isDefault) {
        // Each value in this arm maps to the same target
        for (u32 v = 0; v < caseData.values.count; v++) {
          NodeRef valueRef = caseData.values[v];
          s32 caseVal = 0;
          if (!GetCaseLiteralValue(valueRef, &caseVal)) {
            fprintf(
                stderr,
                "Error: switch case values must be compile-time literals\n");
            return;
          }
          program.switchCaseValues[caseOffset + valueIdx] = caseVal;
          program.switchCaseTargets[caseOffset + valueIdx] = caseTargets[i];
          valueIdx++;
        }
      }
    }

    program.switchDefaultTargets[switchId] =
        hasDefault ? defaultTarget : mergePoint;

    // Annotate switch structure for SPIR-V
    program.structureInfo[switchIdx] =
        IRProgram::PackStructure(IRProgram::STRUCT_SWITCH_HEADER, mergePoint);
  }

  //==========================================================================
  // Expression lowering
  //==========================================================================

  struct LowerDepthGuard {
    u32 &d;
    LowerDepthGuard(u32 &d_) : d(d_) { ++d; }
    ~LowerDepthGuard() { --d; }
  };

  u16 LowerExpression(NodeRef ref) {
    if (ref.IsNull())
      return 0;

    // Recursion guard. Nested function-call / binary-op / ternary AST can
    // cycle or reach extreme depth from pathological inputs, blowing the
    // thread stack inside LowerFunctionCall -> LowerExpression -> ...
    // MAX_LOWER_DEPTH well exceeds any reasonable shader nesting but is
    // far below the ~1MB thread stack (each frame ~200 bytes).
    static constexpr u32 MAX_LOWER_DEPTH = 1024;
    if (lowerDepth >= MAX_LOWER_DEPTH) {
      return 0;
    }
    LowerDepthGuard _guard(lowerDepth);

    // Check if we already computed this (keyed by packed NodeRef)
    auto it = nodeRegisters.find(ref.packed);
    if (it != nodeRegisters.end()) {
      return it->second;
    }

    u16 result = 0;

    switch (ref.Type()) {
    case ASTNodeType::LITERAL:
      result = LowerLiteral(ref);
      break;
    case ASTNodeType::IDENTIFIER:
      result = LowerIdentifier(ref);
      break;
    case ASTNodeType::BINARY_OP:
      result = LowerBinaryOp(ref);
      break;
    case ASTNodeType::UNARY_OP:
      result = LowerUnaryOp(ref);
      break;
    case ASTNodeType::TERNARY_EXPRESSION: {
      auto &tern = ast->GetTernaryExpression(ref);
      u16 condReg = LowerExpression(tern.condition);
      u16 trueReg = LowerExpression(tern.trueExpr);
      u16 falseReg = LowerExpression(tern.falseExpr);
      CoreType resultType = GetRegisterType(trueReg);

      // Matrix-typed ternary: pre-SPIR-V 1.4 `OpSelect` requires scalar or
      // vector types, so decompose a `cond ? matA : matB` into per-column
      // OpSelects and rebuild via OP_MAT_CONSTRUCT. Matches the struct
      // ternary decomposition below.
      if ((mask(resultType) & TypeMasks::MATRIX_TYPES) != 0) {
        u32 cols = (resultType == CoreType::MAT4)   ? 4
                 : (resultType == CoreType::MAT3)   ? 3
                                                    : 2;
        CoreType colType = (resultType == CoreType::MAT4) ? CoreType::FLOAT4
                         : (resultType == CoreType::MAT3) ? CoreType::FLOAT3
                                                          : CoreType::FLOAT2;
        u16 colDests[4] = {0, 0, 0, 0};
        for (u32 c = 0; c < cols; c++) {
          u16 cIdx = EmitConstantInt(static_cast<int>(c));
          u16 colT = AllocateRegister();
          u16 colF = AllocateRegister();
          SetRegisterType(colT, colType);
          SetRegisterType(colF, colType);
          builder.EmitInstruction(OP_ARRAY_LOAD, colT, trueReg, cIdx);
          builder.EmitInstruction(OP_ARRAY_LOAD, colF, falseReg, cIdx);

          u16 pick = AllocateRegister();
          SetRegisterType(pick, colType);
          // OP_SELECT operand order is (false, true, cond)
          builder.EmitInstruction(OP_SELECT, pick, colF, colT, condReg);
          colDests[c] = pick;
        }
        u16 dest = AllocateRegister();
        builder.EmitInstruction(OP_MAT_CONSTRUCT, dest, colDests[0],
                                colDests[1], colDests[2], colDests[3]);
        program.metadata[builder.currentInstruction - 1] = cols;
        SetRegisterType(dest, resultType);
        return dest;
      }

      // Struct-typed ternary can't use a single OpSelect pre-SPIR-V 1.4.
      // Decompose into per-field OpSelect + composite reconstruction.
      if (resultType == CoreType::CUSTOM && trueReg < MAX_REGISTERS &&
          falseReg < MAX_REGISTERS) {
        u32 structHash = program.registerStructTypes[trueReg];
        if (structHash == 0) structHash = program.registerStructTypes[falseReg];
        if (structHash != 0) {
          auto it = structTypeMap.find(structHash);
          if (it != structTypeMap.end()) {
            const IRProgram::StructTypeInfo &info = program.structTypes[it->second];
            // Start from one of the operands and insert selected fields in place.
            u16 current = trueReg;
            for (u32 i = 0; i < info.fieldCount; i++) {
              u16 tf = AllocateRegister();
              builder.EmitInstruction(OP_STRUCT_EXTRACT, tf, trueReg, i);
              program.metadata[builder.currentInstruction - 1] = structHash;
              CoreType fieldType = static_cast<CoreType>(
                  program.structFieldTypes[info.fieldOffset + i]);
              SetRegisterType(tf, fieldType);

              u16 ff = AllocateRegister();
              builder.EmitInstruction(OP_STRUCT_EXTRACT, ff, falseReg, i);
              program.metadata[builder.currentInstruction - 1] = structHash;
              SetRegisterType(ff, fieldType);

              u16 pick = AllocateRegister();
              builder.EmitInstruction(OP_SELECT, pick, ff, tf, condReg);
              SetRegisterType(pick, fieldType);

              u16 next = AllocateRegister();
              builder.EmitInstruction(OP_STRUCT_INSERT, next, current, i, pick);
              program.metadata[builder.currentInstruction - 1] = structHash;
              SetRegisterType(next, CoreType::CUSTOM);
              program.registerStructTypes[next] = structHash;
              current = next;
            }
            return current;
          }
        }
      }

      // Diagnose pointer-typed ternaries. A naive OpSelect on pointers
      // produces `Type Id is 0` SPIR-V validation errors (Function-storage
      // pointer select requires VariablePointers and is backend-specific).
      // Print an actionable diagnostic; downstream SPIR-V validation will
      // then fail the compile with a non-zero exit so the error is not
      // silently swallowed.
      if (trueReg < MAX_REGISTERS && falseReg < MAX_REGISTERS) {
        u32 tInfo = program.registerStorageInfo[trueReg];
        u32 fInfo = program.registerStorageInfo[falseReg];
        if ((tInfo & IR::IRProgram::STORAGE_IS_PTR) ||
            (fInfo & IR::IRProgram::STORAGE_IS_PTR)) {
          fprintf(stderr,
                  "Error: ternary expression with pointer operands is not "
                  "supported; select the dereferenced value instead "
                  "(e.g. `(c ? pa^ : pb^)`) or branch on the pointer with "
                  "if/else.\n");
        }
      }

      u16 dest = AllocateRegister();
      // Note: OP_SELECT order is (false, true, cond)
      builder.EmitInstruction(OP_SELECT, dest, falseReg, trueReg, condReg);
      SetRegisterType(dest, resultType);
      return dest;
    }
    case ASTNodeType::FUNCTION_CALL:
      result = LowerFunctionCall(ref);
      break;
    case ASTNodeType::MEMBER_ACCESS:
      result = LowerMemberAccess(ref);
      break;
    case ASTNodeType::ARRAY_ACCESS:
      result = LowerArrayAccess(ref);
      break;
    case ASTNodeType::BLOCK: {
      // Block as expression - lower statements and capture return value as
      // result
      const BlockData &block = ast->GetBlock(ref);
      result = 0;
      for (u32 i = 0; i < block.statements.count; i++) {
        NodeRef stmt = block.statements[i];
        // Check for RETURN statement - capture its value instead of emitting
        // return
        if (stmt.Type() == ASTNodeType::RETURN) {
          const AssignmentData &ret = ast->GetAssignment(stmt);
          if (!ret.value.IsNull()) {
            result = LowerExpression(ret.value);
          }
          // Don't emit the actual return, just capture the value
          break; // Return terminates the block
        }
        // Last statement might be an expression whose value is the block result
        else if (i == block.statements.count - 1) {
          // Try to lower as expression first
          switch (stmt.Type()) {
          case ASTNodeType::LITERAL:
          case ASTNodeType::IDENTIFIER:
          case ASTNodeType::BINARY_OP:
          case ASTNodeType::UNARY_OP:
          case ASTNodeType::FUNCTION_CALL:
          case ASTNodeType::MEMBER_ACCESS:
          case ASTNodeType::ARRAY_ACCESS:
            result = LowerExpression(stmt);
            break;
          default:
            // Lower as statement, result is 0
            LowerStatement(stmt);
            break;
          }
        } else {
          // Lower as statement
          LowerStatement(stmt);
        }
      }
      break;
    }
    default:
      result = AllocateRegister();
      break;
    }

    nodeRegisters[ref.packed] = result;
    return result;
  }

  // Resolves `^struct_var.field` to an OP_LOCAL_FIELD_PTR instruction.
  // Returns the destination register (non-zero) on success, or 0 if the
  // access pattern isn't a simple local-struct field (e.g. a swizzle,
  // a chained access, or an access through a non-local / non-struct
  // object). The caller falls back to the legacy ADDRESS_OF path on 0.
  u16 TryLowerLocalFieldAddressOf(NodeRef memberRef) {
    const MemberAccessData &access = ast->GetMemberAccess(memberRef);

    if (access.object.Type() != ASTNodeType::IDENTIFIER) return 0;
    const IdentifierData &obj = ast->GetIdentifier(access.object);
    if (obj.identifierKind != SpecialIdentifier::NONE) return 0;

    // The identifier must be a user variable we've already allocated a
    // register for. LowerIdentifier handles symbol lookup + caching.
    u16 baseReg = LowerIdentifier(access.object);
    if (baseReg >= MAX_REGISTERS) return 0;

    // Base must carry a struct type hash (either from declaration or
    // inferred). Swizzle / vector-component cases hit when the base
    // type is a vector with no struct hash — bail and let the existing
    // ADDRESS_OF path reject them.
    u32 structTypeHash = program.registerStructTypes[baseReg];
    if (structTypeHash == 0) {
      auto varIt = variableStructTypes.find(obj.name.nameHash);
      if (varIt != variableStructTypes.end()) {
        structTypeHash = varIt->second;
        program.registerStructTypes[baseReg] = structTypeHash;
      }
    }
    if (structTypeHash == 0) return 0;

    auto structIt = structTypeMap.find(structTypeHash);
    if (structIt == structTypeMap.end()) return 0;

    const IRProgram::StructTypeInfo &info = program.structTypes[structIt->second];
    u32 fieldIndex = 0xFFFFFFFF;
    CoreType fieldType = CoreType::FLOAT;
    u32 fieldTypeHash = 0;
    for (u32 i = 0; i < info.fieldCount; i++) {
      if (program.structFieldNameHashes[info.fieldOffset + i] ==
          access.member.nameHash) {
        fieldIndex = i;
        fieldType = static_cast<CoreType>(
            program.structFieldTypes[info.fieldOffset + i]);
        if (program.structFieldTypeHashes) {
          fieldTypeHash = program.structFieldTypeHashes[info.fieldOffset + i];
        }
        break;
      }
    }
    if (fieldIndex == 0xFFFFFFFF) return 0;

    u16 dest = AllocateRegister();
    if (dest >= MAX_REGISTERS) return 0;

    // Emit: dest = &base.field
    // operand1 carries the literal field index (backends read it via
    // GetOperand(i, 1); SSA skips operand 1 of field-ptr renaming).
    builder.EmitInstruction(OP_LOCAL_FIELD_PTR, dest, baseReg,
                            static_cast<u16>(fieldIndex));
    program.metadata[builder.currentInstruction - 1] = structTypeHash;

    // Pointer value has "no legitimate CoreType" — mark as CUSTOM, same
    // as scalar pointers. Pointee type + base reg + field flags live
    // in registerStorageInfo so OP_LOCAL_LOAD / OP_LOCAL_STORE can
    // recover them.
    SetRegisterType(dest, CoreType::CUSTOM);
    u32 storageVal = (static_cast<u32>(baseReg) << 16) |
                     (static_cast<u32>(fieldType) << 8) |
                     IR::IRProgram::STORAGE_IS_PTR |
                     IR::IRProgram::STORAGE_IS_FIELD_PTR;
    program.registerStorageInfo[dest] = storageVal;

    // Base struct variable's address is effectively taken — SSA must
    // leave its register alone.
    program.registerStorageInfo[baseReg] |=
        IR::IRProgram::STORAGE_IS_ADDRESS_TAKEN;

    // If the field is itself a struct/enum, propagate its type hash so
    // chained member access on the dereferenced pointer sees the right
    // struct info.
    if ((fieldType == CoreType::CUSTOM || fieldType == CoreType::ENUM) &&
        fieldTypeHash != 0) {
      program.registerStructTypes[dest] = fieldTypeHash;
    }

    return dest;
  }

  u16 LowerUnaryOp(NodeRef ref) {
    const UnaryOpData &unop = ast->GetUnaryOp(ref);

    // Special case: address-of a struct-field member access (`^v.pos`).
    // Must fire before the generic LowerExpression path; otherwise
    // LowerMemberAccess emits OP_STRUCT_EXTRACT and we'd be pointing at a
    // by-value copy instead of the field's memory slot.
    if (unop.op == UnaryOpType::ADDRESS_OF &&
        unop.operand.Type() == ASTNodeType::MEMBER_ACCESS) {
      u16 fieldPtr = TryLowerLocalFieldAddressOf(unop.operand);
      if (fieldPtr != 0) return fieldPtr;
    }

    u16 operand = LowerExpression(unop.operand);
    u16 dest = AllocateRegister();

    OpCode op = OP_NOP;
    switch (unop.op) {
    case UnaryOpType::NEGATE: {
      CoreType type = GetRegisterType(operand);
      TypeMask tmask = mask(type);
      // Matrix types are float-valued but not in FLOAT_TYPES, so test them
      // explicitly to avoid falling into the integer path (which emits
      // OpSNegate on a float matrix and fails SPIR-V validation).
      bool isFloatLike = (tmask & TypeMasks::FLOAT_TYPES) ||
                         (tmask & TypeMasks::MATRIX_TYPES);
      op = isFloatLike ? OP_FNEG : OP_INEG;
      builder.EmitInstruction(op, dest, operand);
      SetRegisterType(dest, type); // Result has same type as operand
      return dest;
    }
    case UnaryOpType::NOT: {
      op = OP_NOT;
      CoreType type = GetRegisterType(operand);
      // Preserve bvec width — `!bvec3` must emit `OpLogicalNot %bvec3`, not
      // `%bool`. Scalar bools stay scalar.
      CoreType resultType = (type == CoreType::BOOL2 ||
                             type == CoreType::BOOL3 ||
                             type == CoreType::BOOL4)
                                ? type
                                : CoreType::BOOL;
      builder.EmitInstruction(op, dest, operand);
      SetRegisterType(dest, resultType);
      return dest;
    }
    case UnaryOpType::BITWISE_NOT: {
      CoreType type = GetRegisterType(operand);
      op = OP_NOT;
      builder.EmitInstruction(op, dest, operand);
      SetRegisterType(dest, type); // Bitwise NOT preserves type
      return dest;
    }
    case UnaryOpType::PRE_INCREMENT: {
      // ++x: Add 1 to operand, store back, return new value
      u16 one = EmitConstantInt(1);
      CoreType type = GetRegisterType(operand);
      op = (mask(type) & TypeMasks::FLOAT_TYPES) ? OP_FADD : OP_IADD;
      builder.EmitInstruction(op, dest, operand, one);
      SetRegisterType(dest, type);
      // Store back to the operand location (assuming it's a variable)
      builder.EmitInstruction(OP_STORE_REG, operand, dest);
      return dest;
    }
    case UnaryOpType::PRE_DECREMENT: {
      // --x: Subtract 1 from operand, store back, return new value
      u16 one = EmitConstantInt(1);
      CoreType type = GetRegisterType(operand);
      op = (mask(type) & TypeMasks::FLOAT_TYPES) ? OP_FSUB : OP_ISUB;
      builder.EmitInstruction(op, dest, operand, one);
      SetRegisterType(dest, type);
      builder.EmitInstruction(OP_STORE_REG, operand, dest);
      return dest;
    }
    case UnaryOpType::POST_INCREMENT: {
      // x++: Save old value, add 1 to operand, store back, return old value
      CoreType type = GetRegisterType(operand);
      builder.EmitInstruction(OP_STORE_REG, dest, operand); // dest = old value
      SetRegisterType(dest, type);
      u16 one = EmitConstantInt(1);
      u16 newVal = AllocateRegister();
      op = (mask(type) & TypeMasks::FLOAT_TYPES) ? OP_FADD : OP_IADD;
      builder.EmitInstruction(op, newVal, operand, one);
      SetRegisterType(newVal, type);
      builder.EmitInstruction(OP_STORE_REG, operand, newVal);
      return dest; // Return old value
    }
    case UnaryOpType::POST_DECREMENT: {
      // x--: Save old value, subtract 1 from operand, store back, return old
      // value
      CoreType type = GetRegisterType(operand);
      builder.EmitInstruction(OP_STORE_REG, dest, operand); // dest = old value
      SetRegisterType(dest, type);
      u16 one = EmitConstantInt(1);
      u16 newVal = AllocateRegister();
      op = (mask(type) & TypeMasks::FLOAT_TYPES) ? OP_FSUB : OP_ISUB;
      builder.EmitInstruction(op, newVal, operand, one);
      SetRegisterType(newVal, type);
      builder.EmitInstruction(OP_STORE_REG, operand, newVal);
      return dest; // Return old value
    }
    case UnaryOpType::ADDRESS_OF: {
      // ^x: Get pointer to variable. Rejects constants (bits 0x4000/0x8000),
      // out-of-range register indices, and registers past MAX_REGISTERS —
      // registerStorageInfo has only MAX_REGISTERS entries, and indexing with
      // a constant-encoded operand (>= 0x4000) reads/writes far OOB.
      if ((operand & 0xC000) != 0 || operand >= MAX_REGISTERS ||
          dest >= MAX_REGISTERS) {
        builder.EmitInstruction(OP_LOCAL_VAR_PTR, dest, operand);
        SetRegisterType(dest, CoreType::CUSTOM);
        return dest;
      }
      builder.EmitInstruction(OP_LOCAL_VAR_PTR, dest, operand);
      // Mark as pointer type - store pointee type and source register in
      // storage info Format: bits 0-7: flags, bits 8-15: pointee type, bits
      // 16-31: source register
      CoreType pointeeType = GetRegisterType(operand);
      SetRegisterType(dest, CoreType::CUSTOM); // Use CUSTOM to indicate pointer
      u32 storageVal = (static_cast<u32>(operand) << 16) |
                       (static_cast<u32>(pointeeType) << 8) |
                       IR::IRProgram::STORAGE_IS_PTR;
      builder.program->registerStorageInfo[dest] = storageVal;
      // Propagate the struct type hash so p^.field lowering can find the
      // struct layout. Without this, dereference falls back to vec-extract.
      if (program.registerStructTypes[operand] != 0) {
        program.registerStructTypes[dest] = program.registerStructTypes[operand];
      }
      // Mark the source register as address-taken so SSA doesn't create phi
      // nodes for it
      builder.program->registerStorageInfo[operand] |=
          IR::IRProgram::STORAGE_IS_ADDRESS_TAKEN;
      return dest;
    }
    case UnaryOpType::DEREFERENCE: {
      // x^: Dereference pointer.
      if ((operand & 0xC000) != 0 || operand >= MAX_REGISTERS) {
        builder.EmitInstruction(OP_LOCAL_LOAD, dest, operand);
        return dest;
      }
      u32 storageInfo = builder.program->registerStorageInfo[operand];
      if ((storageInfo & IR::IRProgram::STORAGE_IS_PTR) == 0) {
        // Operand isn't a pointer. This is a common parser-ambiguity
        // symptom: `a ^ -1` parses as `(a^) - 1` where `a` is a scalar
        // int, and the silent codegen used to emit OpLoad on a non-
        // pointer (rejected by SPIR-V validation, confusing error).
        // Explicit compile error: user should parenthesize the intended
        // operator — `a ^ (-1)` for XOR, `(ptr^) - n` for deref.
        fprintf(stderr, "Error: dereference (`^` postfix) applied to a "
                        "non-pointer value. If you meant binary XOR with a "
                        "negative / unary-prefixed operand (e.g. `a ^ -1`), "
                        "wrap the right side in parentheses: `a ^ (-1)`.\n");
        SetRegisterType(dest, CoreType::INT);
        return dest;
      }
      CoreType pointeeType = static_cast<CoreType>((storageInfo >> 8) & 0xFF);
      builder.EmitInstruction(OP_LOCAL_LOAD, dest, operand);
      SetRegisterType(dest, pointeeType);
      // Propagate the struct type hash to the loaded value so member access
      // on `p^.field` can find the struct layout.
      if (program.registerStructTypes[operand] != 0) {
        program.registerStructTypes[dest] = program.registerStructTypes[operand];
      }
      return dest;
    }
    }

    // Fallthrough for unhandled cases (shouldn't happen)
    builder.EmitInstruction(op, dest, operand);
    return dest;
  }

  u16 LowerIdentifier(NodeRef ref) {
    const IdentifierData &ident = ast->GetIdentifier(ref);

    // Check if we already have a register for this variable
    auto it = variableRegisters.find(ident.name.nameHash);
    if (it != variableRegisters.end()) {
      return it->second;
    }

    // Look up in symbol table
    Symbol *sym = SymbolTable::LookupByHash(
        const_cast<SymbolTableData *>(symbols), ident.name.nameHash);
    if (sym && sym->kind == SymbolKind::VARIABLE) {
      const VariableData &varData = symbols->variables[sym->index];
      if (varData.isConst && varData.constExpr.IsValid()) {
        return LowerExpression(varData.constExpr);
      }
      // Allocate and cache
      u16 reg = AllocateRegister();
      variableRegisters[ident.name.nameHash] = reg;

      // Set register type from symbol's variable data
      SetRegisterType(reg, varData.typeInfo.coreType);

      return reg;
    }

    // Fallback: allocate register but emit a zero initialization
    // This handles undefined variables (likely a semantic error, but we need
    // valid IR)
    u16 reg = AllocateRegister();
    variableRegisters[ident.name.nameHash] = reg;
    // Emit a zero constant as fallback value to ensure the register is defined
    u16 zero = builder.EmitConstant(0.0f);
    builder.EmitInstruction(OP_STORE_REG, reg, zero);
    SetRegisterType(reg, CoreType::FLOAT); // Default to float
    return reg;
  }

  void LowerVariableDecl(NodeRef ref) {
    const VariableDeclData &varDecl = ast->GetVariableDecl(ref);

    Symbol *sym = SymbolTable::LookupByHash(
        const_cast<SymbolTableData *>(symbols), varDecl.name.nameHash);
    const VariableData *varData = (sym && sym->kind == SymbolKind::VARIABLE)
                                      ? &symbols->variables[sym->index]
                                      : nullptr;
    bool isShared = (varDecl.storageClass == StorageClass::Shared) ||
                    (varData && varData->storageClass == StorageClass::Shared);
    bool isArray = varData && IsArray(varData->typeInfo);
    u32 arrayLength = isArray ? varData->typeInfo.arrayLength : 0;
    if (!isArray && varDecl.arrayDimensions > 0) {
      isArray = true;
      arrayLength = varDecl.arrayLength;
    }

    // Allocate register for the variable
    u16 varReg = AllocateRegister();

    // Store mapping from variable name hash to register
    variableRegisters[varDecl.name.nameHash] = varReg;

    // Track type from the declaration first to avoid symbol-table collisions
    // for locals.
    u32 customTypeHash = 0;
    CoreType coreType =
        ResolveCoreTypeFromHash(varDecl.type.nameHash, &customTypeHash);
    if ((coreType == CoreType::INVALID || coreType == CoreType::VOID) &&
        varData && varData->typeInfo.coreType != CoreType::INVALID) {
      coreType = varData->typeInfo.coreType;
      customTypeHash = varData->typeInfo.customTypeHash;
    }
    if ((coreType == CoreType::INVALID || coreType == CoreType::VOID) &&
        varDecl.arrayElementTypeHash != 0) {
      coreType = ResolveCoreTypeFromHash(varDecl.arrayElementTypeHash,
                                         &customTypeHash);
    }

    if (isShared) {
      if (currentStage != ShaderStage::Compute) {
        fprintf(
            stderr,
            "Error: shared variables are only allowed in compute shaders\n");
      }
      if (!isArray) {
        fprintf(stderr, "Error: shared variables must be declared as arrays\n");
      }

      SetRegisterType(varReg, coreType);

      if (program.sharedVarCount >= program.sharedVarCapacity) {
        u32 newCapacity = program.sharedVarCapacity * 2;
        u32 *newNameHashes =
            (u32 *)pool->Allocate(newCapacity * sizeof(u32), 64);
        u16 *newTypes = (u16 *)pool->Allocate(newCapacity * sizeof(u16), 64);
        u32 *newArraySizes =
            (u32 *)pool->Allocate(newCapacity * sizeof(u32), 64);
        u16 *newRegisters =
            (u16 *)pool->Allocate(newCapacity * sizeof(u16), 64);
        memcpy(newNameHashes, program.sharedNameHashes,
               program.sharedVarCapacity * sizeof(u32));
        memcpy(newTypes, program.sharedTypes,
               program.sharedVarCapacity * sizeof(u16));
        memcpy(newArraySizes, program.sharedArraySizes,
               program.sharedVarCapacity * sizeof(u32));
        memcpy(newRegisters, program.sharedRegisters,
               program.sharedVarCapacity * sizeof(u16));
        program.sharedNameHashes = newNameHashes;
        program.sharedTypes = newTypes;
        program.sharedArraySizes = newArraySizes;
        program.sharedRegisters = newRegisters;
        program.sharedVarCapacity = newCapacity;
      }

      u32 sharedIndex = program.sharedVarCount++;
      program.sharedNameHashes[sharedIndex] = varDecl.name.nameHash;
      program.sharedTypes[sharedIndex] = static_cast<u16>(coreType);
      program.sharedArraySizes[sharedIndex] = isArray ? arrayLength : 0;
      program.sharedRegisters[sharedIndex] = varReg;

      program.registerStorageInfo[varReg] =
          (sharedIndex << IR::IRProgram::STORAGE_BINDING_SHIFT) |
          IR::IRProgram::STORAGE_IS_PTR | IR::IRProgram::STORAGE_IS_SHARED;
      return;
    }

    // Check if this is a struct or enum sum type
    if (coreType == CoreType::CUSTOM || coreType == CoreType::INVALID) {
      u32 lookupHash =
          (customTypeHash != 0) ? customTypeHash : varDecl.type.nameHash;
      u32 structTypeHash = LookupOrRegisterStructType(lookupHash);
      if (structTypeHash != 0) {
        coreType = CoreType::CUSTOM;
        variableStructTypes[varDecl.name.nameHash] = structTypeHash;
        program.registerStructTypes[varReg] = structTypeHash;
      }
    }

    SetRegisterType(varReg, coreType);

    // Register local arrays (with or without initializer)
    if (isArray && arrayLength > 0) {
      // Get element type from the variable's array element type hash
      CoreType elementType = coreType;
      u32 elementStructHash = 0;
      if (varDecl.arrayElementTypeHash != 0) {
        elementType = ResolveCoreTypeFromHash(varDecl.arrayElementTypeHash,
                                              &elementStructHash);
      } else if (varData) {
        elementType = varData->typeInfo.coreType;
        elementStructHash = varData->typeInfo.customTypeHash;
      }
      // For struct element types, ensure we have the struct type registered
      if ((elementType == CoreType::CUSTOM ||
           elementType == CoreType::INVALID) &&
          elementStructHash == 0 && varDecl.arrayElementTypeHash != 0) {
        elementStructHash =
            LookupOrRegisterStructType(varDecl.arrayElementTypeHash);
        if (elementStructHash != 0) {
          elementType = CoreType::CUSTOM;
        }
      }

      // Register the array for local array tracking
      program.localArrayNameHashes[program.localArrayCount] =
          varDecl.name.nameHash;
      program.localArrayTypes[program.localArrayCount] =
          static_cast<u16>(elementType);
      program.localArrayStructTypes[program.localArrayCount] =
          elementStructHash;
      program.localArraySizes[program.localArrayCount] = arrayLength;
      program.localArrayRegisters[program.localArrayCount] = varReg;
      program.localArrayCount++;

      // Mark the register as an array pointer
      program.registerStorageInfo[varReg] =
          ((program.localArrayCount - 1)
           << IR::IRProgram::STORAGE_BINDING_SHIFT) |
          IR::IRProgram::STORAGE_IS_PTR | IR::IRProgram::STORAGE_IS_LOCAL_ARRAY;
    }

    // If there's an initializer, evaluate it
    if (!varDecl.initializer.IsNull()) {
      // Check for array initializer (BLOCK node with element expressions)
      if (isArray && varDecl.initializer.Type() == ASTNodeType::BLOCK) {
        // Array with block initializer - emit element-by-element stores
        const BlockData &initBlock = ast->GetBlock(varDecl.initializer);
        for (u32 i = 0; i < initBlock.statements.count && i < arrayLength;
             i++) {
          NodeRef elementExpr = initBlock.statements[i];
          u16 elementReg = LowerExpression(elementExpr);

          // Emit array store: store element at index i
          u16 indexReg = EmitConstantUint(i);
          builder.EmitInstruction(OP_ARRAY_STORE, varReg, indexReg, elementReg);
        }
        initializedVariables.insert(varDecl.name.nameHash);
      } else {
        u16 initReg = LowerExpression(varDecl.initializer);

        // If the variable has a known struct type and the initializer doesn't,
        // propagate the struct type to the initializer result. This handles
        // cases like loading from untyped storage buffers into typed variables.
        if (coreType == CoreType::CUSTOM && initReg < MAX_REGISTERS) {
          u32 varStructHash = variableStructTypes[varDecl.name.nameHash];
          if (varStructHash == 0) {
            varStructHash = program.registerStructTypes[varReg];
          }
          if (varStructHash != 0 && program.registerStructTypes[initReg] == 0) {
            program.registerStructTypes[initReg] = varStructHash;
            SetRegisterType(initReg, CoreType::CUSTOM);

            // If the initializer came from a storage buffer load, record the
            // element struct type for this binding
            u32 storageInfo = program.registerStorageInfo[initReg];
            if (storageInfo != 0) {
              u32 binding =
                  (storageInfo >> IR::IRProgram::STORAGE_BINDING_SHIFT);
              if (binding < 32) {
                program.bufferElementStructTypes[binding] = varStructHash;
              }
            }
          }
        }

        // If the initializer came from a storage buffer load, record the
        // element type for this binding (needed for SPIR-V backend to declare
        // correct buffer element types)
        if (initReg < MAX_REGISTERS && coreType != CoreType::CUSTOM) {
          u32 storageInfo = program.registerStorageInfo[initReg];
          if (storageInfo != 0) {
            u32 binding = (storageInfo >> IR::IRProgram::STORAGE_BINDING_SHIFT);
            if (binding < 32 && program.bufferElementCoreTypes[binding] == 0) {
              program.bufferElementCoreTypes[binding] =
                  static_cast<u8>(coreType);
            }
          }
        }

        builder.EmitInstruction(OP_STORE_REG, varReg, initReg);
        // Propagate pointer storage info if the init expression is a pointer
        if (initReg < MAX_REGISTERS && (program.registerStorageInfo[initReg] &
                                        IR::IRProgram::STORAGE_IS_PTR)) {
          program.registerStorageInfo[varReg] =
              program.registerStorageInfo[initReg];
          // Also propagate the struct type hash for `TypeName^ p = ^s;` so
          // `p^.field` later resolves the struct layout.
          if (program.registerStructTypes[initReg] != 0 &&
              varReg < MAX_REGISTERS) {
            program.registerStructTypes[varReg] =
                program.registerStructTypes[initReg];
          }
        }
        initializedVariables.insert(varDecl.name.nameHash);
      }
    } else if (!isArray && coreType == CoreType::CUSTOM) {
      // For single struct types without initializer, emit a zero/undef struct
      // value. Note: struct arrays are handled differently - they are
      // pointer-based and individual elements are stored via ARRAY_STORE.
      u32 structTypeHash = variableStructTypes[varDecl.name.nameHash];
      if (structTypeHash == 0 && varReg < MAX_REGISTERS) {
        structTypeHash = program.registerStructTypes[varReg];
      }
      if (structTypeHash != 0) {
        u16 zeroStruct = EmitZeroStruct(structTypeHash);
        builder.EmitInstruction(OP_STORE_REG, varReg, zeroStruct);
        initializedVariables.insert(varDecl.name.nameHash);
      }
    } else if (!isArray &&
               (coreType == CoreType::FLOAT2 || coreType == CoreType::FLOAT3 ||
                coreType == CoreType::FLOAT4)) {
      // For vector types without initializer, emit a zero vector construct
      // This ensures the variable has a defined value for OpCompositeInsert to
      // use
      u16 zero = builder.EmitConstant(0.0f);
      u32 componentCount = (coreType == CoreType::FLOAT2)   ? 2
                           : (coreType == CoreType::FLOAT3) ? 3
                                                            : 4;
      builder.EmitInstruction(OP_VEC_CONSTRUCT, varReg, zero, zero, zero, zero);
      program.metadata[program.instructionCount - 1] = componentCount;
      initializedVariables.insert(varDecl.name.nameHash);
    }
  }

  u16 EmitZeroStruct(u32 structTypeHash) {
    if (structTypeHash == 0) {
      return EmitZeroConstant(CoreType::FLOAT);
    }

    auto FindStructInfo = [&](u32 hash) -> const IRProgram::StructTypeInfo * {
      for (u32 i = 0; i < program.structTypeCount; i++) {
        if (program.structTypes[i].nameHash == hash) {
          return &program.structTypes[i];
        }
      }
      return nullptr;
    };

    // Find struct info, registering if needed (for nested custom types).
    const IRProgram::StructTypeInfo *structInfo =
        FindStructInfo(structTypeHash);
    if (!structInfo) {
      if (LookupOrRegisterStructType(structTypeHash) != 0) {
        structInfo = FindStructInfo(structTypeHash);
      }
    }
    if (!structInfo || structInfo->fieldCount == 0) {
      return EmitZeroConstant(CoreType::FLOAT);
    }

    u16 baseReg = AllocateRegister();
    SetRegisterType(baseReg, CoreType::CUSTOM);
    if (baseReg < MAX_REGISTERS) {
      program.registerStructTypes[baseReg] = structTypeHash;
    }
    AddUndefRegister(baseReg, CoreType::CUSTOM);

    u16 currentReg = baseReg;
    for (u32 f = 0; f < structInfo->fieldCount; f++) {
      CoreType fieldType = static_cast<CoreType>(
          program.structFieldTypes[structInfo->fieldOffset + f]);
      u32 fieldTypeHash = 0;
      if ((fieldType == CoreType::CUSTOM || fieldType == CoreType::ENUM) &&
          program.structFieldTypeHashes) {
        fieldTypeHash =
            program.structFieldTypeHashes[structInfo->fieldOffset + f];
      }

      // Check if this field is an array - skip zero init for arrays
      // (they will remain undefined, which is valid in SPIR-V)
      u32 arraySize = 0;
      if (program.structFieldArraySizes) {
        arraySize = program.structFieldArraySizes[structInfo->fieldOffset + f];
      }
      if (arraySize > 0) {
        // Skip array fields - they remain as undef in the base struct
        continue;
      }

      u16 valueReg = 0xFFFF;
      if (fieldTypeHash != 0) {
        valueReg = EmitZeroStruct(fieldTypeHash);
      } else {
        valueReg = EmitZeroConstant(fieldType);
      }

      u16 nextReg = AllocateRegister();
      SetRegisterType(nextReg, CoreType::CUSTOM);
      if (nextReg < MAX_REGISTERS) {
        program.registerStructTypes[nextReg] = structTypeHash;
      }
      builder.EmitInstruction(OP_STRUCT_INSERT, nextReg, currentReg, f,
                              valueReg);
      program.metadata[program.instructionCount - 1] = structTypeHash;
      currentReg = nextReg;
    }

    return currentReg;
  }

  // Emit a zero constant for a given type
  u16 EmitZeroConstant(CoreType type) {
    auto EmitZeroVector = [&](CoreType vecType, u16 zero,
                              u32 componentCount) -> u16 {
      u16 destReg = AllocateRegister();
      SetRegisterType(destReg, vecType);
      builder.EmitInstruction(OP_VEC_CONSTRUCT, destReg, zero, zero, zero,
                              zero);
      program.metadata[program.instructionCount - 1] = componentCount;
      return destReg;
    };

    switch (type) {
    case CoreType::FLOAT:
      return builder.EmitConstant(0.0f);
    case CoreType::BOOL:
      return builder.EmitConstantBool(false);
    case CoreType::FLOAT2:
    case CoreType::FLOAT3:
    case CoreType::FLOAT4: {
      u16 zero = builder.EmitConstant(0.0f);
      u32 componentCount = (type == CoreType::FLOAT2)   ? 2
                           : (type == CoreType::FLOAT3) ? 3
                                                        : 4;
      return EmitZeroVector(type, zero, componentCount);
    }
    case CoreType::INT:
      return EmitConstantInt(0);
    case CoreType::UINT:
      return EmitConstantUint(0);
    case CoreType::INT2:
    case CoreType::INT3:
    case CoreType::INT4: {
      u16 zero = EmitConstantInt(0);
      u32 componentCount = (type == CoreType::INT2)   ? 2
                           : (type == CoreType::INT3) ? 3
                                                      : 4;
      return EmitZeroVector(type, zero, componentCount);
    }
    case CoreType::UINT2:
    case CoreType::UINT3:
    case CoreType::UINT4: {
      u16 zero = EmitConstantUint(0);
      u32 componentCount = (type == CoreType::UINT2)   ? 2
                           : (type == CoreType::UINT3) ? 3
                                                       : 4;
      return EmitZeroVector(type, zero, componentCount);
    }
    case CoreType::BOOL2:
    case CoreType::BOOL3:
    case CoreType::BOOL4: {
      u16 zero = builder.EmitConstantBool(false);
      u32 componentCount = (type == CoreType::BOOL2)   ? 2
                           : (type == CoreType::BOOL3) ? 3
                                                       : 4;
      return EmitZeroVector(type, zero, componentCount);
    }
    case CoreType::MAT2:
    case CoreType::MAT3:
    case CoreType::MAT4: {
      u32 columnCount = (type == CoreType::MAT2)   ? 2
                        : (type == CoreType::MAT3) ? 3
                                                   : 4;
      CoreType columnType = (type == CoreType::MAT2)   ? CoreType::FLOAT2
                            : (type == CoreType::MAT3) ? CoreType::FLOAT3
                                                       : CoreType::FLOAT4;
      u32 componentCount = (columnType == CoreType::FLOAT2)   ? 2
                           : (columnType == CoreType::FLOAT3) ? 3
                                                              : 4;
      u16 zero = builder.EmitConstant(0.0f);
      u16 columns[4] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
      for (u32 i = 0; i < columnCount; i++) {
        columns[i] = EmitZeroVector(columnType, zero, componentCount);
      }
      u16 destReg = AllocateRegister();
      SetRegisterType(destReg, type);
      builder.EmitInstruction(OP_MAT_CONSTRUCT, destReg, columns[0], columns[1],
                              columns[2], columns[3]);
      program.metadata[program.instructionCount - 1] = columnCount;
      return destReg;
    }
    default:
      return builder.EmitConstant(0.0f);
    }
  }

  // Look up or register a struct type in the IR program
  u32 LookupOrRegisterStructType(u32 typeNameHash) {
    // Check if already registered with this hash
    auto it = structTypeMap.find(typeNameHash);
    if (it != structTypeMap.end()) {
      // Return the canonical hash stored in structTypes, not the lookup hash
      u32 structIdx = it->second;
      return program.structTypes[structIdx].nameHash;
    }

    // Look up in symbol table
    Symbol *typeSym = SymbolTable::LookupByHash(
        const_cast<SymbolTableData *>(symbols), typeNameHash);
    if (!typeSym || typeSym->kind != SymbolKind::CUSTOM_TYPE) {
      // Check if it's an enum type (ENUM for module enums, ENUM_SYMBOL for
      // pipeline-local enums)
      if (typeSym && (typeSym->kind == SymbolKind::ENUM ||
                      typeSym->kind == SymbolKind::ENUM_SYMBOL)) {
        const EnumData &enumData = symbols->enums[typeSym->index];
        if (enumData.flags & EnumData::IS_SUM_TYPE) {
          return RegisterEnumAsStructType(typeNameHash, typeSym->index);
        }
        return 0;
      }

      // Try looking up via global custom type registry
      StructData *structData = g_customTypes.LookupType(typeNameHash);
      if (structData) {
        return RegisterStructTypeFromData(typeNameHash, structData);
      }
      return 0;
    }
    // Get the struct data - use its name hash as canonical (unqualified name
    // from struct definition) This is important for module-qualified types like
    // PBR::PBRMaterial where:
    // - typeSym->name.nameHash is the qualified hash "PBR::PBRMaterial"
    // - structData.name.nameHash is the unqualified hash "PBRMaterial"
    const StructData &structData = symbols->structs[typeSym->index];
    u32 canonicalHash = structData.name.nameHash;

    // Check if already registered with the canonical hash (may differ from
    // lookup hash)
    auto canonIt = structTypeMap.find(canonicalHash);
    if (canonIt != structTypeMap.end()) {
      // Also register the lookup hash as an alias to the same struct
      if (typeNameHash != canonicalHash) {
        structTypeMap[typeNameHash] = canonIt->second;
      }
      return canonicalHash;
    }

    // Register with canonical hash
    u32 result = RegisterStructTypeFromSymbol(canonicalHash, structData);

    // Also register the lookup hash and symbol name hash as aliases if
    // different
    if (result != 0) {
      u32 structIdx = structTypeMap[canonicalHash];
      if (typeNameHash != canonicalHash) {
        structTypeMap[typeNameHash] = structIdx;
      }
      if (typeSym->name.nameHash != canonicalHash) {
        structTypeMap[typeSym->name.nameHash] = structIdx;
      }
    }

    return result;
  }

  u32 RegisterStructTypeFromSymbol(u32 typeNameHash,
                                   const StructData &structData) {
    if (program.structTypeCount >= program.structTypeCapacity) {
      return 0; // Out of capacity
    }

    u32 structIdx = program.structTypeCount++;
    structTypeMap[typeNameHash] = structIdx;

    IRProgram::StructTypeInfo &info = program.structTypes[structIdx];
    info.nameHash = typeNameHash;
    info.fieldCount = structData.fields.count;
    info.fieldOffset = 0; // Will be set below

    // Calculate field offset in flattened arrays
    u32 fieldOffset = 0;
    for (u32 i = 0; i < structIdx; i++) {
      fieldOffset += program.structTypes[i].fieldCount;
    }
    info.fieldOffset = fieldOffset;

    // Calculate total size and copy field info (std140 layout)
    u32 currentOffset = 0;
    for (u32 i = 0; i < structData.fields.count; i++) {
      if (fieldOffset + i >= program.structFieldCapacity)
        break;

      const StructData::Field &field = structData.fields[i];
      program.structFieldTypes[fieldOffset + i] =
          static_cast<u16>(field.type.coreType);
      program.structFieldNameHashes[fieldOffset + i] = field.name.nameHash;
      program.structFieldTypeHashes[fieldOffset + i] =
          (field.type.coreType == CoreType::CUSTOM) ? field.type.customTypeHash
                                                    : 0;
      program.structFieldArraySizes[fieldOffset + i] = field.arraySize;

      // std140 alignment rules - for arrays, account for array length
      u32 fieldSize = GetTypeSize(field.type.coreType);
      u32 alignment = GetTypeAlignment(field.type.coreType);

      // For CUSTOM types (nested structs), ensure nested struct is registered
      // and get its size
      if (field.type.coreType == CoreType::CUSTOM &&
          field.type.customTypeHash != 0) {
        // First, ensure the nested struct is registered (recursively)
        auto it = structTypeMap.find(field.type.customTypeHash);
        if (it == structTypeMap.end()) {
          // Nested struct not registered yet - register it now
          LookupOrRegisterStructType(field.type.customTypeHash);
          it = structTypeMap.find(field.type.customTypeHash);
        }

        if (it != structTypeMap.end()) {
          u32 nestedIdx = it->second;
          fieldSize = program.structTypes[nestedIdx].totalSize;
          // Struct alignment is max of its member alignments, minimum 16 for
          // std140
          alignment = 16;
        }
      }

      currentOffset = (currentOffset + alignment - 1) & ~(alignment - 1);
      program.structFieldByteOffsets[fieldOffset + i] = currentOffset;
      if (field.arraySize > 0) {
        // std430 layout: scalars and vec2 use natural stride, vec3+ rounds to
        // 16
        u32 arrayStride;
        if (fieldSize <= 8) {
          arrayStride = fieldSize; // Natural size for scalars (4) and vec2 (8)
        } else {
          arrayStride =
              (fieldSize + 15) & ~15; // Round to 16 for vec3, vec4, mat
        }
        currentOffset += arrayStride * field.arraySize;
      } else {
        currentOffset += fieldSize;
      }
    }

    info.totalSize = currentOffset;
    return typeNameHash;
  }

  u32 RegisterStructTypeFromData(u32 typeNameHash, StructData *structData) {
    if (!structData || program.structTypeCount >= program.structTypeCapacity) {
      return 0;
    }

    u32 structIdx = program.structTypeCount++;
    structTypeMap[typeNameHash] = structIdx;

    IRProgram::StructTypeInfo &info = program.structTypes[structIdx];
    info.nameHash = typeNameHash;
    info.fieldCount = structData->fields.count;

    // Calculate field offset in flattened arrays
    u32 fieldOffset = 0;
    for (u32 i = 0; i < structIdx; i++) {
      fieldOffset += program.structTypes[i].fieldCount;
    }
    info.fieldOffset = fieldOffset;

    // Calculate total size and copy field info
    u32 currentOffset = 0;
    for (u32 i = 0; i < structData->fields.count; i++) {
      if (fieldOffset + i >= program.structFieldCapacity)
        break;

      const StructData::Field &field = structData->fields[i];
      program.structFieldTypes[fieldOffset + i] =
          static_cast<u16>(field.type.coreType);
      program.structFieldNameHashes[fieldOffset + i] = field.name.nameHash;
      program.structFieldTypeHashes[fieldOffset + i] =
          (field.type.coreType == CoreType::CUSTOM) ? field.type.customTypeHash
                                                    : 0;
      program.structFieldArraySizes[fieldOffset + i] = field.arraySize;

      u32 fieldSize = GetTypeSize(field.type.coreType);
      u32 alignment = GetTypeAlignment(field.type.coreType);

      // For CUSTOM types (nested structs), ensure nested struct is registered
      // and get its size
      if (field.type.coreType == CoreType::CUSTOM &&
          field.type.customTypeHash != 0) {
        // First, ensure the nested struct is registered (recursively)
        auto it = structTypeMap.find(field.type.customTypeHash);
        if (it == structTypeMap.end()) {
          // Nested struct not registered yet - register it now
          LookupOrRegisterStructType(field.type.customTypeHash);
          it = structTypeMap.find(field.type.customTypeHash);
        }

        if (it != structTypeMap.end()) {
          u32 nestedIdx = it->second;
          fieldSize = program.structTypes[nestedIdx].totalSize;
          // Struct alignment is max of its member alignments, minimum 16 for
          // std140
          alignment = 16;
        }
      }

      currentOffset = (currentOffset + alignment - 1) & ~(alignment - 1);
      program.structFieldByteOffsets[fieldOffset + i] = currentOffset;
      if (field.arraySize > 0) {
        // std430 layout: scalars and vec2 use natural stride, vec3+ rounds to
        // 16
        u32 arrayStride;
        if (fieldSize <= 8) {
          arrayStride = fieldSize; // Natural size for scalars (4) and vec2 (8)
        } else {
          arrayStride =
              (fieldSize + 15) & ~15; // Round to 16 for vec3, vec4, mat
        }
        currentOffset += arrayStride * field.arraySize;
      } else {
        currentOffset += fieldSize;
      }
    }

    info.totalSize = currentOffset;
    return typeNameHash;
  }

  // Register an enum sum type as a struct in the IR
  // Layout: { int tag; <fields for largest variant> }
  u32 RegisterEnumAsStructType(u32 enumNameHash, u32 enumIndex) {
    if (!symbols || enumIndex >= symbols->enums.count) {
      return 0;
    }
    if (program.structTypeCount >= program.structTypeCapacity) {
      return 0;
    }

    const EnumData &enumData = symbols->enums[enumIndex];

    // Calculate max total size across all variants
    // Each variant can have different associated types
    u32 maxScalarCount = 0; // Max number of scalar floats needed
    for (u32 v = 0; v < enumData.variants.count; v++) {
      const EnumData::Variant &variant = enumData.variants[v];
      u32 scalarCount = 0;
      for (u32 t = 0; t < variant.associatedTypes.count; t++) {
        CoreType assocType = variant.associatedTypes[t];
        // Count scalars for this type
        switch (assocType) {
        case CoreType::FLOAT:
        case CoreType::INT:
        case CoreType::UINT:
          scalarCount += 1;
          break;
        case CoreType::FLOAT2:
        case CoreType::INT2:
        case CoreType::UINT2:
          scalarCount += 2;
          break;
        case CoreType::FLOAT3:
        case CoreType::INT3:
        case CoreType::UINT3:
          scalarCount += 3;
          break;
        case CoreType::FLOAT4:
        case CoreType::INT4:
        case CoreType::UINT4:
          scalarCount += 4;
          break;
        default:
          scalarCount += 1; // Fallback
          break;
        }
      }
      if (scalarCount > maxScalarCount) {
        maxScalarCount = scalarCount;
      }
    }

    // Create struct with: tag (int) + maxScalarCount floats
    u32 totalFields = 1 + maxScalarCount; // tag + payload fields

    u32 structIdx = program.structTypeCount++;
    structTypeMap[enumNameHash] = structIdx;

    IRProgram::StructTypeInfo &info = program.structTypes[structIdx];
    info.nameHash = enumNameHash;
    info.fieldCount = totalFields;

    // Calculate field offset in flattened arrays
    u32 fieldOffset = 0;
    for (u32 i = 0; i < structIdx; i++) {
      fieldOffset += program.structTypes[i].fieldCount;
    }
    info.fieldOffset = fieldOffset;

    if (fieldOffset + totalFields > program.structFieldCapacity) {
      program.structTypeCount--; // Rollback
      return 0;
    }

    // Field 0: tag (int)
    u32 currentOffset = 0;
    program.structFieldTypes[fieldOffset] = static_cast<u16>(CoreType::INT);
    program.structFieldNameHashes[fieldOffset] = Utils::HashStr("tag");
    program.structFieldTypeHashes[fieldOffset] = 0;
    program.structFieldArraySizes[fieldOffset] = 0;
    program.structFieldByteOffsets[fieldOffset] = currentOffset;
    currentOffset += 4; // int is 4 bytes

    // Fields 1..N: payload floats
    for (u32 i = 0; i < maxScalarCount; i++) {
      u32 fIdx = fieldOffset + 1 + i;
      program.structFieldTypes[fIdx] = static_cast<u16>(CoreType::FLOAT);
      // Generate field name like "f0", "f1", etc.
      char fieldName[16];
      snprintf(fieldName, sizeof(fieldName), "f%u", i);
      program.structFieldNameHashes[fIdx] = Utils::HashStr(fieldName);
      program.structFieldTypeHashes[fIdx] = 0;
      program.structFieldArraySizes[fIdx] = 0;
      // std140 alignment for float is 4
      currentOffset = (currentOffset + 3) & ~3;
      program.structFieldByteOffsets[fIdx] = currentOffset;
      currentOffset += 4;
    }

    info.totalSize = currentOffset;
    return enumNameHash;
  }

  // Infer resource type from naming conventions
  // This is used when type information isn't explicitly available
  CoreType InferResourceType(u32 nameHash) {
    // Common matrix resource name hashes
    // These are computed from Utils::HashStr at compile time for known names
    static const u32 HASH_modelMatrix = Utils::HashStr("modelMatrix");
    static const u32 HASH_viewMatrix = Utils::HashStr("viewMatrix");
    static const u32 HASH_projMatrix = Utils::HashStr("projMatrix");
    static const u32 HASH_projectionMatrix = Utils::HashStr("projectionMatrix");
    static const u32 HASH_viewProjectionMatrix =
        Utils::HashStr("viewProjectionMatrix");
    static const u32 HASH_viewProjMatrix = Utils::HashStr("viewProjMatrix");
    static const u32 HASH_modelViewMatrix = Utils::HashStr("modelViewMatrix");
    static const u32 HASH_modelViewProjectionMatrix =
        Utils::HashStr("modelViewProjectionMatrix");
    static const u32 HASH_mvpMatrix = Utils::HashStr("mvpMatrix");
    static const u32 HASH_normalMatrix = Utils::HashStr("normalMatrix");
    static const u32 HASH_inverseViewMatrix =
        Utils::HashStr("inverseViewMatrix");
    static const u32 HASH_inverseProjMatrix =
        Utils::HashStr("inverseProjMatrix");
    static const u32 HASH_lightViewProjMatrix =
        Utils::HashStr("lightViewProjMatrix");
    static const u32 HASH_previousModelMatrix =
        Utils::HashStr("previousModelMatrix");
    static const u32 HASH_previousViewProjMatrix =
        Utils::HashStr("previousViewProjMatrix");
    static const u32 HASH_boneMatrices = Utils::HashStr("boneMatrices");

    // Check for known matrix names
    if (nameHash == HASH_modelMatrix || nameHash == HASH_viewMatrix ||
        nameHash == HASH_projMatrix || nameHash == HASH_projectionMatrix ||
        nameHash == HASH_viewProjectionMatrix ||
        nameHash == HASH_viewProjMatrix || nameHash == HASH_modelViewMatrix ||
        nameHash == HASH_modelViewProjectionMatrix ||
        nameHash == HASH_mvpMatrix || nameHash == HASH_normalMatrix ||
        nameHash == HASH_inverseViewMatrix ||
        nameHash == HASH_inverseProjMatrix ||
        nameHash == HASH_lightViewProjMatrix ||
        nameHash == HASH_previousModelMatrix ||
        nameHash == HASH_previousViewProjMatrix ||
        nameHash == HASH_boneMatrices) {
      return CoreType::MAT4;
    }

    // Common vector names
    static const u32 HASH_cameraPosition = Utils::HashStr("cameraPosition");
    static const u32 HASH_lightPosition = Utils::HashStr("lightPosition");
    static const u32 HASH_lightDirection = Utils::HashStr("lightDirection");
    static const u32 HASH_lightColor = Utils::HashStr("lightColor");
    static const u32 HASH_ambientColor = Utils::HashStr("ambientColor");

    if (nameHash == HASH_cameraPosition || nameHash == HASH_lightPosition ||
        nameHash == HASH_lightDirection) {
      return CoreType::FLOAT3;
    }
    if (nameHash == HASH_lightColor || nameHash == HASH_ambientColor) {
      return CoreType::FLOAT4;
    }

    // Default to float4 for unknown buffer resources
    // The proper fix is to infer type from usage context or require explicit
    // type declarations in the render config
    return CoreType::FLOAT4;
  }

  // std140 type sizes
  u32 GetTypeSize(CoreType type) {
    switch (type) {
    case CoreType::FLOAT:
      return 4;
    case CoreType::INT:
      return 4;
    case CoreType::UINT:
      return 4;
    case CoreType::BOOL:
      return 4;
    case CoreType::FLOAT2:
      return 8;
    case CoreType::FLOAT3:
      return 12;
    case CoreType::FLOAT4:
      return 16;
    case CoreType::INT2:
      return 8;
    case CoreType::INT3:
      return 12;
    case CoreType::INT4:
      return 16;
    case CoreType::UINT2:
      return 8;
    case CoreType::UINT3:
      return 12;
    case CoreType::UINT4:
      return 16;
    case CoreType::MAT2:
      return 32; // 2 x float4
    case CoreType::MAT3:
      return 48; // 3 x float4
    case CoreType::MAT4:
      return 64; // 4 x float4
    default:
      return 4;
    }
  }

  // std140 alignment rules
  u32 GetTypeAlignment(CoreType type) {
    switch (type) {
    case CoreType::FLOAT:
      return 4;
    case CoreType::INT:
      return 4;
    case CoreType::UINT:
      return 4;
    case CoreType::BOOL:
      return 4;
    case CoreType::FLOAT2:
      return 8;
    case CoreType::FLOAT3:
      return 16; // vec3 aligned to vec4
    case CoreType::FLOAT4:
      return 16;
    case CoreType::INT2:
      return 8;
    case CoreType::INT3:
      return 16;
    case CoreType::INT4:
      return 16;
    case CoreType::UINT2:
      return 8;
    case CoreType::UINT3:
      return 16;
    case CoreType::UINT4:
      return 16;
    case CoreType::MAT2:
      return 16;
    case CoreType::MAT3:
      return 16;
    case CoreType::MAT4:
      return 16;
    default:
      return 4;
    }
  }

  void LowerAssignment(NodeRef ref) {
    const AssignmentData &assign = ast->GetAssignment(ref);
    u16 valueReg = LowerExpression(assign.value);

    NodeRef target = assign.target;

    if (target.Type() == ASTNodeType::IDENTIFIER) {
      const IdentifierData &ident = ast->GetIdentifier(target);
      u16 varReg = GetOrAllocateVariable(ident.name.nameHash);
      builder.EmitInstruction(OP_STORE_REG, varReg, valueReg);
      // Propagate pointer storage info if the value is a pointer
      if (valueReg < MAX_REGISTERS && (program.registerStorageInfo[valueReg] &
                                       IR::IRProgram::STORAGE_IS_PTR)) {
        program.registerStorageInfo[varReg] =
            program.registerStorageInfo[valueReg];
      }
      initializedVariables.insert(ident.name.nameHash);
    } else if (target.Type() == ASTNodeType::MEMBER_ACCESS) {
      const MemberAccessData &access = ast->GetMemberAccess(target);

      if (access.object.Type() == ASTNodeType::MEMBER_ACCESS) {
        const MemberAccessData &baseAccess =
            ast->GetMemberAccess(access.object);
        if (baseAccess.object.Type() == ASTNodeType::IDENTIFIER) {
          const IdentifierData &baseObj = ast->GetIdentifier(baseAccess.object);
          if (baseObj.identifierKind == SpecialIdentifier::OUTPUT) {
            u32 outputNameHash = baseAccess.member.nameHash;
            CoreType outputType = ResolveOutputTypeForLoad(outputNameHash);

            const char *nameStr = nullptr;
            char nameBuf[32] = {0};
            if (sourceBase && !baseAccess.member.isHashOnly()) {
              auto sv = baseAccess.member.view(sourceBase);
              size_t len = sv.length() < 31 ? sv.length() : 31;
              memcpy(nameBuf, sv.data(), len);
              nameBuf[len] = '\0';
              nameStr = nameBuf;
            }
            u32 slot =
                ResolveOutputSlotForStore(outputNameHash, outputType, nameStr);

            u16 outputReg = AllocateRegister();
            SetRegisterType(outputReg, outputType);
            builder.EmitInstruction(OP_LOAD_OUTPUT, outputReg, slot);
            program.metadata[builder.currentInstruction - 1] = outputNameHash;

            u32 memberHash = access.member.nameHash;
            u32 componentIndex = 0xFFFFFFFF;
            if (memberHash == Utils::HashStr("x") ||
                memberHash == Utils::HashStr("r"))
              componentIndex = 0;
            else if (memberHash == Utils::HashStr("y") ||
                     memberHash == Utils::HashStr("g"))
              componentIndex = 1;
            else if (memberHash == Utils::HashStr("z") ||
                     memberHash == Utils::HashStr("b"))
              componentIndex = 2;
            else if (memberHash == Utils::HashStr("w") ||
                     memberHash == Utils::HashStr("a"))
              componentIndex = 3;

            if (componentIndex != 0xFFFFFFFF) {
              u32 numComponents = GetVectorDimension(outputType);
              if (numComponents < 2) {
                builder.EmitInstruction(OP_STORE_OUTPUT, valueReg, slot);
                program.metadata[builder.currentInstruction - 1] =
                    outputNameHash;
                return;
              }

              u16 newVecReg = AllocateRegister();
              SetRegisterType(newVecReg, outputType);
              builder.EmitInstruction(OP_VEC_INSERT, newVecReg, outputReg,
                                      componentIndex, valueReg);
              builder.EmitInstruction(OP_STORE_OUTPUT, newVecReg, slot);
              program.metadata[builder.currentInstruction - 1] = outputNameHash;
              return;
            }

            const char *swizzlePatterns[] = {
                "xy",  "xz",  "xw",   "yz",  "yw",  "zw",  "xyz", "xyw",
                "xzw", "yzw", "xyzw", "rg",  "rb",  "ra",  "gb",  "ga",
                "ba",  "rgb", "rga",  "rba", "gba", "rgba"};
            const u8 swizzleIndices[][4] = {
                {0, 1, 255, 255}, {0, 255, 1, 255}, {0, 255, 255, 1},
                {255, 0, 1, 255}, {255, 0, 255, 1}, {255, 255, 0, 1},
                {0, 1, 2, 255},   {0, 1, 255, 2},   {0, 255, 1, 2},
                {255, 0, 1, 2},   {0, 1, 2, 3},     {0, 1, 255, 255},
                {0, 255, 1, 255}, {0, 255, 255, 1}, {255, 0, 1, 255},
                {255, 0, 255, 1}, {255, 255, 0, 1}, {0, 1, 2, 255},
                {0, 1, 255, 2},   {0, 255, 1, 2},   {255, 0, 1, 2},
                {0, 1, 2, 3}};

            for (u32 i = 0;
                 i < sizeof(swizzlePatterns) / sizeof(swizzlePatterns[0]);
                 i++) {
              if (memberHash == Utils::HashStr(swizzlePatterns[i])) {
                u32 numComponents = GetVectorDimension(outputType);
                if (numComponents < 2) {
                  builder.EmitInstruction(OP_STORE_OUTPUT, valueReg, slot);
                  program.metadata[builder.currentInstruction - 1] =
                      outputNameHash;
                  return;
                }

                u16 newVecReg = AllocateRegister();
                SetRegisterType(newVecReg, outputType);

                u32 shuffleMask = 0;
                for (u32 j = 0; j < numComponents; j++) {
                  u8 srcIdx = (j < 4) ? swizzleIndices[i][j] : 255;
                  if (srcIdx != 255) {
                    shuffleMask |= ((srcIdx + numComponents) & 0xF) << (j * 4);
                  } else {
                    shuffleMask |= (j & 0xF) << (j * 4);
                  }
                }

                builder.EmitInstruction(OP_VEC_SHUFFLE, newVecReg, outputReg,
                                        valueReg);
                program.metadata[builder.currentInstruction - 1] = shuffleMask;
                builder.EmitInstruction(OP_STORE_OUTPUT, newVecReg, slot);
                program.metadata[builder.currentInstruction - 1] =
                    outputNameHash;
                return;
              }
            }
          }
        }
      }

      // Check if object is 'output'
      if (access.object.Type() == ASTNodeType::IDENTIFIER) {
        const IdentifierData &obj = ast->GetIdentifier(access.object);
        if (obj.identifierKind == SpecialIdentifier::OUTPUT) {
          u32 nameHash = access.member.nameHash;
          CoreType valueType = GetRegisterType(valueReg);

          // Extract name string for GLES output
          const char *nameStr = nullptr;
          char nameBuf[32] = {0};
          if (sourceBase && !access.member.isHashOnly()) {
            auto sv = access.member.view(sourceBase);
            size_t len = sv.length() < 31 ? sv.length() : 31;
            memcpy(nameBuf, sv.data(), len);
            nameBuf[len] = '\0';
            nameStr = nameBuf;
          }
          u32 slot = ResolveOutputSlotForStore(nameHash, valueType, nameStr);

          // Emit OP_STORE_OUTPUT with slot in operand
          builder.EmitInstruction(OP_STORE_OUTPUT, valueReg, slot);
          program.metadata[builder.currentInstruction - 1] =
              nameHash; // Keep name for debugging
        } else if (obj.identifierKind == SpecialIdentifier::NONE) {
          // Struct member assignment: s.field = value
          u16 objReg = GetOrAllocateVariable(obj.name.nameHash);

          // Look up the variable's struct type
          u32 structTypeHash = 0;
          auto varIt = variableStructTypes.find(obj.name.nameHash);
          if (varIt != variableStructTypes.end()) {
            structTypeHash = varIt->second;
          } else if (objReg < MAX_REGISTERS) {
            structTypeHash = program.registerStructTypes[objReg];
          }

          if (structTypeHash != 0) {
            if (initializedVariables.find(obj.name.nameHash) ==
                initializedVariables.end()) {
              u16 zeroStruct = EmitZeroStruct(structTypeHash);
              builder.EmitInstruction(OP_STORE_REG, objReg, zeroStruct);
              initializedVariables.insert(obj.name.nameHash);
            }

            // Find the field index
            u32 fieldIndex = 0xFFFFFFFF;
            auto structIt = structTypeMap.find(structTypeHash);
            if (structIt != structTypeMap.end()) {
              u32 structIdx = structIt->second;
              const IRProgram::StructTypeInfo &info =
                  program.structTypes[structIdx];
              for (u32 i = 0; i < info.fieldCount; i++) {
                if (program.structFieldNameHashes[info.fieldOffset + i] ==
                    access.member.nameHash) {
                  fieldIndex = i;
                  break;
                }
              }
            }

            if (fieldIndex != 0xFFFFFFFF) {
              // Emit struct insert: dest = struct with field=value
              // We modify the struct in place by creating a new struct value
              u16 newStructReg = AllocateRegister();
              SetRegisterType(newStructReg, CoreType::CUSTOM);
              program.registerStructTypes[newStructReg] = structTypeHash;
              builder.EmitInstruction(OP_STRUCT_INSERT, newStructReg, objReg,
                                      fieldIndex, valueReg);
              // Store full struct type hash in metadata (field index is in
              // operand 1)
              program.metadata[builder.currentInstruction - 1] = structTypeHash;

              // Copy the new struct value back to the variable
              builder.EmitInstruction(OP_STORE_REG, objReg, newStructReg);

              // Update struct type for the register
              program.registerStructTypes[newStructReg] = structTypeHash;
              return;
            }
          }

          // Check for vector component assignment (pos.x = value, etc.)
          u32 componentIndex = 0xFFFFFFFF;
          u32 memberHash = access.member.nameHash;
          if (memberHash == Utils::HashStr("x") ||
              memberHash == Utils::HashStr("r"))
            componentIndex = 0;
          else if (memberHash == Utils::HashStr("y") ||
                   memberHash == Utils::HashStr("g"))
            componentIndex = 1;
          else if (memberHash == Utils::HashStr("z") ||
                   memberHash == Utils::HashStr("b"))
            componentIndex = 2;
          else if (memberHash == Utils::HashStr("w") ||
                   memberHash == Utils::HashStr("a"))
            componentIndex = 3;

          if (componentIndex != 0xFFFFFFFF) {
            // Vector component assignment: vec.x = value
            // We need to insert the component into the vector and store back
            CoreType varType = GetRegisterType(objReg);

            // Create a new vector with the component inserted
            u16 newVecReg = AllocateRegister();
            SetRegisterType(newVecReg, varType);

            // OP_VEC_INSERT: dest = insert(src_vec, component_index, value)
            builder.EmitInstruction(OP_VEC_INSERT, newVecReg, objReg,
                                    componentIndex, valueReg);

            // Copy the new vector back to the variable
            builder.EmitInstruction(OP_STORE_REG, objReg, newVecReg);
            return;
          }

          // Multi-component swizzle assignment. Parse the member name
          // character-by-character into output-position -> source-index
          // mapping, then build a single OpVectorShuffle picking from
          // value for written positions and from the original otherwise.
          // Handles in-order (xy, xyz), out-of-order (wz, yx, xzy) and
          // repeated components across either xyzw or rgba sets.
          if (sourceBase && !access.member.isHashOnly()) {
            auto sv = access.member.view(sourceBase);
            u32 swizzleLen = static_cast<u32>(sv.size());
            if (swizzleLen >= 2 && swizzleLen <= 4) {
              u8 targetIdx[4] = {0, 0, 0, 0};
              bool valid = true;
              bool seenXyzw = false;
              bool seenRgba = false;
              for (u32 i = 0; i < swizzleLen; i++) {
                char c = sv[i];
                switch (c) {
                case 'x': targetIdx[i] = 0; seenXyzw = true; break;
                case 'y': targetIdx[i] = 1; seenXyzw = true; break;
                case 'z': targetIdx[i] = 2; seenXyzw = true; break;
                case 'w': targetIdx[i] = 3; seenXyzw = true; break;
                case 'r': targetIdx[i] = 0; seenRgba = true; break;
                case 'g': targetIdx[i] = 1; seenRgba = true; break;
                case 'b': targetIdx[i] = 2; seenRgba = true; break;
                case 'a': targetIdx[i] = 3; seenRgba = true; break;
                default: valid = false; break;
                }
                if (!valid) break;
              }

              if (valid && !(seenXyzw && seenRgba)) {
                CoreType varType = GetRegisterType(objReg);
                u32 numComponents = GetVectorDimension(varType);
                if (numComponents < 2) {
                  builder.EmitInstruction(OP_STORE_REG, objReg, valueReg);
                  return;
                }

                // inverse map: for each original-vec position j,
                // which value-vec component (if any) overwrites it?
                u8 fromValue[4] = {255, 255, 255, 255};
                for (u32 i = 0; i < swizzleLen; i++) {
                  u8 origSlot = targetIdx[i];
                  if (origSlot < numComponents) {
                    fromValue[origSlot] = static_cast<u8>(i);
                  }
                }

                u32 shuffleMask = 0;
                for (u32 j = 0; j < numComponents; j++) {
                  if (fromValue[j] != 255) {
                    shuffleMask |=
                        ((fromValue[j] + numComponents) & 0xF) << (j * 4);
                  } else {
                    shuffleMask |= (j & 0xF) << (j * 4);
                  }
                }

                u16 newVecReg = AllocateRegister();
                SetRegisterType(newVecReg, varType);
                builder.EmitInstruction(OP_VEC_SHUFFLE, newVecReg, objReg,
                                        valueReg);
                program.metadata[builder.currentInstruction - 1] = shuffleMask;

                builder.EmitInstruction(OP_STORE_REG, objReg, newVecReg);
                return;
              }
            }
          }

          // Fallback: unknown struct/field - just store directly
          builder.EmitInstruction(OP_STORE_REG, objReg, valueReg);
        }
      } else if (access.object.Type() == ASTNodeType::ARRAY_ACCESS) {
        // Struct array element field assignment: arr[i].field = value
        // e.g., lights[0].position = float3(10, 10, 10)
        const ArrayAccessData &arrAccess = ast->GetArrayAccess(access.object);
        u16 arrayReg = LowerExpression(arrAccess.array);
        u16 indexReg = LowerExpression(arrAccess.index);

        // Get the struct type for the array elements
        u32 structTypeHash = 0;
        if (arrayReg < MAX_REGISTERS) {
          structTypeHash = program.registerStructTypes[arrayReg];
        }
        // Check localArrayStructTypes if registerStructTypes doesn't have it
        if (structTypeHash == 0 && program.registerStorageInfo &&
            arrayReg < program.registerCount &&
            (program.registerStorageInfo[arrayReg] &
             IR::IRProgram::STORAGE_IS_LOCAL_ARRAY)) {
          u32 arrayIdx = (program.registerStorageInfo[arrayReg] >>
                          IR::IRProgram::STORAGE_BINDING_SHIFT);
          if (arrayIdx < program.localArrayCount &&
              program.localArrayStructTypes) {
            structTypeHash = program.localArrayStructTypes[arrayIdx];
          }
        }

        if (structTypeHash != 0) {
          // Find the field index
          u32 fieldIndex = 0xFFFFFFFF;
          auto structIt = structTypeMap.find(structTypeHash);
          if (structIt != structTypeMap.end()) {
            u32 structIdx = structIt->second;
            const IRProgram::StructTypeInfo &info =
                program.structTypes[structIdx];
            for (u32 i = 0; i < info.fieldCount; i++) {
              if (program.structFieldNameHashes[info.fieldOffset + i] ==
                  access.member.nameHash) {
                fieldIndex = i;
                break;
              }
            }
          }

          if (fieldIndex != 0xFFFFFFFF) {
            // 1. Load the struct element from the array
            u16 elemReg = AllocateRegister();
            SetRegisterType(elemReg, CoreType::CUSTOM);
            program.registerStructTypes[elemReg] = structTypeHash;
            builder.EmitInstruction(OP_ARRAY_LOAD, elemReg, arrayReg, indexReg);

            // 2. Insert the new field value into the struct
            u16 newStructReg = AllocateRegister();
            SetRegisterType(newStructReg, CoreType::CUSTOM);
            program.registerStructTypes[newStructReg] = structTypeHash;
            builder.EmitInstruction(OP_STRUCT_INSERT, newStructReg, elemReg,
                                    fieldIndex, valueReg);
            program.metadata[builder.currentInstruction - 1] = structTypeHash;

            // 3. Store the modified struct back to the array
            builder.EmitInstruction(OP_ARRAY_STORE, arrayReg, indexReg,
                                    newStructReg);
            return;
          }
        }
      }
    } else if (target.Type() == ASTNodeType::ARRAY_ACCESS) {
      const ArrayAccessData &arrAccess = ast->GetArrayAccess(target);
      u16 baseReg = LowerExpression(arrAccess.array);
      u16 indexReg = LowerExpression(arrAccess.index);
      // `v[i] = x` where `v` is a scalar vector local (not `v` declared as
      // `float3[N]`) must go through OpVectorInsertDynamic —
      // OpCompositeInsert can't take a runtime literal index. Distinguish
      // the vector-component case from the array-of-vectors case by
      // comparing the value's type against the base's type:
      //   - vec-component write: base is a vector, value is the scalar
      //     component type (e.g., base=float3, value=float)
      //   - array-of-vectors element write: base tracked as FLOAT3 but
      //     the value is the whole element (float3)
      if (baseReg < MAX_REGISTERS) {
        CoreType baseType =
            static_cast<CoreType>(program.registerTypes[baseReg]);
        CoreType valueType = GetRegisterType(valueReg);
        TypeMask btm = mask(baseType);
        bool baseIsVec = btm & (TypeMasks::FLOAT_VECTORS | TypeMasks::INT_VECTORS |
                                 TypeMasks::UINT_VECTORS | TypeMasks::BOOL_VECTORS);
        bool valueIsComponent =
            (baseType == CoreType::FLOAT2 || baseType == CoreType::FLOAT3 ||
             baseType == CoreType::FLOAT4) ? (valueType == CoreType::FLOAT) :
            (baseType == CoreType::INT2   || baseType == CoreType::INT3 ||
             baseType == CoreType::INT4)  ? (valueType == CoreType::INT) :
            (baseType == CoreType::UINT2  || baseType == CoreType::UINT3 ||
             baseType == CoreType::UINT4) ? (valueType == CoreType::UINT) :
            (baseType == CoreType::BOOL2  || baseType == CoreType::BOOL3 ||
             baseType == CoreType::BOOL4) ? (valueType == CoreType::BOOL) : false;
        if (baseIsVec && valueIsComponent) {
          u16 newVecReg = AllocateRegister();
          SetRegisterType(newVecReg, baseType);
          builder.EmitInstruction(OP_VEC_INSERT_DYNAMIC, newVecReg, baseReg,
                                  valueReg, indexReg);
          builder.EmitInstruction(OP_STORE_REG, baseReg, newVecReg);
          return;
        }

        // Matrix-column write: `M[i] = col_vec` where M is a matrix-typed
        // local and i is a compile-time integer. Emit OP_VEC_INSERT
        // (OpCompositeInsert) so the matrix type is preserved — the
        // generic ARRAY_STORE fallback aliases baseReg to value, turning
        // the matrix into a column-vec and breaking subsequent reads.
        //
        // Only fire when the LHS is `identifier[i]` — a simple local
        // matrix variable. If the LHS is `struct.field[i]` (array-of-
        // matrix field), the CoreType is wrong for matrix semantics
        // (we have no "array of mat4" CoreType, so the register may
        // carry mat4 spuriously). Fall through to generic ARRAY_STORE
        // so STRUCT_INSERT can reconstitute the struct correctly.
        bool baseIsLocalMatrix =
            arrAccess.array.Type() == ASTNodeType::IDENTIFIER;
        bool baseIsMat =
            baseType == CoreType::MAT2 || baseType == CoreType::MAT3 ||
            baseType == CoreType::MAT4;
        CoreType expectedCol = (baseType == CoreType::MAT4)   ? CoreType::FLOAT4
                             : (baseType == CoreType::MAT3)   ? CoreType::FLOAT3
                             : (baseType == CoreType::MAT2)   ? CoreType::FLOAT2
                                                              : CoreType::INVALID;
        bool indexIsConst = (indexReg & 0x4000) != 0;
        if (baseIsLocalMatrix && baseIsMat && indexIsConst &&
            valueType == expectedCol) {
          u16 slot = indexReg & 0x3FFF;
          u16 literalIdx = 0;
          if (slot < program.intCount) {
            literalIdx = static_cast<u16>(program.intConstants[slot]);
          }
          u16 newMatReg = AllocateRegister();
          SetRegisterType(newMatReg, baseType);
          builder.EmitInstruction(OP_VEC_INSERT, newMatReg, baseReg,
                                  literalIdx, valueReg);
          builder.EmitInstruction(OP_STORE_REG, baseReg, newMatReg);
          return;
        }
      }
      if (baseReg < MAX_REGISTERS) {
        CoreType baseType =
            static_cast<CoreType>(program.registerTypes[baseReg]);
        CoreType valueType = GetRegisterType(valueReg);
        if (valueType != CoreType::INVALID && valueType != CoreType::VOID) {
          if (baseType == CoreType::INVALID || baseType == CoreType::VOID ||
              baseType != valueType) {
            SetRegisterType(baseReg, valueType);
          }
          if ((valueType == CoreType::CUSTOM || valueType == CoreType::ENUM) &&
              valueReg < MAX_REGISTERS) {
            u32 structHash = program.registerStructTypes[valueReg];
            if (structHash != 0) {
              program.registerStructTypes[baseReg] = structHash;
            }
          }
        }
      }
      builder.EmitInstruction(OP_ARRAY_STORE, baseReg, indexReg, valueReg);
    } else if (target.Type() == ASTNodeType::UNARY_OP) {
      // Handle dereference as lvalue: ptr^ = value
      const UnaryOpData &unaryOp = ast->GetUnaryOp(target);
      if (unaryOp.op == UnaryOpType::DEREFERENCE) {
        // Get the pointer value
        u16 ptrReg = LowerExpression(unaryOp.operand);
        // Store through the pointer
        // Format: dest=0 (no result), s0=ptr, s1=value
        builder.EmitInstruction(OP_LOCAL_STORE, 0, ptrReg, valueReg);
      }
    }
  }

  void LowerIfStatement(NodeRef ref) {
    const BlockData &block = ast->GetBlock(ref);
    if (block.statements.count < 2)
      return;

    u16 condReg = LowerExpression(block.statements[0]);

    // Emit branch - targets will be patched after we know where blocks end
    u32 branchIdx = builder.currentInstruction;
    builder.EmitInstruction(OP_BRANCH, 0, condReg, 0, 0);

    // True target is immediately after the branch
    u32 trueTarget = builder.currentInstruction;

    // Lower true branch
    LowerStatement(block.statements[1]);

    u32 jumpOverElseIdx = 0;
    if (block.statements.count > 2) {
      jumpOverElseIdx = builder.currentInstruction;
      builder.EmitInstruction(OP_JUMP, 0, 0, 0);
    }

    // False target (else block or merge point if no else)
    u32 falseTarget = builder.currentInstruction;

    bool needMergePad = true;
    if (block.statements.count > 2) {
      // Check if else block is another if statement (else-if chain)
      // We need to mark the instruction BEFORE the else for proper nesting
      u32 elseStartInst = builder.currentInstruction;

      LowerStatement(block.statements[2]);

      // If the else block was an if statement and ended at the same instruction
      // as our merge point would be, emit a NOP to separate merge points
      // This ensures nested if-else chains have distinct merge blocks
      if (block.statements[2].Type() == ASTNodeType::IF_STATEMENT) {
        // The inner if's merge is at currentInstruction
        // We need our merge to be at a DIFFERENT instruction
        // Emit a NOP that will become our distinct merge point
        needMergePad = true;
      }
    } else if (loopDepth > 0) {
      // SPIR-V structured control flow rule: A selection merge block cannot
      // be the same as a loop's continue target. When we're inside a loop
      // and the if-statement has no else clause, the merge point would
      // naturally fall into the continue block. Emit a NOP to create a
      // distinct merge block that will then branch to the continue target.
      needMergePad = true;
    }

    // Set merge point to current instruction (NOP location if we emit one)
    u32 mergePoint = builder.currentInstruction;
    if (needMergePad) {
      builder.EmitInstruction(OP_NOP, 0, 0, 0);
    }

    // Patch branch: true = enter then-block, false = else/merge
    program.SetBranchTargets(branchIdx, trueTarget, falseTarget);

    // ANNOTATE: This branch instruction is an if-header, merge at mergePoint
    program.structureInfo[branchIdx] =
        IRProgram::PackStructure(IRProgram::STRUCT_IF_HEADER, mergePoint);

    // Patch jump over else to go to merge point
    if (jumpOverElseIdx != 0) {
      program.metadata[jumpOverElseIdx] = mergePoint;
    }
  }

  void LowerReturn(NodeRef ref) {
    // Return reuses AssignmentData (target unused, value is return expr)
    const AssignmentData &ret = ast->GetAssignment(ref);
    bool inlineReturn = (inlineDepth > 0 && inlineReturnReg != 0xFFFF &&
                         inlineReturnFlagReg != 0xFFFF);
    if (!ret.value.IsNull()) {
      u16 valueReg = LowerExpression(ret.value);

      // If we're inlining a function, store to the return register instead of
      // emitting OP_RET
      if (inlineReturn) {
        builder.EmitInstruction(OP_STORE_REG, inlineReturnReg, valueReg);
        // Copy the type info to the return register
        CoreType valueType = GetRegisterType(valueReg);
        if (valueType != CoreType::INVALID) {
          SetRegisterType(inlineReturnReg, valueType);
        }
        if ((valueType == CoreType::CUSTOM || valueType == CoreType::ENUM) &&
            inlineReturnReg < MAX_REGISTERS && valueReg < MAX_REGISTERS) {
          u32 structHash = program.registerStructTypes[valueReg];
          if (structHash != 0) {
            program.registerStructTypes[inlineReturnReg] = structHash;
          }
        }
        u16 trueConst = builder.EmitConstantBool(true);
        builder.EmitInstruction(OP_STORE_REG, inlineReturnFlagReg, trueConst);
        inlineReturnCounter++;
      } else {
        builder.EmitInstruction(OP_RET, valueReg, 0);
      }
    } else {
      if (inlineReturn) {
        u16 trueConst = builder.EmitConstantBool(true);
        builder.EmitInstruction(OP_STORE_REG, inlineReturnFlagReg, trueConst);
        inlineReturnCounter++;
      } else {
        builder.EmitInstruction(OP_RET, 0, 0);
      }
    }
  }

  u16 LowerLiteral(NodeRef ref) {
    const LiteralData &lit = ast->GetLiteral(ref);
    switch (lit.value.type) {
    case LiteralValue::FLOAT:
      return builder.EmitConstant(lit.value.floatValue);
    case LiteralValue::INT:
      return EmitConstantInt(lit.value.intValue);
    case LiteralValue::UINT:
      return EmitConstantUint(lit.value.uintValue);
    case LiteralValue::BOOL:
      return builder.EmitConstantBool(lit.value.boolValue);
    default:
      return 0;
    }
  }

  u16 LowerBinaryOp(NodeRef ref) {
    const BinaryOpData &binop = ast->GetBinaryOp(ref);
    u16 left = LowerExpression(binop.left);
    u16 right = LowerExpression(binop.right);
    u16 dest = AllocateRegister();

    CoreType leftType = GetRegisterType(left);
    CoreType rightType = GetRegisterType(right);
    TypeMask leftMask = mask(leftType);
    TypeMask rightMask = mask(rightType);

    OpCode op = OP_NOP;
    CoreType resultType = leftType;

    // Handle matrix operations first (before float types, since mat4 isn't in
    // FLOAT_TYPES)
    if (leftMask & TypeMasks::MATRIX_TYPES) {
      if (binop.op == BinaryOpType::MULTIPLY) {
        if (rightMask & TypeMasks::MATRIX_TYPES) {
          // Matrix * Matrix
          op = OP_MAT_MUL;
          resultType = leftType; // mat4 * mat4 = mat4
        } else if (rightMask & TypeMasks::FLOAT_VECTORS) {
          // Matrix * Vector with dimension handling
          // For normal transformation: mat4 * float3 should return float3
          // (use upper-left 3x3 semantically, extend with w=0, multiply,
          // truncate)
          op = OP_MAT_VEC_MUL;

          u32 matDim = (leftType == CoreType::MAT4)   ? 4
                       : (leftType == CoreType::MAT3) ? 3
                                                      : 2;
          u32 vecDim = GetVectorDimension(rightType);

          if (vecDim < matDim) {
            // Vector is smaller than matrix - extend vector, multiply, truncate
            // This handles mat4 * float3 (normal transform) and mat4 * float2,
            // etc.

            // Step 1: Extend vector to match matrix dimension
            // For normals/directions, use w=0 (homogeneous direction)
            CoreType extendedType = (matDim == 4)   ? CoreType::FLOAT4
                                    : (matDim == 3) ? CoreType::FLOAT3
                                                    : CoreType::FLOAT2;
            u16 extendedVec = AllocateRegister();
            u16 zero = builder.EmitConstant(0.0f);

            // Construct extended vector: (vec.xyz, 0) for mat4*vec3, etc.
            if (vecDim == 3 && matDim == 4) {
              builder.EmitInstruction(OP_VEC_CONSTRUCT, extendedVec, right,
                                      zero);
              program.metadata[builder.currentInstruction - 1] =
                  4; // 4 components
            } else if (vecDim == 2 && matDim == 4) {
              builder.EmitInstruction(OP_VEC_CONSTRUCT, extendedVec, right,
                                      zero, zero);
              program.metadata[builder.currentInstruction - 1] = 4;
            } else if (vecDim == 2 && matDim == 3) {
              builder.EmitInstruction(OP_VEC_CONSTRUCT, extendedVec, right,
                                      zero);
              program.metadata[builder.currentInstruction - 1] = 3;
            } else {
              // Fallback: use original
              extendedVec = right;
              extendedType = rightType;
            }
            SetRegisterType(extendedVec, extendedType);

            // Step 2: Matrix * extended vector
            u16 fullResult = AllocateRegister();
            builder.EmitInstruction(OP_MAT_VEC_MUL, fullResult, left,
                                    extendedVec);
            SetRegisterType(fullResult, extendedType);

            // Step 3: Truncate result back to original vector dimension
            u32 shuffleMask = 0;
            for (u32 i = 0; i < vecDim; i++) {
              shuffleMask |= (i << (i * 8));
            }
            builder.EmitInstruction(OP_VEC_SHUFFLE, dest, fullResult,
                                    fullResult);
            program.metadata[builder.currentInstruction - 1] =
                shuffleMask | (vecDim << 24);
            SetRegisterType(dest,
                            rightType); // Result has same type as input vector
            return dest;
          } else {
            // Dimensions match or vector is larger (unusual but handle
            // gracefully) mat4 * vec4 = vec4, mat3 * vec3 = vec3, mat2 * vec2 =
            // vec2
            if (leftType == CoreType::MAT4)
              resultType = CoreType::FLOAT4;
            else if (leftType == CoreType::MAT3)
              resultType = CoreType::FLOAT3;
            else if (leftType == CoreType::MAT2)
              resultType = CoreType::FLOAT2;
          }
        } else if (rightMask & TypeMasks::SCALAR_TYPES) {
          // Matrix * Scalar
          op = OP_MAT_SCALE;
          resultType = leftType;
        }
      } else if ((binop.op == BinaryOpType::ADD ||
                  binop.op == BinaryOpType::SUBTRACT ||
                  binop.op == BinaryOpType::DIVIDE) &&
                 (rightMask & TypeMasks::MATRIX_TYPES)) {
        // Element-wise matrix-matrix arithmetic. OpFAdd / OpFSub / OpFDiv
        // only accept scalar / vector types in SPIR-V, so decompose into
        // per-column column-extract + column-add + matrix-construct. This
        // matches what glslang / dxc emit for mat + mat in source code.
        u32 cols = (leftType == CoreType::MAT4)   ? 4
                 : (leftType == CoreType::MAT3)   ? 3
                                                  : 2;
        CoreType colType = (leftType == CoreType::MAT4)   ? CoreType::FLOAT4
                         : (leftType == CoreType::MAT3)   ? CoreType::FLOAT3
                                                          : CoreType::FLOAT2;
        OpCode colOp = (binop.op == BinaryOpType::ADD)      ? OP_FADD
                     : (binop.op == BinaryOpType::SUBTRACT) ? OP_FSUB
                                                            : OP_FDIV;

        u16 colDests[4] = {0, 0, 0, 0};
        for (u32 c = 0; c < cols; c++) {
          u16 idx = EmitConstantInt(static_cast<int>(c));
          u16 colL = AllocateRegister();
          u16 colR = AllocateRegister();
          SetRegisterType(colL, colType);
          SetRegisterType(colR, colType);
          builder.EmitInstruction(OP_ARRAY_LOAD, colL, left, idx);
          builder.EmitInstruction(OP_ARRAY_LOAD, colR, right, idx);

          u16 colDest = AllocateRegister();
          SetRegisterType(colDest, colType);
          builder.EmitInstruction(colOp, colDest, colL, colR);
          colDests[c] = colDest;
        }

        builder.EmitInstruction(OP_MAT_CONSTRUCT, dest, colDests[0],
                                colDests[1], colDests[2], colDests[3]);
        program.metadata[builder.currentInstruction - 1] = cols;
        SetRegisterType(dest, leftType);
        return dest;
      } else if ((binop.op == BinaryOpType::ADD ||
                  binop.op == BinaryOpType::SUBTRACT ||
                  binop.op == BinaryOpType::DIVIDE) &&
                 (rightMask & TypeMasks::SCALAR_TYPES)) {
        // Matrix element-wise op scalar. SPIR-V has no OpMatrixPlusScalar —
        // splat the scalar into a column-sized vector and apply the op
        // per column, then reconstruct the matrix.
        u32 cols = (leftType == CoreType::MAT4)   ? 4
                 : (leftType == CoreType::MAT3)   ? 3
                                                  : 2;
        CoreType colType = (leftType == CoreType::MAT4)   ? CoreType::FLOAT4
                         : (leftType == CoreType::MAT3)   ? CoreType::FLOAT3
                                                          : CoreType::FLOAT2;
        OpCode colOp = (binop.op == BinaryOpType::ADD)      ? OP_FADD
                     : (binop.op == BinaryOpType::SUBTRACT) ? OP_FSUB
                                                            : OP_FDIV;

        u16 splat = AllocateRegister();
        builder.EmitInstruction(OP_VEC_CONSTRUCT, splat, right, right, right,
                                right);
        program.metadata[builder.currentInstruction - 1] = cols;
        SetRegisterType(splat, colType);

        u16 colDests[4] = {0, 0, 0, 0};
        for (u32 c = 0; c < cols; c++) {
          u16 idx = EmitConstantInt(static_cast<int>(c));
          u16 colL = AllocateRegister();
          SetRegisterType(colL, colType);
          builder.EmitInstruction(OP_ARRAY_LOAD, colL, left, idx);

          u16 colDest = AllocateRegister();
          SetRegisterType(colDest, colType);
          builder.EmitInstruction(colOp, colDest, colL, splat);
          colDests[c] = colDest;
        }

        builder.EmitInstruction(OP_MAT_CONSTRUCT, dest, colDests[0],
                                colDests[1], colDests[2], colDests[3]);
        program.metadata[builder.currentInstruction - 1] = cols;
        SetRegisterType(dest, leftType);
        return dest;
      }
      if (op != OP_NOP) {
        builder.EmitInstruction(op, dest, left, right);
        SetRegisterType(dest, resultType);
        return dest;
      }
    } else if ((leftMask & TypeMasks::SCALAR_TYPES) &&
               (rightMask & TypeMasks::MATRIX_TYPES)) {
      // Scalar * Matrix is commutative with the matrix-scalar path;
      // reorder so OP_MAT_SCALE sees (matrix, scalar). Without this the
      // expression falls into the scalar-arithmetic branch and emits
      // OpFMul with a scalar result on matrix operands.
      if (binop.op == BinaryOpType::MULTIPLY) {
        builder.EmitInstruction(OP_MAT_SCALE, dest, right, left);
        SetRegisterType(dest, rightType);
        return dest;
      }
      if (binop.op == BinaryOpType::ADD ||
          binop.op == BinaryOpType::SUBTRACT ||
          binop.op == BinaryOpType::DIVIDE) {
        // Scalar element-wise op matrix. Same splat-then-per-column
        // strategy as the matrix-on-left case. Operand order matters
        // for SUBTRACT / DIVIDE: emit FOP(splat, col).
        u32 cols = (rightType == CoreType::MAT4)   ? 4
                 : (rightType == CoreType::MAT3)   ? 3
                                                  : 2;
        CoreType colType = (rightType == CoreType::MAT4)   ? CoreType::FLOAT4
                         : (rightType == CoreType::MAT3)   ? CoreType::FLOAT3
                                                          : CoreType::FLOAT2;
        OpCode colOp = (binop.op == BinaryOpType::ADD)      ? OP_FADD
                     : (binop.op == BinaryOpType::SUBTRACT) ? OP_FSUB
                                                            : OP_FDIV;

        u16 splat = AllocateRegister();
        builder.EmitInstruction(OP_VEC_CONSTRUCT, splat, left, left, left,
                                left);
        program.metadata[builder.currentInstruction - 1] = cols;
        SetRegisterType(splat, colType);

        u16 colDests[4] = {0, 0, 0, 0};
        for (u32 c = 0; c < cols; c++) {
          u16 idx = EmitConstantInt(static_cast<int>(c));
          u16 colR = AllocateRegister();
          SetRegisterType(colR, colType);
          builder.EmitInstruction(OP_ARRAY_LOAD, colR, right, idx);

          u16 colDest = AllocateRegister();
          SetRegisterType(colDest, colType);
          builder.EmitInstruction(colOp, colDest, splat, colR);
          colDests[c] = colDest;
        }

        builder.EmitInstruction(OP_MAT_CONSTRUCT, dest, colDests[0],
                                colDests[1], colDests[2], colDests[3]);
        program.metadata[builder.currentInstruction - 1] = cols;
        SetRegisterType(dest, rightType);
        return dest;
      }
    } else if ((leftMask & TypeMasks::FLOAT_VECTORS) &&
               (rightMask & TypeMasks::MATRIX_TYPES)) {
      // Vector * Matrix with dimension handling
      if (binop.op == BinaryOpType::MULTIPLY) {
        op = OP_VEC_MAT_MUL;

        u32 vecDim = GetVectorDimension(leftType);
        u32 matDim = (rightType == CoreType::MAT4)   ? 4
                     : (rightType == CoreType::MAT3) ? 3
                                                     : 2;

        if (vecDim < matDim) {
          // Vector is smaller than matrix - extend, multiply, truncate
          CoreType extendedType = (matDim == 4)   ? CoreType::FLOAT4
                                  : (matDim == 3) ? CoreType::FLOAT3
                                                  : CoreType::FLOAT2;
          u16 extendedVec = AllocateRegister();
          u16 zero = builder.EmitConstant(0.0f);

          if (vecDim == 3 && matDim == 4) {
            builder.EmitInstruction(OP_VEC_CONSTRUCT, extendedVec, left, zero);
            program.metadata[builder.currentInstruction - 1] = 4;
          } else if (vecDim == 2 && matDim == 4) {
            builder.EmitInstruction(OP_VEC_CONSTRUCT, extendedVec, left, zero,
                                    zero);
            program.metadata[builder.currentInstruction - 1] = 4;
          } else if (vecDim == 2 && matDim == 3) {
            builder.EmitInstruction(OP_VEC_CONSTRUCT, extendedVec, left, zero);
            program.metadata[builder.currentInstruction - 1] = 3;
          } else {
            extendedVec = left;
            extendedType = leftType;
          }
          SetRegisterType(extendedVec, extendedType);

          // Vector * Matrix
          u16 fullResult = AllocateRegister();
          builder.EmitInstruction(OP_VEC_MAT_MUL, fullResult, extendedVec,
                                  right);
          SetRegisterType(fullResult, extendedType);

          // Truncate back to original dimension
          u32 shuffleMask = 0;
          for (u32 i = 0; i < vecDim; i++) {
            shuffleMask |= (i << (i * 8));
          }
          builder.EmitInstruction(OP_VEC_SHUFFLE, dest, fullResult, fullResult);
          program.metadata[builder.currentInstruction - 1] =
              shuffleMask | (vecDim << 24);
          SetRegisterType(dest, leftType);
          return dest;
        } else {
          // Dimensions match or vector is larger
          resultType = leftType;
          builder.EmitInstruction(op, dest, left, right);
          SetRegisterType(dest, resultType);
          return dest;
        }
      }
    }

    // Use leftMask for type dispatch (renamed from typeMask for clarity)
    TypeMask typeMask = leftMask;

    // Handle vector dimension mismatch: if both operands are vectors but with
    // different dimensions, truncate the larger vector to match the smaller
    // one. This handles cases like: vec4 - vec3 -> truncate vec4 to vec3, then
    // subtract
    u32 leftDim = GetVectorDimension(leftType);
    u32 rightDim = GetVectorDimension(rightType);

    // Handle vector-scalar ops by splatting the scalar to a matching vector.
    if ((leftDim > 1 && rightDim == 1) || (rightDim > 1 && leftDim == 1)) {
      bool leftIsVector = (leftDim > 1);
      CoreType vectorType = leftIsVector ? leftType : rightType;
      CoreType scalarType = leftIsVector ? rightType : leftType;
      CoreType vectorScalarType = CoreType::INVALID;
      switch (vectorType) {
      case CoreType::FLOAT2:
      case CoreType::FLOAT3:
      case CoreType::FLOAT4:
        vectorScalarType = CoreType::FLOAT;
        break;
      case CoreType::INT2:
      case CoreType::INT3:
      case CoreType::INT4:
        vectorScalarType = CoreType::INT;
        break;
      case CoreType::UINT2:
      case CoreType::UINT3:
      case CoreType::UINT4:
        vectorScalarType = CoreType::UINT;
        break;
      default:
        break;
      }

      if (vectorScalarType != CoreType::INVALID &&
          scalarType == vectorScalarType) {
        u16 scalarReg = leftIsVector ? right : left;
        u16 splat = AllocateRegister();
        builder.EmitInstruction(OP_VEC_CONSTRUCT, splat, scalarReg, scalarReg,
                                scalarReg, scalarReg);
        program.metadata[builder.currentInstruction - 1] =
            GetVectorDimension(vectorType);
        SetRegisterType(splat, vectorType);

        if (leftIsVector) {
          right = splat;
          rightType = vectorType;
          rightMask = mask(rightType);
        } else {
          left = splat;
          leftType = vectorType;
          leftMask = mask(leftType);
        }

        leftDim = GetVectorDimension(leftType);
        rightDim = GetVectorDimension(rightType);
        resultType = leftType;
      }
    }

    if (leftDim > 1 && rightDim > 1 && leftDim != rightDim) {
      u32 targetDim = (leftDim < rightDim) ? leftDim : rightDim;
      CoreType targetType = GetVectorTypeWithDimension(leftType, targetDim);

      if (leftDim > targetDim) {
        // Truncate left operand using VEC_SHUFFLE
        u16 truncated = AllocateRegister();
        // Emit VEC_SHUFFLE with identity indices for first targetDim components
        // The metadata encodes the shuffle mask: component indices 0, 1, 2 for
        // vec3
        u32 shuffleMask = 0;
        for (u32 i = 0; i < targetDim; i++) {
          shuffleMask |= (i << (i * 8)); // Each component index in 8 bits
        }
        builder.EmitInstruction(OP_VEC_SHUFFLE, truncated, left, left);
        program.metadata[builder.currentInstruction - 1] =
            shuffleMask | (targetDim << 24);
        SetRegisterType(truncated, targetType);
        left = truncated;
        leftType = targetType;
        leftMask = mask(leftType);
        typeMask = leftMask;
      }

      if (rightDim > targetDim) {
        // Truncate right operand using VEC_SHUFFLE
        u16 truncated = AllocateRegister();
        u32 shuffleMask = 0;
        for (u32 i = 0; i < targetDim; i++) {
          shuffleMask |= (i << (i * 8));
        }
        builder.EmitInstruction(OP_VEC_SHUFFLE, truncated, right, right);
        program.metadata[builder.currentInstruction - 1] =
            shuffleMask | (targetDim << 24);
        SetRegisterType(truncated, targetType);
        right = truncated;
        rightType = targetType;
        rightMask = mask(rightType);
      }

      resultType = targetType;
    }

    if (typeMask & TypeMasks::FLOAT_TYPES) {
      switch (binop.op) {
      case BinaryOpType::ADD:
        op = OP_FADD;
        break;
      case BinaryOpType::SUBTRACT:
        op = OP_FSUB;
        break;
      case BinaryOpType::MULTIPLY:
        op = OP_FMUL;
        break;
      case BinaryOpType::DIVIDE:
        op = OP_FDIV;
        break;
      case BinaryOpType::MODULO:
        op = OP_FMOD;
        break;
      case BinaryOpType::EQUALS:
        op = OP_FEQ;
        break;
      case BinaryOpType::NOT_EQUALS:
        op = OP_FNE;
        break;
      case BinaryOpType::LESS:
        op = OP_FLT;
        break;
      case BinaryOpType::LESS_EQUAL:
        op = OP_FLE;
        break;
      case BinaryOpType::GREATER:
        op = OP_FGT;
        break;
      case BinaryOpType::GREATER_EQUAL:
        op = OP_FGE;
        break;
      case BinaryOpType::AND:
        op = OP_AND;
        break;
      case BinaryOpType::OR:
        op = OP_OR;
        break;
      default:
        op = OP_NOP;
        break;
      }
    } else if (typeMask & TypeMasks::UINT_TYPES) {
      switch (binop.op) {
      case BinaryOpType::ADD:
        op = OP_IADD;
        break;
      case BinaryOpType::SUBTRACT:
        op = OP_ISUB;
        break;
      case BinaryOpType::MULTIPLY:
        op = OP_IMUL;
        break;
      case BinaryOpType::DIVIDE:
        op = OP_IDIV;
        break; // Note: should use UDIV
      case BinaryOpType::MODULO:
        op = OP_IMOD;
        break;
      case BinaryOpType::EQUALS:
        op = OP_IEQ;
        break;
      case BinaryOpType::NOT_EQUALS:
        op = OP_INE;
        break;
      case BinaryOpType::LESS:
        op = OP_ULT;
        break;
      case BinaryOpType::LESS_EQUAL:
        op = OP_ULE;
        break;
      case BinaryOpType::GREATER:
        op = OP_UGT;
        break;
      case BinaryOpType::GREATER_EQUAL:
        op = OP_UGE;
        break;
      case BinaryOpType::AND:
        op = OP_AND;
        break;
      case BinaryOpType::OR:
        op = OP_OR;
        break;
      case BinaryOpType::BITWISE_AND:
        op = OP_AND;
        break;
      case BinaryOpType::BITWISE_OR:
        op = OP_OR;
        break;
      case BinaryOpType::BITWISE_XOR:
        op = OP_XOR;
        break;
      case BinaryOpType::LEFT_SHIFT:
        op = OP_SHL;
        break;
      case BinaryOpType::RIGHT_SHIFT:
        op = OP_SHR;
        break;
      default:
        op = OP_NOP;
        break;
      }
    } else if (typeMask & TypeMasks::INT_TYPES) {
      switch (binop.op) {
      case BinaryOpType::ADD:
        op = OP_IADD;
        break;
      case BinaryOpType::SUBTRACT:
        op = OP_ISUB;
        break;
      case BinaryOpType::MULTIPLY:
        op = OP_IMUL;
        break;
      case BinaryOpType::DIVIDE:
        op = OP_IDIV;
        break;
      case BinaryOpType::MODULO:
        op = OP_IMOD;
        break;
      case BinaryOpType::EQUALS:
        op = OP_IEQ;
        break;
      case BinaryOpType::NOT_EQUALS:
        op = OP_INE;
        break;
      case BinaryOpType::LESS:
        op = OP_ILT;
        break;
      case BinaryOpType::LESS_EQUAL:
        op = OP_ILE;
        break;
      case BinaryOpType::GREATER:
        op = OP_IGT;
        break;
      case BinaryOpType::GREATER_EQUAL:
        op = OP_IGE;
        break;
      case BinaryOpType::AND:
        op = OP_AND;
        break;
      case BinaryOpType::OR:
        op = OP_OR;
        break;
      case BinaryOpType::BITWISE_AND:
        op = OP_AND;
        break;
      case BinaryOpType::BITWISE_OR:
        op = OP_OR;
        break;
      case BinaryOpType::BITWISE_XOR:
        op = OP_XOR;
        break;
      case BinaryOpType::LEFT_SHIFT:
        op = OP_SHL;
        break;
      case BinaryOpType::RIGHT_SHIFT:
        op = OP_ASR;
        break; // Arithmetic shift for signed
      default:
        op = OP_NOP;
        break;
      }
    } else {
      // Bool or other types
      switch (binop.op) {
      case BinaryOpType::EQUALS:
        op = OP_IEQ;
        break;
      case BinaryOpType::NOT_EQUALS:
        op = OP_INE;
        break;
      case BinaryOpType::AND:
        op = OP_AND;
        break;
      case BinaryOpType::OR:
        op = OP_OR;
        break;
      case BinaryOpType::BITWISE_AND:
        op = OP_AND;
        break;
      case BinaryOpType::BITWISE_OR:
        op = OP_OR;
        break;
      case BinaryOpType::BITWISE_XOR:
        op = OP_XOR;
        break;
      case BinaryOpType::LEFT_SHIFT:
        op = OP_SHL;
        break;
      case BinaryOpType::RIGHT_SHIFT:
        op = OP_SHR;
        break;
      default:
        op = OP_NOP;
        break;
      }
    }

    builder.EmitInstruction(op, dest, left, right);

    // Comparison operations produce BOOL, other operations produce the operand
    // type
    bool isComparison = (binop.op == BinaryOpType::EQUALS ||
                         binop.op == BinaryOpType::NOT_EQUALS ||
                         binop.op == BinaryOpType::LESS ||
                         binop.op == BinaryOpType::LESS_EQUAL ||
                         binop.op == BinaryOpType::GREATER ||
                         binop.op == BinaryOpType::GREATER_EQUAL);

    // For scalar-vector operations, the result type is the wider type (vector)
    // This handles cases like: 1.0 - vec2, 1.0 * vec3, etc.
    if (!isComparison) {
      bool leftIsScalar = (leftMask & TypeMasks::SCALAR_TYPES) != 0;
      bool rightIsVector =
          (rightMask & (TypeMasks::FLOAT_VECTORS | TypeMasks::INT_VECTORS |
                        TypeMasks::UINT_VECTORS)) != 0;
      if (leftIsScalar && rightIsVector) {
        resultType = rightType; // Use the vector type
      }
    }

    SetRegisterType(dest, isComparison ? CoreType::BOOL : resultType);
    return dest;
  }

  u16 LowerArrayAccess(NodeRef ref) {
    const ArrayAccessData &access = ast->GetArrayAccess(ref);

    u16 baseReg = LowerExpression(access.array);
    u16 indexReg = LowerExpression(access.index);
    u16 dest = AllocateRegister();

    // Check if the base register is a storage buffer pointer (but NOT a local
    // array)
    bool isStoragePtr = baseReg < MAX_REGISTERS &&
                        (program.registerStorageInfo[baseReg] &
                         IR::IRProgram::STORAGE_IS_PTR) &&
                        !(program.registerStorageInfo[baseReg] &
                          IR::IRProgram::STORAGE_IS_LOCAL_ARRAY);

    if (isStoragePtr) {
      // Storage buffer array indexing:
      // 1. Emit OP_STORAGE_INDEX to compute the element pointer
      // 2. Emit OP_STORAGE_LOAD to load the value

      u16 ptrReg = AllocateRegister();
      builder.EmitInstruction(OP_STORAGE_INDEX, ptrReg, baseReg, indexReg);

      // Propagate storage info for the pointer (in case of further chaining)
      u32 srcInfo = program.registerStorageInfo[baseReg];
      u32 binding = (srcInfo >> IR::IRProgram::STORAGE_BINDING_SHIFT);
      u32 depth = ((srcInfo & IR::IRProgram::STORAGE_DEPTH_MASK) >>
                   IR::IRProgram::STORAGE_DEPTH_SHIFT) +
                  1;
      u32 sharedFlag = srcInfo & IR::IRProgram::STORAGE_IS_SHARED;
      program.registerStorageInfo[ptrReg] =
          (binding << IR::IRProgram::STORAGE_BINDING_SHIFT) |
          (depth << IR::IRProgram::STORAGE_DEPTH_SHIFT) |
          IR::IRProgram::STORAGE_IS_PTR | sharedFlag;

      CoreType baseType = GetRegisterType(baseReg);
      if (baseType != CoreType::INVALID && baseType != CoreType::VOID) {
        SetRegisterType(ptrReg, baseType);
      }

      // Now load the value from the element pointer
      builder.EmitInstruction(OP_STORAGE_LOAD, dest, ptrReg);

      // Store the binding info in dest for later struct type association
      // (without IS_PTR flag since this is a loaded value, not a pointer)
      if (dest < MAX_REGISTERS) {
        program.registerStorageInfo[dest] =
            (binding << IR::IRProgram::STORAGE_BINDING_SHIFT);
      }

      // Propagate element type from base array type
      if (baseReg < MAX_REGISTERS) {
        CoreType baseType =
            static_cast<CoreType>(program.registerTypes[baseReg]);
        SetRegisterType(dest, baseType);
        if ((baseType == CoreType::CUSTOM || baseType == CoreType::ENUM) &&
            baseReg < MAX_REGISTERS) {
          u32 srcHash = program.registerStructTypes[baseReg];
          if (srcHash != 0) {
            // Ensure the struct type is registered in structTypeMap
            u32 canonicalHash = LookupOrRegisterStructType(srcHash);
            program.registerStructTypes[dest] =
                canonicalHash != 0 ? canonicalHash : srcHash;
          }
        }
      }
    } else {
      // Regular array load
      builder.EmitInstruction(OP_ARRAY_LOAD, dest, baseReg, indexReg);

      // Compute correct element type based on base type
      if (baseReg < MAX_REGISTERS) {
        CoreType baseType =
            static_cast<CoreType>(program.registerTypes[baseReg]);
        CoreType elemType = baseType;

        // Check if this is a local array (subscripting returns element, not
        // scalar)
        bool isLocalArray = (program.registerStorageInfo[baseReg] &
                             IR::IRProgram::STORAGE_IS_LOCAL_ARRAY);

        if (!isLocalArray) {
          // Matrix subscript returns a column vector
          if (baseType == CoreType::MAT2)
            elemType = CoreType::FLOAT2;
          else if (baseType == CoreType::MAT3)
            elemType = CoreType::FLOAT3;
          else if (baseType == CoreType::MAT4)
            elemType = CoreType::FLOAT4;
          // Vector subscript returns a scalar (but NOT for local arrays of
          // vectors)
          else if (baseType == CoreType::FLOAT2 ||
                   baseType == CoreType::FLOAT3 || baseType == CoreType::FLOAT4)
            elemType = CoreType::FLOAT;
          else if (baseType == CoreType::INT2 || baseType == CoreType::INT3 ||
                   baseType == CoreType::INT4)
            elemType = CoreType::INT;
          else if (baseType == CoreType::UINT2 || baseType == CoreType::UINT3 ||
                   baseType == CoreType::UINT4)
            elemType = CoreType::UINT;
        }
        // For local arrays, elemType stays as baseType (the array element type)

        SetRegisterType(dest, elemType);
        if ((baseType == CoreType::CUSTOM || baseType == CoreType::ENUM) &&
            baseReg < MAX_REGISTERS) {
          u32 srcHash = program.registerStructTypes[baseReg];
          if (srcHash != 0) {
            // Ensure the struct type is registered in structTypeMap
            u32 canonicalHash = LookupOrRegisterStructType(srcHash);
            program.registerStructTypes[dest] =
                canonicalHash != 0 ? canonicalHash : srcHash;
          }
        }
      }
    }

    return dest;
  }

  u16 LowerStoragePointerForAtomic(NodeRef ref) {
    if (ref.Type() != ASTNodeType::ARRAY_ACCESS) {
      return LowerExpression(ref);
    }

    const ArrayAccessData &access = ast->GetArrayAccess(ref);

    u16 baseReg = LowerExpression(access.array);
    u16 indexReg = LowerExpression(access.index);

    bool isStoragePtr =
        baseReg < MAX_REGISTERS &&
        (program.registerStorageInfo[baseReg] & IR::IRProgram::STORAGE_IS_PTR);

    if (!isStoragePtr && access.array.Type() == ASTNodeType::IDENTIFIER) {
      for (u32 i = 0; i < program.sharedVarCount; i++) {
        if (program.sharedRegisters[i] == baseReg) {
          program.registerStorageInfo[baseReg] =
              (i << IR::IRProgram::STORAGE_BINDING_SHIFT) |
              IR::IRProgram::STORAGE_IS_PTR | IR::IRProgram::STORAGE_IS_SHARED;
          isStoragePtr = true;
          break;
        }
      }
    }

    if (!isStoragePtr) {
      return LowerExpression(ref);
    }

    u16 ptrReg = AllocateRegister();
    builder.EmitInstruction(OP_STORAGE_INDEX, ptrReg, baseReg, indexReg);

    u32 srcInfo = program.registerStorageInfo[baseReg];
    u32 binding = (srcInfo >> IR::IRProgram::STORAGE_BINDING_SHIFT);
    u32 depth = ((srcInfo & IR::IRProgram::STORAGE_DEPTH_MASK) >>
                 IR::IRProgram::STORAGE_DEPTH_SHIFT) +
                1;
    u32 sharedFlag = srcInfo & IR::IRProgram::STORAGE_IS_SHARED;
    program.registerStorageInfo[ptrReg] =
        (binding << IR::IRProgram::STORAGE_BINDING_SHIFT) |
        (depth << IR::IRProgram::STORAGE_DEPTH_SHIFT) |
        IR::IRProgram::STORAGE_IS_PTR | sharedFlag;

    CoreType baseType = GetRegisterType(baseReg);
    if (baseType != CoreType::INVALID && baseType != CoreType::VOID) {
      SetRegisterType(ptrReg, baseType);
    }

    return ptrReg;
  }

  u16 LowerMemberAccess(NodeRef ref) {
    const MemberAccessData &access = ast->GetMemberAccess(ref);

    // Resolve `variants.<name>` to the variant's current value as a
    // constant. Raster stages (vertex/fragment) go through
    // CloneShaderStageWithParams which substitutes variants at clone time,
    // but compute / direct stages reach IR lowering with the AST still
    // containing MemberAccess(VARIANTS, name). Without this, the default
    // MemberAccess path treats `variants` as a struct-valued register and
    // emits a bogus OpCompositeExtract on a scalar constant.
    if (access.object.Type() == ASTNodeType::IDENTIFIER &&
        !currentPipeline.IsNull()) {
      const IdentifierData &objIdent = ast->GetIdentifier(access.object);
      if (objIdent.identifierKind == SpecialIdentifier::VARIANTS) {
        const PipelineData &pipeline = ast->GetPipeline(currentPipeline);
        for (u32 i = 0; i < pipeline.variantDecls.count; i++) {
          const PipelineVariantDeclData &decl = pipeline.variantDecls[i];
          if (decl.name.nameHash == access.member.nameHash && decl.defaultResolved) {
            switch (decl.defaultValue.type) {
              case LiteralValue::FLOAT:
                return builder.EmitConstant(decl.defaultValue.floatValue);
              case LiteralValue::INT:
                return EmitConstantInt(decl.defaultValue.intValue);
              case LiteralValue::UINT:
                return EmitConstantUint(decl.defaultValue.uintValue);
              case LiteralValue::BOOL:
                return builder.EmitConstantBool(decl.defaultValue.boolValue);
              default:
                break;
            }
          }
        }
      }
    }

    // Handle chained member access (e.g., attributes.normal.y or
    // resources.lights.positions)
    if (access.object.Type() == ASTNodeType::MEMBER_ACCESS ||
        access.object.Type() == ASTNodeType::ARRAY_ACCESS) {
      // First, lower the object (e.g., attributes.normal -> float3 register)
      u16 objectReg = LowerExpression(access.object);

      // Check if the object register has a struct type (e.g., resources.lights
      // -> LightSourcesSoA)
      u32 structTypeHash = 0;
      if (objectReg < MAX_REGISTERS) {
        structTypeHash = program.registerStructTypes[objectReg];
      }

      if (structTypeHash != 0) {
        // This is a struct field access (e.g., lights.positions)
        u32 fieldIndex = 0xFFFFFFFF;
        CoreType fieldType = CoreType::FLOAT;
        u32 fieldOffset = 0;

        auto structIt = structTypeMap.find(structTypeHash);

        if (structIt != structTypeMap.end()) {
          u32 structIdx = structIt->second;
          const IRProgram::StructTypeInfo &info =
              program.structTypes[structIdx];
          fieldOffset = info.fieldOffset;
          for (u32 i = 0; i < info.fieldCount; i++) {
            if (program.structFieldNameHashes[info.fieldOffset + i] ==
                access.member.nameHash) {
              fieldIndex = i;
              fieldType = static_cast<CoreType>(
                  program.structFieldTypes[info.fieldOffset + i]);
              break;
            }
          }
        }

        if (fieldIndex != 0xFFFFFFFF) {
          u16 dest = AllocateRegister();

          // Check if the object register is a storage buffer pointer
          bool isStoragePtr = objectReg < MAX_REGISTERS &&
                              (program.registerStorageInfo[objectReg] &
                               IR::IRProgram::STORAGE_IS_PTR);

          if (isStoragePtr) {
            // Storage buffer field access - emit OP_STORAGE_FIELD to maintain
            // pointer semantics
            builder.EmitInstruction(OP_STORAGE_FIELD, dest, objectReg,
                                    fieldIndex);
            program.metadata[builder.currentInstruction - 1] = structTypeHash;

            // Propagate storage pointer info (increment depth)
            u32 srcInfo = program.registerStorageInfo[objectReg];
            u32 binding = (srcInfo >> IR::IRProgram::STORAGE_BINDING_SHIFT);
            u32 depth = ((srcInfo & IR::IRProgram::STORAGE_DEPTH_MASK) >>
                         IR::IRProgram::STORAGE_DEPTH_SHIFT) +
                        1;
            u32 sharedFlag = srcInfo & IR::IRProgram::STORAGE_IS_SHARED;
            program.registerStorageInfo[dest] =
                (binding << IR::IRProgram::STORAGE_BINDING_SHIFT) |
                (depth << IR::IRProgram::STORAGE_DEPTH_SHIFT) |
                IR::IRProgram::STORAGE_IS_PTR | sharedFlag;
          } else {
            // Regular struct field access - emit OP_STRUCT_EXTRACT
            builder.EmitInstruction(OP_STRUCT_EXTRACT, dest, objectReg,
                                    fieldIndex);
            program.metadata[builder.currentInstruction - 1] = structTypeHash;
          }

          SetRegisterType(dest, fieldType);
          if ((fieldType == CoreType::CUSTOM || fieldType == CoreType::ENUM) &&
              program.structFieldTypeHashes) {
            u32 fieldTypeHash =
                program.structFieldTypeHashes[fieldOffset + fieldIndex];
            if (fieldTypeHash != 0 && dest < MAX_REGISTERS) {
              program.registerStructTypes[dest] = fieldTypeHash;
            }
          }
          return dest;
        }
      }

      // Then extract the component based on the member name (x/y/z/w or
      // r/g/b/a)
      u32 memberHash = access.member.nameHash;
      u32 componentIndex = 0xFFFFFFFF;

      // Check for xyzw swizzle
      if (memberHash == Utils::HashStr("x") ||
          memberHash == Utils::HashStr("r"))
        componentIndex = 0;
      else if (memberHash == Utils::HashStr("y") ||
               memberHash == Utils::HashStr("g"))
        componentIndex = 1;
      else if (memberHash == Utils::HashStr("z") ||
               memberHash == Utils::HashStr("b"))
        componentIndex = 2;
      else if (memberHash == Utils::HashStr("w") ||
               memberHash == Utils::HashStr("a"))
        componentIndex = 3;

      if (componentIndex != 0xFFFFFFFF) {
        // If objectReg is a storage pointer, load it first before swizzle
        u16 srcReg = objectReg;
        if (objectReg < MAX_REGISTERS &&
            (program.registerStorageInfo[objectReg] &
             IR::IRProgram::STORAGE_IS_PTR)) {
          srcReg = AllocateRegister();
          CoreType loadType = GetRegisterType(objectReg);
          SetRegisterType(srcReg, loadType);
          builder.EmitInstruction(OP_STORAGE_LOAD, srcReg, objectReg);
        }
        // `.x` / `.r` on an already-scalar value is a no-op (e.g. the
        // inner `.r` of `v.r.r`). Emitting OP_VEC_EXTRACT on a scalar
        // makes the SPIR-V backend synthesise OpCompositeExtract on a
        // non-composite and trips the validator. Skip when the object
        // came from an ARRAY_ACCESS — array loads of struct-array
        // fields carry an IR-level scalar CoreType but a SPIR-V
        // vector type override, so the guard would incorrectly
        // suppress a needed extract.
        CoreType srcType = GetRegisterType(srcReg);
        bool srcFromArrayAccess =
            access.object.Type() == ASTNodeType::ARRAY_ACCESS;
        if (!srcFromArrayAccess &&
            (mask(srcType) & TypeMasks::SCALAR_TYPES) && componentIndex == 0) {
          return srcReg;
        }
        u16 dest = AllocateRegister();
        builder.EmitInstruction(OP_VEC_EXTRACT, dest, srcReg, componentIndex);
        // Get the scalar type from the vector type
        CoreType vectorType = GetRegisterType(srcReg);
        CoreType scalarType = GetScalarComponentType(vectorType);
        SetRegisterType(dest, scalarType);
        return dest;
      }

      // Multi-component swizzle (.xyz/.rgb/.wzy/etc): parse each character
      // from the member name. Supports any 2-, 3-, or 4-character combination
      // of x/y/z/w or r/g/b/a.
      if (sourceBase && !access.member.isHashOnly()) {
        auto sv = access.member.view(sourceBase);
        u32 swizzleLen = static_cast<u32>(sv.size());
        if (swizzleLen >= 2 && swizzleLen <= 4) {
          u8 indices[4] = {0, 0, 0, 0};
          bool valid = true;
          bool seenXyzw = false;
          bool seenRgba = false;
          for (u32 i = 0; i < swizzleLen; i++) {
            char c = sv[i];
            switch (c) {
            case 'x': indices[i] = 0; seenXyzw = true; break;
            case 'y': indices[i] = 1; seenXyzw = true; break;
            case 'z': indices[i] = 2; seenXyzw = true; break;
            case 'w': indices[i] = 3; seenXyzw = true; break;
            case 'r': indices[i] = 0; seenRgba = true; break;
            case 'g': indices[i] = 1; seenRgba = true; break;
            case 'b': indices[i] = 2; seenRgba = true; break;
            case 'a': indices[i] = 3; seenRgba = true; break;
            default: valid = false; break;
            }
            if (!valid) break;
          }

          if (valid && !(seenXyzw && seenRgba)) {
            u16 srcReg = objectReg;
            if (objectReg < MAX_REGISTERS &&
                (program.registerStorageInfo[objectReg] &
                 IR::IRProgram::STORAGE_IS_PTR)) {
              srcReg = AllocateRegister();
              CoreType loadType = GetRegisterType(objectReg);
              SetRegisterType(srcReg, loadType);
              builder.EmitInstruction(OP_STORAGE_LOAD, srcReg, objectReg);
            }
            u16 dest = AllocateRegister();
            u32 shuffleMask = 0;
            for (u32 i = 0; i < swizzleLen; i++) {
              shuffleMask |= (indices[i] & 0xF) << (i * 4);
            }
            builder.EmitInstruction(OP_VEC_SHUFFLE, dest, srcReg, srcReg);
            program.metadata[builder.currentInstruction - 1] = shuffleMask;

            CoreType vectorType = GetRegisterType(srcReg);
            CoreType scalarType = GetScalarComponentType(vectorType);
            SetRegisterType(dest, GetVectorType(scalarType, swizzleLen));
            return dest;
          }
        }
      }

      // Not a simple component access, fall through to return object
      return objectReg;
    }

    // Handle any expression object (function call, binary/unary expression,
    // constructor, etc.) e.g., sample(tex, uv).rgb, (matrix * vec4).xyz,
    // float4(x,y,z,w).xy
    if (access.object.Type() != ASTNodeType::IDENTIFIER) {
      // Lower the expression first to get the result
      u16 objectReg = LowerExpression(access.object);

      // Struct-field access on a non-identifier expression (e.g.,
      // `makeF(1.0).v`, `(a + b).field`). Without this, falling through
      // to the swizzle-only path returns the whole object for unknown
      // member names and downstream component extracts hit an
      // out-of-bounds index on the composite.
      if (objectReg < MAX_REGISTERS) {
        u32 structTypeHash = program.registerStructTypes[objectReg];
        if (structTypeHash != 0) {
          auto structIt = structTypeMap.find(structTypeHash);
          if (structIt != structTypeMap.end()) {
            u32 structIdx = structIt->second;
            const IRProgram::StructTypeInfo &info =
                program.structTypes[structIdx];
            for (u32 i = 0; i < info.fieldCount; i++) {
              if (program.structFieldNameHashes[info.fieldOffset + i] ==
                  access.member.nameHash) {
                u16 dest = AllocateRegister();
                builder.EmitInstruction(OP_STRUCT_EXTRACT, dest, objectReg, i);
                program.metadata[builder.currentInstruction - 1] =
                    structTypeHash;
                CoreType fieldType = static_cast<CoreType>(
                    program.structFieldTypes[info.fieldOffset + i]);
                SetRegisterType(dest, fieldType);
                // Propagate nested-struct type info for further chaining.
                if ((fieldType == CoreType::CUSTOM ||
                     fieldType == CoreType::ENUM) &&
                    program.structFieldTypeHashes && dest < MAX_REGISTERS) {
                  u32 fieldTypeHash =
                      program.structFieldTypeHashes[info.fieldOffset + i];
                  if (fieldTypeHash != 0) {
                    program.registerStructTypes[dest] = fieldTypeHash;
                  }
                }
                return dest;
              }
            }
          }
        }
      }

      // Then apply swizzle/member access
      u32 memberHash = access.member.nameHash;
      u32 componentIndex = 0xFFFFFFFF;

      // Check for single component swizzle (x/y/z/w or r/g/b/a)
      if (memberHash == Utils::HashStr("x") ||
          memberHash == Utils::HashStr("r"))
        componentIndex = 0;
      else if (memberHash == Utils::HashStr("y") ||
               memberHash == Utils::HashStr("g"))
        componentIndex = 1;
      else if (memberHash == Utils::HashStr("z") ||
               memberHash == Utils::HashStr("b"))
        componentIndex = 2;
      else if (memberHash == Utils::HashStr("w") ||
               memberHash == Utils::HashStr("a"))
        componentIndex = 3;

      if (componentIndex != 0xFFFFFFFF) {
        CoreType objType = GetRegisterType(objectReg);
        if ((mask(objType) & TypeMasks::SCALAR_TYPES) && componentIndex == 0) {
          return objectReg;
        }
        u16 dest = AllocateRegister();
        builder.EmitInstruction(OP_VEC_EXTRACT, dest, objectReg,
                                componentIndex);
        // Get the scalar type from the vector type
        CoreType vectorType = GetRegisterType(objectReg);
        CoreType scalarType = GetScalarComponentType(vectorType);
        SetRegisterType(dest, scalarType);
        return dest;
      }

      // Check for multi-component swizzle (rgb, xyz, etc.)
      static const u32 HASH_RGB = Utils::HashStr("rgb");
      static const u32 HASH_XYZ = Utils::HashStr("xyz");
      static const u32 HASH_XY = Utils::HashStr("xy");
      static const u32 HASH_RG = Utils::HashStr("rg");

      if (memberHash == HASH_RGB || memberHash == HASH_XYZ) {
        // Extract first 3 components (vec4 -> vec3)
        u16 dest = AllocateRegister();
        // For VEC_SHUFFLE: operand0 = src0, operand1 = src1 (same for
        // single-source swizzle) Metadata = shuffle mask (4 bits per component)
        u32 shuffleMask = (0 << 0) | (1 << 4) | (2 << 8); // components 0, 1, 2
        builder.EmitInstruction(OP_VEC_SHUFFLE, dest, objectReg, objectReg);
        program.metadata[builder.currentInstruction - 1] = shuffleMask;
        // Get the scalar type and build the appropriate vec3 type
        CoreType vectorType = GetRegisterType(objectReg);
        CoreType scalarType = GetScalarComponentType(vectorType);
        SetRegisterType(dest, GetVectorType(scalarType, 3));
        return dest;
      }

      if (memberHash == HASH_XY || memberHash == HASH_RG) {
        // Extract first 2 components (vec4/vec3 -> vec2)
        u16 dest = AllocateRegister();
        u32 shuffleMask = (0 << 0) | (1 << 4); // components 0, 1
        builder.EmitInstruction(OP_VEC_SHUFFLE, dest, objectReg, objectReg);
        program.metadata[builder.currentInstruction - 1] = shuffleMask;
        // Get the scalar type and build the appropriate vec2 type
        CoreType vectorType = GetRegisterType(objectReg);
        CoreType scalarType = GetScalarComponentType(vectorType);
        SetRegisterType(dest, GetVectorType(scalarType, 2));
        return dest;
      }

      // No swizzle or unknown member, return object as-is
      return objectReg;
    }

    const IdentifierData &obj = ast->GetIdentifier(access.object);

    switch (obj.identifierKind) {
    case SpecialIdentifier::ATTRIBUTES: {
      u16 dest = AllocateRegister();
      u32 attrIndex = GetAttributeIndex(access.member.nameHash);
      builder.EmitInstruction(OP_LOAD_ATTR, dest, attrIndex);

      // Check if attribute is compressed - if so, return raw uint type
      CompressionFormat compression = GetAttributeCompression(attrIndex);
      CoreType attrType = CoreType::FLOAT3; // Default for position/normal

      if (compression != CompressionFormat::NONE) {
        // Compressed attributes load raw uint data
        attrType = GetRawTypeForCompression(compression);
      } else {
        // Look up semantic type from AST pipeline attributes
        attrType = GetInputTypeFromAttribute(access.member.nameHash);
      }
      SetRegisterType(dest, attrType);
      return dest;
    }

    case SpecialIdentifier::OUTPUT: {
      u16 dest = AllocateRegister();
      u32 nameHash = access.member.nameHash;
      u32 slot = ResolveOutputSlotForLoad(nameHash);
      CoreType outputType = ResolveOutputTypeForLoad(nameHash);
      builder.EmitInstruction(OP_LOAD_OUTPUT, dest, slot);
      program.metadata[builder.currentInstruction - 1] = nameHash;
      SetRegisterType(dest, outputType);
      return dest;
    }

    case SpecialIdentifier::RESOURCES: {
      // Resources are registered with their short name in RESOURCES namespace
      // The namespace disambiguates them from other symbols
      Symbol *sym = SymbolTable::LookupByHash(
          const_cast<SymbolTableData *>(symbols), access.member.nameHash);
      if (!sym || sym->kind != SymbolKind::RESOURCE)
        return 0;

      const ResourceData &resData = symbols->resources[sym->index];
      u16 dest = AllocateRegister();

      switch (resData.type) {
      case ResourceBinding::UniformBuffer:
        // Uniform buffers (declared with "uniform" keyword) contain a single
        // value They emit OP_LOAD_UNIFORM to load the value directly
        builder.EmitInstruction(OP_LOAD_UNIFORM, dest, resData.bindingIndex);
        break;

      case ResourceBinding::StorageBuffer:
        // Storage buffers (declared with "buffer" keyword) are arrays
        // They emit OP_STORAGE_PTR to enable dynamic indexing via OpAccessChain
        builder.EmitInstruction(OP_STORAGE_PTR, dest, resData.bindingIndex);
        program.metadata[builder.currentInstruction - 1] =
            resData.structTypeHash;

        // Mark this register as holding a storage buffer pointer
        if (dest < MAX_REGISTERS) {
          program.registerStorageInfo[dest] =
              (resData.bindingIndex << IR::IRProgram::STORAGE_BINDING_SHIFT) |
              IR::IRProgram::STORAGE_IS_PTR;
        }

        // Register buffer element types for SPIR-V backend
        if (resData.bindingIndex < 32) {
          if (resData.structTypeHash != 0) {
            program.bufferElementStructTypes[resData.bindingIndex] =
                resData.structTypeHash;
          } else {
            CoreType coreType = static_cast<CoreType>(resData.coreType);
            if (coreType != CoreType::VOID && coreType != CoreType::INVALID) {
              program.bufferElementCoreTypes[resData.bindingIndex] =
                  resData.coreType;
            }
          }
        }
        break;

      case ResourceBinding::Texture:
        // Traditional bound texture - encode slot in register
        dest = 0x2000 | resData.bindingIndex;
        break;

      case ResourceBinding::StorageImage:
        // Storage image (read/write texture) - encode slot same as texture
        dest = 0x2000 | resData.bindingIndex;
        break;

      case ResourceBinding::Sampler:
        dest = 0x3000 | resData.bindingIndex;
        break;

      default:
        break;
      }

      // Use explicit type from render config
      CoreType resourceType = static_cast<CoreType>(resData.coreType);
      SetRegisterType(dest, resourceType);

      // Store struct type hash for storage buffers with struct types
      // Also ensure the struct type is registered in the IRProgram
      if (resData.structTypeHash != 0 && dest < MAX_REGISTERS) {
        // Ensure struct type is registered so field info is available
        LookupOrRegisterStructType(resData.structTypeHash);
        program.registerStructTypes[dest] = resData.structTypeHash;
      }

      return dest;
    }

    case SpecialIdentifier::INPUT: {
      u16 dest = AllocateRegister();
      u32 memberHash = access.member.nameHash;

      // Check for built-in inputs first
      switch (memberHash) {
      case BuiltinHash::VERTEX_ID:
        if (currentStage != ShaderStage::Vertex) {
          fprintf(
              stderr,
              "Error: input.vertex_id is only available in vertex shaders\n");
          return 0;
        }
        builder.EmitInstruction(OP_LOAD_INPUT, dest,
                                BuiltinInputSlot::VERTEX_ID);
        SetRegisterType(dest, CoreType::UINT);
        return dest;

      case BuiltinHash::INSTANCE_ID:
        if (currentStage != ShaderStage::Vertex) {
          fprintf(
              stderr,
              "Error: input.instance_id is only available in vertex shaders\n");
          return 0;
        }
        builder.EmitInstruction(OP_LOAD_INPUT, dest,
                                BuiltinInputSlot::INSTANCE_ID);
        SetRegisterType(dest, CoreType::UINT);
        return dest;

      case BuiltinHash::GLOBAL_ID:
        if (currentStage != ShaderStage::Compute) {
          fprintf(
              stderr,
              "Error: input.global_id is only available in compute shaders\n");
          return 0;
        }
        builder.EmitInstruction(OP_LOAD_INPUT, dest,
                                BuiltinInputSlot::GLOBAL_INVOCATION_ID);
        SetRegisterType(dest, CoreType::UINT3);
        return dest;

      case BuiltinHash::LOCAL_ID:
        if (currentStage != ShaderStage::Compute) {
          fprintf(
              stderr,
              "Error: input.local_id is only available in compute shaders\n");
          return 0;
        }
        builder.EmitInstruction(OP_LOAD_INPUT, dest,
                                BuiltinInputSlot::LOCAL_INVOCATION_ID);
        SetRegisterType(dest, CoreType::UINT3);
        return dest;

      case BuiltinHash::WORKGROUP_ID:
        if (currentStage != ShaderStage::Compute) {
          fprintf(stderr, "Error: input.workgroup_id is only available in "
                          "compute shaders\n");
          return 0;
        }
        builder.EmitInstruction(OP_LOAD_INPUT, dest,
                                BuiltinInputSlot::WORKGROUP_ID);
        SetRegisterType(dest, CoreType::UINT3);
        return dest;

      case BuiltinHash::NUM_WORKGROUPS:
        if (currentStage != ShaderStage::Compute) {
          fprintf(stderr, "Error: input.num_workgroups is only available in "
                          "compute shaders\n");
          return 0;
        }
        builder.EmitInstruction(OP_LOAD_INPUT, dest,
                                BuiltinInputSlot::NUM_WORKGROUPS);
        SetRegisterType(dest, CoreType::UINT3);
        return dest;

      case BuiltinHash::LOCAL_INDEX:
        if (currentStage != ShaderStage::Compute) {
          fprintf(stderr, "Error: input.local_index is only available in "
                          "compute shaders\n");
          return 0;
        }
        builder.EmitInstruction(OP_LOAD_INPUT, dest,
                                BuiltinInputSlot::LOCAL_INVOCATION_INDEX);
        SetRegisterType(dest, CoreType::UINT);
        return dest;

      case BuiltinHash::POSITION:
        // In fragment shaders, input.position refers to gl_FragCoord
        if (currentStage == ShaderStage::Fragment) {
          builder.EmitInstruction(OP_LOAD_INPUT, dest,
                                  BuiltinInputSlot::FRAG_COORD);
          SetRegisterType(dest, CoreType::FLOAT4);
          return dest;
        }
        // In other stages, fall through to varying lookup
        break;

      default:
        break;
      }

      // Fragment shader reading interpolated varyings from vertex output
      // input.xxx -> OP_LOAD_INPUT with slot index

      // Map varying name to slot index using pass context if available
      u32 inputSlot = GetInputSlotIndex(memberHash);
      builder.EmitInstruction(OP_LOAD_INPUT, dest, inputSlot);

      // Get type from varying context if available (preferred)
      // Otherwise fall back to pipeline attribute declarations
      CoreType inputType;
      if (currentPassVaryings) {
        // inputSlot is VARYING0 + index, so subtract VARYING0 to get raw index
        u32 varyingIndex = inputSlot - OutputSlot::VARYING0;
        inputType = currentPassVaryings->GetType(varyingIndex);
      } else {
        inputType = GetInputTypeFromAttribute(memberHash);
      }
      SetRegisterType(dest, inputType);
      return dest;
    }

    case SpecialIdentifier::NONE: {
      // Regular struct member access
      u16 objReg = LowerExpression(access.object);

      // Look up the variable's struct type
      u32 structTypeHash = 0;
      auto varIt = variableStructTypes.find(obj.name.nameHash);
      if (varIt != variableStructTypes.end()) {
        structTypeHash = varIt->second;
      } else if (objReg < MAX_REGISTERS) {
        structTypeHash = program.registerStructTypes[objReg];
      }

      if (structTypeHash != 0) {
        // Find the field index and type
        u32 fieldIndex = 0xFFFFFFFF;
        CoreType fieldType = CoreType::FLOAT;
        u32 fieldOffset = 0;
        auto structIt = structTypeMap.find(structTypeHash);
        if (structIt != structTypeMap.end()) {
          u32 structIdx = structIt->second;
          const IRProgram::StructTypeInfo &info =
              program.structTypes[structIdx];
          fieldOffset = info.fieldOffset;
          for (u32 i = 0; i < info.fieldCount; i++) {
            if (program.structFieldNameHashes[info.fieldOffset + i] ==
                access.member.nameHash) {
              fieldIndex = i;
              fieldType = static_cast<CoreType>(
                  program.structFieldTypes[info.fieldOffset + i]);
              break;
            }
          }
        }

        if (fieldIndex != 0xFFFFFFFF) {
          u16 dest = AllocateRegister();
          // Emit struct extract: dest = struct.field[fieldIndex]
          builder.EmitInstruction(OP_STRUCT_EXTRACT, dest, objReg, fieldIndex);
          // Store full struct type hash in metadata (field index is in operand
          // 1)
          program.metadata[builder.currentInstruction - 1] = structTypeHash;
          SetRegisterType(dest, fieldType);
          if ((fieldType == CoreType::CUSTOM || fieldType == CoreType::ENUM) &&
              program.structFieldTypeHashes) {
            u32 fieldTypeHash =
                program.structFieldTypeHashes[fieldOffset + fieldIndex];
            if (fieldTypeHash != 0 && dest < MAX_REGISTERS) {
              program.registerStructTypes[dest] = fieldTypeHash;
            }
          }
          return dest;
        }
      }

      // Fallback: try vector component access (e.g., pos.x, pos.y, pos.z)
      u32 componentIndex = 0xFFFFFFFF;
      if (access.member.nameHash == Utils::HashStr("x") ||
          access.member.nameHash == Utils::HashStr("r"))
        componentIndex = 0;
      else if (access.member.nameHash == Utils::HashStr("y") ||
               access.member.nameHash == Utils::HashStr("g"))
        componentIndex = 1;
      else if (access.member.nameHash == Utils::HashStr("z") ||
               access.member.nameHash == Utils::HashStr("b"))
        componentIndex = 2;
      else if (access.member.nameHash == Utils::HashStr("w") ||
               access.member.nameHash == Utils::HashStr("a"))
        componentIndex = 3;

      if (componentIndex != 0xFFFFFFFF) {
        CoreType objType = GetRegisterType(objReg);
        if ((mask(objType) & TypeMasks::SCALAR_TYPES) && componentIndex == 0) {
          return objReg;
        }
        u16 dest = AllocateRegister();
        builder.EmitInstruction(OP_VEC_EXTRACT, dest, objReg, componentIndex);
        // Get the scalar type from the vector type
        CoreType vectorType = GetRegisterType(objReg);
        CoreType scalarType = GetScalarComponentType(vectorType);
        SetRegisterType(dest, scalarType);
        return dest;
      }

      // Multi-component swizzle reads: parse each character of the member
      // name. Accepts any 2/3/4-character combination of x/y/z/w or r/g/b/a
      // (mixing the two sets is rejected).
      if (sourceBase && !access.member.isHashOnly()) {
        auto sv = access.member.view(sourceBase);
        u32 swizzleLen = static_cast<u32>(sv.size());
        if (swizzleLen >= 2 && swizzleLen <= 4) {
          u8 indices[4] = {0, 0, 0, 0};
          bool valid = true;
          bool seenXyzw = false;
          bool seenRgba = false;
          for (u32 i = 0; i < swizzleLen; i++) {
            char c = sv[i];
            switch (c) {
            case 'x': indices[i] = 0; seenXyzw = true; break;
            case 'y': indices[i] = 1; seenXyzw = true; break;
            case 'z': indices[i] = 2; seenXyzw = true; break;
            case 'w': indices[i] = 3; seenXyzw = true; break;
            case 'r': indices[i] = 0; seenRgba = true; break;
            case 'g': indices[i] = 1; seenRgba = true; break;
            case 'b': indices[i] = 2; seenRgba = true; break;
            case 'a': indices[i] = 3; seenRgba = true; break;
            default: valid = false; break;
            }
            if (!valid) break;
          }

          if (valid && !(seenXyzw && seenRgba)) {
            u16 dest = AllocateRegister();
            u32 shuffleMask = 0;
            for (u32 i = 0; i < swizzleLen; i++) {
              shuffleMask |= (indices[i] & 0xF) << (i * 4);
            }
            builder.EmitInstruction(OP_VEC_SHUFFLE, dest, objReg, objReg);
            program.metadata[builder.currentInstruction - 1] = shuffleMask;

            CoreType vectorType = GetRegisterType(objReg);
            CoreType scalarType = GetScalarComponentType(vectorType);
            CoreType resultType = GetVectorType(scalarType, swizzleLen);
            SetRegisterType(dest, resultType);
            return dest;
          }
        }
      }

      // Unknown member access - emit placeholder
      u16 dest = AllocateRegister();
      builder.EmitInstruction(OP_LOAD_REG, dest, objReg);
      program.metadata[builder.currentInstruction - 1] = access.member.nameHash;
      // Copy type from source register
      CoreType srcType = GetRegisterType(objReg);
      SetRegisterType(dest, srcType);
      return dest;
    }

    default:
      return 0;
    }
  }

  u16 LowerFunctionCall(NodeRef ref) {
    const FunctionCallData &call = ast->GetFunctionCall(ref);

    // Collect argument registers
    // Initialize to 0x3FFF sentinel (unused operand slot marker, skipped by
    // SSA) Support up to 16 arguments for mat4 construction
    u16 args[16] = {0x3FFF, 0x3FFF, 0x3FFF, 0x3FFF, 0x3FFF, 0x3FFF,
                    0x3FFF, 0x3FFF, 0x3FFF, 0x3FFF, 0x3FFF, 0x3FFF,
                    0x3FFF, 0x3FFF, 0x3FFF, 0x3FFF};
    u32 argCount = call.arguments.count < 16 ? call.arguments.count : 16;
    for (u32 i = 0; i < argCount; i++) {
      args[i] = LowerExpression(call.arguments[i]);
    }

    u16 dest = AllocateRegister();

    // Check for enum variant construction (e.g., SDFShape::Sphere(0.5))
    // The moduleObject will reference the enum type identifier
    if (!call.moduleObject.IsNull() &&
        call.moduleObject.Type() == ASTNodeType::IDENTIFIER) {
      const IdentifierData &enumIdent = ast->GetIdentifier(call.moduleObject);
      u32 enumHash = enumIdent.name.nameHash;

      // Look up in symbol table to check if it's an enum type
      // ENUM for module enums, ENUM_SYMBOL for pipeline-local enums
      Symbol *enumSym = SymbolTable::LookupByHash(
          const_cast<SymbolTableData *>(symbols), enumHash);
      if (enumSym && (enumSym->kind == SymbolKind::ENUM ||
                      enumSym->kind == SymbolKind::ENUM_SYMBOL)) {
        const EnumData &enumData = symbols->enums[enumSym->index];

        // Find the variant by name
        u32 variantIndex = 0xFFFFFFFF;
        for (u32 v = 0; v < enumData.variants.count; v++) {
          if (enumData.variants[v].name.nameHash == call.name.nameHash) {
            variantIndex = v;
            break;
          }
        }

        if (variantIndex != 0xFFFFFFFF) {
          // Register the enum as a struct type if not already done
          u32 enumStructHash = LookupOrRegisterStructType(enumHash);

          // Emit OP_ENUM_CONSTRUCT
          // operands[0..3] = argument values (field data)
          builder.EmitInstruction(OP_ENUM_CONSTRUCT, dest, args[0], args[1],
                                  args[2], args[3]);
          // metadata = (variantIndex << 16) | (argCount << 8) | enumStructHash
          // lower bits
          program.metadata[builder.currentInstruction - 1] =
              (variantIndex << 16) | (argCount << 8) | (enumHash & 0xFF);

          // Set register type to the enum struct type
          SetRegisterType(dest, CoreType::CUSTOM);
          program.registerStructTypes[dest] = enumStructHash;

          return dest;
        }
      }
    }

    // Check for enum method call (e.g., shape.distance(p))
    if (call.flags & FunctionCallFlags::IS_METHOD_CALL) {
      // Get the receiver object
      if (!call.moduleObject.IsNull()) {
        u16 receiverReg = LowerExpression(call.moduleObject);
        // registerStructTypes has MAX_REGISTERS entries. Constant-encoded
        // registers (bits 0x4000 / 0x8000) and any reg past MAX_REGISTERS
        // can't have a valid struct-type hash; skip method dispatch.
        if ((receiverReg & 0xC000) != 0 || receiverReg >= MAX_REGISTERS) {
          return 0;
        }
        u32 receiverStructHash = program.registerStructTypes[receiverReg];

        if (receiverStructHash != 0) {
          // Look up the enum type by its struct hash
          // ENUM for module enums, ENUM_SYMBOL for pipeline-local enums
          Symbol *enumSym = SymbolTable::LookupByHash(
              const_cast<SymbolTableData *>(symbols), receiverStructHash);
          if (enumSym && (enumSym->kind == SymbolKind::ENUM ||
                          enumSym->kind == SymbolKind::ENUM_SYMBOL)) {
            const EnumData &enumData = symbols->enums[enumSym->index];

            // Find the method by name
            NodeRef methodRef = NodeRef::Null();
            for (u32 i = 0; i < enumData.methodIndices.count; i++) {
              u32 funcIdx = enumData.methodIndices[i];
              if (funcIdx < symbols->functions.count) {
                const FunctionData &funcData = symbols->functions[funcIdx];
                if (funcData.returnType != CoreType::INVALID) {
                  // Look up method in AST to check name
                  for (u32 e = 0; e < ast->enumDecls.count; e++) {
                    const EnumDeclData &enumDecl = ast->enumDecls[e];
                    for (u32 m = 0; m < enumDecl.methods.count; m++) {
                      NodeRef mRef = enumDecl.methods[m];
                      if (mRef.Type() == ASTNodeType::FUNCTION) {
                        const FunctionDeclData &fn = ast->GetFunction(mRef);
                        if (fn.name.nameHash == call.name.nameHash) {
                          methodRef = mRef;
                          break;
                        }
                      }
                    }
                    if (!methodRef.IsNull())
                      break;
                  }
                }
              }
              if (!methodRef.IsNull())
                break;
            }

            if (!methodRef.IsNull()) {
              // Get method body (should be a PatternMatch for eval methods)
              const FunctionDeclData &method = ast->GetFunction(methodRef);
              auto savedNodeRegisters = nodeRegisters;

              // Extract the enum tag for dispatch
              u16 tagReg = AllocateRegister();
              builder.EmitInstruction(OP_ENUM_TAG, tagReg, receiverReg);
              SetRegisterType(tagReg, CoreType::INT);

              // For now, emit a simplified dispatch using select operations
              // This avoids the complexity of full switch generation
              // We inline each variant case and use SELECT to pick result

              // Store arguments for use in inlined method body
              // Map 'self' to receiverReg and parameters to args[0..3]
              u16 selfReg = receiverReg;

              // If the method body is a PatternMatch, we need to handle it
              // For simplicity, generate a chain of comparisons + select
              if (method.body.Type() == ASTNodeType::PATTERN_MATCH) {
                const PatternMatchData &pm = ast->GetPatternMatch(method.body);
                auto savedVarRegs = variableRegisters;
                u32 selfHash = Utils::HashStr("self");
                variableRegisters[selfHash] = selfReg;

                // Bind method parameters that are not 'self' once for all arms
                u32 argIdx = 0;
                for (u32 p = 0; p < method.parameters.count && argIdx < 4;
                     p++) {
                  const auto &param = method.parameters[p];
                  if (param.second.nameHash != Utils::HashStr("self") &&
                      param.first.nameHash != 0) {
                    variableRegisters[param.first.nameHash] = args[argIdx++];
                  }
                }
                auto baseVarRegs = variableRegisters;

                auto scalarCountFor = [](CoreType type) -> u32 {
                  switch (type) {
                  case CoreType::FLOAT2:
                  case CoreType::INT2:
                  case CoreType::UINT2:
                  case CoreType::BOOL2:
                    return 2;
                  case CoreType::FLOAT3:
                  case CoreType::INT3:
                  case CoreType::UINT3:
                  case CoreType::BOOL3:
                    return 3;
                  case CoreType::FLOAT4:
                  case CoreType::INT4:
                  case CoreType::UINT4:
                  case CoreType::BOOL4:
                    return 4;
                  default:
                    return 1;
                  }
                };

                // For each arm, check if tag == variantIndex, evaluate body,
                // select
                u16 resultReg = AllocateRegister();
                SetRegisterType(resultReg, method.returnType);

                u16 initValue = EmitZeroConstant(method.returnType);
                builder.EmitInstruction(OP_STORE_REG, resultReg, initValue);

                u16 matchedReg = builder.EmitConstantBool(false);
                u32 underscoreHash = Utils::HashStr("_");

                for (u32 armIdx = 0; armIdx < pm.arms.count; armIdx++) {
                  NodeRef armRef = pm.arms[armIdx];
                  if (armRef.Type() != ASTNodeType::PATTERN_MATCH_ARM) {
                    continue;
                  }
                  const PatternMatchData &arm = ast->GetPatternMatch(armRef);

                  u16 condReg = 0xFFFF;
                  u32 variantIdx = 0xFFFFFFFF;
                  if (!arm.isDefault) {
                    for (u32 v = 0; v < enumData.variants.count; v++) {
                      if (enumData.variants[v].name.nameHash ==
                          arm.variantHash) {
                        variantIdx = v;
                        break;
                      }
                    }
                    if (variantIdx == 0xFFFFFFFF) {
                      continue;
                    }
                    u16 expectedTag = EmitConstantInt(variantIdx);
                    condReg = AllocateRegister();
                    builder.EmitInstruction(OP_IEQ, condReg, tagReg,
                                            expectedTag);
                    SetRegisterType(condReg, CoreType::BOOL);

                    u16 newMatched = AllocateRegister();
                    builder.EmitInstruction(OP_OR, newMatched, matchedReg,
                                            condReg);
                    SetRegisterType(newMatched, CoreType::BOOL);
                    matchedReg = newMatched;
                  } else {
                    u16 notMatched = AllocateRegister();
                    builder.EmitInstruction(OP_NOT, notMatched, matchedReg);
                    SetRegisterType(notMatched, CoreType::BOOL);
                    condReg = notMatched;
                  }

                  variableRegisters = baseVarRegs;

                  if (variantIdx != 0xFFFFFFFF) {
                    const EnumData::Variant &variant =
                        enumData.variants[variantIdx];
                    u32 payloadIndex = 0;
                    u32 bindingIdx = 0;

                    for (u32 t = 0; t < variant.associatedTypes.count; t++) {
                      CoreType assocType = variant.associatedTypes[t];
                      u32 componentCount = scalarCountFor(assocType);
                      if (bindingIdx >= arm.bindings.count) {
                        payloadIndex += componentCount;
                        continue;
                      }

                      const auto &binding = arm.bindings[bindingIdx++];
                      if (binding.first.nameHash == underscoreHash) {
                        payloadIndex += componentCount;
                        continue;
                      }

                      if (componentCount == 1) {
                        u16 fieldReg = AllocateRegister();
                        u16 fieldIdx = EmitConstantInt(payloadIndex);
                        builder.EmitInstruction(OP_ENUM_FIELD, fieldReg,
                                                selfReg, fieldIdx);
                        SetRegisterType(fieldReg, assocType);
                        variableRegisters[binding.first.nameHash] = fieldReg;
                      } else {
                        u16 components[4] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
                        CoreType scalarType = GetScalarComponentType(assocType);
                        for (u32 c = 0; c < componentCount && c < 4; c++) {
                          u16 compReg = AllocateRegister();
                          u16 compIdx = EmitConstantInt(payloadIndex + c);
                          builder.EmitInstruction(OP_ENUM_FIELD, compReg,
                                                  selfReg, compIdx);
                          SetRegisterType(compReg, scalarType);
                          components[c] = compReg;
                        }

                        u16 vecReg = AllocateRegister();
                        builder.EmitInstruction(OP_VEC_CONSTRUCT, vecReg,
                                                components[0], components[1],
                                                components[2], components[3]);
                        program.metadata[builder.currentInstruction - 1] =
                            componentCount;
                        SetRegisterType(vecReg, assocType);
                        variableRegisters[binding.first.nameHash] = vecReg;
                      }

                      payloadIndex += componentCount;
                    }
                  }

                  u16 armResult = 0;
                  if (!arm.body.IsNull()) {
                    armResult = LowerExpression(arm.body);
                  }

                  u16 newResult = AllocateRegister();
                  builder.EmitInstruction(OP_SELECT, newResult, resultReg,
                                          armResult, condReg);
                  SetRegisterType(newResult, method.returnType);
                  resultReg = newResult;
                }

                variableRegisters = savedVarRegs;
                nodeRegisters = savedNodeRegisters;

                builder.EmitInstruction(OP_STORE_REG, dest, resultReg);
                SetRegisterType(dest, method.returnType);
                return dest;
              } else {
                // Non-pattern-match method body (e.g., normal method that calls
                // self.distance) Inline the method body similar to regular
                // function inlining

                // Save variableRegisters to restore after inlining
                auto savedVarRegs = variableRegisters;

                // Bind method parameters to argument registers
                // param.first = name, param.second = type
                u32 argIdx = 0;
                for (u32 p = 0; p < method.parameters.count && argIdx < 4;
                     p++) {
                  const auto &param = method.parameters[p];
                  // Skip 'self' parameter (type is "self")
                  if (param.second.nameHash != Utils::HashStr("self") &&
                      param.first.nameHash != 0) {
                    // Map parameter name to argument register
                    variableRegisters[param.first.nameHash] = args[argIdx++];
                  }
                }

                // Track self for nested method calls
                // Store the receiver in a way the nested calls can find it
                u32 selfHash = Utils::HashStr("self");
                variableRegisters[selfHash] = selfReg;

                // Check if method body is valid
                if (method.body.IsNull()) {
                  // No body - return zero
                  builder.EmitInstruction(OP_LOAD_CONST, dest,
                                          builder.EmitConstant(0.0f));
                  SetRegisterType(dest, method.returnType);
                  nodeRegisters = savedNodeRegisters;
                  return dest;
                }

                // Evaluate the method body
                u16 bodyResult = LowerExpression(method.body);

                // Restore variableRegisters
                variableRegisters = savedVarRegs;
                nodeRegisters = savedNodeRegisters;

                // Store result
                builder.EmitInstruction(OP_STORE_REG, dest, bodyResult);
                SetRegisterType(dest, method.returnType);
                return dest;
              }
            }
          }
        }
      }
    }

    if (call.flags & FunctionCallFlags::IS_INTRINSIC) {
      using Intrinsic = StdLib::Intrinsic;
      Intrinsic intrinsic =
          static_cast<Intrinsic>(StdLib::INTRINSICS[call.intrinsicIndex].enumIndex);

      OpCode op = OP_NOP;

      switch (intrinsic) {
      case Intrinsic::ABS: {
        CoreType argType = GetRegisterType(args[0]);
        op = (mask(argType) & TypeMasks::FLOAT_TYPES) ? OP_FABS : OP_IABS;
        break;
      }
      case Intrinsic::MIN: {
        CoreType argType = GetRegisterType(args[0]);
        if (mask(argType) & TypeMasks::FLOAT_TYPES)
          op = OP_FMIN;
        else if (mask(argType) & TypeMasks::UINT_TYPES)
          op = OP_UMIN;
        else
          op = OP_IMIN;
        break;
      }
      case Intrinsic::MAX: {
        CoreType argType = GetRegisterType(args[0]);
        if (mask(argType) & TypeMasks::FLOAT_TYPES)
          op = OP_FMAX;
        else if (mask(argType) & TypeMasks::UINT_TYPES)
          op = OP_UMAX;
        else
          op = OP_IMAX;
        break;
      }
      case Intrinsic::CLAMP: {
        CoreType argType = GetRegisterType(args[0]);
        if (mask(argType) & TypeMasks::FLOAT_TYPES)
          op = OP_FCLAMP;
        else if (mask(argType) & TypeMasks::UINT_TYPES)
          op = OP_UCLAMP;
        else
          op = OP_ICLAMP;
        break;
      }
      case Intrinsic::MOD: {
        CoreType argType = GetRegisterType(args[0]);
        op = (mask(argType) & TypeMasks::FLOAT_TYPES) ? OP_FMOD : OP_IMOD;
        break;
      }
      case Intrinsic::SINCOS: {
        u16 sinDest = dest;
        u16 cosDest = AllocateRegister();
        builder.EmitInstruction(OP_SIN, sinDest, args[0]);
        builder.EmitInstruction(OP_COS, cosDest, args[0]);
        return sinDest;
      }
      case Intrinsic::BARRIER:
      case Intrinsic::MEMORY_BARRIER:
      case Intrinsic::STORAGE_BARRIER: {
        if (currentStage != ShaderStage::Compute) {
          fprintf(stderr, "Error: barrier intrinsics are only available in "
                          "compute shaders\n");
          return 0;
        }
        op = IntrinsicToOpcode(intrinsic);
        if (op != OP_NOP) {
          builder.EmitInstruction(op, dest, args[0], args[1], args[2]);
          SetRegisterType(dest, CoreType::VOID);
          return dest;
        }
        break;
      }
      case Intrinsic::ATOMIC_ADD:
      case Intrinsic::ATOMIC_MIN:
      case Intrinsic::ATOMIC_MAX:
      case Intrinsic::ATOMIC_AND:
      case Intrinsic::ATOMIC_OR:
      case Intrinsic::ATOMIC_XOR:
      case Intrinsic::ATOMIC_EXCHANGE:
      case Intrinsic::ATOMIC_CMP_EXCHANGE: {
        if (call.arguments.count > 0) {
          args[0] = LowerStoragePointerForAtomic(call.arguments[0]);
        }
        op = IntrinsicToOpcode(intrinsic);
        break;
      }
      // Texture operations need special handling:
      // sample(texture, coord)
      // sample(texture, sampler, coord)
      // sample_lod/bias/cmp(texture, [sampler,] coord, extra)
      // sample_grad(texture, [sampler,] coord, ddx, ddy)
      // Result type is always FLOAT4
      case Intrinsic::SAMPLE:
      case Intrinsic::SAMPLE_LOD:
      case Intrinsic::SAMPLE_BIAS:
      case Intrinsic::SAMPLE_GRAD:
      case Intrinsic::SAMPLE_CMP: {
        OpCode texOp = IntrinsicToOpcode(intrinsic);
        u16 texReg = args[0]; // Texture with 0x2000 marker
        const bool hasExplicitSampler =
            argCount >= 3 && (args[1] & 0xF000) == 0x3000;
        u16 coordReg = hasExplicitSampler ? args[2] : args[1];

        switch (intrinsic) {
          case Intrinsic::SAMPLE:
            builder.EmitInstruction(texOp, dest, texReg, coordReg);
            break;
          case Intrinsic::SAMPLE_LOD:
          case Intrinsic::SAMPLE_BIAS:
          case Intrinsic::SAMPLE_CMP: {
            u16 extraReg = hasExplicitSampler ? args[3] : args[2];
            builder.EmitInstruction(texOp, dest, texReg, coordReg, extraReg);
            break;
          }
          case Intrinsic::SAMPLE_GRAD: {
            u16 ddxReg = hasExplicitSampler ? args[3] : args[2];
            u16 ddyReg = hasExplicitSampler ? args[4] : args[3];
            builder.EmitInstruction(texOp, dest, texReg, coordReg, ddxReg, ddyReg);
            break;
          }
          default:
            break;
        }

        if (hasExplicitSampler) {
          BWSL::SetTextureOpExplicitSamplerMetadata(program,
                                                    builder.currentInstruction - 1,
                                                    static_cast<u16>(args[1] & 0x0FFFu));
        }

        // Texture samples always return float4
        SetRegisterType(dest, CoreType::FLOAT4);
        return dest;
      }
      case Intrinsic::GATHER:
      case Intrinsic::LOAD: {
        OpCode texOp = IntrinsicToOpcode(intrinsic);
        u16 texReg = args[0]; // Texture with 0x2000 marker
        // Coordinate is the last argument - handle both 2-arg and 3-arg forms
        u16 coordReg = (argCount >= 3) ? args[2] : args[1];

        // Emit with texture in operand 0 (preserving 0x2000 marker for
        // analysis) and coordinates in operand 1
        builder.EmitInstruction(texOp, dest, texReg, coordReg);

        SetRegisterType(dest, CoreType::FLOAT4);
        return dest;
      }
      default:
        op = IntrinsicToOpcode(intrinsic);
        break;
      }

      if (op != OP_NOP) {
        builder.EmitInstruction(op, dest, args[0], args[1], args[2]);

        // Set the result type based on the intrinsic
        // Most intrinsics return the same type as their first argument
        // Exceptions: length, dot, distance return scalar float
        CoreType resultType = CoreType::FLOAT;
        switch (intrinsic) {
        case Intrinsic::LENGTH:
        case Intrinsic::DISTANCE:
        case Intrinsic::DOT:
          resultType = CoreType::FLOAT;
          break;
        case Intrinsic::ANY:
        case Intrinsic::ALL:
          resultType = CoreType::BOOL;
          break;
        case Intrinsic::IS_NAN:
        case Intrinsic::IS_INF: {
          // isnan/isinf return a bool (or bvec matching input width).
          CoreType argType = argCount > 0 ? GetRegisterType(args[0]) : CoreType::FLOAT;
          switch (argType) {
            case CoreType::FLOAT2: resultType = CoreType::BOOL2; break;
            case CoreType::FLOAT3: resultType = CoreType::BOOL3; break;
            case CoreType::FLOAT4: resultType = CoreType::BOOL4; break;
            default:               resultType = CoreType::BOOL;  break;
          }
          break;
        }
        default:
          // Most intrinsics return the same type as first arg
          if (argCount > 0) {
            resultType = GetRegisterType(args[0]);
          }
          break;
        }
        SetRegisterType(dest, resultType);
      }
    } else {
      // Check if it's a type constructor (float4, float3, int4, etc.)
      CoreType constructedType = LookupCoreType(call.name.nameHash);
      // GENERIC_T/U/V collide with short user struct names (`T`, `U`,
      // `V`). A user-defined struct/enum with that name should win, so
      // `V(a, b)` doesn't silently degrade into a vec-construct call.
      if (constructedType == CoreType::GENERIC_T ||
          constructedType == CoreType::GENERIC_U ||
          constructedType == CoreType::GENERIC_V) {
        Symbol *userSym = SymbolTable::LookupByHash(
            const_cast<SymbolTableData *>(symbols), call.name.nameHash);
        if (userSym && (userSym->kind == SymbolKind::CUSTOM_TYPE ||
                        userSym->kind == SymbolKind::ENUM ||
                        userSym->kind == SymbolKind::ENUM_SYMBOL)) {
          constructedType = CoreType::INVALID;
        }
      }
      if (constructedType != CoreType::INVALID &&
          constructedType != CoreType::VOID) {
        // Check if this is a scalar type conversion (float(x), int(x), uint(x))
        bool isScalarConversion = (constructedType == CoreType::FLOAT ||
                                   constructedType == CoreType::INT ||
                                   constructedType == CoreType::UINT) &&
                                  argCount == 1;

        if (isScalarConversion) {
          // Scalar type conversion - emit appropriate conversion opcode
          CoreType srcType = GetRegisterType(args[0]);
          if (srcType == constructedType) {
            return args[0];
          }
          OpCode convOp = OP_NOP;

          if (constructedType == CoreType::FLOAT) {
            if (srcType == CoreType::INT)
              convOp = OP_I2F;
            else if (srcType == CoreType::UINT)
              convOp = OP_U2F;
            else if (srcType == CoreType::BOOL) {
              // Bool to float: use select(0.0, 1.0, bool)
              // This becomes OpSelect in SPIR-V
              u16 zero = builder.EmitConstant(0.0f);
              u16 one = builder.EmitConstant(1.0f);
              builder.EmitInstruction(OP_SELECT, dest, zero, one, args[0]);
              SetRegisterType(dest, CoreType::FLOAT);
              return dest;
            } else
              convOp = OP_I2F; // Default to I2F
          } else if (constructedType == CoreType::INT) {
            if (srcType == CoreType::BOOL) {
              // Bool to int: use select(0, 1, bool)
              u16 zero = EmitConstantInt(0);
              u16 one = EmitConstantInt(1);
              builder.EmitInstruction(OP_SELECT, dest, zero, one, args[0]);
              SetRegisterType(dest, CoreType::INT);
              return dest;
            }
            if (srcType == CoreType::FLOAT)
              convOp = OP_F2I;
            else if (srcType == CoreType::UINT)
              convOp = OP_U2I;
            else
              convOp = OP_F2I;
          } else if (constructedType == CoreType::UINT) {
            if (srcType == CoreType::BOOL) {
              // Bool to uint: use select(0, 1, bool)
              u16 zero = EmitConstantUint(0);
              u16 one = EmitConstantUint(1);
              builder.EmitInstruction(OP_SELECT, dest, zero, one, args[0]);
              SetRegisterType(dest, CoreType::UINT);
              return dest;
            }
            if (srcType == CoreType::FLOAT)
              convOp = OP_F2U;
            else if (srcType == CoreType::INT)
              convOp = OP_I2U;
            else
              convOp = OP_F2U;
          }

          if (convOp != OP_NOP) {
            builder.EmitInstruction(convOp, dest, args[0]);
            SetRegisterType(dest, constructedType);
          }
        } else if (constructedType == CoreType::MAT2 ||
                   constructedType == CoreType::MAT3 ||
                   constructedType == CoreType::MAT4) {
          // Matrix type constructor
          // mat2 = 2 columns of vec2 (4 floats or 2 vec2s)
          // mat3 = 3 columns of vec3 (9 floats or 3 vec3s)
          // mat4 = 4 columns of vec4 (16 floats or 4 vec4s)
          u32 numColumns = (constructedType == CoreType::MAT2)   ? 2
                           : (constructedType == CoreType::MAT3) ? 3
                                                                 : 4;
          u32 numRows = numColumns;
          CoreType columnType = (numColumns == 2)   ? CoreType::FLOAT2
                                : (numColumns == 3) ? CoreType::FLOAT3
                                                    : CoreType::FLOAT4;

          // Check if arguments are already column vectors (e.g., mat4(vec4,
          // vec4, vec4, vec4))
          bool argsAreColumnVectors = (argCount == numColumns);
          if (argsAreColumnVectors) {
            for (u32 i = 0; i < argCount; i++) {
              CoreType argType = GetRegisterType(args[i]);
              if (argType != columnType) {
                argsAreColumnVectors = false;
                break;
              }
            }
          }

          u16 columnRegs[4];
          if (argsAreColumnVectors) {
            // Arguments are already column vectors - use them directly
            for (u32 col = 0; col < numColumns; col++) {
              columnRegs[col] = args[col];
            }
          } else {
            // Arguments are scalars - build column vectors
            for (u32 col = 0; col < numColumns; col++) {
              columnRegs[col] = AllocateRegister();
              SetRegisterType(columnRegs[col], columnType);

              // Get the scalars for this column
              u16 s0 = (col * numRows + 0 < argCount) ? args[col * numRows + 0]
                                                      : 0xFFFF;
              u16 s1 = (col * numRows + 1 < argCount) ? args[col * numRows + 1]
                                                      : 0xFFFF;
              u16 s2 = (numRows >= 3 && col * numRows + 2 < argCount)
                           ? args[col * numRows + 2]
                           : 0xFFFF;
              u16 s3 = (numRows >= 4 && col * numRows + 3 < argCount)
                           ? args[col * numRows + 3]
                           : 0xFFFF;

              builder.EmitInstruction(OP_VEC_CONSTRUCT, columnRegs[col], s0, s1,
                                      s2, s3);
              program.metadata[builder.currentInstruction - 1] = numRows;
            }
          }

          // Now construct the matrix from column vectors
          u16 c0 = columnRegs[0];
          u16 c1 = columnRegs[1];
          u16 c2 = (numColumns >= 3) ? columnRegs[2] : 0xFFFF;
          u16 c3 = (numColumns >= 4) ? columnRegs[3] : 0xFFFF;
          builder.EmitInstruction(OP_MAT_CONSTRUCT, dest, c0, c1, c2, c3);
          program.metadata[builder.currentInstruction - 1] = numColumns;
          SetRegisterType(dest, constructedType);
        } else {
          // Vector type constructor - emit OP_VEC_CONSTRUCT with up to 4
          // operands Use 0xFFFF as sentinel for unused operands
          u16 op0 = argCount > 0 ? args[0] : 0xFFFF;
          u16 op1 = argCount > 1 ? args[1] : 0xFFFF;
          u16 op2 = argCount > 2 ? args[2] : 0xFFFF;
          u16 op3 = argCount > 3 ? args[3] : 0xFFFF;
          builder.EmitInstruction(OP_VEC_CONSTRUCT, dest, op0, op1, op2, op3);
          // Store arg count in metadata for SPIR-V backend
          program.metadata[builder.currentInstruction - 1] = argCount;
          SetRegisterType(dest, constructedType);
        }
      } else {
        // Try to inline user function call
        u16 inlinedResult = TryInlineFunction(call, args, argCount);
        if (inlinedResult != 0xFFFF) {
          // Successfully inlined - copy result to dest
          builder.EmitInstruction(OP_STORE_REG, dest, inlinedResult);
          // Inherit type from inlined result
          CoreType resultType = GetRegisterType(inlinedResult);
          if (resultType != CoreType::INVALID) {
            SetRegisterType(dest, resultType);
          }
          if ((resultType == CoreType::CUSTOM ||
               resultType == CoreType::ENUM) &&
              dest < MAX_REGISTERS && inlinedResult < MAX_REGISTERS) {
            u32 structHash = program.registerStructTypes[inlinedResult];
            if (structHash != 0) {
              program.registerStructTypes[dest] = structHash;
            }
          }
        } else {
          // Check for a user-struct positional constructor: `V(a, b, c)`
          // where V is a declared struct type. Previously fell through to
          // OP_CALL which emits OpUndef. For structs with up to 4 fields
          // we can fit the args into OP_STRUCT_CONSTRUCT directly (4
          // operand slots). For structs with more fields, start from an
          // undef composite and chain OP_STRUCT_INSERT per field so the
          // backend still produces a fully-populated OpCompositeInsert
          // sequence that matches the struct member count.
          u32 customHash = 0;
          CoreType resolved =
              ResolveCoreTypeFromHash(call.name.nameHash, &customHash);
          if (resolved == CoreType::CUSTOM && customHash != 0) {
            u32 structTypeHash = LookupOrRegisterStructType(customHash);
            if (structTypeHash != 0) {
              auto it = structTypeMap.find(structTypeHash);
              u32 fieldCount = 0;
              if (it != structTypeMap.end()) {
                fieldCount = program.structTypes[it->second].fieldCount;
              }

              if (argCount <= 4 && fieldCount <= 4) {
                u16 op0 = argCount > 0 ? args[0] : 0xFFFF;
                u16 op1 = argCount > 1 ? args[1] : 0xFFFF;
                u16 op2 = argCount > 2 ? args[2] : 0xFFFF;
                u16 op3 = argCount > 3 ? args[3] : 0xFFFF;
                builder.EmitInstruction(OP_STRUCT_CONSTRUCT, dest, op0, op1,
                                        op2, op3);
                program.metadata[builder.currentInstruction - 1] =
                    structTypeHash;
                SetRegisterType(dest, CoreType::CUSTOM);
                if (dest < MAX_REGISTERS) {
                  program.registerStructTypes[dest] = structTypeHash;
                }
                return dest;
              }

              // Struct has more than 4 fields (or user passed more than
              // 4 positional args). Build via undef-base + chained
              // STRUCT_INSERTs.
              u16 base = AllocateRegister();
              SetRegisterType(base, CoreType::CUSTOM);
              if (base < MAX_REGISTERS) {
                program.registerStructTypes[base] = structTypeHash;
              }
              AddUndefRegister(base, CoreType::CUSTOM);

              u16 current = base;
              u32 slots = fieldCount > 0 ? fieldCount : argCount;
              if (slots > argCount) slots = argCount;
              for (u32 i = 0; i < slots; i++) {
                u16 next = AllocateRegister();
                builder.EmitInstruction(OP_STRUCT_INSERT, next, current,
                                        static_cast<u16>(i), args[i]);
                program.metadata[builder.currentInstruction - 1] =
                    structTypeHash;
                SetRegisterType(next, CoreType::CUSTOM);
                if (next < MAX_REGISTERS) {
                  program.registerStructTypes[next] = structTypeHash;
                }
                current = next;
              }

              // Copy the final composite into the originally-allocated
              // `dest` so subsequent code that references the call's
              // destination sees the fully-built struct.
              builder.EmitInstruction(OP_STORE_REG, dest, current);
              SetRegisterType(dest, CoreType::CUSTOM);
              if (dest < MAX_REGISTERS) {
                program.registerStructTypes[dest] = structTypeHash;
              }
              return dest;
            }
          }

          // Fallback: emit OP_CALL (will produce OpUndef in SPIR-V)
          builder.EmitInstruction(OP_CALL, dest, call.name.nameHash);
          program.metadata[builder.currentInstruction - 1] =
              (args[0] << 16) | (args[1] << 8) | args[2];
        }
      }
    }

    return dest;
  }

  // Try to inline a function call, returns the result register or 0xFFFF if
  // inlining failed
  u16 TryInlineFunction(const FunctionCallData &call, u16 *args, u32 argCount) {
    // Look up the function in the AST
    // Check if this is a module-qualified call (e.g.,
    // Globals::decompressPosition)
    NodeRef funcRef;
    u32 foundModuleIndex =
        0xFFFFFFFF; // Track which module we found the function in
    std::vector<OverloadTypeMask> argMasks;
    argMasks.reserve(argCount);
    for (u32 i = 0; i < argCount; i++) {
      CoreType argType = GetRegisterType(args[i]);
      u32 customHash = 0;
      if (argType == CoreType::CUSTOM) {
        customHash = program.registerStructTypes[args[i]];
      }
      argMasks.push_back(MakeOverloadMask(argType, customHash));
    }

    auto matchesSignature = [&](const FunctionDeclData &fn) -> bool {
      if (fn.parameters.count != argCount)
        return false;
      for (u32 i = 0; i < argCount; i++) {
        OverloadTypeMask paramMask = MakeOverloadMaskFromResolvedTypeHash(
            fn.parameters[i].second.nameHash);
        if (!OverloadMaskMatches(paramMask, argMasks[i])) {
          // Overload rejection is normal during resolution — caller
          // tries multiple candidates. Downstream "Function not found"
          // error covers the truly-unresolvable case.
          return false;
        }
      }
      return true;
    };

    if (call.moduleIndex != 0xFFFFFFFF &&
        call.moduleIndex < ast->modules.count) {
      // Module-qualified call - search in the module's function list
      const ModuleNodeData &module = ast->modules[call.moduleIndex];

      for (u32 i = 0; i < module.functions.count; i++) {
        NodeRef fnRef = module.functions[i];
        if (fnRef.Type() == ASTNodeType::FUNCTION) {
          const FunctionDeclData &fn = ast->GetFunction(fnRef);
          if (fn.name.nameHash == call.name.nameHash && matchesSignature(fn)) {
            funcRef = fnRef;
            foundModuleIndex = call.moduleIndex;
            break;
          }
        }
      }
    }

    // Also try looking up by qualified hash in all modules
    if (funcRef.IsNull() && call.moduleQualifiedHash != 0) {
      for (u32 m = 0; m < ast->modules.count; m++) {
        const ModuleNodeData &module = ast->modules[m];

        for (u32 i = 0; i < module.functions.count; i++) {
          NodeRef fnRef = module.functions[i];
          if (fnRef.Type() == ASTNodeType::FUNCTION) {
            const FunctionDeclData &fn = ast->GetFunction(fnRef);
            // Check both plain name and qualified hash match
            if (fn.name.nameHash == call.name.nameHash ||
                fn.name.nameHash == call.moduleQualifiedHash) {
              if (!matchesSignature(fn)) {
                continue;
              }
              funcRef = fnRef;
              foundModuleIndex = m;
              break;
            }
          }
        }
        if (!funcRef.IsNull())
          break;
      }
    }

    // For unqualified calls during inlining, search in the current inline
    // module
    if (funcRef.IsNull() && inlineModuleIndex != 0xFFFFFFFF &&
        inlineModuleIndex < ast->modules.count) {
      const ModuleNodeData &module = ast->modules[inlineModuleIndex];
      for (u32 i = 0; i < module.functions.count; i++) {
        NodeRef fnRef = module.functions[i];
        if (fnRef.Type() == ASTNodeType::FUNCTION) {
          const FunctionDeclData &fn = ast->GetFunction(fnRef);
          if (fn.name.nameHash == call.name.nameHash && matchesSignature(fn)) {
            funcRef = fnRef;
            foundModuleIndex = inlineModuleIndex;
            break;
          }
        }
      }
    }

    // Try looking up in the current pass's function list (pass-scoped
    // functions)
    if (funcRef.IsNull() && !currentPass.IsNull()) {
      const PassData &pass = ast->GetPass(currentPass);
      for (u32 i = 0; i < pass.functions.count; i++) {
        NodeRef fnRef = pass.functions[i];
        if (fnRef.Type() == ASTNodeType::FUNCTION) {
          const FunctionDeclData &fn = ast->GetFunction(fnRef);
          if (fn.name.nameHash == call.name.nameHash && matchesSignature(fn)) {
            funcRef = fnRef;
            break;
          }
        }
      }
    }

    // Also try looking up in the current pipeline's function list
    if (funcRef.IsNull() && !currentPipeline.IsNull()) {
      const PipelineData &pipeline = ast->GetPipeline(currentPipeline);
      for (u32 i = 0; i < pipeline.functions.count; i++) {
        NodeRef fnRef = pipeline.functions[i];
        if (fnRef.Type() == ASTNodeType::FUNCTION) {
          const FunctionDeclData &fn = ast->GetFunction(fnRef);
          if (fn.name.nameHash == call.name.nameHash && matchesSignature(fn)) {
            funcRef = fnRef;
            break;
          }
        }
      }
    }

    // If regular function not found, try generic function resolution
    if (funcRef.IsNull()) {
      funcRef = TryResolveGenericFunction(call, args, argCount);
    }

    if (funcRef.IsNull()) {
      // "Function not found" is a normal lookup outcome during
      // constructor calls and intrinsics — the parent dispatch has
      // its own handling for truly-unresolved symbols. Staying silent
      // here keeps end-user stderr clean.
      return 0xFFFF;
    }

    const FunctionDeclData &func = ast->GetFunction(funcRef);

    // Skip eval functions for now (they should be evaluated at compile time)
    if (func.isEval) {
      return 0xFFFF;
    }

    if (func.body.IsNull()) {
      return 0xFFFF; // No function body to inline
    }

    // Direct / indirect recursion check. SPIR-V has no call-stack
    // semantics — OpFunctionCall cannot target a caller in its own
    // chain. Previously recursive calls silently produced OpUndef
    // operands; validation then surfaced a cryptic "Expected int scalar
    // or vector type" error far from the root cause.
    for (u32 i = 0; i < inlineStackDepth; i++) {
      if (inlineStackPacked[i] == funcRef.packed) {
        if (!recursionDiagnosed) {
          fprintf(stderr,
                  "Error: recursion is not supported — a function calls "
                  "itself (directly or through another function). SPIR-V "
                  "execution is stack-less; rewrite the algorithm "
                  "iteratively.\n");
          recursionDiagnosed = true;
        }
        return 0xFFFF;
      }
    }

    // Check inline depth to prevent infinite recursion
    if (inlineDepth >= MAX_INLINE_DEPTH) {
      return 0xFFFF;
    }

    // Save current variable register mappings, struct types, and node register
    // cache Node register cache must be saved because we're re-lowering the
    // same AST nodes and each inlining needs fresh computations (not cached
    // results from previous inlining)
    auto savedVariableRegisters = variableRegisters;
    auto savedVariableStructTypes = variableStructTypes;
    auto savedNodeRegisters = nodeRegisters;

    // Bind parameters to argument registers
    u32 paramCount =
        func.parameters.count < argCount ? func.parameters.count : argCount;
    for (u32 i = 0; i < paramCount; i++) {
      const auto &param = func.parameters[i];
      u32 paramNameHash = param.first.nameHash; // Parameter name
      variableRegisters[paramNameHash] = args[i];

      // Also set the type for the parameter based on the type name
      // The second element of the pair is the type name (e.g., "uint",
      // "float3")
      u32 paramTypeHash = 0;
      CoreType paramType =
          ResolveCoreTypeFromHash(param.second.nameHash, &paramTypeHash);

      if (paramType != CoreType::INVALID && paramType != CoreType::VOID) {
        SetRegisterType(args[i], paramType);
        if ((paramType == CoreType::CUSTOM || paramType == CoreType::ENUM) &&
            paramTypeHash != 0) {
          u32 structHash = LookupOrRegisterStructType(paramTypeHash);

          if (structHash != 0) {
            program.registerStructTypes[args[i]] = structHash;
            // Also set variableStructTypes for local struct member access
            // (e.g., mat.albedo)
            variableStructTypes[paramNameHash] = structHash;
          }
        }
      }
    }

    // Create a register for the return value
    u16 returnReg = AllocateRegister();

    // Save the current return register and module index for nested inlining
    u16 savedReturnReg = inlineReturnReg;
    u16 savedReturnFlagReg = inlineReturnFlagReg;
    u32 savedReturnCounter = inlineReturnCounter;
    u32 savedModuleIndex = inlineModuleIndex;
    inlineReturnReg = returnReg;
    inlineReturnFlagReg = AllocateRegister();
    SetRegisterType(inlineReturnFlagReg, CoreType::BOOL);
    builder.EmitInstruction(OP_STORE_REG, inlineReturnFlagReg,
                            builder.EmitConstantBool(false));
    inlineReturnCounter = 0;
    inlineModuleIndex =
        foundModuleIndex; // Set module context for nested unqualified calls
    inlineDepth++;
    if (inlineStackDepth < MAX_INLINE_DEPTH) {
      inlineStackPacked[inlineStackDepth++] = funcRef.packed;
    }

    // Lower the function body
    if (func.body.Type() == ASTNodeType::BLOCK) {
      LowerBlock(func.body);
    } else {
      // Single expression body
      u16 exprResult = LowerExpression(func.body);
      builder.EmitInstruction(OP_STORE_REG, returnReg, exprResult);
    }

    // Restore state
    if (inlineStackDepth > 0) {
      inlineStackDepth--;
    }
    inlineDepth--;
    inlineReturnReg = savedReturnReg;
    inlineReturnFlagReg = savedReturnFlagReg;
    inlineReturnCounter = savedReturnCounter;
    inlineModuleIndex = savedModuleIndex;
    variableRegisters = savedVariableRegisters;
    variableStructTypes = savedVariableStructTypes;
    nodeRegisters = savedNodeRegisters;

    // Set return type
    if (func.returnType != CoreType::INVALID &&
        func.returnType != CoreType::VOID) {
      SetRegisterType(returnReg, func.returnType);
    }

    return returnReg;
  }

  // Get type name hash for a CoreType using TypeHashes constants
  static u32 GetCoreTypeNameHash(CoreType type) {
    return SymbolTable::GetCoreTypeNameHash(type);
  }

  // Try to resolve a generic function call by finding or instantiating a
  // specialization
  NodeRef TryResolveGenericFunction(const FunctionCallData &call, u16 *args,
                                    u32 argCount) {
    // Get argument types for constraint checking
    CoreType argTypes[16];
    bool isConstrained[16];
    if (argCount > 16)
      return NodeRef::Null();

    for (u32 i = 0; i < argCount; i++) {
      argTypes[i] = GetRegisterType(args[i]);
      isConstrained[i] = false; // Will be set based on generic function
    }

    // Find a matching generic function (symbols is const, need to cast for
    // lookup)
    SymbolTableData *mutableSymbols = const_cast<SymbolTableData *>(symbols);
    GenericFunctionData *gfn = SymbolTable::FindMatchingGenericFunction(
        mutableSymbols, call.name.nameHash, argTypes, argCount);

    if (!gfn) {
      return NodeRef::Null(); // No matching generic function
    }

    // Build isConstrained array from the generic function's parameter info
    for (u32 i = 0; i < argCount && i < gfn->parameters.count; i++) {
      isConstrained[i] = gfn->parameters[i].isConstrained;
    }

    // Compute specialization hash
    u64 specHash = SpecializationRegistry::HashSpecialization(
        argTypes, isConstrained, argCount);

    // Check if specialization already exists
    u32 existingFunc =
        mutableSymbols->specializationRegistry.Find(gfn->nameHash, specHash);
    if (existingFunc != UINT32_MAX) {
      // Return the existing specialized function's AST node
      // Look it up in the functions array
      for (u32 i = 0; i < ast->functions.count; i++) {
        NodeRef fnRef(ASTNodeType::FUNCTION, i);
        if (ast->GetFunction(fnRef).name.nameHash == existingFunc) {
          return fnRef;
        }
      }
    }

    // Need to instantiate a new specialization
    // Build type substitutions
    ASTClone::TypeSubstitution substitutions[16];
    u32 subCount = 0;

    for (u32 i = 0; i < gfn->parameters.count && i < argCount; i++) {
      if (gfn->parameters[i].isConstrained) {
        // Check if we already have this constraint in substitutions
        bool found = false;
        for (u32 j = 0; j < subCount; j++) {
          if (substitutions[j].constraintHash ==
              gfn->parameters[i].typeName.nameHash) {
            found = true;
            break;
          }
        }
        if (!found) {
          // Create ArenaString with the type's hash (hash-only, no source
          // backing)
          ArenaString typeStr;
          typeStr.nameHash = GetCoreTypeNameHash(argTypes[i]);
          typeStr.sourceOffset = 0;
          typeStr.nameLength = 0;

          substitutions[subCount].constraintHash =
              gfn->parameters[i].typeName.nameHash;
          substitutions[subCount].concreteType = typeStr;
          substitutions[subCount].coreType = argTypes[i];
          subCount++;
        }
      }
    }

    // Build mangled name
    std::string mangledName = SymbolTable::MangleSpecializationName(
        gfn->name, argTypes, isConstrained, argCount);

    // Clone the generic function with type substitutions
    NodeRef srcFuncRef(ASTNodeType::FUNCTION, 0);
    // Find the source function by AST index
    for (u32 i = 0; i < ast->functions.count; i++) {
      if (NodeRef(ASTNodeType::FUNCTION, i).packed == gfn->astNodeIndex) {
        srcFuncRef = NodeRef(ASTNodeType::FUNCTION, i);
        break;
      }
    }

    if (srcFuncRef.IsNull()) {
      return NodeRef::Null();
    }

    NodeRef specializedFunc = ASTClone::CloneFunction(
        ast, ast->arena, srcFuncRef, substitutions, subCount, mangledName);

    if (specializedFunc.IsNull()) {
      return NodeRef::Null();
    }

    // Determine return type for the specialized function
    CoreType returnType = ast->GetFunction(srcFuncRef).returnType;
    if (gfn->returnMatchesParam >= 0 &&
        gfn->returnMatchesParam < static_cast<s8>(argCount)) {
      // Return type matches a parameter's type
      returnType = argTypes[gfn->returnMatchesParam];
    } else if (gfn->returnConstraint != 0) {
      // Return type is a constraint - use the first matching substitution
      for (u32 i = 0; i < subCount; i++) {
        if (substitutions[i].constraintHash == gfn->returnTypeName.nameHash) {
          returnType = substitutions[i].coreType;
          break;
        }
      }
    }

    // Update the specialized function's return type
    ast->GetFunction(specializedFunc).returnType = returnType;

    // Register the specialization
    mutableSymbols->specializationRegistry.Register(
        gfn->nameHash, specHash,
        ast->GetFunction(specializedFunc).name.nameHash);

    return specializedFunc;
  }

  u16 LowerTextureSample(NodeRef ref) {
    const FunctionCallData &call = ast->GetFunctionCall(ref);

    u16 texReg = LowerExpression(call.arguments[0]);
    u16 coordReg = LowerExpression(call.arguments[1]);
    u16 dest = AllocateRegister();

    // IR format: s0 = texture (with 0x2000 marker for slot), s1 = coordinate
    if (texReg & 0x2000) {
      // texReg already has 0x2000 marker, pass it directly
      builder.EmitInstruction(OP_TEX_SAMPLE, dest, texReg, coordReg);
    } else {
      // Bindless - texReg is a register containing texture handle
      builder.EmitInstruction(OP_TEX_SAMPLE, dest, texReg, coordReg);
    }

    return dest;
  }

  //==========================================================================
  // Helper functions
  //==========================================================================

  u16 EnsureBoolCondition(u16 condReg) {
    CoreType condType = GetRegisterType(condReg);
    if (condType == CoreType::BOOL) {
      return condReg;
    }

    u16 boolReg = AllocateRegister();
    SetRegisterType(boolReg, CoreType::BOOL);

    if (mask(condType) & TypeMasks::FLOAT_TYPES) {
      u16 zero = builder.EmitConstant(0.0f);
      builder.EmitInstruction(OP_FNE, boolReg, condReg, zero);
    } else if (mask(condType) & TypeMasks::UINT_TYPES) {
      u16 zero = EmitConstantUint(0);
      builder.EmitInstruction(OP_INE, boolReg, condReg, zero);
    } else {
      u16 zero = EmitConstantInt(0);
      builder.EmitInstruction(OP_INE, boolReg, condReg, zero);
    }

    return boolReg;
  }

  u16 CombineLoopCondition(u16 condReg) {
    if (inlineDepth == 0 || inlineReturnFlagReg == 0xFFFF) {
      return condReg;
    }

    u16 boolCond = EnsureBoolCondition(condReg);
    u16 notReturn = AllocateRegister();
    builder.EmitInstruction(OP_NOT, notReturn, inlineReturnFlagReg);
    SetRegisterType(notReturn, CoreType::BOOL);

    u16 combined = AllocateRegister();
    builder.EmitInstruction(OP_AND, combined, boolCond, notReturn);
    SetRegisterType(combined, CoreType::BOOL);
    return combined;
  }

  u16 AllocateRegister() {
    // Cap at MAX_REGISTERS - 1. Pathological inputs can exhaust u16 register
    // space; once past MAX_REGISTERS, registerStorageInfo / registerTypes
    // indexing OOBs. Returning the same sentinel repeatedly produces bad
    // SPIR-V (SPIR-V validation will reject it) but avoids the crash.
    if (builder.nextRegister >= MAX_REGISTERS - 1) {
      return MAX_REGISTERS - 1;
    }
    u16 reg = builder.nextRegister++;
    if (reg >= program.registerCount) {
      program.registerCount = reg + 1;
    }
    return reg;
  }

  u16 EmitConstantInt(u32 value) {
    for (u32 i = 0; i < program.intCount; i++) {
      if (program.intConstants[i] == value) {
        return 0x4000 | i;
      }
    }
    u32 slot = program.intCount++;
    program.intConstants[slot] = value;
    return 0x4000 | slot;
  }

  u16 EmitConstantUint(u32 value) {
    for (u32 i = 0; i < program.uintCount; i++) {
      if (program.uintConstants[i] == value) {
        return 0x2000 | i;
      }
    }
    u32 slot = program.uintCount++;
    program.uintConstants[slot] = value;
    return 0x2000 | slot;
  }

  u16 GetOrAllocateVariable(u32 nameHash) {
    auto it = variableRegisters.find(nameHash);
    if (it != variableRegisters.end()) {
      return it->second;
    }
    u16 reg = AllocateRegister();
    variableRegisters[nameHash] = reg;
    return reg;
  }

  // ==========================================================================
  // Output slot helpers for varying management
  // ==========================================================================

  // Check if an output name is a builtin (stage-aware).
  // - Position is always a builtin output.
  // - Color/depth are builtin outputs only in fragment stage.
  bool IsBuiltinOutput(u32 nameHash) {
    static const u32 HASH_POSITION = Utils::HashStr("position");
    static const u32 HASH_COLOR = Utils::HashStr("color");
    static const u32 HASH_DEPTH = Utils::HashStr("depth");
    if (nameHash == HASH_POSITION)
      return true;
    if (currentStage == ShaderStage::Fragment) {
      return nameHash == HASH_COLOR || nameHash == HASH_DEPTH;
    }
    return false;
  }

  // Get the output slot for builtin outputs
  u32 GetBuiltinOutputSlot(u32 nameHash) {
    static const u32 HASH_POSITION = Utils::HashStr("position");
    static const u32 HASH_COLOR = Utils::HashStr("color");
    static const u32 HASH_DEPTH = Utils::HashStr("depth");
    if (nameHash == HASH_POSITION)
      return OutputSlot::POSITION;
    if (nameHash == HASH_COLOR)
      return OutputSlot::COLOR;
    if (nameHash == HASH_DEPTH)
      return OutputSlot::DEPTH;
    return OutputSlot::VARYING0; // Fallback
  }

  CoreType GetBuiltinOutputType(u32 nameHash) {
    static const u32 HASH_POSITION = Utils::HashStr("position");
    static const u32 HASH_COLOR = Utils::HashStr("color");
    static const u32 HASH_DEPTH = Utils::HashStr("depth");
    if (nameHash == HASH_POSITION || nameHash == HASH_COLOR)
      return CoreType::FLOAT4;
    if (nameHash == HASH_DEPTH)
      return CoreType::FLOAT;
    return CoreType::INVALID;
  }

  u32 ResolveOutputSlotForStore(u32 nameHash, CoreType valueType,
                                const char *nameStr) {
    if (IsBuiltinOutput(nameHash)) {
      return GetBuiltinOutputSlot(nameHash);
    }
    if (currentPassVaryings && currentStage == ShaderStage::Vertex) {
      u32 varyingIndex =
          currentPassVaryings->AddOrGetSlot(nameHash, valueType, nameStr);
      return OutputSlot::VARYING0 + varyingIndex;
    }
    return OutputSlot::VARYING0;
  }

  u32 ResolveOutputSlotForLoad(u32 nameHash) {
    if (IsBuiltinOutput(nameHash)) {
      return GetBuiltinOutputSlot(nameHash);
    }
    if (currentPassVaryings && currentStage == ShaderStage::Vertex) {
      s32 varyingIndex = currentPassVaryings->GetSlot(nameHash);
      if (varyingIndex >= 0) {
        return OutputSlot::VARYING0 + (u32)varyingIndex;
      }
    }
    return OutputSlot::VARYING0;
  }

  CoreType ResolveOutputTypeForLoad(u32 nameHash) {
    CoreType builtinType = GetBuiltinOutputType(nameHash);
    if (builtinType != CoreType::INVALID) {
      return builtinType;
    }
    if (currentPassVaryings && currentStage == ShaderStage::Vertex) {
      s32 varyingIndex = currentPassVaryings->GetSlot(nameHash);
      if (varyingIndex >= 0) {
        return currentPassVaryings->GetType((u32)varyingIndex);
      }
    }
    return CoreType::FLOAT4;
  }

  u32 GetAttributeIndex(u32 nameHash) {
    // Look up attribute by name in current pipeline's attribute list
    // Returns the declaration-order index (position is always 0)
    if (currentPipeline.IsNull())
      return 0;

    const PipelineData &pipeline = ast->GetPipeline(currentPipeline);
    for (u32 i = 0; i < pipeline.attributes.count; i++) {
      NodeRef attrRef = pipeline.attributes[i];
      if (attrRef.Type() == ASTNodeType::ATTRIBUTE_DECL) {
        const AttributeDeclData &attr = ast->GetAttributeDecl(attrRef);
        if (attr.name.nameHash == nameHash) {
          return attr.attributeIndex;
        }
      }
    }
    return 0; // Fallback to position
  }

  u32 GetInputSlotIndex(u32 nameHash) {
    // Fragment shader input slot indices (matching vertex output locations)
    // For varyings passed from vertex to fragment shader
    //
    // IMPORTANT: Slots must match vertex output slots which use VARYING0 +
    // index so fragment inputs must also use VARYING0 + index for locations to
    // match

    // First, check the pass varying context for dynamic lookup
    // This allows any name used in output.xxx to be used in input.xxx
    if (currentPassVaryings) {
      s32 varyingIndex = currentPassVaryings->GetSlot(nameHash);
      if (varyingIndex >= 0) {
        // Return VARYING0 + index to match vertex output slot assignments
        return OutputSlot::VARYING0 + (u32)varyingIndex;
      }
      // Not found in vertex outputs - fall through to legacy mappings
    }

    // Fallback to legacy hardcoded mappings for backward compatibility
    // These also need to use VARYING0 + offset to match vertex outputs
    static const u32 NORMAL_HASH = Utils::HashStr("normal");
    static const u32 TEXCOORD_HASH = Utils::HashStr("texcoord");
    static const u32 COLOR_HASH = Utils::HashStr("color");
    static const u32 TANGENT_HASH = Utils::HashStr("tangent");
    static const u32 WORLDPOS_HASH = Utils::HashStr("worldPosition");

    // Slot indices use VARYING0 + offset to match vertex output slots
    if (nameHash == NORMAL_HASH)
      return OutputSlot::VARYING0 + 0;
    if (nameHash == TEXCOORD_HASH)
      return OutputSlot::VARYING0 + 1;
    if (nameHash == COLOR_HASH)
      return OutputSlot::VARYING0 + 2;
    if (nameHash == TANGENT_HASH)
      return OutputSlot::VARYING0 + 3;
    if (nameHash == WORLDPOS_HASH)
      return OutputSlot::VARYING0 + 4;
    return OutputSlot::VARYING0;
  }

  // Get the type of a fragment shader input (varying) from the pipeline
  // attributes
  CoreType GetInputTypeFromAttribute(u32 nameHash) {
    if (currentPipeline.IsNull())
      return CoreType::FLOAT3;

    // Type name hashes for common types
    static const u32 FLOAT_HASH = Utils::HashStr("float");
    static const u32 FLOAT2_HASH = Utils::HashStr("float2");
    static const u32 FLOAT3_HASH = Utils::HashStr("float3");
    static const u32 FLOAT4_HASH = Utils::HashStr("float4");
    static const u32 INT_HASH = Utils::HashStr("int");
    static const u32 INT2_HASH = Utils::HashStr("int2");
    static const u32 INT3_HASH = Utils::HashStr("int3");
    static const u32 INT4_HASH = Utils::HashStr("int4");
    static const u32 UINT_HASH = Utils::HashStr("uint");
    static const u32 UINT2_HASH = Utils::HashStr("uint2");
    static const u32 UINT3_HASH = Utils::HashStr("uint3");
    static const u32 UINT4_HASH = Utils::HashStr("uint4");

    const PipelineData &pipeline = ast->GetPipeline(currentPipeline);
    for (u32 i = 0; i < pipeline.attributes.count; i++) {
      NodeRef attrRef = pipeline.attributes[i];
      if (attrRef.Type() == ASTNodeType::ATTRIBUTE_DECL) {
        const AttributeDeclData &attrDecl = ast->GetAttributeDecl(attrRef);
        if (attrDecl.name.nameHash == nameHash) {
          // Found the attribute, parse its type
          u32 typeHash = attrDecl.dataType.nameHash;
          if (typeHash == FLOAT_HASH)
            return CoreType::FLOAT;
          if (typeHash == FLOAT2_HASH)
            return CoreType::FLOAT2;
          if (typeHash == FLOAT3_HASH)
            return CoreType::FLOAT3;
          if (typeHash == FLOAT4_HASH)
            return CoreType::FLOAT4;
          if (typeHash == INT_HASH)
            return CoreType::INT;
          if (typeHash == INT2_HASH)
            return CoreType::INT2;
          if (typeHash == INT3_HASH)
            return CoreType::INT3;
          if (typeHash == INT4_HASH)
            return CoreType::INT4;
          if (typeHash == UINT_HASH)
            return CoreType::UINT;
          if (typeHash == UINT2_HASH)
            return CoreType::UINT2;
          if (typeHash == UINT3_HASH)
            return CoreType::UINT3;
          if (typeHash == UINT4_HASH)
            return CoreType::UINT4;
        }
      }
    }
    return CoreType::FLOAT3; // Default fallback
  }

  // Check if an attribute is compressed and return appropriate type
  // Returns UINT for compressed attributes (raw packed data), otherwise INVALID
  CompressionFormat GetAttributeCompression(u32 attrIndex) {
    if (currentPipeline.IsNull())
      return CompressionFormat::NONE;

    const PipelineData &pipeline = ast->GetPipeline(currentPipeline);
    for (u32 i = 0; i < pipeline.attributes.count; i++) {
      NodeRef attrRef = pipeline.attributes[i];
      if (attrRef.Type() == ASTNodeType::ATTRIBUTE_DECL) {
        const AttributeDeclData &attrDecl = ast->GetAttributeDecl(attrRef);
        if (attrDecl.attributeIndex == attrIndex) {
          // Parse compression format from the ArenaString hash
          return ParseCompressionFormat(attrDecl.compression.nameHash);
        }
      }
    }
    return CompressionFormat::NONE;
  }

  CoreType GetRegisterType(u16 reg) {
    // Constants have type encoded in high bits
    // Check order matters: bool (0xC000) must be checked before float (0x8000)
    if ((reg & 0xC000) == 0xC000)
      return CoreType::BOOL; // Bool constant (0xC000 prefix)
    if (reg & 0x8000)
      return CoreType::FLOAT; // Float constant (0x8000 prefix)
    if (reg & 0x4000)
      return CoreType::INT; // Int constant (0x4000 prefix)
    if (reg & 0x2000)
      return CoreType::UINT; // Uint constant (0x2000 prefix)

    if (reg < program.registerCount) {
      return static_cast<CoreType>(program.registerTypes[reg]);
    }
    return CoreType::FLOAT; // Default
  }

  void SetRegisterType(u16 reg, CoreType type) {
    if (reg < MAX_REGISTERS) {
      program.registerTypes[reg] = static_cast<u16>(type);
    }
  }

  void AddUndefRegister(u16 reg, CoreType type) {
    if (program.undefRegCount >= program.undefRegCapacity) {
      u32 newCapacity = program.undefRegCapacity * 2;
      u16 *newRegs = (u16 *)pool->Allocate(newCapacity * sizeof(u16), 64);
      u16 *newTypes = (u16 *)pool->Allocate(newCapacity * sizeof(u16), 64);
      memcpy(newRegs, program.undefRegs,
             program.undefRegCapacity * sizeof(u16));
      memcpy(newTypes, program.undefRegTypes,
             program.undefRegCapacity * sizeof(u16));
      program.undefRegs = newRegs;
      program.undefRegTypes = newTypes;
      program.undefRegCapacity = newCapacity;
    }
    program.undefRegs[program.undefRegCount] = reg;
    program.undefRegTypes[program.undefRegCount] = static_cast<u16>(type);
    program.undefRegCount++;
  }

  // Get the scalar component type of a vector type
  // FLOAT2/3/4 -> FLOAT, INT2/3/4 -> INT, UINT2/3/4 -> UINT
  // For non-vector types, returns the type itself
  CoreType GetScalarComponentType(CoreType type) {
    switch (type) {
    case CoreType::FLOAT2:
    case CoreType::FLOAT3:
    case CoreType::FLOAT4:
      return CoreType::FLOAT;
    case CoreType::INT2:
    case CoreType::INT3:
    case CoreType::INT4:
      return CoreType::INT;
    case CoreType::UINT2:
    case CoreType::UINT3:
    case CoreType::UINT4:
      return CoreType::UINT;
    case CoreType::BOOL2:
    case CoreType::BOOL3:
    case CoreType::BOOL4:
      return CoreType::BOOL;
    default:
      return type; // Already scalar or not a vector
    }
  }

  // Get a vector type from scalar type and component count
  // e.g., (FLOAT, 3) -> FLOAT3, (UINT, 2) -> UINT2
  CoreType GetVectorType(CoreType scalarType, int componentCount) {
    switch (scalarType) {
    case CoreType::FLOAT:
      if (componentCount == 2)
        return CoreType::FLOAT2;
      if (componentCount == 3)
        return CoreType::FLOAT3;
      if (componentCount == 4)
        return CoreType::FLOAT4;
      break;
    case CoreType::INT:
      if (componentCount == 2)
        return CoreType::INT2;
      if (componentCount == 3)
        return CoreType::INT3;
      if (componentCount == 4)
        return CoreType::INT4;
      break;
    case CoreType::UINT:
      if (componentCount == 2)
        return CoreType::UINT2;
      if (componentCount == 3)
        return CoreType::UINT3;
      if (componentCount == 4)
        return CoreType::UINT4;
      break;
    case CoreType::BOOL:
      if (componentCount == 2)
        return CoreType::BOOL2;
      if (componentCount == 3)
        return CoreType::BOOL3;
      if (componentCount == 4)
        return CoreType::BOOL4;
      break;
    default:
      break;
    }
    return scalarType; // Fallback to scalar
  }

  // Infer the type of an expression from the AST without lowering it
  // Used for determining loop iterator types before the loop header
  CoreType InferExpressionType(NodeRef ref) {
    if (ref.IsNull())
      return CoreType::INT; // Default

    switch (ref.Type()) {
    case ASTNodeType::LITERAL: {
      const LiteralData &lit = ast->GetLiteral(ref);
      // Convert LiteralValue::Type to CoreType
      switch (lit.value.type) {
      case LiteralValue::FLOAT:
        return CoreType::FLOAT;
      case LiteralValue::INT:
        return CoreType::INT;
      case LiteralValue::UINT:
        return CoreType::UINT;
      case LiteralValue::BOOL:
        return CoreType::BOOL;
      default:
        return CoreType::INT;
      }
    }
    case ASTNodeType::IDENTIFIER: {
      const IdentifierData &ident = ast->GetIdentifier(ref);
      // Check if it's a variable we've already allocated
      auto it = variableRegisters.find(ident.name.nameHash);
      if (it != variableRegisters.end()) {
        return GetRegisterType(it->second);
      }
      // Check function parameters (during inlining)
      // For now, assume INT as default for identifiers
      return CoreType::INT;
    }
    case ASTNodeType::BINARY_OP: {
      const BinaryOpData &binOp = ast->GetBinaryOp(ref);
      // For arithmetic ops, type is determined by operands
      CoreType leftType = InferExpressionType(binOp.left);
      return leftType; // Simplified: just use left operand type
    }
    case ASTNodeType::FUNCTION_CALL: {
      // Function return type - check if it's a known function
      // For now, default to FLOAT for function calls
      return CoreType::FLOAT;
    }
    default:
      return CoreType::INT; // Default fallback
    }
  }

  // Check if an expression would yield an unsigned integer type
  bool IsExpressionUnsigned(NodeRef ref) {
    CoreType type = InferExpressionType(ref);
    return (type == CoreType::UINT || type == CoreType::UINT2 ||
            type == CoreType::UINT3 || type == CoreType::UINT4);
  }

  // Get the number of components for a vector type (1 for scalars, 2-4 for
  // vectors)
  u32 GetVectorDimension(CoreType type) {
    switch (type) {
    case CoreType::FLOAT2:
    case CoreType::INT2:
    case CoreType::UINT2:
    case CoreType::BOOL2:
      return 2;
    case CoreType::FLOAT3:
    case CoreType::INT3:
    case CoreType::UINT3:
    case CoreType::BOOL3:
      return 3;
    case CoreType::FLOAT4:
    case CoreType::INT4:
    case CoreType::UINT4:
    case CoreType::BOOL4:
      return 4;
    default:
      return 1; // Scalar types
    }
  }

  // Get the vector type with a specific dimension for a base scalar/vector type
  CoreType GetVectorTypeWithDimension(CoreType baseType, u32 dim) {
    // Determine the base element type
    bool isFloat =
        (baseType == CoreType::FLOAT || baseType == CoreType::FLOAT2 ||
         baseType == CoreType::FLOAT3 || baseType == CoreType::FLOAT4);
    bool isInt = (baseType == CoreType::INT || baseType == CoreType::INT2 ||
                  baseType == CoreType::INT3 || baseType == CoreType::INT4);
    bool isUint = (baseType == CoreType::UINT || baseType == CoreType::UINT2 ||
                   baseType == CoreType::UINT3 || baseType == CoreType::UINT4);

    if (isFloat) {
      switch (dim) {
      case 1:
        return CoreType::FLOAT;
      case 2:
        return CoreType::FLOAT2;
      case 3:
        return CoreType::FLOAT3;
      case 4:
        return CoreType::FLOAT4;
      }
    } else if (isInt) {
      switch (dim) {
      case 1:
        return CoreType::INT;
      case 2:
        return CoreType::INT2;
      case 3:
        return CoreType::INT3;
      case 4:
        return CoreType::INT4;
      }
    } else if (isUint) {
      switch (dim) {
      case 1:
        return CoreType::UINT;
      case 2:
        return CoreType::UINT2;
      case 3:
        return CoreType::UINT3;
      case 4:
        return CoreType::UINT4;
      }
    }
    return baseType; // Return as-is if unknown
  }

  CoreType LookupCoreType(u32 typeHash) {
    // Use TypeHashes from bwsl_types.h
    for (u32 i = 0; i < TypeHashes::HASH_TABLE_SIZE; i++) {
      if (TypeHashes::HASH_TABLE[i].hash == typeHash) {
        return TypeHashes::HASH_TABLE[i].info.coreType;
      }
    }
    return CoreType::INVALID;
  }

  CoreType ResolveCoreTypeFromHash(u32 typeHash, u32 *outCustomHash = nullptr) {
    if (outCustomHash) {
      *outCustomHash = 0;
    }

    CoreType coreType = LookupCoreType(typeHash);
    // GENERIC_T/U/V collide with single-letter user struct names (`V`,
    // `T`, `U` — common in shader code for vertex, texture, etc.). Prefer
    // a user-defined struct/enum over the generic placeholder if one
    // exists. GENERIC_* is only meaningful inside generic-fn signatures,
    // which have their own type-resolution path.
    if (coreType == CoreType::GENERIC_T || coreType == CoreType::GENERIC_U ||
        coreType == CoreType::GENERIC_V) {
      Symbol *userSym = SymbolTable::LookupByHash(
          const_cast<SymbolTableData *>(symbols), typeHash);
      if (userSym && (userSym->kind == SymbolKind::CUSTOM_TYPE ||
                      userSym->kind == SymbolKind::ENUM ||
                      userSym->kind == SymbolKind::ENUM_SYMBOL)) {
        coreType = CoreType::INVALID; // fall through to user-symbol path
      }
    }
    if (coreType != CoreType::INVALID && coreType != CoreType::VOID) {
      return coreType;
    }

    Symbol *sym = SymbolTable::LookupByHash(
        const_cast<SymbolTableData *>(symbols), typeHash);
    if (sym && (sym->kind == SymbolKind::ENUM ||
                sym->kind == SymbolKind::ENUM_SYMBOL)) {
      const EnumData &enumData = symbols->enums[sym->index];
      if (enumData.flags & EnumData::IS_SUM_TYPE) {
        if (outCustomHash) {
          *outCustomHash = enumData.name.nameHash;
        }
        return CoreType::CUSTOM;
      }

      CoreType baseType = enumData.underlyingType;
      if (baseType == CoreType::INVALID) {
        baseType = CoreType::INT;
      }
      return baseType;
    }

    if (sym && sym->kind == SymbolKind::CUSTOM_TYPE) {
      if (outCustomHash) {
        // Use the struct's unqualified name hash for consistency
        const StructData &structData = symbols->structs[sym->index];
        *outCustomHash = structData.name.nameHash;
      }
      return CoreType::CUSTOM;
    }

    // Fall back to global custom type registry
    // This handles cases where the type is known by unqualified name (e.g.,
    // "PBRMaterial") but registered in symbol table with qualified name (e.g.,
    // "PBR::PBRMaterial")
    StructData *structData = g_customTypes.LookupType(typeHash);
    if (structData) {
      if (outCustomHash) {
        *outCustomHash = structData->name.nameHash;
      }
      return CoreType::CUSTOM;
    }

    return CoreType::INVALID;
  }

  OverloadTypeMask MakeOverloadMaskFromResolvedTypeHash(u32 typeHash) {
    u32 customHash = 0;
    CoreType coreType = ResolveCoreTypeFromHash(typeHash, &customHash);
    if (coreType == CoreType::CUSTOM && customHash != 0) {
      return MakeOverloadMask(coreType, customHash);
    }
    if (coreType != CoreType::INVALID && coreType != CoreType::VOID) {
      return MakeOverloadMask(coreType);
    }
    return MakeOverloadMaskFromTypeHash(typeHash);
  }

  OpCode IntrinsicToOpcode(StdLib::Intrinsic intrinsic) {
    using Intrinsic = StdLib::Intrinsic;
    switch (intrinsic) {
    // Math
    case Intrinsic::LERP:
      return OP_LERP;
    case Intrinsic::SMOOTHSTEP:
      return OP_SMOOTHSTEP;
    case Intrinsic::SATURATE:
      return OP_SATURATE;
    case Intrinsic::FRACT:
      return OP_FRACT;
    case Intrinsic::STEP:
      return OP_STEP;
    case Intrinsic::SIGN:
      return OP_SIGN;
    case Intrinsic::FLOOR:
      return OP_FLOOR;
    case Intrinsic::CEIL:
      return OP_CEIL;
    case Intrinsic::ROUND:
      return OP_ROUND;
    case Intrinsic::POW:
      return OP_POW;
    case Intrinsic::SQRT:
      return OP_SQRT;
    case Intrinsic::RSQRT:
      return OP_RSQRT;
    case Intrinsic::EXP:
      return OP_EXP;
    case Intrinsic::EXP2:
      return OP_EXP2;
    case Intrinsic::LOG:
      return OP_LOG;
    case Intrinsic::LOG2:
      return OP_LOG2;

    // Trigonometry
    case Intrinsic::SIN:
      return OP_SIN;
    case Intrinsic::COS:
      return OP_COS;
    case Intrinsic::TAN:
      return OP_TAN;
    case Intrinsic::ASIN:
      return OP_ASIN;
    case Intrinsic::ACOS:
      return OP_ACOS;
    case Intrinsic::ATAN:
      return OP_ATAN;
    case Intrinsic::ATAN2:
      return OP_ATAN2;
    case Intrinsic::DEGREES:
      return OP_DEGREES;
    case Intrinsic::RADIANS:
      return OP_RADIANS;

    // Vector
    case Intrinsic::DOT:
      return OP_DOT;
    case Intrinsic::CROSS:
      return OP_CROSS;
    case Intrinsic::NORMALIZE:
      return OP_NORMALIZE;
    case Intrinsic::LENGTH:
      return OP_LENGTH;
    case Intrinsic::DISTANCE:
      return OP_DISTANCE;
    case Intrinsic::REFLECT:
      return OP_REFLECT;
    case Intrinsic::REFRACT:
      return OP_REFRACT;
    case Intrinsic::FACEFORWARD:
      return OP_FACEFORWARD;

    // Matrix
    case Intrinsic::TRANSPOSE:
      return OP_MAT_TRANSPOSE;
    case Intrinsic::DETERMINANT:
      return OP_MAT_DET;
    case Intrinsic::INVERSE:
      return OP_MAT_INVERSE;

    // Derivatives (fragment only)
    case Intrinsic::DDX:
      return OP_DDX;
    case Intrinsic::DDY:
      return OP_DDY;
    case Intrinsic::DDX_FINE:
      return OP_DDX_FINE;
    case Intrinsic::DDY_FINE:
      return OP_DDY_FINE;
    case Intrinsic::DDX_COARSE:
      return OP_DDX_COARSE;
    case Intrinsic::DDY_COARSE:
      return OP_DDY_COARSE;
    case Intrinsic::FWIDTH:
      return OP_FWIDTH;

    // Texture operations - these need special handling
    case Intrinsic::SAMPLE:
      return OP_TEX_SAMPLE;
    case Intrinsic::SAMPLE_LOD:
      return OP_TEX_SAMPLE_LOD;
    case Intrinsic::SAMPLE_BIAS:
      return OP_TEX_SAMPLE_BIAS;
    case Intrinsic::SAMPLE_GRAD:
      return OP_TEX_SAMPLE_GRAD;
    case Intrinsic::SAMPLE_CMP:
      return OP_TEX_SAMPLE_CMP;
    case Intrinsic::GATHER:
      return OP_TEX_GATHER;
    case Intrinsic::LOAD:
      return OP_TEX_FETCH;
    case Intrinsic::STORE:
      return OP_IMG_STORE;

    // Synchronization
    case Intrinsic::BARRIER:
      return OP_BARRIER;
    case Intrinsic::MEMORY_BARRIER:
      return OP_MEM_FENCE;
    case Intrinsic::STORAGE_BARRIER:
      return OP_MEM_FENCE;

    // Wave/SIMD operations
    case Intrinsic::WAVE_ACTIVE_SUM:
      return OP_WAVE_SUM;
    case Intrinsic::WAVE_ACTIVE_PRODUCT:
      return OP_WAVE_MUL;
    case Intrinsic::WAVE_ACTIVE_MIN:
      return OP_WAVE_MIN;
    case Intrinsic::WAVE_ACTIVE_MAX:
      return OP_WAVE_MAX;
    case Intrinsic::WAVE_ACTIVE_ALL:
      return OP_WAVE_ALL;
    case Intrinsic::WAVE_ACTIVE_ANY:
      return OP_WAVE_ANY;
    case Intrinsic::WAVE_BROADCAST:
      return OP_WAVE_READ_LANE;
    case Intrinsic::WAVE_READ_FIRST:
      return OP_WAVE_READ_FIRST;

    // Atomics
    case Intrinsic::ATOMIC_ADD:
      return OP_ATOMIC_ADD;
    case Intrinsic::ATOMIC_MIN:
      return OP_ATOMIC_MIN;
    case Intrinsic::ATOMIC_MAX:
      return OP_ATOMIC_MAX;
    case Intrinsic::ATOMIC_AND:
      return OP_ATOMIC_AND;
    case Intrinsic::ATOMIC_OR:
      return OP_ATOMIC_OR;
    case Intrinsic::ATOMIC_XOR:
      return OP_ATOMIC_XOR;
    case Intrinsic::ATOMIC_EXCHANGE:
      return OP_ATOMIC_XCHG;
    case Intrinsic::ATOMIC_CMP_EXCHANGE:
      return OP_ATOMIC_CMP_XCHG;

    // Bit operations
    case Intrinsic::COUNT_BITS:
      return OP_POPCNT;
    case Intrinsic::REVERSE_BITS:
      return OP_REVERSE_BITS;
    case Intrinsic::FIRST_BIT_LOW:
      return OP_CTZ;
    case Intrinsic::FIRST_BIT_HIGH:
      return OP_CLZ;

    // Control flow
    case Intrinsic::SELECT:
      return OP_SELECT;

    // Boolean reductions
    case Intrinsic::ANY:
      return OP_ANY;
    case Intrinsic::ALL:
      return OP_ALL;

    // Float classification
    case Intrinsic::IS_NAN:
      return OP_ISNAN;
    case Intrinsic::IS_INF:
      return OP_ISINF;

    default:
      return OP_NOP;
    }
  }
};

} // namespace BWSL::IR
