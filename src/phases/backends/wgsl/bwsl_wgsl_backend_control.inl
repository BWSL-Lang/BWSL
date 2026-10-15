// Included by bwsl_wgsl_backend.cpp inside BWSL::WGSL.

bool WGSLBuilder::PrepareControl() {
    const u32 blocks = cfg->blockCount;
    if (cfg->firstInst == nullptr || cfg->lastInst == nullptr || cfg->successors == nullptr ||
        cfg->successorCount == nullptr || ir->structureInfo == nullptr || ir->continueInfo == nullptr ||
        (ir->phiCount != 0 && (ir->phiBlockIndices == nullptr || ir->phiResultRegs == nullptr ||
         ir->phiTypes == nullptr || ir->phiOperandOffsets == nullptr || ir->phiOperandValues == nullptr ||
         ir->phiOperandBlocks == nullptr))) {
        return Fail("WGSL structured IR is missing required block or phi arrays");
    }
    control.merges = static_cast<u32*>(Allocate(blocks, sizeof(u32)));
    control.continues = static_cast<u32*>(Allocate(blocks, sizeof(u32)));
    control.kinds = static_cast<u32*>(Allocate(blocks, sizeof(u32)));
    control.phiOffsets = static_cast<u32*>(Allocate(blocks + 1, sizeof(u32)));
    control.phis = static_cast<u32*>(Allocate(ir->phiCount, sizeof(u32)));
    control.phiLive = static_cast<u8*>(Allocate(ir->phiCount, sizeof(u8)));
    control.visited = static_cast<u8*>(Allocate(blocks, sizeof(u8)));
    control.reachable = static_cast<u8*>(Allocate(blocks, sizeof(u8)));
    if (errorMessage[0] != '\0' || cfg->instToBlock == nullptr || cfg->entryBlock >= blocks) {
        return Fail("WGSL structured entry point has an invalid CFG");
    }
    std::memset(control.phiOffsets, 0, (blocks + 1) * sizeof(u32));
    std::memset(control.reachable, 0, blocks);
    u32* reachableWorklist = static_cast<u32*>(Allocate(blocks, sizeof(u32)));
    if (reachableWorklist == nullptr) {
        return false;
    }
    u32 reachableCount = 1;
    reachableWorklist[0] = cfg->entryBlock;
    control.reachable[cfg->entryBlock] = 1;
    for (u32 next = 0; next < reachableCount; ++next) {
        const u32 block = reachableWorklist[next];
        for (u32 edge = 0; edge < cfg->TotalSuccessorCount(block); ++edge) {
            const u32 target = cfg->GetAnySuccessor(block, edge);
            if (target >= blocks) {
                return Fail("WGSL CFG successor is out of bounds");
            }
            if (!control.reachable[target]) {
                control.reachable[target] = 1;
                reachableWorklist[reachableCount++] = target;
            }
        }
    }
    for (u32 block = 0; block < blocks; ++block) {
        control.merges[block] = InvalidId;
        control.continues[block] = InvalidId;
        control.kinds[block] = 0;
        if (cfg->firstInst[block] > cfg->lastInst[block] || cfg->lastInst[block] >= ir->instructionCount) {
            return Fail("WGSL CFG block instruction range is invalid");
        }
        for (u32 instruction = cfg->firstInst[block]; instruction <= cfg->lastInst[block]; ++instruction) {
            const u32 info = ir->structureInfo[instruction];
            if (info == 0) {
                continue;
            }
            const u32 merge = info & IR::IRProgram::STRUCT_TARGET_MASK;
            if (merge >= ir->instructionCount) {
                return Fail("WGSL structured merge target is out of bounds");
            }
            control.kinds[block] = info & IR::IRProgram::STRUCT_TYPE_MASK;
            control.merges[block] = cfg->instToBlock[merge];
            if (control.kinds[block] == IR::IRProgram::STRUCT_LOOP_HEADER) {
                const u32 continuation = ir->continueInfo[instruction];
                if (continuation >= ir->instructionCount) {
                    return Fail("WGSL loop is missing a valid continuing block");
                }
                control.continues[block] = cfg->instToBlock[continuation];
            }
        }
    }
    // SSA placement can include dead loop phis with undefined incoming values
    // for body-local variables. Retain only phis reached from executable uses.
    u32* registerPhi = static_cast<u32*>(Allocate(ir->registerCount, sizeof(u32)));
    u32* pending = static_cast<u32*>(Allocate(ir->phiCount, sizeof(u32)));
    if (errorMessage[0] != '\0') {
        return false;
    }
    std::memset(registerPhi, 0xFF, ir->registerCount * sizeof(u32));
    if (ir->phiCount != 0) {
        std::memset(control.phiLive, 0, ir->phiCount);
    }
    for (u32 phi = 0; phi < ir->phiCount; ++phi) {
        if (ir->phiResultRegs[phi] >= ir->registerCount) {
            return Fail("WGSL phi result register is out of bounds");
        }
        registerPhi[ir->phiResultRegs[phi]] = phi;
    }
    u32 pendingCount = 0;
    const auto mark = [&](u16 operand) {
        if (IR::IsConstant(operand) || operand >= ir->registerCount) {
            return;
        }
        const u32 phi = registerPhi[operand];
        if (phi != InvalidId && !control.phiLive[phi]) {
            control.phiLive[phi] = 1;
            pending[pendingCount++] = phi;
        }
    };
    for (u32 instruction = 0; instruction < ir->instructionCount; ++instruction) {
        if (!control.reachable[cfg->instToBlock[instruction]]) {
            continue;
        }
        const u16 opcode = ir->opcodes[instruction];
        u32 mask = ValueOperandMask(opcode, ir->metadata[instruction]);
        if (IsCopy(opcode) || opcode == IR::OP_STORAGE_LOAD || opcode == IR::OP_STORAGE_FIELD ||
            opcode == IR::OP_STRUCT_EXTRACT || opcode == IR::OP_BRANCH || opcode == IR::OP_SWITCH) {
            mask = 1;
        } else if (opcode == IR::OP_STORAGE_INDEX) {
            mask = 3;
        } else if (opcode == IR::OP_STRUCT_ARRAY_EXTRACT) {
            mask = 5;
        }
        for (u32 operand = 0; operand < 4; ++operand) {
            if (mask & (1u << operand)) {
                mark(ir->GetOperand(instruction, operand));
            }
        }
        if (opcode == IR::OP_STORE_OUTPUT || opcode == IR::OP_ARRAY_STORE) {
            mark(ir->destinations[instruction]);
        }
    }
    for (u32 next = 0; next < pendingCount; ++next) {
        const u32 phi = pending[next];
        for (u32 operand = 0; operand < ir->GetPhiOperandCount(phi); ++operand) {
            if (control.reachable[ir->GetPhiOperandBlock(phi, operand)]) {
                mark(ir->GetPhiOperandValue(phi, operand));
            }
        }
    }
    for (u32 phi = 0; phi < ir->phiCount; ++phi) {
        if (!control.phiLive[phi]) {
            continue;
        }
        const u32 block = ir->phiBlockIndices[phi];
        const u16 reg = ir->phiResultRegs[phi];
        const CoreType type = static_cast<CoreType>(ir->phiTypes[phi]);
        if (block >= blocks || reg >= ir->registerCount || reg >= IR::RegisterLimit ||
            registerToValue[reg] != InvalidId || Width(type) == 0) {
            return Fail("WGSL phi has an invalid block, register or type");
        }
        ++control.phiOffsets[block + 1];
        const u32 value = values.count++;
        registerToValue[reg] = value;
        values.types[value] = static_cast<u16>(type);
        values.producerInstructions[value] = InvalidId;
        values.kinds[value] = static_cast<u8>(ValueKind::Value);
        values.resourceIds[value] = InvalidId;
        values.indexOperands[value] = 0xFFFF;
        values.aliasOperands[value] = reg;
        valueOrigins.registers[value] = reg;
    }
    for (u32 block = 1; block <= blocks; ++block) {
        control.phiOffsets[block] += control.phiOffsets[block - 1];
    }
    u32* cursor = static_cast<u32*>(Allocate(blocks, sizeof(u32)));
    if (cursor == nullptr) {
        return false;
    }
    std::memcpy(cursor, control.phiOffsets, blocks * sizeof(u32));
    for (u32 phi = 0; phi < ir->phiCount; ++phi) {
        if (control.phiLive[phi]) {
            control.phis[cursor[ir->phiBlockIndices[phi]]++] = phi;
        }
    }
    return true;
}

