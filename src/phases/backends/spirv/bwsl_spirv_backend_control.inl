// Part of bwsl_spirv_backend.cpp. Include from that file only.
// Block labels, branches, structured control flow, phi nodes, and function body emission.
#pragma once
#include "bwsl_spirv_backend.cpp"

namespace BWSL {

u32 SPIRVBuilder::TranslateArrayStoreSequence(u32 first, u32 end) {
    if (ir->opcodes[first] != IR::OP_ARRAY_STORE ||
        !Optimizations::IsEnabled(optimizations, OptimizationId::ARRAY_FILL_STORES)) return 0;
  u16 base = ir->destinations[first];
  if (base >= ir->registerCount || !ir->registerStorageInfo ||
      !(ir->registerStorageInfo[base] & IR::IRProgram::STORAGE_IS_LOCAL_ARRAY)) return 0;
  u32 array = ir->registerStorageInfo[base] >> IR::IRProgram::STORAGE_BINDING_SHIFT;
  if (array >= ir->localArrayCount) return 0;
  u32 count = ir->localArraySizes[array];
  // Stay inside the current basic block and the SPIR-V instruction word limit.
  // Partial writes and sequences interrupted by an expression retain their
  // original element stores, so no reads or side effects move across a write.
  if (count == 0 || count > end - first || count > 0xFFFF - 3) return 0;
  bool constant = true;
  for (u32 i = 0; i < count; ++i) {
    u32 inst = first + i;
    if (ir->opcodes[inst] != IR::OP_ARRAY_STORE || ir->destinations[inst] != base) return 0;
    u16 index = ir->GetOperand(inst, 0);
    if (!IR::IsUintConstant(index) || ir->uintConstants[IR::UintConstantIndex(index)] != i) return 0;
    u16 value = ir->GetOperand(inst, 1);
    if (value == 0xFFFF) return 0;
    constant &= IR::IsConstant(value) ||
                (value < idCapacity && constantComposites[value]);
  }
  std::vector<u32> values;
  values.reserve(count);
  for (u32 i = 0; i < count; ++i) values.push_back(GetSpirvId(ir->GetOperand(first + i, 1)));
  u32 composite;
  if (constant) {
    composite = GetCompositeConstantId(localArrayTypeIds[array], values.data(), count);
  } else {
    composite = AllocateId();
    u32 words = 3 + count;
    while (currentFunctionSize + words > currentFunctionCapacity) GrowCurrentFunction();
    currentFunction[currentFunctionSize++] = (words << 16) | spv::OpCompositeConstruct;
    currentFunction[currentFunctionSize++] = localArrayTypeIds[array];
    currentFunction[currentFunctionSize++] = composite;
    for (u32 value : values) currentFunction[currentFunctionSize++] = value;
  }
  Emit(spv::OpStore, localArrayVarIds[array], composite);
  return count;
}

u32 SPIRVBuilder::GetOrCreateBlockLabel(u32 ir_idx) {
  // Check if we already have a label for this IR index
  for (u32 i = 0; i < blockCount; i++) {
    if (blockIRIndices[i] == ir_idx) {
      return blockLabels[i];
    }
  }

  if (blockCount == blockCapacity) {
    u32 newCapacity = blockCapacity * 2;
    auto grow = [&](u32*& values) {
      u32* replacement = (u32*)arena->Allocate(newCapacity * sizeof(u32), 64);
      memcpy(replacement, values, blockCount * sizeof(u32));
      memset(replacement + blockCount, 0, (newCapacity - blockCount) * sizeof(u32));
      values = replacement;
    };
    grow(blockLabels);
    grow(blockIRIndices);
    grow(blockMergePoints);
    blockCapacity = newCapacity;
  }

  // Create new label
  u32 label_id = AllocateId();
  blockLabels[blockCount] = label_id;
  blockIRIndices[blockCount] = ir_idx;
  blockCount++;

  return label_id;
}

// True when jumping straight to `targetInst` would drop an edge a phi reads:
// the phi would then name a block that no longer branches to it.
bool SPIRVBuilder::DropsPhiEdge(u32 branchInst, u32 targetInst) const {
    if (!cfg || !ir->phiBlockIndices) return false;
    u32 fromBlock = cfg->instToBlock[branchInst];
    u32 toBlock = cfg->instToBlock[targetInst];
    for (u32 phi = 0; phi < ir->phiCount; phi++) {
        if (ir->phiBlockIndices[phi] == toBlock) continue;
        for (u32 op = 0; op < ir->GetPhiOperandCount(phi); op++) {
            if (ir->GetPhiOperandBlock(phi, op) == fromBlock) return true;
        }
    }
    return false;
}

// A constant condition or selector (usually left by constant propagation)
// always takes one edge, which needs no selection construct; the rest are
// unreachable. This also keeps a literal selector out of HLSL, where older
// DXC rejects `switch (2)`.
bool SPIRVBuilder::TakesConstantEdge(u32 branchInst, u32* targetInst) const {
    return IR::TryGetConstantTarget(ir, branchInst, targetInst) &&
           !DropsPhiEdge(branchInst, *targetInst);
}

// Marks the blocks reachable from the entry along the edges EmitBranch
// emits, and those edges. A block a constant branch skips is emitted as a
// label and OpUnreachable, so its outgoing edges are gone too. SPIR-V still
// requires the back edge of an emitted loop when nothing reaches its
// continue target (a body that always breaks), so such a target is emitted
// as a label and a branch to the loop header, and nothing else: what it
// computed came from blocks that are skipped.
void SPIRVBuilder::FindEmittedBlocks() {
    u32 blockCount = std::max<u32>(cfg->blockCount, 1);
    u32 edgeCount = std::max<u32>(cfg->predecessorDataCount, 1);
    blockEmission = (BlockEmission*)arena->Allocate(blockCount * sizeof(BlockEmission), 64);
    backEdgeTargets = (u32*)arena->Allocate(blockCount * sizeof(u32), 64);
    edgeEmitted = (bool*)arena->Allocate(edgeCount * sizeof(bool), 64);
    u32* queue = (u32*)arena->Allocate(blockCount * sizeof(u32), 64);
    memset(blockEmission, 0, blockCount * sizeof(BlockEmission));
    memset(backEdgeTargets, 0, blockCount * sizeof(u32));
    memset(edgeEmitted, 0, edgeCount * sizeof(bool));
    u32 head = 0, tail = 0;
    auto emitEdge = [&](u32 from, u32 to) {
        if (to >= cfg->blockCount) return;
        for (u32 k = cfg->predecessorOffsets[to]; k < cfg->predecessorOffsets[to + 1]; k++) {
            if (cfg->predecessorData[k] == from) edgeEmitted[k] = true;
        }
        if (blockEmission[to] != BlockEmission::REACHABLE) {
            blockEmission[to] = BlockEmission::REACHABLE;
            queue[tail++] = to;
        }
    };
    blockEmission[cfg->entryBlock] = BlockEmission::REACHABLE;
    queue[tail++] = cfg->entryBlock;
    while (head < tail) {
        u32 block = queue[head++];
        u32 target;
        if (TakesConstantEdge(cfg->lastInst[block], &target)) {
            emitEdge(block, cfg->instToBlock[target]);
            continue;
        }
        for (u32 s = 0; s < cfg->TotalSuccessorCount(block); s++) {
            emitEdge(block, cfg->GetAnySuccessor(block, s));
        }
    }

    if (!ir->structureInfo || !ir->continueInfo) return;
    for (u32 block = 0; block < cfg->blockCount; block++) {
        if (blockEmission[block] != BlockEmission::REACHABLE) continue;
        for (u32 i = cfg->firstInst[block]; i <= cfg->lastInst[block]; i++) {
            if ((ir->structureInfo[i] & IR::IRProgram::STRUCT_TYPE_MASK) !=
                    IR::IRProgram::STRUCT_LOOP_HEADER ||
                ir->continueInfo[i] >= ir->instructionCount) {
                continue;
            }
            u32 continueBlock = cfg->instToBlock[ir->continueInfo[i]];
            if (continueBlock < cfg->blockCount &&
                blockEmission[continueBlock] == BlockEmission::SKIPPED) {
                blockEmission[continueBlock] = BlockEmission::BACK_EDGE;
                backEdgeTargets[continueBlock] = block;
            }
        }
    }
}

bool SPIRVBuilder::IsEdgeEmitted(u32 fromBlock, u32 toBlock) const {
    if (!edgeEmitted || toBlock >= cfg->blockCount) return true;
    for (u32 k = cfg->predecessorOffsets[toBlock]; k < cfg->predecessorOffsets[toBlock + 1]; k++) {
        if (cfg->predecessorData[k] == fromBlock && edgeEmitted[k]) return true;
    }
    return false;
}

// The edge from an unreachable continue target to its loop header. A phi
// takes an undefined value on it.
bool SPIRVBuilder::IsBackEdgeOnly(u32 fromBlock, u32 toBlock) const {
    return blockEmission && fromBlock < cfg->blockCount &&
           blockEmission[fromBlock] == BlockEmission::BACK_EDGE &&
           backEdgeTargets[fromBlock] == toBlock;
}

// An OpUndef among the module's constants.
u32 SPIRVBuilder::EmitUndefConstant(u32 typeId) {
    u32 id = AllocateId();
    u32 ops[2] = {typeId, id};
    EmitToSection(&typesConstants, spv::OpUndef, ops, 2);
    return id;
}

// Translates a block's instructions, for the module-level declarations
// they create, then drops their words. The registers they define keep
// their previous IDs.
void SPIRVBuilder::DropBlockBody(u32 firstInst, u32 lastInst) {
    u32 bodyStart = currentFunctionSize;
    bool endsInTerminator = IR::IsTerminator(static_cast<IR::OpCode>(ir->opcodes[lastInst]));
    u32 bodyEnd = endsInTerminator ? lastInst : lastInst + 1;
    u32 count = bodyEnd - firstInst;
    u32* savedIds = (u32*)arena->Allocate(std::max<u32>(count, 1) * sizeof(u32), 64);
    for (u32 i = firstInst; i < bodyEnd; i++) {
        u16 reg = ir->destinations[i];
        savedIds[i - firstInst] = reg < idCapacity ? spirvIds[reg] : 0;
    }
    for (u32 i = firstInst; i < bodyEnd;) {
        u32 translated = TranslateArrayStoreSequence(i, bodyEnd);
        if (translated) i += translated;
        else TranslateInstruction(i++);
    }
    currentFunctionSize = bodyStart;
    // In reverse, so a register defined twice gets its first ID back.
    for (u32 i = bodyEnd; i > firstInst; i--) {
        u16 reg = ir->destinations[i - 1];
        if (reg < idCapacity) spirvIds[reg] = savedIds[i - 1 - firstInst];
    }
}

void SPIRVBuilder::EmitBranch(u32 ir_idx) {
  // For OP_BRANCH, we may need to convert the condition to bool BEFORE
  // emitting OpSelectionMerge, since SPIR-V requires OpSelectionMerge
  // to be immediately followed by OpBranchConditional
  IR::OpCode op = static_cast<IR::OpCode>(ir->opcodes[ir_idx]);

  u32 constantTarget;
  if (TakesConstantEdge(ir_idx, &constantTarget)) {
    Emit(spv::OpBranch, GetOrCreateBlockLabel(constantTarget));
    return;
  }

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
        if (IR::IsConstant(cond_reg)) {
            // Constant-encoded condition
            if (IR::IsBoolConstant(cond_reg) || cond_reg == IR::UnusedOperand) {
                condType = CoreType::BOOL;
            } else if (IR::IsFloatConstant(cond_reg)) {
                condType = CoreType::FLOAT;
            } else if (IR::IsIntConstant(cond_reg)) {
                condType = CoreType::INT;
            } else if (IR::IsUintConstant(cond_reg)) {
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
    } else if (cond_reg < phiLoadCount && phiLoadVariables[cond_reg] != 0) {
        // A phi kept in a variable is loaded where it's read: before the
        // merge instruction, which must come right before the branch.
        branchConditionOverride = GetSpirvId(cond_reg);
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
    u32 control = spv::SelectionControlMaskNone;
    if (structInfo & IR::IRProgram::STRUCT_HINT_BRANCH) control = spv::SelectionControlDontFlattenMask;
    else if (structInfo & IR::IRProgram::STRUCT_HINT_FLATTEN) control = spv::SelectionControlFlattenMask;
    Emit(spv::OpSelectionMerge, mergeLabel, control);
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

u32 SPIRVBuilder::GetPhiTypeId(u32 phi) {
    CoreType phiType = static_cast<CoreType>(ir->phiTypes[phi]);
    u32 typeId = 0;
    u16 firstValueReg = ir->GetPhiOperandCount(phi) > 0 ? ir->GetPhiOperandValue(phi, 0) : 0xFFFF;
    if (phiType == CoreType::INVALID || phiType == CoreType::VOID) {
        // Infer from the first operand register's type.
        if (firstValueReg < ir->registerCount && ir->registerTypes) {
            phiType = static_cast<CoreType>(ir->registerTypes[firstValueReg]);
        }
    }
    if (phiType == CoreType::CUSTOM || phiType == CoreType::ENUM) {
        // For struct/custom types, get the type from one of the PHI operands
        if (firstValueReg < ir->registerCount && ir->registerStructTypes) {
            typeId = GetStructTypeId(ir->registerStructTypes[firstValueReg]);
        }
    } else {
        typeId = GetTypeId(phiType);
    }
    // Final fallback to a safe scalar type
    return typeId != 0 ? typeId : GetTypeId(CoreType::FLOAT);
}

// What a phi's emitted incoming edges carry (see FindEmittedBlocks). Edges
// from skipped blocks don't count, and the back edge from an unreachable
// continue target carries an undefined value, so it agrees with anything.
SPIRVBuilder::PhiShape SPIRVBuilder::GetPhiShape(u32 phi, u16* trivialValue) const {
    u32 operandCount = ir->GetPhiOperandCount(phi);
    if (operandCount == 0) return PhiShape::EMPTY;
    u32 block = ir->phiBlockIndices[phi];
    bool found = false;
    for (u32 op = 0; op < operandCount; op++) {
        if (!IsEdgeEmitted(ir->GetPhiOperandBlock(phi, op), block)) continue;
        u16 value = ir->GetPhiOperandValue(phi, op);
        if (!found) {
            *trivialValue = value;
            found = true;
        } else if (value != *trivialValue) {
            return PhiShape::MIXED;
        }
    }
    if (found) return PhiShape::TRIVIAL;
    return PhiShape::UNDEFINED;
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
    // Kept in its web's variable, read where it's used (see GetSpirvId).
    if (phi_idx < phiVariableIdCount && phiVariableIds[phi_idx] != 0)
        continue;

    u16 phiResultReg = ir->phiResultRegs[phi_idx];
    u32 operandCount = ir->GetPhiOperandCount(phi_idx);
    u16 trivialValue;
    PhiShape shape = GetPhiShape(phi_idx, &trivialValue);
    if (shape == PhiShape::EMPTY)
      continue;

    if (shape == PhiShape::TRIVIAL) {
      // All operands are the same - just alias the result to the source value
      // Don't emit an OpPhi, just map the result register to the source's ID
      u32 source_id = GetSpirvId(trivialValue);
      if (phiResultReg < idCapacity) {
        spirvIds[phiResultReg] = source_id;
      }
      continue;
    }

    // No emitted edge brings a value: the block is entered only from
    // blocks that don't run. Defined, but never read at run time.
    if (shape == PhiShape::UNDEFINED) {
      u32 result_id = phiResultReg < idCapacity ? spirvIds[phiResultReg] : 0;
      if (result_id == 0) result_id = AllocateId();
      u32 ops[2] = {GetPhiTypeId(phi_idx), result_id};
      EmitToSection(&typesConstants, spv::OpUndef, ops, 2);
      if (phiResultReg < idCapacity) spirvIds[phiResultReg] = result_id;
      continue;
    }

    // Only emitted edges can feed the phi: a block a constant branch skips
    // ends in OpUnreachable instead of branching here. An unreachable
    // continue target still branches back, with an undefined value.
    auto isLive = [&](u32 op) {
        return IsEdgeEmitted(ir->GetPhiOperandBlock(phi_idx, op), blockIndex);
    };
    auto isBackEdgeOnly = [&](u32 op) {
        return IsBackEdgeOnly(ir->GetPhiOperandBlock(phi_idx, op), blockIndex);
    };

    // Check if any operand is address-taken (has a localVarId)
    // For address-taken variables, we need to load from the OpVariable instead
    // of using SSA
    bool hasAddressTakenOperand = false;
    u32 addressTakenVarId = 0;
    CoreType addressTakenType = CoreType::INT;
    for (u32 op = 0; op < operandCount; op++) {
      u16 valueReg = ir->GetPhiOperandValue(phi_idx, op);
      if (isLive(op) && valueReg < idCapacity && localVarIds[valueReg] != 0) {
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
    u32 type_id = GetPhiTypeId(phi_idx);

    // Values first: an undefined one goes to the module's constants.
    u32* values = (u32*)arena->Allocate(operandCount * sizeof(u32), 64);
    u32 pairCount = 0;
    for (u32 op = 0; op < operandCount; op++) {
      if (isLive(op)) values[op] = GetSpirvId(ir->GetPhiOperandValue(phi_idx, op));
      else if (isBackEdgeOnly(op)) values[op] = EmitUndefConstant(type_id);
      else continue;
      pairCount++;
    }

    // Build OpPhi instruction
    // Format: OpPhi result_type result_id (variable_id, parent_block_id)+
    u32 totalWords = 3 + pairCount * 2; // opcode + type + result + pairs

    if (currentFunctionSize + totalWords > currentFunctionCapacity) {
      GrowCurrentFunction();
    }

    currentFunction[currentFunctionSize++] = (totalWords << 16) | spv::OpPhi;
    currentFunction[currentFunctionSize++] = type_id;
    currentFunction[currentFunctionSize++] = result_id;

    // Emit operand pairs (value, block label)
    for (u32 op = 0; op < operandCount; op++) {
      if (!isLive(op) && !isBackEdgeOnly(op)) continue;
      u32 sourceBlockIdx = ir->GetPhiOperandBlock(phi_idx, op);

      // Get the block label - the source block's first instruction determines
      // its label
      u32 sourceBlockFirstInst = cfg->firstInst[sourceBlockIdx];
      u32 block_label = GetOrCreateBlockLabel(sourceBlockFirstInst);

      currentFunction[currentFunctionSize++] = values[op];
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
    // Only operands on emitted edges count (see GetPhiShape).
    u16 value;
    if (GetPhiShape(phi_idx, &value) != PhiShape::TRIVIAL)
      continue;
    // Trivial PHI - map result register to source value's ID
    u16 phiResultReg = ir->phiResultRegs[phi_idx];
    u32 source_id = GetSpirvId(value);
    if (phiResultReg < idCapacity) {
      spirvIds[phiResultReg] = source_id;
    }
  }
}

bool SPIRVBuilder::IsUndefRegister(u16 reg) const {
    for (u32 i = 0; ir->undefRegs && i < ir->undefRegCount; i++) {
        if (ir->undefRegs[i] == reg) return true;
    }
    return false;
}

// Distinct successors EmitFunctionBody branches to from `block`.
u32 SPIRVBuilder::GetEmittedSuccessorCount(u32 block) const {
    u32 count = 0;
    u32 total = cfg->TotalSuccessorCount(block);
    for (u32 s = 0; s < total; s++) {
        u32 successor = cfg->GetAnySuccessor(block, s);
        bool seen = false;
        for (u32 t = 0; t < s; t++) seen |= cfg->GetAnySuccessor(block, t) == successor;
        if (!seen && IsEdgeEmitted(block, successor)) count++;
    }
    return count;
}

// SPIRV-Cross turns every OpPhi into a variable assigned on each incoming
// edge. A value updated in a loop under a condition (`if (t < best) best =
// t;`) passes through a phi at each merge and at the loop header, so it gets
// copied in both arms of the if and again at the loop's continue. NVIDIA's GL
// compiler runs that markedly slower than the plain assignment (a ray
// tracer's closest-hit loops: 1.03 vs 0.91 ms at 4K). For output that
// SPIRV-Cross translates, the phis of one source variable (a web) share one
// Function variable instead, as glslang's output does: each read of a phi
// loads it, and only an edge that brings a value from outside the web stores
// to it. A web stays phis unless a forward walk shows that the variable
// holds the right member on every incoming edge and wherever a member is
// read, no two members share a block, and each store sits on an edge from a
// block with a single successor.
//
// With ENFORCE_BRANCH_HINTS, on every target, a web with a phi where an
// @branch if merges is kept in a variable too (see LowerIfWithRest): AMD's
// compiler can't turn a store on one side of the branch into a select, so it
// keeps the branch. It must be a one-element array: a scalar variable is
// promoted to a register before the compiler decides, an array only after.
void SPIRVBuilder::PlanPhiVariables() {
    phiVariableIdCount = 0;
    phiVariableCount = 0;
    phiEdgeBlockCount = 0;
    phiLoadCount = 0;
    if (!cfg || cfg->blockCount == 0 || ir->phiCount == 0 || !ir->phiOperandValues ||
        !ir->phiBlockIndices || !ir->phiVariables || !blockEmission)
        return;

    // Merge blocks of @branch ifs.
    bool* branchMerge = (bool*)arena->Allocate(cfg->blockCount * sizeof(bool), 64);
    if (!branchMerge) return;
    memset(branchMerge, 0, cfg->blockCount * sizeof(bool));
    bool anyBranchMerge = false;
    bool enforce = Optimizations::IsEnabled(optimizations, OptimizationId::ENFORCE_BRANCH_HINTS);
    for (u32 i = 0; enforce && ir->structureInfo && i < ir->instructionCount; i++) {
        u32 info = ir->structureInfo[i];
        u32 merge = info & IR::IRProgram::STRUCT_TARGET_MASK;
        if ((info & IR::IRProgram::STRUCT_TYPE_MASK) != IR::IRProgram::STRUCT_IF_HEADER ||
            !(info & IR::IRProgram::STRUCT_HINT_BRANCH) || merge >= ir->instructionCount)
            continue;
        u32 block = cfg->instToBlock[merge];
        if (block < cfg->blockCount) {
            branchMerge[block] = true;
            anyBranchMerge = true;
        }
    }
    if (!usePhiVariables && !anyBranchMerge) return;

    constexpr u32 NONE = 0xFFFFFFFF;
    constexpr u32 CONFLICT = 0xFFFFFFFE;
    u32 regCount = std::min<u32>(ir->registerCount, idCapacity);
    u32 phiCount = ir->phiCount;
    u32 blockCount = cfg->blockCount;
    u32 operandCount = ir->phiOperandOffsets[phiCount];
    struct PendingStore {
        u32 block;
        u32 variableId;
        u32 elementPointerTypeId;
        u16 value;
    };
    // Everything is per register, phi, block or phi operand; without the
    // memory, every phi stays an OpPhi.
    auto allocate = [&](size_t bytes) {
        void* memory = arena->Allocate(std::max<size_t>(bytes, 1), 64);
        if (memory) memset(memory, 0, std::max<size_t>(bytes, 1));
        return memory;
    };
    u32* variableIds = (u32*)allocate(phiCount * sizeof(u32));
    PhiVariable* variables = (PhiVariable*)allocate(phiCount * sizeof(PhiVariable));
    u32* loadVariables = (u32*)allocate(idCapacity * sizeof(u32));
    u32* loadTypeIds = (u32*)allocate(idCapacity * sizeof(u32));
    u32* loadElementPointerTypeIds = (u32*)allocate(idCapacity * sizeof(u32));
    u32* phiOfReg = (u32*)allocate(regCount * sizeof(u32));
    bool* candidate = (bool*)allocate(phiCount * sizeof(bool));
    bool* planned = (bool*)allocate(phiCount * sizeof(bool));
    u32* members = (u32*)allocate(phiCount * sizeof(u32));
    u32* memberAt = (u32*)allocate(blockCount * sizeof(u32));
    u32* held = (u32*)allocate(blockCount * sizeof(u32));
    PendingStore* pending = (PendingStore*)allocate(operandCount * sizeof(PendingStore));
    u32* storeOffsets = (u32*)allocate((blockCount + 1) * sizeof(u32));
    PhiEdgeStore* stores = (PhiEdgeStore*)allocate(operandCount * sizeof(PhiEdgeStore));
    if (!variableIds || !variables || !loadVariables || !loadTypeIds || !loadElementPointerTypeIds ||
        !phiOfReg || !candidate ||
        !planned || !members || !memberAt || !held || !pending || !storeOffsets || !stores)
        return;
    for (u32 reg = 0; reg < regCount; reg++) phiOfReg[reg] = NONE;

    // Pointers and memory-backed registers keep their own handling.
    auto isPlainRegister = [&](u16 reg) {
        return reg < regCount && localVarIds[reg] == 0 && localPointerTargets[reg].empty() &&
               (!ir->registerStorageInfo || ir->registerStorageInfo[reg] == 0);
    };
    auto isLive = [&](u32 phi, u32 op) {
        return IsEdgeEmitted(ir->GetPhiOperandBlock(phi, op), ir->phiBlockIndices[phi]);
    };

    for (u32 phi = 0; phi < phiCount; phi++) {
        u16 trivialValue;
        u32 block = ir->phiBlockIndices[phi];
        CoreType type = static_cast<CoreType>(ir->phiTypes[phi]);
        if (block >= blockCount || blockEmission[block] != BlockEmission::REACHABLE ||
            GetPhiShape(phi, &trivialValue) != PhiShape::MIXED ||
            type < CoreType::BOOL || type > CoreType::MAT4 ||
            !isPlainRegister(ir->phiResultRegs[phi]))
            continue;
        bool plainOperands = true;
        for (u32 op = 0; op < ir->GetPhiOperandCount(phi); op++) {
            u16 value = ir->GetPhiOperandValue(phi, op);
            if (isLive(phi, op) && !IR::IsConstant(value)) plainOperands &= isPlainRegister(value);
        }
        if (!plainOperands) continue;
        candidate[phi] = true;
        phiOfReg[ir->phiResultRegs[phi]] = phi;
    }

    u32 variableCount = 0;
    u32 pendingCount = 0;
    for (u32 first = 0; first < phiCount; first++) {
        if (!candidate[first] || planned[first]) continue;
        u16 web = ir->phiVariables[first];
        u32 memberCount = 0;
        for (u32 phi = first; phi < phiCount; phi++) {
            if (candidate[phi] && ir->phiVariables[phi] == web) {
                members[memberCount++] = phi;
                planned[phi] = true;
            }
        }
        // A lone phi gains nothing: SPIRV-Cross assigns it per edge either way.
        if (memberCount < 2) continue;
        bool atBranch = false;
        for (u32 m = 0; m < memberCount; m++) atBranch |= branchMerge[ir->phiBlockIndices[members[m]]];
        if (!usePhiVariables && !atBranch) continue;

        bool valid = true;
        for (u32 block = 0; block < blockCount; block++) memberAt[block] = NONE;
        for (u32 m = 0; m < memberCount; m++) {
            u32 block = ir->phiBlockIndices[members[m]];
            if (memberAt[block] != NONE) valid = false;
            memberAt[block] = members[m];
        }
        if (!valid) continue;

        // Which member the variable holds in each block. Only edges into a
        // member's block change it, so a block holds what it was entered
        // with.
        for (u32 block = 0; block < blockCount; block++) held[block] = NONE;
        for (bool changed = true; changed;) {
            changed = false;
            for (u32 block = 0; block < blockCount; block++) {
                if (blockEmission[block] != BlockEmission::REACHABLE) continue;
                u32 state = memberAt[block];
                if (state == NONE) {
                    for (u32 k = cfg->predecessorOffsets[block];
                         k < cfg->predecessorOffsets[block + 1]; k++) {
                        u32 pred = cfg->predecessorData[k];
                        if (!edgeEmitted[k] || held[pred] == NONE) continue;
                        if (state == NONE) state = held[pred];
                        else if (state != held[pred]) state = CONFLICT;
                    }
                }
                if (state != held[block]) {
                    held[block] = state;
                    changed = true;
                }
            }
        }

        // This web's stores go after the accepted webs' until it's accepted.
        u32 webStores = pendingCount;
        for (u32 m = 0; m < memberCount && valid; m++) {
            u32 phi = members[m];
            for (u32 op = 0; op < ir->GetPhiOperandCount(phi) && valid; op++) {
                if (!isLive(phi, op)) continue;
                u32 pred = ir->GetPhiOperandBlock(phi, op);
                u16 value = ir->GetPhiOperandValue(phi, op);
                bool member = !IR::IsConstant(value) && value < regCount &&
                              phiOfReg[value] != NONE && ir->phiVariables[phiOfReg[value]] == web;
                if (member) {
                    valid = held[pred] == phiOfReg[value];
                } else if (IR::IsConstant(value) || !IsUndefRegister(value)) {
                    valid = GetEmittedSuccessorCount(pred) == 1;
                    pending[pendingCount++] = {pred, 0, 0, value};
                }
            }
        }

        // Members are read where they're used (see GetSpirvId), so the
        // variable must hold the member in every block that reads it. A phi
        // outside the web can't take one as an incoming value: it would
        // read it among the phis of its own block. A switch selector or
        // wave lane would be read without GetSpirvId or after a merge.
        auto isMember = [&](u16 value) {
            return !IR::IsConstant(value) && value < regCount && phiOfReg[value] != NONE &&
                   ir->phiVariables[phiOfReg[value]] == web;
        };
        for (u32 i = 0; i < ir->instructionCount && valid; i++) {
            u32 block = cfg->instToBlock[i];
            if (block >= blockCount || blockEmission[block] != BlockEmission::REACHABLE)
                continue;
            u16 op = ir->opcodes[i];
            for (u32 k = 0; k < 4 && valid; k++) {
                u16 value = ir->GetOperand(i, k);
                if (!isMember(value)) continue;
                valid = held[block] == phiOfReg[value] && op != IR::OP_SWITCH &&
                        op != IR::OP_WAVE_READ_LANE;
            }
            // These read their destination: the stored value, the array.
            if (valid && (op == IR::OP_STORE_OUTPUT || op == IR::OP_ARRAY_STORE) &&
                isMember(ir->destinations[i])) {
                valid = held[block] == phiOfReg[ir->destinations[i]];
            }
        }
        for (u32 phi = 0; phi < phiCount && valid; phi++) {
            if (candidate[phi] && ir->phiVariables[phi] == web) continue;
            for (u32 op = 0; op < ir->GetPhiOperandCount(phi) && valid; op++) {
                valid = !isMember(ir->GetPhiOperandValue(phi, op));
            }
        }
        if (!valid) {
            pendingCount = webStores;
            continue;
        }

        u32 typeId = GetPhiTypeId(members[0]);
        u32 variableTypeId = typeId;
        u32 elementPointerTypeId = 0;
        if (atBranch) {
            variableTypeId = AllocateId();
            u32 arrayOps[] = {variableTypeId, typeId, GetIntConstantId(1, true)};
            EmitToSection(&typesConstants, spv::OpTypeArray, arrayOps, 3);
            elementPointerTypeId = GetPointerTypeId(typeId, spv::StorageClassFunction);
        }
        PhiVariable variable{AllocateId(), GetPointerTypeId(variableTypeId, spv::StorageClassFunction)};
        variables[variableCount++] = variable;
        for (u32 m = 0; m < memberCount; m++) {
            variableIds[members[m]] = variable.variableId;
            loadVariables[ir->phiResultRegs[members[m]]] = variable.variableId;
            loadTypeIds[ir->phiResultRegs[members[m]]] = typeId;
            loadElementPointerTypeIds[ir->phiResultRegs[members[m]]] = elementPointerTypeId;
        }
        for (u32 k = webStores; k < pendingCount; k++) {
            pending[k].variableId = variable.variableId;
            pending[k].elementPointerTypeId = elementPointerTypeId;
        }
    }

    // Each block's stores together, in the order they were planned.
    for (u32 k = 0; k < pendingCount; k++) storeOffsets[pending[k].block + 1]++;
    for (u32 block = 0; block < blockCount; block++) storeOffsets[block + 1] += storeOffsets[block];
    for (u32 block = 0; block < blockCount; block++) memberAt[block] = storeOffsets[block];
    for (u32 k = 0; k < pendingCount; k++) {
        stores[memberAt[pending[k].block]++] = {pending[k].variableId, pending[k].value,
                                                pending[k].elementPointerTypeId};
    }

    phiVariableIds = variableIds;
    phiVariableIdCount = phiCount;
    phiVariables = variables;
    phiVariableCount = variableCount;
    phiEdgeStoreOffsets = storeOffsets;
    phiEdgeStores = stores;
    phiEdgeBlockCount = blockCount;
    phiLoadVariables = loadVariables;
    phiLoadTypeIds = loadTypeIds;
    phiLoadElementPointerTypeIds = loadElementPointerTypeIds;
    phiLoadCount = idCapacity;
}

void SPIRVBuilder::EmitPhiEdgeStores(u32 blockIndex) {
    if (blockIndex >= phiEdgeBlockCount) return;
    for (u32 k = phiEdgeStoreOffsets[blockIndex]; k < phiEdgeStoreOffsets[blockIndex + 1]; k++) {
        const PhiEdgeStore& store = phiEdgeStores[k];
        u32 value = GetSpirvId(store.value);
        u32 pointer = store.variableId;
        if (store.elementPointerTypeId != 0) {
            pointer = AllocateId();
            Emit(spv::OpAccessChain, store.elementPointerTypeId, pointer, store.variableId,
                 GetIntConstantId(0));
        }
        Emit(spv::OpStore, pointer, value);
    }
}

void SPIRVBuilder::EmitLocalNames() {
  if (!emitDebugNames) return;

  // Wait until body emission has resolved copies and trivial PHIs. Never
  // allocate IDs for debug metadata: unused locals must stay unused, and the
  // default binary must not change. Several registers can alias a single ID.
  // IDs preallocated for PHIs in terminated blocks may never be defined.
  // Inspect emitted instructions so debug names cannot create dangling refs.
  std::vector<bool> defined(nextId, false);
  auto collectDefinitions = [&](const u32* words, u32 size) {
    for (u32 offset = 0; offset < size;) {
      u32 header = words[offset];
      u32 count = header >> 16;
      bool hasResult, hasResultType;
      spv::HasResultAndType(static_cast<spv::Op>(header & 0xFFFF),
                            &hasResult, &hasResultType);
      if (hasResult) {
        u32 id = words[offset + 1 + (hasResultType ? 1 : 0)];
        if (id < defined.size()) defined[id] = true;
      }
      offset += count;
    }
  };
  collectDefinitions(typesConstants.words, typesConstants.count);
  collectDefinitions(globals.words, globals.count);
  collectDefinitions(currentFunction, currentFunctionSize);

  std::vector<bool> named(nextId, false);
  for (u32 offset = 0; offset < debugNames.count;) {
    u32 header = debugNames.words[offset];
    u32 count = header >> 16;
    if ((header & 0xFFFF) == spv::OpName && count >= 3) {
      u32 id = debugNames.words[offset + 1];
      if (id < named.size()) named[id] = true;
    }
    offset += count;
  }
  auto nameId = [&](u32 id, u32 hash) {
    if (id == 0 || id >= named.size() || !defined[id] || named[id] || hash == 0) return;
    std::string name = ReverseLookup::GetString(hash);
    if (avoidGlslReservedNames && IsGlslReservedName(name.c_str())) name += '_';
    if (!name.empty()) {
      EmitName(id, name.c_str());
      named[id] = true;
    }
  };

  if (ir->registerNameHashes) {
    u32 count = std::min(ir->registerCount, IR::RegisterLimit);
    // Address-taken locals have a separate Function-storage variable. Name
    // those slots first so a pointer alias cannot rename the pointee's storage.
    for (u32 reg = 0; reg < count && reg < idCapacity; reg++) {
      nameId(localVarIds[reg], ir->registerNameHashes[reg]);
    }
    // An undef register stands for "not assigned on this path" and becomes an
    // OpUndef, which SPIRV-Cross declares at module scope. Naming it after the
    // variable would give every inlined copy a global of the same name.
    std::vector<bool> undef(count, false);
    for (u32 i = 0; ir->undefRegs && i < ir->undefRegCount; i++) {
      if (ir->undefRegs[i] < count) undef[ir->undefRegs[i]] = true;
    }
    for (u32 reg = 0; reg < count && reg < idCapacity; reg++) {
      if (!undef[reg]) nameId(spirvIds[reg], ir->registerNameHashes[reg]);
    }
  }
  if (ir->localArrayNameHashes) {
    for (u32 i = 0; i < ir->localArrayCount; i++) {
      nameId(localArrayVarIds[i], ir->localArrayNameHashes[i]);
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
  if (cfg && cfg->blockCount > 0) FindEmittedBlocks();
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
  EmitLocalNames();

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
                // differently)
                if (IR::IsConstant(valueReg))
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

  localPointerTargets.assign(idCapacity, {});
  localPointerTagVars.assign(idCapacity, 0);
  localPointerIndexVars.assign(idCapacity, 0);
  for (u32 i = 0; i < ir->instructionCount; ++i) {
    u16 reg = ir->destinations[i];
    IR::OpCode op = static_cast<IR::OpCode>(ir->opcodes[i]);
    u16 base = ir->GetOperand(i, 0);
    bool arrayAddress = op == IR::OP_STORAGE_INDEX && ir->registerStorageInfo &&
        base < ir->registerCount &&
        (ir->registerStorageInfo[base] & IR::IRProgram::STORAGE_IS_LOCAL_ARRAY);
    if (reg < idCapacity && (op == IR::OP_LOCAL_VAR_PTR ||
                            op == IR::OP_LOCAL_FIELD_PTR || arrayAddress))
      localPointerTargets[reg].push_back(i);
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (u32 i = 0; i < ir->instructionCount; ++i) {
      if (ir->opcodes[i] != IR::OP_STORE_REG && ir->opcodes[i] != IR::OP_LOAD_REG) continue;
      u16 dst = ir->destinations[i], src = ir->GetOperand(i, 0);
      if (dst >= idCapacity || src >= idCapacity) continue;
      for (u32 target : localPointerTargets[src]) {
        auto &targets = localPointerTargets[dst];
        if (std::find(targets.begin(), targets.end(), target) == targets.end()) {
          targets.push_back(target);
          changed = true;
        }
      }
    }
  }
  for (u32 r = 0; r < idCapacity; ++r) {
    bool hasArrayTarget = false;
    for (u32 producer : localPointerTargets[r])
      hasArrayTarget |= ir->opcodes[producer] == IR::OP_STORAGE_INDEX;
    if (localPointerTargets[r].size() > 1 || hasArrayTarget) {
      localPointerTagVars[r] = AllocateId();
      // An index is part of the pointer value. Copies must snapshot it, so
      // rebinding the original pointer in a later loop iteration cannot
      // retarget an earlier alias.
      if (hasArrayTarget) localPointerIndexVars[r] = AllocateId();
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
      if (isFieldPtr && ir->registerStorageInfo && var_reg < ir->registerCount &&
          (ir->registerStorageInfo[var_reg] & IR::IRProgram::STORAGE_IS_FIELD_PTR)) {
        continue; // Nested field access extends an existing pointer.
      }

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

  // After localVarIds and localPointerTargets: those registers stay out.
  PlanPhiVariables();

  // Helper lambda to emit all local pointer OpVariables
  auto emitLocalPointerVars = [&]() {
    for (u32 v = 0; v < phiVariableCount; v++) {
        Emit(spv::OpVariable, phiVariables[v].pointerTypeId, phiVariables[v].variableId,
             spv::StorageClassFunction);
    }
    for (u32 i = 0; i < ir->instructionCount; i++) {
      bool isVarPtr = (ir->opcodes[i] == IR::OP_LOCAL_VAR_PTR);
      bool isFieldPtr = (ir->opcodes[i] == IR::OP_LOCAL_FIELD_PTR);
      if (isVarPtr || isFieldPtr) {
        u16 var_reg = ir->GetOperand(i, 0);
      if (isFieldPtr && ir->registerStorageInfo && var_reg < ir->registerCount &&
          (ir->registerStorageInfo[var_reg] & IR::IRProgram::STORAGE_IS_FIELD_PTR)) {
        continue; // Nested field access extends an existing pointer.
      }
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

    u32 tagType = GetPointerTypeId(GetTypeId(CoreType::UINT), spv::StorageClassFunction);
    u32 zeroTag = GetIntConstantId(0, true);
    for (u32 variable : localPointerTagVars) {
      if (variable) Emit(spv::OpVariable, tagType, variable, spv::StorageClassFunction, zeroTag);
    }
    for (u32 variable : localPointerIndexVars) {
      if (variable) Emit(spv::OpVariable, tagType, variable, spv::StorageClassFunction, zeroTag);
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
        localArrayTypeIds[i] = arrayTypeId;
      }
    }

        // Pre-declare scratch variables for dynamically-indexed struct array
        // fields. OP_STRUCT_ARRAY_EXTRACT/INSERT with a runtime element index
        // spill the struct value into one of these Function-storage variables
        // and address the element with OpAccessChain. OpVariable must appear in
        // the entry block, so scan the IR up front and declare one scratch
        // variable per distinct struct type.
        if (ir) {
            for (u32 i = 0; i < ir->instructionCount; i++) {
                IR::OpCode scanOp = static_cast<IR::OpCode>(ir->opcodes[i]);
                if (scanOp != IR::OP_STRUCT_ARRAY_EXTRACT &&
                        scanOp != IR::OP_STRUCT_ARRAY_INSERT) {
                    continue;
                }
                u16 indexReg = ir->GetOperand(i, 2);
                if (IR::IsIntConstant(indexReg)) {
                    continue; // Constant index uses OpCompositeExtract/Insert directly
                }
                u32 structHash = ir->metadata[i];
                if (structHash == 0) {
                    continue;
                }
                u32 structTypeId = GetStructTypeId(structHash);
                if (structTypeId != 0 &&
                        GetStructArrayScratchVar(structTypeId) == 0 &&
                        structArrayScratchCount < MAX_STRUCT_ARRAY_SCRATCH) {
                    u32 ptrTypeId =
                            GetPointerTypeId(structTypeId, spv::StorageClassFunction);
                    u32 varId = AllocateId();
                    Emit(spv::OpVariable, ptrTypeId, varId, spv::StorageClassFunction);
                    structArrayScratchTypeIds[structArrayScratchCount] = structTypeId;
                    structArrayScratchVarIds[structArrayScratchCount] = varId;
                    structArrayScratchCount++;
                }
            }
        }
    };

  if (!cfg || cfg->blockCount == 0) {
    // Fallback for trivial shaders without CFG
    u32 entry_label = GetOrCreateBlockLabel(0);
    Emit(spv::OpLabel, entry_label);

    // Emit all local pointer OpVariables at the start of the function
    emitLocalPointerVars();

    for (u32 i = 0; i < ir->instructionCount;) {
      u32 count = TranslateArrayStoreSequence(i, ir->instructionCount);
      if (count) i += count;
      else TranslateInstruction(i++);
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

    // A block a constant branch skips is kept as a label (a construct may
    // still name it as its merge block) and OpUnreachable. Its values can
    // only reach other skipped blocks, and SPIRV-Cross fails to place one
    // that several of those share. An unreachable continue target keeps the
    // branch back to its loop header instead.
    if (blockIdx != cfg->entryBlock && blockEmission[blockIdx] != BlockEmission::REACHABLE) {
        DropBlockBody(firstInst, lastInst);
        if (blockEmission[blockIdx] == BlockEmission::BACK_EDGE) {
            u32 header = backEdgeTargets[blockIdx];
            Emit(spv::OpBranch, GetOrCreateBlockLabel(cfg->firstInst[header]));
        } else {
            Emit(spv::OpUnreachable);
        }
        continue;
    }

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
    for (u32 i = firstInst; i < endInst;) {
      u32 count = TranslateArrayStoreSequence(i, endInst);
      if (count) i += count;
      else TranslateInstruction(i++);
    }

    // Values this block's single outgoing edge brings into phi variables.
    EmitPhiEdgeStores(blockIdx);

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
