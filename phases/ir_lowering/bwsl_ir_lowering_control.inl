// Part of the header-only IRLowering implementation. Include via bwsl_ir_lowering.h only.

inline void IRLowering::LowerStatement(NodeRef ref) {
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

inline void IRLowering::LowerStatementWithReturnGuard(NodeRef ref) {
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

inline void IRLowering::LowerBreak() {
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

inline void IRLowering::LowerSkip() {
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

inline void IRLowering::LowerDiscard() {
  // Emit discard instruction - terminates fragment shader execution
  // This is a terminator instruction (like return) that ends the current
  // block
  builder.EmitInstruction(OP_DISCARD, 0, 0);
}

inline void IRLowering::PushLoopContext() {
  if (loopStackDepth < MAX_LOOP_NESTING) {
    pendingBreakCounts[loopStackDepth] = 0;
    pendingSkipCounts[loopStackDepth] = 0;
    loopStackDepth++;
  }
}

inline void IRLowering::PopLoopContext(u32 continueTarget, u32 breakTarget) {
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

inline void IRLowering::LowerForCStyle(NodeRef ref) {
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

inline void IRLowering::LowerForRange(NodeRef ref) {
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

inline void IRLowering::LowerForCollection(NodeRef ref) {
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

inline void IRLowering::LowerLoop(NodeRef ref) {
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

inline void IRLowering::LowerSwitch(NodeRef ref) {
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
          ReportError(
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
          ReportError(
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

inline void IRLowering::LowerIfStatement(NodeRef ref) {
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

inline void IRLowering::LowerReturn(NodeRef ref) {
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

inline u16 IRLowering::EnsureBoolCondition(u16 condReg) {
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

inline u16 IRLowering::CombineLoopCondition(u16 condReg) {
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