bool WGSLBuilder::ValidateOutputAssignments() {
    const u32 blocks = cfg->blockCount;
    u32* assignedIn = static_cast<u32*>(Allocate(blocks, sizeof(u32)));
    u32* assignedOut = static_cast<u32*>(Allocate(blocks, sizeof(u32)));
    u32* worklist = static_cast<u32*>(Allocate(blocks, sizeof(u32)));
    if (errorMessage[0] != '\0') {
        return false;
    }
    std::memset(control.visited, 0, blocks);
    u32 count = 1;
    worklist[0] = cfg->entryBlock;
    control.visited[cfg->entryBlock] = 1;
    for (u32 next = 0; next < count; ++next) {
        const u32 block = worklist[next];
        for (u32 edge = 0; edge < cfg->TotalSuccessorCount(block); ++edge) {
            const u32 target = cfg->GetAnySuccessor(block, edge);
            if (target >= blocks) {
                return Fail("WGSL CFG edge target is out of bounds");
            }
            if (!control.visited[target]) {
                control.visited[target] = 1;
                worklist[count++] = target;
            }
        }
    }
    for (u32 block = 0; block < blocks; ++block) {
        assignedIn[block] = assignedOut[block] = 0xFFFFFFFFu;
    }
    bool changed;
    do {
        changed = false;
        for (u32 next = 0; next < count; ++next) {
            const u32 block = worklist[next];
            u32 incoming = block == cfg->entryBlock ? 0 : 0xFFFFFFFFu;
            for (u32 predecessor = 0; predecessor < cfg->PredecessorCount(block); ++predecessor) {
                const u32 sourceBlock = cfg->GetPredecessor(block, predecessor);
                if (control.visited[sourceBlock]) {
                    incoming &= assignedOut[sourceBlock];
                }
            }
            u32 outgoing = incoming;
            for (u32 instruction = cfg->firstInst[block]; instruction <= cfg->lastInst[block]; ++instruction) {
                if (ir->opcodes[instruction] == IR::OP_STORE_OUTPUT) {
                    outgoing |= 1u << ir->GetOperand(instruction, 0);
                }
            }
            changed |= incoming != assignedIn[block] || outgoing != assignedOut[block];
            assignedIn[block] = incoming;
            assignedOut[block] = outgoing;
        }
    } while (changed);
    for (u32 next = 0; next < count; ++next) {
        const u32 block = worklist[next];
        u32 assigned = assignedIn[block];
        for (u32 instruction = cfg->firstInst[block]; instruction <= cfg->lastInst[block]; ++instruction) {
            currentInstruction = instruction;
            const u16 opcode = ir->opcodes[instruction];
            const u16 slot = ir->GetOperand(instruction, 0);
            if (opcode == IR::OP_STORE_OUTPUT) {
                assigned |= 1u << slot;
            } else if (opcode == IR::OP_LOAD_OUTPUT && !(assigned & (1u << slot))) {
                return Fail("WGSL output is read before assignment on every incoming path");
            } else if (opcode == IR::OP_RET && stage == ShaderStage::Vertex &&
                       !(assigned & (1u << OutputSlot::POSITION))) {
                return Fail("WGSL vertex position must be assigned on every return path");
            }
        }
    }
    currentInstruction = InvalidId;
    for (u32 phi = 0; phi < ir->phiCount; ++phi) {
        if (!control.phiLive[phi]) {
            continue;
        }
        for (u32 operand = 0; operand < ir->GetPhiOperandCount(phi); ++operand) {
            if (!control.reachable[ir->GetPhiOperandBlock(phi, operand)]) {
                continue;
            }
            CoreType type = CoreType::INVALID;
            if (!OperandType(ir->GetPhiOperandValue(phi, operand), type) ||
                type != static_cast<CoreType>(ir->phiTypes[phi])) {
                return Fail("WGSL phi operand type disagrees with its result");
            }
        }
    }
    currentInstruction = InvalidId;
    return true;
}

