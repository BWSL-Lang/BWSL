// Part of bwsl_spirv_backend.cpp. Include from that file only.
// Block labels, branches, structured control flow, phi nodes, and function body emission.
#pragma once
#include "bwsl_spirv_backend.cpp"

namespace BWSL {

u32 SPIRVBuilder::GetOrCreateBlockLabel(u32 ir_idx) {
  // Check if we already have a label for this IR index
  for (u32 i = 0; i < blockCount; i++) {
    if (blockIRIndices[i] == ir_idx) {
      return blockLabels[i];
    }
  }

  // Create new label
  u32 label_id = AllocateId();
  blockLabels[blockCount] = label_id;
  blockIRIndices[blockCount] = ir_idx;
  blockCount++;

  return label_id;
}

void SPIRVBuilder::EmitBranch(u32 ir_idx) {
  // For OP_BRANCH, we may need to convert the condition to bool BEFORE
  // emitting OpSelectionMerge, since SPIR-V requires OpSelectionMerge
  // to be immediately followed by OpBranchConditional
  IR::OpCode op = static_cast<IR::OpCode>(ir->opcodes[ir_idx]);

  if (op == IR::OP_BRANCH) {
    // Check if this is an empty if-block (both targets are the same)
    // In this case, we emit OpBranch instead of OpBranchConditional
    // and skip the OpSelectionMerge entirely (fixes SPIRV-Cross DCE bug)
    u32 true_target = ir->GetBranchTrueTarget(ir_idx);
    u32 false_target = ir->GetBranchFalseTarget(ir_idx);

    if (true_target == false_target) {
      // Empty if-block: just emit unconditional branch, skip merge
      u32 label = GetOrCreateBlockLabel(true_target);
      Emit(spv::OpBranch, label);
      return;
    }

    // Pre-convert condition to bool if needed (before merge instruction)
    u16 cond_reg = ir->GetOperand(ir_idx, 0);
    CoreType condType = CoreType::BOOL;
    if (cond_reg & 0xC000) {
      // Constant-encoded condition
      if ((cond_reg & 0xC000) == 0xC000) {
        condType = CoreType::BOOL;
      } else if (cond_reg & 0x8000) {
        condType = CoreType::FLOAT;
      } else if (cond_reg & 0x4000) {
        condType = CoreType::INT;
      } else if (cond_reg & 0x2000) {
        condType = CoreType::UINT;
      }
    } else if (cond_reg < ir->registerCount && ir->registerTypes) {
      condType = static_cast<CoreType>(ir->registerTypes[cond_reg]);
    }

    if (condType != CoreType::BOOL) {
      // Convert non-bool to bool: condition != 0
      u32 condition = GetSpirvId(cond_reg);
      u32 bool_type = GetTypeId(CoreType::BOOL);
      u32 bool_result = AllocateId();

      if (mask(condType) & TypeMasks::FLOAT_TYPES) {
        u32 zero = GetFloatConstantId(0.0f);
        Emit(spv::OpFOrdNotEqual, bool_type, bool_result, condition, zero);
      } else if (mask(condType) & TypeMasks::UINT_TYPES) {
        u32 zero = GetIntConstantId(0, true);
        Emit(spv::OpINotEqual, bool_type, bool_result, condition, zero);
      } else {
        u32 zero = GetIntConstantId(0, false);
        Emit(spv::OpINotEqual, bool_type, bool_result, condition, zero);
      }

      // Store the converted bool ID for use in TranslateInstruction
      // We use a simple approach: override the register mapping temporarily
      branchConditionOverride = bool_result;
      branchConditionOverrideReg = cond_reg;
    } else {
      branchConditionOverride = 0;
    }
  }

  // Check if this instruction has structured control flow info
  u32 structInfo = ir->structureInfo[ir_idx];

  if (structInfo != 0) {
    // This is a structured control flow header - emit merge instruction first
    EmitStructuredControlFlow(ir_idx);
  }

  // Now emit the actual branch
  TranslateInstruction(ir_idx);

  // Reset override
  branchConditionOverride = 0;
}

void SPIRVBuilder::EmitStructuredControlFlow(u32 ir_idx) {
  // Read structure info from IR
  u32 structInfo = ir->structureInfo[ir_idx];
  if (structInfo == 0)
    return;

  u32 structType = structInfo & IR::IRProgram::STRUCT_TYPE_MASK;
  u32 mergeInst = structInfo & IR::IRProgram::STRUCT_TARGET_MASK;

  // Get the merge block label
  u32 mergeLabel = GetOrCreateBlockLabel(mergeInst);

  switch (structType) {
  case IR::IRProgram::STRUCT_IF_HEADER: {
    // OpSelectionMerge: emit before OpBranchConditional
    // Format: OpSelectionMerge merge_block selection_control
    // Selection control 0 = None
    Emit(spv::OpSelectionMerge, mergeLabel,
         static_cast<u32>(spv::SelectionControlMaskNone));
    break;
  }

  case IR::IRProgram::STRUCT_LOOP_HEADER: {
    // OpLoopMerge: emit before OpBranch or OpBranchConditional in loop header
    // Format: OpLoopMerge merge_block continue_block loop_control

    // Get continue target from IR
    u32 continueInst = ir->continueInfo[ir_idx];
    u32 continueLabel = (continueInst != 0xFFFFFFFF)
                            ? GetOrCreateBlockLabel(continueInst)
                            : mergeLabel;

    Emit(spv::OpLoopMerge, mergeLabel, continueLabel,
         static_cast<u32>(spv::LoopControlMaskNone));
    break;
  }

  case IR::IRProgram::STRUCT_SWITCH_HEADER: {
    // OpSelectionMerge: emit before OpSwitch
    Emit(spv::OpSelectionMerge, mergeLabel,
         static_cast<u32>(spv::SelectionControlMaskNone));
    break;
  }

  default:
    break;
  }
}

void SPIRVBuilder::EmitPhiNodes(u32 blockIndex) {
  // Emit all PHI nodes for a given block
  // PHI nodes must be emitted at the start of a block, right after OpLabel

  // IR PHI storage:
  // - phiBlockIndices[i]: which block the PHI belongs to
  // - phiResultRegs[i]: result register
  // - phiTypes[i]: result type
  // - phiOperandOffsets[i]: start index into phiOperandValues/phiOperandBlocks
  // - phiOperandValues[]: values from each predecessor
  // - phiOperandBlocks[]: which predecessor each value comes from

  if (!ir->phiBlockIndices || !ir->phiOperandOffsets || !ir->phiOperandValues ||
      ir->phiCount == 0)
    return;

  for (u32 phi_idx = 0; phi_idx < ir->phiCount; phi_idx++) {
    if (ir->phiBlockIndices[phi_idx] != blockIndex)
      continue;

    // Get PHI operand count using the IR program's helper
    u32 operandCount = ir->GetPhiOperandCount(phi_idx);
    if (operandCount == 0)
      continue;

    u16 phiResultReg = ir->phiResultRegs[phi_idx];

    // Check if all PHI operands have the same value (trivial PHI)
    // This can happen after variant specialization eliminates one branch
    bool isTrivial = true;
    u16 firstValueReg = ir->GetPhiOperandValue(phi_idx, 0);
    for (u32 op = 1; op < operandCount; op++) {
      if (ir->GetPhiOperandValue(phi_idx, op) != firstValueReg) {
        isTrivial = false;
        break;
      }
    }

    if (isTrivial) {
      // All operands are the same - just alias the result to the source value
      // Don't emit an OpPhi, just map the result register to the source's ID
      u32 source_id = GetSpirvId(firstValueReg);
      if (phiResultReg < idCapacity) {
        spirvIds[phiResultReg] = source_id;
      }
      continue;
    }

    // Check if any operand is address-taken (has a localVarId)
    // For address-taken variables, we need to load from the OpVariable instead
    // of using SSA
    bool hasAddressTakenOperand = false;
    u32 addressTakenVarId = 0;
    CoreType addressTakenType = CoreType::INT;
    for (u32 op = 0; op < operandCount; op++) {
      u16 valueReg = ir->GetPhiOperandValue(phi_idx, op);
      if (valueReg < idCapacity && localVarIds[valueReg] != 0) {
        hasAddressTakenOperand = true;
        addressTakenVarId = localVarIds[valueReg];
        // Get the type for the load
        if (valueReg < ir->registerCount && ir->registerTypes) {
          addressTakenType = static_cast<CoreType>(ir->registerTypes[valueReg]);
        }
        break;
      }
    }

    if (hasAddressTakenOperand && addressTakenVarId != 0) {
      // For address-taken variables, skip the phi and emit OpLoad instead
      // The OpVariable holds the current value after all branches
      u32 result_type_id = GetTypeId(addressTakenType);
      if (result_type_id == 0)
        result_type_id = GetTypeId(CoreType::INT);
      u32 load_result = AllocateId();
      Emit(spv::OpLoad, result_type_id, load_result, addressTakenVarId);
      if (phiResultReg < idCapacity) {
        spirvIds[phiResultReg] = load_result;
      }
      continue;
    }

    // Get PHI result type and register
    // Use pre-allocated ID if available (from EmitFunctionBody Phase 1),
    // otherwise allocate a fresh one
    u32 result_id;
    if (phiResultReg < idCapacity && spirvIds[phiResultReg] != 0) {
      // Use the pre-allocated ID
      result_id = spirvIds[phiResultReg];
    } else {
      // Fallback: allocate a fresh ID and map it
      result_id = AllocateId();
      if (phiResultReg < idCapacity) {
        spirvIds[phiResultReg] = result_id;
      }
    }

    // Get the PHI result type
    CoreType phiType = static_cast<CoreType>(ir->phiTypes[phi_idx]);
    u32 type_id = 0;
    if (phiType == CoreType::CUSTOM || phiType == CoreType::ENUM) {
      // For struct/custom types, get the type from one of the PHI operands
      u16 firstValueReg = ir->GetPhiOperandValue(phi_idx, 0);
      if (firstValueReg < ir->registerCount && ir->registerStructTypes) {
        u32 structHash = ir->registerStructTypes[firstValueReg];
        type_id = GetStructTypeId(structHash);
      }
    } else if (phiType == CoreType::INVALID || phiType == CoreType::VOID) {
      // Try to infer from the first operand register's type
      u16 firstValueReg = ir->GetPhiOperandValue(phi_idx, 0);
      if (firstValueReg < ir->registerCount && ir->registerTypes) {
        CoreType opType =
            static_cast<CoreType>(ir->registerTypes[firstValueReg]);
        if (opType == CoreType::CUSTOM || opType == CoreType::ENUM) {
          if (ir->registerStructTypes) {
            u32 structHash = ir->registerStructTypes[firstValueReg];
            type_id = GetStructTypeId(structHash);
          }
        } else {
          type_id = GetTypeId(opType);
        }
      }
    } else {
      type_id = GetTypeId(phiType);
    }
    if (type_id == 0) {
      // Final fallback to a safe scalar type
      type_id = GetTypeId(CoreType::FLOAT);
    }

    // Build OpPhi instruction
    // Format: OpPhi result_type result_id (variable_id, parent_block_id)+
    u32 totalWords = 3 + operandCount * 2; // opcode + type + result + pairs

    if (currentFunctionSize + totalWords > currentFunctionCapacity) {
      GrowCurrentFunction();
    }

    currentFunction[currentFunctionSize++] = (totalWords << 16) | spv::OpPhi;
    currentFunction[currentFunctionSize++] = type_id;
    currentFunction[currentFunctionSize++] = result_id;

    // Emit operand pairs (value, block label)
    for (u32 op = 0; op < operandCount; op++) {
      u16 valueReg = ir->GetPhiOperandValue(phi_idx, op);
      u32 sourceBlockIdx = ir->GetPhiOperandBlock(phi_idx, op);

      // Get the SPIR-V ID for the value
      u32 value_id = GetSpirvId(valueReg);

      // Get the block label - the source block's first instruction determines
      // its label
      u32 sourceBlockFirstInst = cfg->firstInst[sourceBlockIdx];
      u32 block_label = GetOrCreateBlockLabel(sourceBlockFirstInst);

      currentFunction[currentFunctionSize++] = value_id;
      currentFunction[currentFunctionSize++] = block_label;
    }
  }
}

// ============= Function Emission =============

void SPIRVBuilder::SimplifyTrivialPhis() {
  // Pre-process PHIs before emission: if all operands of a PHI have the same
  // value, simply map the PHI result register to that value's SPIR-V ID. This
  // handles cases where variant specialization eliminates one branch, making a
  // PHI trivial.

  if (!ir)
    return;
  if (!ir->phiBlockIndices || !ir->phiResultRegs || !ir->phiOperandOffsets ||
      !ir->phiOperandValues)
    return;
  if (ir->phiCount == 0)
    return;

  for (u32 phi_idx = 0; phi_idx < ir->phiCount; phi_idx++) {
    u32 operandCount = ir->GetPhiOperandCount(phi_idx);
    if (operandCount == 0)
      continue;

    // Check if all operands have the same value register
    u16 firstValueReg = ir->GetPhiOperandValue(phi_idx, 0);
    bool allSame = true;
    for (u32 op = 1; op < operandCount; op++) {
      if (ir->GetPhiOperandValue(phi_idx, op) != firstValueReg) {
        allSame = false;
        break;
      }
    }

    if (allSame) {
      // Trivial PHI - map result register to source value's ID
      u16 phiResultReg = ir->phiResultRegs[phi_idx];
      u32 source_id = GetSpirvId(firstValueReg);
      if (phiResultReg < idCapacity) {
        spirvIds[phiResultReg] = source_id;
      }
    }
  }
}

void SPIRVBuilder::EmitFunction() {
  // Emit the main shader function
  // 1. Declare interface variables
  DeclareInputOutput();
  DeclareResources();
  DeclareSharedVariables();

  // 2. Emit entry point (now that we know all interface variables)
  EmitEntryPoint();

  // 3. Pre-simplify trivial PHIs before function body
  // This handles cases where variant specialization makes a PHI trivial
  // Must be after DeclareInputOutput sets up spirvIds array
  SimplifyTrivialPhis();

  // 4. Emit function type (void function taking no parameters)
  u32 void_type = GetTypeId(CoreType::VOID);
  u32 func_type_id = GetFunctionTypeId(void_type, nullptr, 0);

  // 5. OpFunction
  // Format: result_type, result_id, function_control, function_type
  Emit(spv::OpFunction, void_type, entryPointId,
       static_cast<u32>(spv::FunctionControlMaskNone), func_type_id);

  // 6. Emit function body
  EmitFunctionBody();

  // 7. OpFunctionEnd
  Emit(spv::OpFunctionEnd);

  // 8. Copy current function to functions section
  if (functions.count + currentFunctionSize > functions.capacity) {
    while (functions.count + currentFunctionSize > functions.capacity) {
      GrowSection(&functions);
    }
  }
  memcpy(&functions.words[functions.count], currentFunction,
         currentFunctionSize * sizeof(u32));
  functions.count += currentFunctionSize;
}

void SPIRVBuilder::EmitFunctionBody() {
  // Emit all basic blocks in CFG order
  // This ensures proper PHI node emission and structured control flow

  // IMPORTANT: Pre-allocate SPIR-V IDs in the correct order to handle PHI
  // dependencies.
  //
  // Problem: When PHI A references PHI B's result (e.g., loop header PHI
  // references if-merge PHI), and PHI A is emitted before PHI B, we need PHI
  // B's result ID to already be allocated. Otherwise, we'd allocate a temporary
  // ID for the operand that never gets defined.
  //
  // Solution: Pre-allocate PHI RESULT IDs first, then pre-allocate operand IDs.
  // This ensures that when we reference a PHI result as an operand, we get the
  // correct ID that will be defined when that PHI is emitted.

  // Phase 1: Pre-allocate IDs for all PHI RESULTS
  if (ir->phiCount > 0 && ir->phiResultRegs) {
    for (u32 phi_idx = 0; phi_idx < ir->phiCount; phi_idx++) {
      u16 phiResultReg = ir->phiResultRegs[phi_idx];
      if (phiResultReg < idCapacity && spirvIds[phiResultReg] == 0) {
        spirvIds[phiResultReg] = AllocateId();
      }
    }
  }

  // Phase 2: Pre-allocate IDs for PHI OPERANDS (that aren't PHI results)
  // This handles cases where a PHI references a value from a block that comes
  // later in CFG order (e.g., back edges in loops).
  //
  // We also need to check if the register is an "undef" register (from SSA when
  // a variable isn't defined on some path to a PHI) and emit OpUndef for it.
  if (ir->phiCount > 0 && ir->phiOperandValues) {
    for (u32 phi_idx = 0; phi_idx < ir->phiCount; phi_idx++) {
      u32 operandCount = ir->GetPhiOperandCount(phi_idx);
      for (u32 op = 0; op < operandCount; op++) {
        u16 valueReg = ir->GetPhiOperandValue(phi_idx, op);
        // Skip constants (they have special encoding and are handled
        // differently) 0x8000=float, 0x4000=int, 0x2000=uint, 0xC000=bool
        if (valueReg & 0xE000)
          continue;
        // Pre-allocate an ID for this register if it doesn't have one
        // Use GetSpirvId which handles undef registers properly
        if (valueReg < idCapacity && spirvIds[valueReg] == 0) {
          // Check if this is an undef register first
          bool isUndef = false;
          if (ir->undefRegs && ir->undefRegCount > 0) {
            for (u32 i = 0; i < ir->undefRegCount; i++) {
              if (ir->undefRegs[i] == valueReg) {
                isUndef = true;
                break;
              }
            }
          }
          // Call GetSpirvId - it will check for undef registers and emit
          // OpUndef if needed
          (void)GetSpirvId(valueReg);
          // Mark as pre-allocated if NOT an undef (undef already has
          // definition) This flag tells STORE_REG to emit OpCopyObject to
          // define this ID
          if (!isUndef && valueReg < idCapacity) {
            hasPreAllocatedId[valueReg] = true;
          }
        }
      }
    }
  }

  // Pre-pass: collect all OP_LOCAL_VAR_PTR instructions and pre-allocate
  // OpVariable IDs. SPIR-V requires all OpVariable instructions to be at the
  // start of the first block. We use a separate "emittedLocalVars" flag array
  // to track which have been emitted.
  bool *emittedLocalVars = nullptr;
  if (idCapacity > 0) {
    emittedLocalVars = (bool *)arena->Allocate(idCapacity * sizeof(bool), 64);
    memset(emittedLocalVars, 0, idCapacity * sizeof(bool));
  }

  for (u32 i = 0; i < ir->instructionCount; i++) {
    bool isVarPtr = (ir->opcodes[i] == IR::OP_LOCAL_VAR_PTR);
    bool isFieldPtr = (ir->opcodes[i] == IR::OP_LOCAL_FIELD_PTR);
    if (isVarPtr || isFieldPtr) {
      u16 var_reg = ir->GetOperand(i, 0);

      // Check if we already created a variable for this source register
      if (var_reg < idCapacity && localVarIds[var_reg] == 0) {
        // Get the type of the source value
        CoreType varType = CoreType::FLOAT;
        if (var_reg < ir->registerCount && ir->registerTypes) {
          varType = static_cast<CoreType>(ir->registerTypes[var_reg]);
        }
        if (varType == CoreType::INVALID || varType == CoreType::VOID) {
          varType = CoreType::INT; // Fallback
        }

        // Allocate ID for the OpVariable (will be emitted after first OpLabel)
        u32 var_id = AllocateId();
        localVarIds[var_reg] = var_id;
      }
    }
  }

  // Helper lambda to emit all local pointer OpVariables
  auto emitLocalPointerVars = [&]() {
    for (u32 i = 0; i < ir->instructionCount; i++) {
      bool isVarPtr = (ir->opcodes[i] == IR::OP_LOCAL_VAR_PTR);
      bool isFieldPtr = (ir->opcodes[i] == IR::OP_LOCAL_FIELD_PTR);
      if (isVarPtr || isFieldPtr) {
        u16 var_reg = ir->GetOperand(i, 0);
        if (var_reg < idCapacity && localVarIds[var_reg] != 0 &&
            emittedLocalVars && !emittedLocalVars[var_reg]) {
          CoreType varType = CoreType::FLOAT;
          if (var_reg < ir->registerCount && ir->registerTypes) {
            varType = static_cast<CoreType>(ir->registerTypes[var_reg]);
          }
          if (varType == CoreType::INVALID || varType == CoreType::VOID) {
            varType = CoreType::INT;
          }
          // Struct-field pointers keep the base variable's struct type —
          // resolve via the struct type hash stored in registerStructTypes
          // (GetTypeId returns 0 for CoreType::CUSTOM).
          u32 elem_type_id = 0;
          if (varType == CoreType::CUSTOM || varType == CoreType::ENUM) {
            u32 structHash = (ir->registerStructTypes && var_reg < ir->registerCount)
                                 ? ir->registerStructTypes[var_reg]
                                 : 0;
            if (structHash != 0) {
              elem_type_id = GetStructTypeId(structHash);
            }
          }
          if (elem_type_id == 0) {
            elem_type_id = GetTypeId(varType);
          }
          if (elem_type_id == 0) {
            elem_type_id = GetTypeId(CoreType::FLOAT);
          }
          u32 ptr_type_id =
              GetPointerTypeId(elem_type_id, spv::StorageClassFunction);
          u32 var_id = localVarIds[var_reg];
          Emit(spv::OpVariable, ptr_type_id, var_id, spv::StorageClassFunction);
          emittedLocalVars[var_reg] = true;
        }
      }
    }

    // Emit local array OpVariables
    if (ir && ir->localArrayCount > 0) {
      for (u32 i = 0; i < ir->localArrayCount; i++) {
        CoreType elemType = static_cast<CoreType>(ir->localArrayTypes[i]);
        u32 elemTypeId = GetTypeId(elemType);

        // Handle struct element types
        if (elemTypeId == 0 &&
            (elemType == CoreType::CUSTOM || elemType == CoreType::INVALID)) {
          if (ir->localArrayStructTypes && ir->localArrayStructTypes[i] != 0) {
            elemTypeId = GetStructTypeId(ir->localArrayStructTypes[i]);
          }
        }
        if (elemTypeId == 0) {
          elemTypeId = GetTypeId(CoreType::FLOAT);
        }

        u32 arraySize = ir->localArraySizes[i];
        u32 arrayTypeId = AllocateId();
        u32 lengthConstId = GetIntConstantId(arraySize, true);
        u32 arrayOps[] = {arrayTypeId, elemTypeId, lengthConstId};
        EmitToSection(&typesConstants, spv::OpTypeArray, arrayOps, 3);

        u32 ptrTypeId =
            GetPointerTypeId(arrayTypeId, spv::StorageClassFunction);
        u32 varId = AllocateId();
        Emit(spv::OpVariable, ptrTypeId, varId, spv::StorageClassFunction);

        u16 reg = ir->localArrayRegisters[i];
        if (reg < idCapacity) {
          spirvIds[reg] = varId;
        }

        // Store the element pointer type for later use in array access
        localArrayElemPtrTypes[i] =
            GetPointerTypeId(elemTypeId, spv::StorageClassFunction);
        localArrayVarIds[i] = varId;
      }
    }
  };

  if (!cfg || cfg->blockCount == 0) {
    // Fallback for trivial shaders without CFG
    u32 entry_label = GetOrCreateBlockLabel(0);
    Emit(spv::OpLabel, entry_label);

    // Emit all local pointer OpVariables at the start of the function
    emitLocalPointerVars();

    for (u32 i = 0; i < ir->instructionCount; i++) {
      TranslateInstruction(i);
    }

    if (ir->instructionCount == 0 ||
        !IR::IsTerminator(
            static_cast<IR::OpCode>(ir->opcodes[ir->instructionCount - 1]))) {
      Emit(spv::OpReturn);
    }
    return;
  }

  // Iterate blocks in CFG order
  for (u32 blockIdx = 0; blockIdx < cfg->blockCount; blockIdx++) {
    u32 firstInst = cfg->firstInst[blockIdx];
    u32 lastInst = cfg->lastInst[blockIdx];

    // Emit block label
    u32 labelId = GetOrCreateBlockLabel(firstInst);
    Emit(spv::OpLabel, labelId);

    // Emit all local pointer OpVariables at the start of the FIRST block
    if (blockIdx == 0) {
      emitLocalPointerVars();
    }

    // Emit PHI nodes first (required by SPIR-V spec)
    EmitPhiNodes(blockIdx);

    // Emit all instructions in this block EXCEPT the terminator
    // We need special handling for terminators to emit merge instructions first
    IR::OpCode lastOp = static_cast<IR::OpCode>(ir->opcodes[lastInst]);
    bool lastIsTerminator = IR::IsTerminator(lastOp);

    u32 endInst = lastIsTerminator ? lastInst : lastInst + 1;
    for (u32 i = firstInst; i < endInst; i++) {
      TranslateInstruction(i);
    }

    // Handle terminator with structured control flow
    if (lastIsTerminator) {
      // Use EmitBranch which handles bool conversion BEFORE merge instruction
      EmitBranch(lastInst);
    } else {
      // Block doesn't end with an explicit terminator
      // Fall-through blocks need explicit branches in SPIR-V
      // Use CFG successor information to find the target
      u32 succCount = cfg->TotalSuccessorCount(blockIdx);
      if (succCount > 0) {
        u32 succBlockIdx = cfg->GetAnySuccessor(blockIdx, 0);
        if (succBlockIdx != NO_BLOCK && succBlockIdx < cfg->blockCount) {
          u32 succFirstInst = cfg->firstInst[succBlockIdx];
          u32 succLabel = GetOrCreateBlockLabel(succFirstInst);
          Emit(spv::OpBranch, succLabel);
        } else {
          Emit(spv::OpReturn);
        }
      } else if (blockIdx + 1 < cfg->blockCount) {
        // Fallback: branch to next block in order
        u32 nextBlockFirstInst = cfg->firstInst[blockIdx + 1];
        u32 nextLabel = GetOrCreateBlockLabel(nextBlockFirstInst);
        Emit(spv::OpBranch, nextLabel);
      } else {
        // Last block with no terminator - emit return
        Emit(spv::OpReturn);
      }
    }
  }
}

// ============= Interface Setup =============

// Helper to create an interface variable with decoration


} // namespace BWSL
