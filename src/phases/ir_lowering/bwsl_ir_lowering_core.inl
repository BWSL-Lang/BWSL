// Part of the header-only IRLowering implementation. Include via bwsl_ir_lowering.h only.
#pragma once
#include "bwsl_ir_lowering.h"

namespace BWSL::IR {

inline void IRLowering::ReportError(const char *message) {
  ReportErrorAt(NodeRef::Null(), message);
}

inline void IRLowering::ReportErrorAt(NodeRef node, const char *message) {
    if (message) {
        DiagnosticSpan span{};
        if (node.IsValid() && ast) {
            u32 position = ast->GetNamePosition(node);
            if (position == 0)
                position = ast->FindPosition(node);
            AST::UnpackPosition(position, span.line, span.column);
            if (span.line != 0)
                span.SetLocation();
        }
        std::string locatedMessage = message;
        if (span.HasLocation()) {
            locatedMessage = "line " + std::to_string(span.line) + ":" +
                             std::to_string(span.column) + ": " + locatedMessage;
        }
        diagnostics.emplace_back(locatedMessage);
        if (diagnosticStream) {
            const char *stageName = "unknown";
            switch (currentStage) {
            case ShaderStage::Vertex:
                stageName = "vertex";
                break;
            case ShaderStage::Fragment:
                stageName = "fragment";
                break;
            case ShaderStage::Compute:
                stageName = "compute";
                break;
            default:
                break;
            }
            std::string passName;
            if (!diagnosticPassName.empty()) {
                passName = diagnosticPassName;
            } else if (currentPassData) {
                passName = currentPassData->name.ToString(sourceBase);
            }
            diagnosticStream->AddRaw(DiagnosticSeverity::Error, DiagnosticPhase::Lowering, message,
                                     span, "", passName, stageName,
                                     DiagnosticMessageId::LoweringError);
        }
        if (!suppressErrorOutput) {
            fprintf(stderr, "%s", locatedMessage.c_str());
        }
    }
    hadError = true;
}

inline NamespaceKind IRLowering::AliasOwnerKind() const {
  return (inlineModuleIndex != 0xFFFFFFFF)
             ? NamespaceKind::MODULE
             : NamespaceKind::GLOBAL;
}

inline u32 IRLowering::AliasOwnerModuleIndex() const {
  return (inlineModuleIndex != 0xFFFFFFFF) ? inlineModuleIndex : INVALID_INDEX;
}

inline void IRLowering::Initialize(IRMemoryPool *memPool, const SymbolTableData *symTable,
                AST *astData, const char *srcBase) {
  pool = memPool;
  symbols = symTable;
  ast = astData;
  sourceBase = srcBase;
  initializedVariables.clear();
  constVariables.clear();
  constantVectors.clear();
  builder.pool = pool;
  builder.program = &program;
  builder.currentInstruction = 0;
    builder.currentSourceNode = 0xFFFFFFFF;
  builder.nextRegister = 0;
  recursionDiagnosed = false;
  hadError = false;
  suppressErrorOutput = false;
  diagnostics.clear();
  diagnosticStream = nullptr;
  diagnosticPassName.clear();
  currentPassData = nullptr;
  currentStructMethodTypeHash = 0;
  currentStructMethodSelfReg = 0xFFFF;
  currentStructMethodIsConst = false;

  // Allocate IR arrays. instructionCount must be zeroed explicitly — it's
  // a plain u32 field with no inline initializer, and the enclosing
  // IRLowering is stack-allocated in the fuzz/test harnesses, so without
  // this we read stack garbage if no instructions end up being emitted.
  u32 initialSize = 1024;
  program.instructionCount = 0;
  program.instructionCapacity = initialSize;
    program.sourceNodes = (u32*)pool->Allocate(initialSize * sizeof(u32), 64);
    program.ssaFailureSourceNode = 0xFFFFFFFF;
  program.opcodes = (u16 *)pool->Allocate(initialSize * sizeof(u16), 64);
  program.types = (u16 *)pool->Allocate(initialSize * sizeof(u16), 64);
  program.flags = (u16 *)pool->Allocate(initialSize * sizeof(u16), 64);
  program.destinations = (u16 *)pool->Allocate(initialSize * sizeof(u16), 64);
  program.operands = (u16 *)pool->Allocate(initialSize * 4 * sizeof(u16), 64);
  // Initialize unused operand slots to the explicit sentinel used by SSA and
  // the backends.
  for (u32 i = 0; i < initialSize * 4; i++) {
    program.operands[i] = 0xFFFF;
  }
  program.metadata = (u32 *)pool->Allocate(initialSize * sizeof(u32), 64);
  memset(program.metadata, 0, initialSize * sizeof(u32));
  memset(program.outputInterpolations, 0, sizeof(program.outputInterpolations));
  memset(program.inputInterpolations, 0, sizeof(program.inputInterpolations));
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
    program.floatCapacity = 256;
    program.intCapacity = 256;
    program.uintCapacity = 256;
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
  // AllocateRegister initializes each live slot, including placeholders whose
  // final type is patched later. Unused register capacity need not be touched.
  program.registerCount = 0;
  program.registerNameHashes =
      (u32 *)pool->Allocate(MAX_REGISTERS * sizeof(u32), 64);
  memset(program.registerNameHashes, 0, MAX_REGISTERS * sizeof(u32));

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
  program.structFieldBounds = (const DeclaredBounds **)pool->Allocate(
      fieldCapacity * sizeof(const DeclaredBounds *), 64);
  memset(program.structFieldBounds, 0, fieldCapacity * sizeof(const DeclaredBounds *));
  memset(program.attributeBounds, 0, sizeof(program.attributeBounds));
  memset(program.uniformBounds, 0, sizeof(program.uniformBounds));
  memset(program.workgroupSize, 0, sizeof(program.workgroupSize));
  program.varyingBoundsOut = nullptr;
  memset(program.inputBounds, 0, sizeof(program.inputBounds));
  program.registerStructTypes =
      (u32 *)pool->Allocate(MAX_REGISTERS * sizeof(u32), 64);
  program.registerStorageInfo =
      (u32 *)pool->Allocate(MAX_REGISTERS * sizeof(u32), 64);
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
  memset(program.uniformArrayLengths, 0, sizeof(program.uniformArrayLengths));

  // PHI fields (will be populated by SSA if needed)
  program.phiBlockIndices = nullptr;
  program.phiResultRegs = nullptr;
  program.phiTypes = nullptr;
  program.phiVariables = nullptr;
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
  constVariables.clear();
  constantVectors.clear();
  arrayBaseRegisters.clear();
  structTypeMap.clear();
  variableStructTypes.clear();
}

inline IRProgram *IRLowering::LowerPipeline(NodeRef pipelineRef) {
  currentPipeline = pipelineRef;
  const PipelineData &pipeline = ast->GetPipeline(pipelineRef);
  for (u32 i = 0; i < pipeline.passes.count; i++) {
    LowerPass(pipeline.passes[i]);
  }
  return &program;
}

inline void IRLowering::LowerPassConstants(const PassData &pass) {
  for (u32 i = 0; i < pass.consts.count; i++) {
    LowerVariableDecl(pass.consts[i]);
  }
}

inline void IRLowering::LowerGlobalArrayConstants(NodeRef stageBody,
                                                  const PassData &pass) {
  // These arrays are materialized in function storage. Emit their initializer
  // before stage control flow so a cached register is initialized on every path.
  // Scan the stage and pass constants first. Function bodies are included
  // conservatively because calls may be inlined from inside control flow.
  std::vector<NodeRef> pending;
  pending.push_back(stageBody);
  for (u32 i = 0; i < pass.consts.count; ++i) pending.push_back(pass.consts[i]);
  for (u32 i = 0; i < ast->functions.count; ++i)
    pending.push_back(ast->functions[i].body);
  std::unordered_set<u32> visited;
  std::unordered_set<u32> usedNames;
  std::vector<u32> bindings;
  while (!pending.empty()) {
    NodeRef ref = pending.back();
    pending.pop_back();
    if (!ref.IsValid() || !visited.insert(ref.packed).second) continue;
    auto push = [&](NodeRef child) { if (child.IsValid()) pending.push_back(child); };
    auto pushBlock = [&](const BlockData &block) {
      for (u32 i = 0; i < block.statements.count; ++i) push(block.statements[i]);
    };
    switch (ref.Type()) {
    case ASTNodeType::IDENTIFIER:
      usedNames.insert(ast->GetIdentifier(ref).name.nameHash);
      bindings.push_back(ast->identifierConstBindingHashes[ref.Index()]);
      break;
    case ASTNodeType::MEMBER_ACCESS: {
      const MemberAccessData &member = ast->GetMemberAccess(ref);
      if (member.isModuleQualified) bindings.push_back(member.qualifiedNameHash);
      push(member.object);
      break;
    }
    case ASTNodeType::ARRAY_ACCESS: {
      const ArrayAccessData &access = ast->GetArrayAccess(ref);
      push(access.array); push(access.index);
      break;
    }
    case ASTNodeType::BINARY_OP: {
      const BinaryOpData &op = ast->GetBinaryOp(ref);
      push(op.left); push(op.right);
      break;
    }
    case ASTNodeType::UNARY_OP:
      push(ast->GetUnaryOp(ref).operand);
      break;
    case ASTNodeType::TERNARY_EXPRESSION: {
      const TernaryExprData &expr = ast->GetTernaryExpression(ref);
      push(expr.condition); push(expr.trueExpr); push(expr.falseExpr);
      break;
    }
    case ASTNodeType::ASSIGNMENT:
    case ASTNodeType::RETURN: {
      const AssignmentData &assignment = ast->GetAssignment(ref);
      push(assignment.target); push(assignment.value);
      break;
    }
    case ASTNodeType::VARIABLE_DECL:
      push(ast->GetVariableDecl(ref).initializer);
      break;
    case ASTNodeType::FUNCTION_CALL: {
      const FunctionCallData &call = ast->GetFunctionCall(ref);
      push(call.moduleObject);
      for (u32 i = 0; i < call.arguments.count; ++i) push(call.arguments[i]);
      break;
    }
    case ASTNodeType::BLOCK:
    case ASTNodeType::EVAL_BLOCK:
    case ASTNodeType::IF_STATEMENT:
    case ASTNodeType::EVAL_IF:
      pushBlock(ast->GetBlock(ref));
      break;
    case ASTNodeType::FOR_CSTYLE: {
      const ForCStyleData &loop = ast->GetForCStyle(ref);
      push(loop.init); push(loop.condition); push(loop.increment); push(loop.body);
      break;
    }
    case ASTNodeType::FOR_RANGE: {
      const ForRangeData &loop = ast->GetForRange(ref);
      push(loop.iteratorVar); push(loop.rangeStart); push(loop.rangeEnd);
      push(loop.step); push(loop.body);
      break;
    }
    case ASTNodeType::FOR_COLLECTION: {
      const ForCollectionData &loop = ast->GetForCollection(ref);
      push(loop.iteratorVar); push(loop.collection); push(loop.body);
      break;
    }
    case ASTNodeType::LOOP: {
      const LoopData &loop = ast->GetLoop(ref);
      push(loop.count); push(loop.body); push(loop.untilCondition);
      break;
    }
    case ASTNodeType::SWITCH: {
      const SwitchData &branch = ast->GetSwitch(ref);
      push(branch.expression); push(branch.defaultCase);
      for (u32 i = 0; i < branch.cases.count; ++i) push(branch.cases[i]);
      break;
    }
    case ASTNodeType::SWITCH_CASE: {
      const SwitchCaseData &arm = ast->GetSwitchCase(ref);
      push(arm.body);
      for (u32 i = 0; i < arm.values.count; ++i) push(arm.values[i]);
      break;
    }
    case ASTNodeType::PATTERN_MATCH:
    case ASTNodeType::PATTERN_MATCH_ARM: {
      const PatternMatchData &match = ast->GetPatternMatch(ref);
      push(match.body); push(match.defaultArm);
      for (u32 i = 0; i < match.statements.count; ++i) push(match.statements[i]);
      for (u32 i = 0; i < match.arms.count; ++i) push(match.arms[i]);
      break;
    }
    case ASTNodeType::TYPE_PATTERN_MATCH: {
      const TypePatternMatchData &match = ast->GetTypePatternMatch(ref);
      push(match.defaultArm);
      for (u32 i = 0; i < match.arms.count; ++i) push(match.arms[i]);
      break;
    }
    case ASTNodeType::TYPE_PATTERN_ARM:
      push(ast->GetTypePatternArm(ref).body);
      break;
    default:
      break;
    }
  }

  if (currentPipeline.Type() == ASTNodeType::PIPELINE &&
      currentPipeline.Index() < ast->pipelines.count) {
    const PipelineData &pipeline = ast->GetPipeline(currentPipeline);
    for (u32 i = 0; i < pipeline.consts.count; ++i) {
      NodeRef decl = pipeline.consts[i];
      if (decl.Type() == ASTNodeType::VARIABLE_DECL &&
          ast->GetVariableDecl(decl).arrayDimensions > 0 &&
          usedNames.count(ast->GetVariableDecl(decl).name.nameHash))
        LowerVariableDecl(decl);
    }
  }

  auto lowerBinding = [&](u32 hash) {
    if (hash == 0 || variableRegisters.find(hash) != variableRegisters.end()) return;
    Symbol *sym = SymbolTable::LookupByHash(
        const_cast<SymbolTableData *>(symbols), hash);
    if (!sym || sym->kind != SymbolKind::VARIABLE) return;
    const VariableData &data = symbols->variables[sym->index];
    if (data.isConst && IsArray(data.typeInfo) && data.constDecl.IsValid())
      LowerModuleArrayConstant(data, hash);
  };
  std::sort(bindings.begin(), bindings.end());
  bindings.erase(std::unique(bindings.begin(), bindings.end()), bindings.end());
  for (u32 hash : bindings) lowerBinding(hash);
}

inline void IRLowering::LowerPass(NodeRef passRef) {
  const PassData &pass = ast->GetPass(passRef);
  currentPass = passRef; // Track current pass for pass-scoped function lookup
  currentPassData = &pass;

  // Create varying context for this pass - tracks vertex outputs
  // so fragment shader can resolve input.xxx to matching slots
  PassVaryingContext varyingContext;
  currentPassVaryings = &varyingContext;

  // Phase 1: Lower vertex shader (collects output.xxx usages)
  if (!pass.vertexShader.IsNull()) {
    currentStage = ShaderStage::Vertex;
    LowerGlobalArrayConstants(ast->GetShaderStage(pass.vertexShader).body, pass);
    LowerPassConstants(pass);
    LowerShaderStage(pass.vertexShader);
    varyingContext.vertexStageLowered = !hadError;
  }

  // Phase 2: Lower fragment shader (uses collected varyings for input
  // resolution)
  if (!pass.fragmentShader.IsNull()) {
    currentStage = ShaderStage::Fragment;
    LowerGlobalArrayConstants(ast->GetShaderStage(pass.fragmentShader).body, pass);
    LowerPassConstants(pass);
    LowerShaderStage(pass.fragmentShader);
  }

  if (!pass.computeShader.IsNull()) {
    currentStage = ShaderStage::Compute;
    LowerGlobalArrayConstants(ast->GetShaderStage(pass.computeShader).body, pass);
    LowerPassConstants(pass);
    LowerShaderStage(pass.computeShader);
  }

  currentPassVaryings = nullptr;
  currentPassData = nullptr;
  currentPass = NodeRef::Null();
}

// The workgroup size the backends declare for this pass's compute stage,
// which bounds input.local_id and input.local_index.
inline void IRLowering::RecordWorkgroupSize() {
  if (!currentPassData || currentPassData->computeShader.IsNull()) return;
  const ShaderStageData &stage = ast->GetShaderStage(currentPassData->computeShader);
  program.workgroupSize[0] = stage.workgroupSizeX;
  program.workgroupSize[1] = stage.workgroupSizeY;
  program.workgroupSize[2] = stage.workgroupSizeZ;
}

inline void IRLowering::LowerShaderStage(NodeRef stageRef) {
  const ShaderStageData &stage = ast->GetShaderStage(stageRef);
  if (!stage.body.IsNull()) {
    LowerBlock(stage.body);
  }
  // Emit return at end of shader
  builder.EmitInstruction(OP_RET, 0, 0);
}

inline void IRLowering::LowerBlock(NodeRef blockRef) {
  const BlockData &block = ast->GetBlock(blockRef);
  bool guardInlineReturns =
      (inlineDepth > 0 && inlineReturnFlagReg != 0xFFFF);

  // Save variable-register map so that inner-scope redeclarations
  // (`{ float x = …; }` inside an outer `int x`) don't overwrite the
  // outer binding. Without this, references to `x` after the inner
  // block resolve to the inner (now-dead) slot and get the wrong
  // type. The parser's symbol table does scope-stack pops; the
  // lowering flat-map didn't.
  auto savedVariableRegisters = variableRegisters;
  auto savedVariableStructTypes = variableStructTypes;

  LowerStatements(block, 0, guardInlineReturns);

  variableRegisters = savedVariableRegisters;
  variableStructTypes = savedVariableStructTypes;
}


} // namespace BWSL::IR