bool WGSLBuilder::WritePhiCopies(TextWriter& writer, u32 from, u32 to) {
    const u32 first = control.phiOffsets[to];
    const u32 end = control.phiOffsets[to + 1];
    if (first == end) {
        return true;
    }
    writer.Indent();
    writer.Text("{\n");
    ++writer.indentation;
    // Snapshot every source before assigning any destination: phi swaps and
    // cycles on loop backedges must behave like parallel copies.
    for (u32 index = first; index < end; ++index) {
        const u32 phi = control.phis[index];
        bool found = false;
        for (u32 operand = 0; operand < ir->GetPhiOperandCount(phi); ++operand) {
            if (ir->GetPhiOperandBlock(phi, operand) != from) {
                continue;
            }
            writer.Indent();
            writer.Text("let bwsl_edge_");
            writer.Number(phi);
            writer.Text(" = ");
            if (!WriteOperand(writer, ir->GetPhiOperandValue(phi, operand))) {
                return false;
            }
            writer.Text(";\n");
            found = true;
            break;
        }
        if (!found) {
            return Fail("WGSL phi is missing an incoming CFG edge");
        }
    }
    for (u32 index = first; index < end; ++index) {
        const u32 phi = control.phis[index];
        writer.Indent();
        WriteValueName(writer, registerToValue[ir->phiResultRegs[phi]]);
        writer.Text(" = bwsl_edge_");
        writer.Number(phi);
        writer.Text(";\n");
    }
    --writer.indentation;
    writer.Indent();
    writer.Text("}\n");
    return true;
}

