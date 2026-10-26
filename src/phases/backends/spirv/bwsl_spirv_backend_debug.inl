#pragma once
// Source-level debug info (SetDebugInfo), for GPU debuggers such as
// RenderDoc and Nsight:
// - each source file's path as an OpString, and the stage's file as OpSource
//   with its text;
// - an OpLine where an instruction's source line changes;
// - NonSemantic.Shader.DebugInfo.100: a DebugSource with the text of every
//   file code comes from, the compilation unit, a DebugFunction for the
//   entry and for each function inlined into it, per block a DebugScope
//   (with DebugInlinedAt for inlined code) and DebugLine where the line
//   changes, and a DebugLocalVariable for each named local: a DebugValue
//   where a register gives it a value, or one DebugDeclare for a local kept
//   in an OpVariable. Scalars, vectors and matrices are described, structs
//   (DebugTypeComposite) whose fields are, and arrays of either.
// Locations come from the IR's source nodes (an instruction's statement or
// expression) and debug scopes (the inlined function body it's in).

namespace BWSL {

namespace {

// A string's bytes up to `limit`, ending before a UTF-8 continuation byte
// so a split keeps each piece valid UTF-8.
u32 DebugTextChunk(const char* text, u32 length, u32 limit) {
  if (length <= limit) return length;
  u32 end = limit;
  while (end > 0 && (static_cast<u8>(text[end]) & 0xC0) == 0x80) end--;
  return end > 0 ? end : limit;
}

// Most bytes of a literal string in one instruction of `fixedWords` other
// words: the word count is 16 bits, and the string ends with a NUL.
constexpr u32 DebugLiteralLimit(u32 fixedWords) { return (0xFFFF - fixedWords) * 4 - 1; }

void AppendLiteral(std::vector<u32>& words, const char* text, u32 length) {
  u32 count = (length + 4) / 4;  // Including the terminating NUL
  size_t start = words.size();
  words.resize(start + count, 0);
  memcpy(&words[start], text, length);
}

}  // namespace

u32 SPIRVBuilder::DebugConstant(u32 value) {
  for (const auto& entry : debugConstants) {
    if (entry.first == value) return entry.second;
  }
  u32 id = AllocateId();
  u32 ops[] = {GetTypeId(CoreType::UINT), id, value};
  EmitToSection(&typesConstants, spv::OpConstant, ops, 3);
  debugConstants.push_back({value, id});
  return id;
}

// An OpString of `text`, shortened to fit one instruction.
u32 SPIRVBuilder::DebugString(const char* text, u32 length) {
  u32 id = AllocateId();
  std::vector<u32> words;
  words.push_back(id);
  AppendLiteral(words, text, DebugTextChunk(text, length, DebugLiteralLimit(2)));
  EmitToSection(&debugSources, spv::OpString, words.data(), static_cast<u32>(words.size()));
  return id;
}

// An OpExtInst of the debug info set outside the function; returns its id.
u32 SPIRVBuilder::DebugGlobal(u32 instruction, const u32* operands, u32 count) {
  u32 id = AllocateId();
  std::vector<u32> words = {GetTypeId(CoreType::VOID), id, debugInfoImportId, instruction};
  words.insert(words.end(), operands, operands + count);
  EmitToSection(&debugGlobals, spv::OpExtInst, words.data(), static_cast<u32>(words.size()));
  return id;
}

void SPIRVBuilder::DebugInFunction(u32 instruction, const u32* operands, u32 count) {
  u32 wordCount = 5 + count;
  while (currentFunctionSize + wordCount > currentFunctionCapacity) GrowCurrentFunction();
  currentFunction[currentFunctionSize++] = (wordCount << 16) | spv::OpExtInst;
  currentFunction[currentFunctionSize++] = GetTypeId(CoreType::VOID);
  currentFunction[currentFunctionSize++] = AllocateId();
  currentFunction[currentFunctionSize++] = debugInfoImportId;
  currentFunction[currentFunctionSize++] = instruction;
  for (u32 i = 0; i < count; i++) currentFunction[currentFunctionSize++] = operands[i];
}

u32 SPIRVBuilder::DebugFileString(u32 file) {
  if (file >= debugFileStrings.size()) return 0;
  if (debugFileStrings[file] == 0) {
    const char* path = debugAst->sourceFiles[file];
    debugFileStrings[file] = DebugString(path, static_cast<u32>(strlen(path)));
  }
  return debugFileStrings[file];
}

// The file's DebugSource, with its text when the parser kept it; text
// longer than one OpString continues in DebugSourceContinued.
u32 SPIRVBuilder::DebugSourceOf(u32 file) {
  if (file >= debugSourceIds.size()) return 0;
  if (debugSourceIds[file] != 0) return debugSourceIds[file];
  const char* text = file < debugAst->sourceTexts.count ? debugAst->sourceTexts[file] : nullptr;
  u32 length = text ? debugAst->sourceTextLengths[file] : 0;
  u32 path = DebugFileString(file);
  if (!text) {
    u32 ops[] = {path};
    return debugSourceIds[file] = DebugGlobal(NonSemanticShaderDebugInfo100DebugSource, ops, 1);
  }
  u32 first = DebugTextChunk(text, length, DebugLiteralLimit(2));
  u32 ops[] = {path, DebugString(text, first)};
  debugSourceIds[file] = DebugGlobal(NonSemanticShaderDebugInfo100DebugSource, ops, 2);
  for (u32 offset = first; offset < length;) {
    u32 chunk = DebugTextChunk(text + offset, length - offset, DebugLiteralLimit(2));
    u32 more[] = {DebugString(text + offset, chunk)};
    DebugGlobal(NonSemanticShaderDebugInfo100DebugSourceContinued, more, 1);
    offset += chunk;
  }
  return debugSourceIds[file];
}

// The file a debug scope's code was written in: an inlined function's own,
// or the stage's.
u32 SPIRVBuilder::DebugFileOfScope(u32 scope) {
  if (scope == 0 || scope >= ir->debugScopeCount) return debugMainFile;
  NodeRef function;
  function.packed = ir->debugScopeFunctions[scope];
  u32 file = debugAst->GetDeclarationSourceIndex(function);
  return file < debugAst->sourceFiles.count ? file : debugMainFile;
}

bool SPIRVBuilder::DebugPosition(u32 packedNode, u32* line, u32* column) const {
  if (packedNode == 0xFFFFFFFF) return false;
  NodeRef node;
  node.packed = packedNode;
  u32 position = debugAst->FindPosition(node);
  if (position == 0) return false;
  AST::UnpackPosition(position, *line, *column);
  return *line != 0;
}

// One DebugFunction per function, however often it's inlined.
u32 SPIRVBuilder::DebugFunctionOf(NodeRef function) {
  for (const auto& entry : debugFunctionIds) {
    if (entry.first == function.packed) return entry.second;
  }
  std::string name = "function";
  if (function.Type() == ASTNodeType::FUNCTION) {
    std::string written = ReverseLookup::GetString(debugAst->GetFunction(function).name.nameHash);
    if (!written.empty() && written.rfind("<hash:", 0) != 0) name = written;
  }
  u32 file = debugAst->GetDeclarationSourceIndex(function);
  if (file >= debugAst->sourceFiles.count) file = debugMainFile;
  u32 line = 0, column = 0;
  DebugPosition(function.packed, &line, &column);
  u32 nameString = DebugString(name.c_str(), static_cast<u32>(name.size()));
  u32 ops[] = {nameString, debugFunctionType, DebugSourceOf(file), DebugConstant(line), DebugConstant(column),
               debugCompilationUnit, nameString, DebugConstant(NonSemanticShaderDebugInfo100FlagIsPublic),
               DebugConstant(line)};
  u32 id = DebugGlobal(NonSemanticShaderDebugInfo100DebugFunction, ops, 9);
  debugFunctionIds.push_back({function.packed, id});
  return id;
}

u32 SPIRVBuilder::DebugScopeFunction(u32 scope) {
  if (scope >= debugScopeIds.size()) scope = 0;
  if (debugScopeIds[scope] == 0) {
    NodeRef function;
    function.packed = ir->debugScopeFunctions[scope];
    debugScopeIds[scope] = DebugFunctionOf(function);
  }
  return debugScopeIds[scope];
}

// Where an inlined scope was called: the call's line in the calling scope,
// itself inlined from its own caller.
u32 SPIRVBuilder::DebugInlinedAt(u32 scope) {
  if (scope == 0 || scope >= debugInlinedAtIds.size()) return 0;
  if (debugInlinedAtIds[scope] != 0) return debugInlinedAtIds[scope];
  u32 parent = ir->debugScopeParents[scope];
  if (parent >= ir->debugScopeCount) parent = 0;
  u32 line = 0, column = 0;
  DebugPosition(ir->debugScopeCalls[scope], &line, &column);
  u32 ops[] = {DebugConstant(line), DebugScopeFunction(parent), DebugInlinedAt(parent)};
  return debugInlinedAtIds[scope] = DebugGlobal(NonSemanticShaderDebugInfo100DebugInlinedAt, ops, parent != 0 ? 3 : 2);
}

// The extension and the import. Sections are put in order when the module
// is finished, so this can run after the preamble (SetDebugInfo comes later).
void SPIRVBuilder::BeginDebugInfo() {
  if (!debugAst) return;
  EmitExtension("SPV_KHR_non_semantic_info");
  debugInfoImportId = AllocateId();
  std::vector<u32> words = {debugInfoImportId};
  const char* name = "NonSemantic.Shader.DebugInfo.100";
  AppendLiteral(words, name, static_cast<u32>(strlen(name)));
  EmitToSection(&extInstImports, spv::OpExtInstImport, words.data(), static_cast<u32>(words.size()));
}

// The source text, the compilation unit and the entry's DebugFunction,
// before the function.
void SPIRVBuilder::BeginDebugFunction() {
  if (!debugAst) return;
  u32 files = debugAst->sourceFiles.count;
  if (debugMainFile >= files) {
    debugAst = nullptr;  // No file to point at
    return;
  }
  BeginDebugInfo();
  debugFileStrings.assign(files, 0);
  debugSourceIds.assign(files, 0);
  debugScopeIds.assign(ir->debugScopeCount ? ir->debugScopeCount : 1, 0);
  debugInlinedAtIds.assign(debugScopeIds.size(), 0);

  // OpSource: the stage's file, its text continued past one instruction.
  const char* text = debugAst->sourceTexts[debugMainFile];
  u32 length = text ? debugAst->sourceTextLengths[debugMainFile] : 0;
  std::vector<u32> source = {static_cast<u32>(spv::SourceLanguageUnknown), 0, DebugFileString(debugMainFile)};
  u32 first = text ? DebugTextChunk(text, length, DebugLiteralLimit(4)) : 0;
  if (text) AppendLiteral(source, text, first);
  EmitToSection(&debugSources, spv::OpSource, source.data(), static_cast<u32>(source.size()));
  for (u32 offset = first; text && offset < length;) {
    u32 chunk = DebugTextChunk(text + offset, length - offset, DebugLiteralLimit(1));
    std::vector<u32> more;
    AppendLiteral(more, text + offset, chunk);
    EmitToSection(&debugSources, spv::OpSourceContinued, more.data(), static_cast<u32>(more.size()));
    offset += chunk;
  }

  u32 unit[] = {DebugConstant(100), DebugConstant(4), DebugSourceOf(debugMainFile),
                DebugConstant(static_cast<u32>(spv::SourceLanguageUnknown))};
  debugCompilationUnit = DebugGlobal(NonSemanticShaderDebugInfo100DebugCompilationUnit, unit, 4);
  u32 type[] = {DebugConstant(NonSemanticShaderDebugInfo100FlagIsPublic), GetTypeId(CoreType::VOID)};
  debugFunctionType = DebugGlobal(NonSemanticShaderDebugInfo100DebugTypeFunction, type, 2);

  // The entry: scope 0, starting at the stage.
  u32 line = 0, column = 0;
  DebugPosition(debugStage.packed, &line, &column);
  u32 name = DebugString(debugEntryName.c_str(), static_cast<u32>(debugEntryName.size()));
  u32 entry[] = {name, debugFunctionType, DebugSourceOf(debugMainFile), DebugConstant(line), DebugConstant(column),
                 debugCompilationUnit, name, DebugConstant(NonSemanticShaderDebugInfo100FlagIsPublic),
                 DebugConstant(line)};
  debugScopeIds[0] = DebugGlobal(NonSemanticShaderDebugInfo100DebugFunction, entry, 9);
}

// Ties the entry's DebugFunction to the OpFunction: in the entry block,
// after its OpVariables.
void SPIRVBuilder::EmitDebugFunctionDefinition() {
  if (!debugAst) return;
  u32 ops[] = {debugScopeIds[0], entryPointId};
  DebugInFunction(NonSemanticShaderDebugInfo100DebugFunctionDefinition, ops, 2);
  // DebugDeclare's own scope: the entry's, which the first line keeps.
  u32 scope[] = {debugScopeIds[0]};
  DebugInFunction(NonSemanticShaderDebugInfo100DebugScope, scope, 1);
  EmitDebugDeclares();
  debugLocationValid = true;
  debugLastScope = 0;
  debugLastFile = 0xFFFFFFFF;
  debugLastLine = 0;
}

// Before IR instruction `irIdx`: its scope and line, where they differ from
// what the block last emitted. Columns don't count, so a debugger stops
// once per line rather than per expression on it.
void SPIRVBuilder::EmitDebugLocation(u32 irIdx) {
  if (!debugAst || !ir->sourceNodes || irIdx >= ir->instructionCount) return;
  u32 line = 0, column = 0;
  if (!DebugPosition(ir->sourceNodes[irIdx], &line, &column)) return;
  u32 scope = ir->sourceScopes ? ir->sourceScopes[irIdx] : 0;
  if (scope >= debugScopeIds.size()) scope = 0;
  u32 file = DebugFileOfScope(scope);
  bool scopeChanged = !debugLocationValid || scope != debugLastScope;
  if (!scopeChanged && file == debugLastFile && line == debugLastLine) return;
  if (scopeChanged) {
    u32 ops[] = {DebugScopeFunction(scope), DebugInlinedAt(scope)};
    DebugInFunction(NonSemanticShaderDebugInfo100DebugScope, ops, scope != 0 ? 2 : 1);
  }
  u32 lineOps[] = {DebugSourceOf(file), DebugConstant(line), DebugConstant(line), DebugConstant(column),
                   DebugConstant(column)};
  DebugInFunction(NonSemanticShaderDebugInfo100DebugLine, lineOps, 5);
  Emit(spv::OpLine, DebugFileString(file), line, column);
  debugLocationValid = true;
  debugLastScope = scope;
  debugLastFile = file;
  debugLastLine = line;
}

// ---- Local variables ----

// The debug type of a scalar, vector or matrix of 32-bit floats, ints,
// uints or bools; 0 for any other type (a struct's is DebugStructType's).
u32 SPIRVBuilder::DebugTypeOf(CoreType type) {
  u32 index = static_cast<u32>(type);
  if (index >= debugTypeIds.size()) debugTypeIds.resize(static_cast<u32>(CoreType::COUNT), 0);
  if (index >= debugTypeIds.size()) return 0;
  if (debugTypeIds[index] != 0) return debugTypeIds[index];
  u32 id = 0;
  if (type == CoreType::MAT2 || type == CoreType::MAT3 || type == CoreType::MAT4) {
    CoreType column = type == CoreType::MAT2 ? CoreType::FLOAT2 : type == CoreType::MAT3 ? CoreType::FLOAT3 : CoreType::FLOAT4;
    u32 columns = type == CoreType::MAT2 ? 2 : type == CoreType::MAT3 ? 3 : 4;
    u32 ops[] = {DebugTypeOf(column), DebugConstant(columns), GetBoolConstantId(true)};
    id = DebugGlobal(NonSemanticShaderDebugInfo100DebugTypeMatrix, ops, 3);
  } else {
    CoreType scalar = CoreTypeScalarType(type);
    if (scalar == CoreType::INVALID) return 0;
    if (scalar != type) {
      u32 ops[] = {DebugTypeOf(scalar), DebugConstant(CoreTypeScalarComponentCount(type))};
      id = DebugGlobal(NonSemanticShaderDebugInfo100DebugTypeVector, ops, 2);
    } else {
      const char* name = scalar == CoreType::FLOAT ? "float" : scalar == CoreType::INT ? "int"
                       : scalar == CoreType::UINT ? "uint" : "bool";
      u32 encoding = scalar == CoreType::FLOAT ? NonSemanticShaderDebugInfo100Float
                   : scalar == CoreType::INT ? NonSemanticShaderDebugInfo100Signed
                   : scalar == CoreType::UINT ? NonSemanticShaderDebugInfo100Unsigned
                                              : NonSemanticShaderDebugInfo100Boolean;
      u32 ops[] = {DebugString(name, static_cast<u32>(strlen(name))), DebugConstant(32), DebugConstant(encoding),
                   DebugConstant(NonSemanticShaderDebugInfo100None)};
      id = DebugGlobal(NonSemanticShaderDebugInfo100DebugTypeBasic, ops, 4);
    }
  }
  return debugTypeIds[index] = id;
}

// The debug type of a value of `type`, or of the struct `structHash` names
// for a struct; 0 when it can't be described.
u32 SPIRVBuilder::DebugTypeOf(CoreType type, u32 structHash) {
  if (type == CoreType::CUSTOM || type == CoreType::ENUM) return structHash != 0 ? DebugStructType(structHash) : 0;
  return DebugTypeOf(type);
}

// The size in bits of a value, as DebugTypeMember and DebugTypeComposite
// give it; 0 for a type DebugTypeOf doesn't describe.
u32 SPIRVBuilder::DebugBitsOf(CoreType type, u32 structHash) const {
  if (type == CoreType::CUSTOM || type == CoreType::ENUM) {
    for (u32 i = 0; i < ir->structTypeCount; i++) {
      if (ir->structTypes[i].nameHash == structHash) return ir->structTypes[i].totalSize * 8;
    }
    return 0;
  }
  if (type == CoreType::MAT2) return 4 * 32;
  if (type == CoreType::MAT3) return 9 * 32;
  if (type == CoreType::MAT4) return 16 * 32;
  return CoreTypeScalarComponentCount(type) * 32;
}

// A struct's DebugTypeComposite with a DebugTypeMember per field, at the
// struct's declaration; 0 when a field's type can't be described.
u32 SPIRVBuilder::DebugStructType(u32 structHash) {
  for (const auto& entry : debugStructTypes) {
    if (entry.first == structHash) return entry.second;
  }
  // Not described until its fields are: a struct can't contain itself.
  size_t slot = debugStructTypes.size();
  debugStructTypes.push_back({structHash, 0});
  const IR::IRProgram::StructTypeInfo* info = nullptr;
  for (u32 i = 0; ir->structTypes && i < ir->structTypeCount && !info; i++) {
    if (ir->structTypes[i].nameHash == structHash) info = &ir->structTypes[i];
  }
  if (!info || info->fieldCount == 0) return 0;

  NodeRef declaration;
  for (u32 i = 0; i < debugAst->structDecls.count && declaration.IsNull(); i++) {
    if (debugAst->structDecls[i].name.nameHash == structHash) declaration = NodeRef(ASTNodeType::STRUCT_DECL, i);
  }
  u32 file = debugAst->GetDeclarationSourceIndex(declaration);
  if (file >= debugAst->sourceFiles.count) file = debugMainFile;
  u32 line = 0, column = 0;
  if (!declaration.IsNull()) DebugPosition(declaration.packed, &line, &column);
  u32 source = DebugSourceOf(file);
  u32 isPublic = DebugConstant(NonSemanticShaderDebugInfo100FlagIsPublic);

  std::vector<u32> members;
  for (u32 f = info->fieldOffset; f < info->fieldOffset + info->fieldCount; f++) {
    CoreType fieldType = static_cast<CoreType>(ir->structFieldTypes[f]);
    u32 fieldHash = ir->structFieldTypeHashes ? ir->structFieldTypeHashes[f] : 0;
    u32 type = DebugTypeOf(fieldType, fieldHash);
    if (type == 0) return 0;
    u32 bits = DebugBitsOf(fieldType, fieldHash);
    u32 count = ir->structFieldArraySizes ? ir->structFieldArraySizes[f] : 0;
    if (count > 0) {
      type = DebugArrayTypeOf(type, count);
      bits *= count;
    }
    std::string name = ir->structFieldNameHashes ? ReverseLookup::GetString(ir->structFieldNameHashes[f]) : "";
    if (name.empty() || name.rfind("<hash:", 0) == 0) name = "field" + std::to_string(f - info->fieldOffset);
    u32 offset = ir->structFieldByteOffsets ? ir->structFieldByteOffsets[f] * 8 : 0;
    u32 ops[] = {DebugString(name.c_str(), static_cast<u32>(name.size())), type, source, DebugConstant(line),
                 DebugConstant(column), DebugConstant(offset), DebugConstant(bits), isPublic};
    members.push_back(DebugGlobal(NonSemanticShaderDebugInfo100DebugTypeMember, ops, 8));
  }

  std::string name = ReverseLookup::GetString(structHash);
  if (name.empty() || name.rfind("<hash:", 0) == 0) name = "struct";
  u32 nameString = DebugString(name.c_str(), static_cast<u32>(name.size()));
  std::vector<u32> ops = {nameString, DebugConstant(NonSemanticShaderDebugInfo100Structure), source,
                          DebugConstant(line), DebugConstant(column), debugCompilationUnit, nameString,
                          DebugConstant(info->totalSize * 8), isPublic};
  ops.insert(ops.end(), members.begin(), members.end());
  u32 id = DebugGlobal(NonSemanticShaderDebugInfo100DebugTypeComposite, ops.data(), static_cast<u32>(ops.size()));
  return debugStructTypes[slot].second = id;
}

// A fixed-size array of `count` elements of the debug type `element`.
u32 SPIRVBuilder::DebugArrayTypeOf(u32 element, u32 count) {
  for (const DebugArrayType& array : debugArrayTypes) {
    if (array.element == element && array.count == count) return array.id;
  }
  u32 ops[] = {element, DebugConstant(count)};
  u32 id = DebugGlobal(NonSemanticShaderDebugInfo100DebugTypeArray, ops, 2);
  debugArrayTypes.push_back({element, count, id});
  return id;
}

u32 SPIRVBuilder::DebugEmptyExpression() {
  if (debugExpression == 0) debugExpression = DebugGlobal(NonSemanticShaderDebugInfo100DebugExpression, nullptr, 0);
  return debugExpression;
}

// The first instruction that defines `reg`, or instructionCount.
u32 SPIRVBuilder::DebugFirstDefinition(u16 reg) const {
  for (u32 i = 0; i < ir->instructionCount; i++) {
    if (ir->destinations[i] == reg) return i;
  }
  return ir->instructionCount;
}

u32 SPIRVBuilder::DebugScopeAt(u32 irIdx) const {
  u32 scope = irIdx < ir->instructionCount && ir->sourceScopes ? ir->sourceScopes[irIdx] : 0;
  return scope < debugScopeIds.size() ? scope : 0;
}

// The source variable named `nameHash` in a scope: one per name and inlined
// function body, of debug type `type`, declared at the position of the
// source node `packedNode` (where it's first assigned).
u32 SPIRVBuilder::DebugVariableOf(u32 nameHash, u32 type, u32 scope, u32 packedNode) {
  for (const DebugVariable& variable : debugVariables) {
    if (variable.nameHash == nameHash && variable.scope == scope) return variable.id;
  }
  std::string name = ReverseLookup::GetString(nameHash);
  if (type == 0 || name.empty() || name.rfind("<hash:", 0) == 0) return 0;
  u32 line = 0, column = 0;
  DebugPosition(packedNode, &line, &column);
  u32 ops[] = {DebugString(name.c_str(), static_cast<u32>(name.size())), type, DebugSourceOf(DebugFileOfScope(scope)),
               DebugConstant(line), DebugConstant(column), DebugScopeFunction(scope),
               DebugConstant(NonSemanticShaderDebugInfo100FlagIsLocal)};
  u32 id = DebugGlobal(NonSemanticShaderDebugInfo100DebugLocalVariable, ops, 7);
  debugVariables.push_back({nameHash, scope, id, false});
  return id;
}

// A DebugValue: named register `reg` holds its source variable's value from
// here on. Registers kept in memory (address-taken, or a phi variable) are
// declared instead, or not described.
void SPIRVBuilder::EmitDebugValue(u16 reg, u32 irIdx) {
  if (!debugAst || !ir->registerNameHashes || reg >= idCapacity || ir->registerNameHashes[reg] == 0) return;
  if (localVarIds && localVarIds[reg] != 0) return;
  if (reg < phiLoadCount && phiLoadVariables[reg] != 0) return;
  // A pointer is a tag in its own variable, not a value; and a value whose
  // id a later instruction defines (a phi's operand) isn't one yet.
  if (ir->registerStorageInfo && (ir->registerStorageInfo[reg] & IR::IRProgram::STORAGE_IS_PTR)) return;
  if (reg < localPointerTagVars.size() && localPointerTagVars[reg] != 0) return;
  if (hasPreAllocatedId && hasPreAllocatedId[reg]) return;
  u32 value = spirvIds[reg];
  if (value == 0 || IsUndefRegister(reg)) return;
  u32 structHash = ir->registerStructTypes ? ir->registerStructTypes[reg] : 0;
  u32 type = DebugTypeOf(static_cast<CoreType>(ir->registerTypes[reg]), structHash);
  u32 node = irIdx < ir->instructionCount ? ir->sourceNodes[irIdx] : 0xFFFFFFFF;
  u32 variable = DebugVariableOf(ir->registerNameHashes[reg], type, DebugScopeAt(irIdx), node);
  if (variable == 0) return;
  // A variable in memory is described by its DebugDeclare.
  for (const DebugVariable& known : debugVariables) {
    if (known.id == variable && known.declared) return;
  }
  // A copy of the value the variable already has here adds nothing.
  for (const auto& last : debugBlockValues) {
    if (last.first == variable && last.second == value) return;
  }
  bool replaced = false;
  for (auto& last : debugBlockValues) {
    if (last.first == variable) {
      last.second = value;
      replaced = true;
    }
  }
  if (!replaced) debugBlockValues.push_back({variable, value});
  u32 ops[] = {variable, value, DebugEmptyExpression()};
  DebugInFunction(NonSemanticShaderDebugInfo100DebugValue, ops, 3);
}

// After IR instruction irIdx: the value it gave a named register.
void SPIRVBuilder::EmitDebugValues(u32 irIdx) {
  if (!debugAst || irIdx >= ir->instructionCount) return;
  switch (ir->opcodes[irIdx]) {
    case IR::OP_NOP: case IR::OP_JUMP: case IR::OP_BRANCH: case IR::OP_RET: case IR::OP_SWITCH:
    case IR::OP_DISCARD: case IR::OP_LOCAL_STORE: case IR::OP_ARRAY_STORE: case IR::OP_STORE_OUTPUT:
    case IR::OP_STORE_BUFFER: case IR::OP_STORE_SHARED: case IR::OP_STORE_LOCAL: case IR::OP_IMG_STORE:
    case IR::OP_STRUCT_STORE: case IR::OP_BARRIER: case IR::OP_MEM_FENCE:
      return;
    default:
      break;
  }
  u16 dest = ir->destinations[irIdx];
  if (IR::IsConstant(dest)) return;
  EmitDebugValue(dest, irIdx);
}

// The values a block's phis give named registers, after the phis.
void SPIRVBuilder::EmitDebugPhiValues(u32 block) {
  if (!debugAst || !cfg || block >= cfg->blockCount) return;
  u32 first = cfg->firstInst[block];
  for (u32 p = 0; p < ir->phiCount; p++) {
    if (ir->phiBlockIndices[p] != block) continue;
    EmitDebugValue(ir->phiResultRegs[p], first);
  }
}

// A DebugDeclare: the variable named `nameHash`, in `scope` from the source
// node `packedNode` on, lives in the OpVariable `pointer`.
void SPIRVBuilder::EmitDebugDeclare(u32 nameHash, u32 type, u32 scope, u32 packedNode, u32 pointer) {
  u32 variable = DebugVariableOf(nameHash, type, scope, packedNode);
  if (variable == 0) return;
  for (DebugVariable& known : debugVariables) {
    if (known.id != variable) continue;
    if (known.declared) return;  // Another register of the same variable
    known.declared = true;
  }
  u32 ops[] = {variable, pointer, DebugEmptyExpression()};
  DebugInFunction(NonSemanticShaderDebugInfo100DebugDeclare, ops, 3);
}

// Locals that live in an OpVariable (address-taken or struct-field-written
// locals, and local arrays): declared once, in the entry block after the
// variables, at the scope and line of their first definition.
void SPIRVBuilder::EmitDebugDeclares() {
  if (!debugAst) return;
  if (localVarIds && ir->registerNameHashes) {
    u32 count = std::min<u32>(ir->registerCount, idCapacity);
    for (u32 reg = 0; reg < count; reg++) {
      if (localVarIds[reg] == 0 || ir->registerNameHashes[reg] == 0) continue;
      // A local array's register: the array is declared below.
      if (ir->registerStorageInfo && (ir->registerStorageInfo[reg] & IR::IRProgram::STORAGE_IS_LOCAL_ARRAY)) continue;
      u32 definition = DebugFirstDefinition(static_cast<u16>(reg));
      u32 scope = DebugScopeAt(definition);
      u32 node = definition < ir->instructionCount ? ir->sourceNodes[definition] : 0xFFFFFFFF;
      // A definition in a function inlined into the scope that names the
      // variable, as `Outer made = make(uv);` assigns in make's body, is
      // the variable's at the call there.
      u32 named = ir->instructionCount;
      for (u32 i = 0; i < ir->instructionCount && named == ir->instructionCount; i++) {
        u16 op = ir->opcodes[i];
        if ((op == IR::OP_LOCAL_VAR_PTR || op == IR::OP_LOCAL_FIELD_PTR) && ir->GetOperand(i, 0) == reg) named = i;
      }
      u32 target = DebugScopeAt(named);
      u32 callNode = node;
      for (u32 s = scope; s != target && s != 0 && s < ir->debugScopeCount;) {
        callNode = ir->debugScopeCalls[s];
        s = ir->debugScopeParents[s];
        if (s == target) {
          scope = target;
          node = callNode;
        }
      }
      u32 structHash = ir->registerStructTypes ? ir->registerStructTypes[reg] : 0;
      u32 type = DebugTypeOf(static_cast<CoreType>(ir->registerTypes[reg]), structHash);
      EmitDebugDeclare(ir->registerNameHashes[reg], type, scope, node, localVarIds[reg]);
    }
  }
  for (u32 i = 0; ir->localArrayNameHashes && i < ir->localArrayCount; i++) {
    if (localArrayVarIds[i] == 0 || ir->localArrayNameHashes[i] == 0) continue;
    CoreType elementType = static_cast<CoreType>(ir->localArrayTypes[i]);
    u32 structHash = ir->localArrayStructTypes ? ir->localArrayStructTypes[i] : 0;
    if (elementType == CoreType::INVALID && structHash != 0) elementType = CoreType::CUSTOM;
    u32 element = DebugTypeOf(elementType, structHash);
    if (element == 0) continue;
    u32 definition = DebugFirstDefinition(ir->localArrayRegisters[i]);
    u32 node = definition < ir->instructionCount ? ir->sourceNodes[definition] : 0xFFFFFFFF;
    EmitDebugDeclare(ir->localArrayNameHashes[i], DebugArrayTypeOf(element, ir->localArraySizes[i]),
                     DebugScopeAt(definition), node, localArrayVarIds[i]);
  }
}

}  // namespace BWSL