bool WGSLBuilder::WriteCondition(TextWriter& writer, u16 operand) {
    CoreType type = CoreType::INVALID;
    if (!OperandType(operand, type) || Width(type) != 1) {
        return Fail("WGSL branch condition must be a scalar");
    }
    if (type == CoreType::BOOL) {
        return WriteOperand(writer, operand);
    }
    writer.Text("(");
    if (!WriteOperand(writer, operand)) {
        return false;
    }
    writer.Text(type == CoreType::FLOAT ? " != 0.0f)" : type == CoreType::UINT ? " != 0u)" : " != 0i)");
    return true;
}

bool WGSLBuilder::WriteTransfer(TextWriter& writer, u32 from, u32 target, u32 stop,
                                 ControlScope scope, u32 depth) {
    if (target >= cfg->blockCount || !WritePhiCopies(writer, from, target)) {
        return Fail("WGSL branch target or phi transfer is invalid");
    }
    if (target == stop) {
        return true;
    }
    if (target == scope.loopMerge) {
        if (scope.continuing) {
            return Fail("WGSL continuing exits require a final conditional break");
        }
        writer.Indent();
        writer.Text("break;\n");
        return true;
    }
    if (target == scope.loopContinue || target == scope.loopHeader) {
        if (scope.continuing) {
            return Fail("WGSL continuing cannot transfer back into itself");
        }
        writer.Indent();
        writer.Text("continue;\n");
        return true;
    }
    return WriteRegion(writer, target, stop, scope, depth + 1);
}

bool WGSLBuilder::WriteRegion(TextWriter& writer, u32 block, u32 stop,
                               ControlScope scope, u32 depth) {
    if (depth > 128) {
        return Fail("WGSL structured control nesting exceeds 128 levels");
    }
    while (block != stop) {
        if (block < cfg->blockCount && !control.reachable[block]) {
            return true;
        }
        if (block >= cfg->blockCount || control.visited[block]) {
            return Fail("WGSL CFG cannot be represented by its structured regions");
        }
        control.visited[block] = 1;
        const u32 kind = control.kinds[block];
        const u32 merge = control.merges[block];
        const bool loop = kind == IR::IRProgram::STRUCT_LOOP_HEADER;
        const ControlScope outer = scope;
        if (loop) {
            writer.Indent();
            writer.Text("loop {\n");
            ++writer.indentation;
            scope = {block, control.continues[block], merge, InvalidId, false};
        }
        const u32 last = cfg->lastInst[block];
        for (u32 instruction = cfg->firstInst[block]; instruction <= last; ++instruction) {
            currentInstruction = instruction;
            if (IR::IsTerminator(static_cast<IR::OpCode>(ir->opcodes[instruction]))) {
                break;
            }
            if (!WriteInstruction(writer, instruction)) {
                return false;
            }
        }
        currentInstruction = last;
        const u16 opcode = ir->opcodes[last];
        const u32 regionStop = loop ? scope.loopContinue : stop;
        if (opcode == IR::OP_RET || opcode == IR::OP_DISCARD) {
            if (scope.continuing) {
                return Fail("WGSL continuing blocks cannot return or discard");
            }
            return WriteInstruction(writer, last);
        }
        if (opcode == IR::OP_BRANCH) {
            const u32 yes = cfg->instToBlock[ir->GetBranchTrueTarget(last)];
            const u32 no = cfg->instToBlock[ir->GetBranchFalseTarget(last)];
            if (scope.continuing && ((yes == scope.loopMerge && no == scope.loopHeader) ||
                                     (no == scope.loopMerge && yes == scope.loopHeader))) {
                writer.Indent();
                writer.Text("let bwsl_break_");
                writer.Number(block);
                writer.Text(" = ");
                if (!WriteCondition(writer, ir->GetOperand(last, 0))) {
                    return false;
                }
                writer.Text(";\n");
                writer.Indent();
                writer.Text("if (bwsl_break_");
                writer.Number(block);
                writer.Text(") {\n");
                ++writer.indentation;
                if (!WritePhiCopies(writer, block, yes)) {
                    return false;
                }
                --writer.indentation;
                writer.Indent();
                writer.Text("} else {\n");
                ++writer.indentation;
                if (!WritePhiCopies(writer, block, no)) {
                    return false;
                }
                --writer.indentation;
                writer.Indent();
                writer.Text("}\n");
                writer.Indent();
                writer.Text(yes == scope.loopMerge ? "break if bwsl_break_" : "break if !bwsl_break_");
                writer.Number(block);
                writer.Text(";\n");
                return true;
            }
            if (yes == no) {
                if (!WriteTransfer(writer, block, yes, regionStop, scope, depth)) {
                    return false;
                }
            } else {
                const u32 selectionStop = kind == IR::IRProgram::STRUCT_IF_HEADER ? merge : regionStop;
                writer.Indent();
                writer.Text("if (");
                if (!WriteCondition(writer, ir->GetOperand(last, 0))) {
                    return false;
                }
                writer.Text(") {\n");
                ++writer.indentation;
                if (!WriteTransfer(writer, block, yes, selectionStop, scope, depth)) {
                    return false;
                }
                --writer.indentation;
                writer.Indent();
                writer.Text("} else {\n");
                ++writer.indentation;
                if (!WriteTransfer(writer, block, no, selectionStop, scope, depth)) {
                    return false;
                }
                --writer.indentation;
                writer.Indent();
                writer.Text("}\n");
                if (!loop && kind == IR::IRProgram::STRUCT_IF_HEADER) {
                    block = merge;
                    continue;
                }
            }
        } else if (opcode == IR::OP_SWITCH) {
            u32 switchId = 0;
            while (switchId < ir->switchCount && ir->switchInstructionIndices[switchId] != last) {
                ++switchId;
            }
            if (switchId == ir->switchCount || merge == InvalidId) {
                return Fail("WGSL switch is missing structured case metadata");
            }
            const u32 begin = ir->switchCaseOffsets[switchId];
            const u32 end = ir->switchCaseOffsets[switchId + 1];
            for (u32 item = begin; item <= end; ++item) {
                if (item < end) {
                    bool duplicate = false;
                    for (u32 earlier = begin; earlier < item; ++earlier) {
                        duplicate |= ir->switchCaseTargets[earlier] == ir->switchCaseTargets[item];
                    }
                    if (duplicate) {
                        continue;
                    }
                }
                writer.Indent();
                if (item == begin) {
                    writer.Text(item == end ? "{\n" : "if (");
                } else {
                    writer.Text(item == end ? "else {\n" : "else if (");
                }
                if (item < end) {
                    bool first = true;
                    CoreType selectorType = CoreType::INVALID;
                    if (!OperandType(ir->GetOperand(last, 0), selectorType)) {
                        return false;
                    }
                    for (u32 value = item; value < end; ++value) {
                        if (ir->switchCaseTargets[value] != ir->switchCaseTargets[item]) {
                            continue;
                        }
                        writer.Text(first ? "" : " || ");
                        if (!WriteOperand(writer, ir->GetOperand(last, 0))) {
                            return false;
                        }
                        writer.Text(" == ");
                        const s32 literal = ir->switchCaseValues[value];
                        if (selectorType == CoreType::INT && literal < 0) {
                            writer.Text("-");
                            writer.Number(0u - static_cast<u32>(literal));
                        } else {
                            writer.Number(static_cast<u32>(literal));
                        }
                        writer.Text(selectorType == CoreType::UINT ? "u" : "i");
                        first = false;
                    }
                    writer.Text(") {\n");
                }
                ++writer.indentation;
                const u32 target = cfg->instToBlock[item == end ? ir->switchDefaultTargets[switchId] : ir->switchCaseTargets[item]];
                ControlScope caseScope = scope;
                caseScope.switchMerge = merge;
                if (!WriteTransfer(writer, block, target, merge, caseScope, depth)) {
                    return false;
                }
                --writer.indentation;
                writer.Indent();
                writer.Text("}\n");
            }
            block = merge;
            continue;
        } else {
            const u32 target = opcode == IR::OP_JUMP ? cfg->instToBlock[ir->metadata[last]] :
                               cfg->TotalSuccessorCount(block) == 1 ? cfg->GetAnySuccessor(block, 0) : InvalidId;
            if (target == InvalidId) {
                return Fail("WGSL structured block has no terminator");
            }
            if (!loop && target != stop && target != scope.loopMerge &&
                target != scope.loopContinue && target != scope.loopHeader) {
                if (!WritePhiCopies(writer, block, target)) {
                    return false;
                }
                block = target;
                continue;
            }
            if (!WriteTransfer(writer, block, target, regionStop, scope, depth)) {
                return false;
            }
        }
        if (!loop) {
            return true;
        }
        if (scope.loopContinue != scope.loopHeader) {
            writer.Indent();
            writer.Text("continuing {\n");
            ++writer.indentation;
            scope.continuing = true;
            if (!WriteRegion(writer, scope.loopContinue, scope.loopHeader, scope, depth + 1)) {
                return false;
            }
            --writer.indentation;
            writer.Indent();
            writer.Text("}\n");
        }
        --writer.indentation;
        writer.Indent();
        writer.Text("}\n");
        scope = outer;
        block = merge;
    }
    return true;
}
