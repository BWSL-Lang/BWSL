// Part of the header-only IRLowering implementation. Include via bwsl_ir_lowering.h only.
//
// Loop unrolling (LOOP_UNROLLING). A for loop is lowered as a loop first.
// If the instructions it produced show a trip count known at compile time,
// small enough, and a body that leaves the loop only at its end, the loop is
// rolled back and lowered again as one copy of the body per iteration. Each
// copy is lowered from the AST, so it gets its own registers, exactly as if
// the shader had written the iterations out, and constant propagation then
// sees the iterator as a constant in every copy.
#pragma once
#include "bwsl_ir_lowering.h"

namespace BWSL::IR {
namespace Unroll {

// Instructions whose destination slot isn't a register they define.
inline bool Defines(u16 op) {
    switch (op) {
        case OP_NOP: case OP_JUMP: case OP_BRANCH: case OP_RET: case OP_SWITCH:
        case OP_DISCARD: case OP_BARRIER: case OP_MEM_FENCE:
        case OP_LOCAL_STORE: case OP_ARRAY_STORE: case OP_STORE_OUTPUT: case OP_STORE_BUFFER:
        case OP_STORE_SHARED: case OP_STORE_LOCAL: case OP_IMG_STORE: case OP_STRUCT_STORE:
            return false;
        default:
            return true;
    }
}

// Runs the instructions that decide a loop's trip count: integer and bool
// arithmetic on constants and on registers whose value is known.
struct Simulation {
    const IRProgram* program;
    u32 start;                             // First instruction of the loop's lowering
    std::unordered_map<u16, u32> values;   // Every register read or written, by its bits
    std::vector<bool> targets;             // [start] Instructions some jump lands on; filled when needed
};

inline bool ReadConstant(const IRProgram* program, u16 operand, u32* bits) {
    if (IsBoolConstant(operand)) {
        u32 index = BoolConstantIndex(operand);
        if (index >= program->boolCount) return false;
        *bits = program->boolConstants[index] != 0;
        return true;
    }
    return TryGetConstantInteger(program, operand, bits);
}

// Applies one instruction to the values `read` gives its operands.
template <typename Read>
inline bool Evaluate(const IRProgram* program, u32 inst, Read read, u32* result) {
    u16 op = program->opcodes[inst];
    u32 a = 0, b = 0;
    auto operand = [&](u32 slot, u32* bits) { return read(program->GetOperand(inst, slot), bits); };
    switch (op) {
        case OP_LOAD_REG: case OP_STORE_REG: case OP_I2U: case OP_U2I:
            return operand(0, result);
        case OP_INEG:
            if (!operand(0, &a)) return false;
            *result = 0u - a;
            return true;
        case OP_NOT: {
            if (!operand(0, &a)) return false;
            u16 dest = program->destinations[inst];
            bool isBool = dest < program->registerCount &&
                          static_cast<CoreType>(program->registerTypes[dest]) == CoreType::BOOL;
            *result = isBool ? u32(a == 0) : ~a;
            return true;
        }
        default:
            break;
    }
    if (!operand(0, &a) || !operand(1, &b)) return false;
    switch (op) {
        case OP_IADD: *result = a + b; return true;
        case OP_ISUB: *result = a - b; return true;
        case OP_IMUL: *result = a * b; return true;
        case OP_AND: *result = a & b; return true;
        case OP_OR: *result = a | b; return true;
        case OP_XOR: *result = a ^ b; return true;
        case OP_IEQ: *result = a == b; return true;
        case OP_INE: *result = a != b; return true;
        case OP_ILT: *result = s32(a) < s32(b); return true;
        case OP_ILE: *result = s32(a) <= s32(b); return true;
        case OP_IGT: *result = s32(a) > s32(b); return true;
        case OP_IGE: *result = s32(a) >= s32(b); return true;
        case OP_ULT: *result = a < b; return true;
        case OP_ULE: *result = a <= b; return true;
        case OP_UGT: *result = a > b; return true;
        case OP_UGE: *result = a >= b; return true;
        default: return false;
    }
}

// The value `reg` holds at instruction `before`, from its last assignment in
// the straight-line code just before it: no jump lands in between, so that
// assignment always runs on the way. Enough for `int n = 4; for (... i < n ...)`,
// a constant argument of an inlined function, and an inner loop's bound
// after the outer loop is unrolled.
inline bool KnownBefore(Simulation* sim, u16 reg, u32 before, u32 depth, u32* value) {
    const IRProgram* program = sim->program;
    if (depth > 64 || !IsRegister(reg) || reg >= program->registerCount) return false;
    if (sim->targets.empty()) {
        sim->targets.assign(sim->start + 1, false);
        auto mark = [&](u32 target) {
            if (target <= sim->start) sim->targets[target] = true;
        };
        for (u32 i = 0; i < sim->start; i++) {
            u16 op = program->opcodes[i];
            if (op == OP_JUMP) mark(program->metadata[i]);
            if (op == OP_BRANCH) {
                mark(program->GetBranchTrueTarget(i));
                mark(program->GetBranchFalseTarget(i));
            }
            if (program->structureInfo[i] != 0) {
                mark(program->structureInfo[i] & IRProgram::STRUCT_TARGET_MASK);
            }
            if (program->continueInfo[i] != NO_BLOCK) mark(program->continueInfo[i]);
        }
        for (u32 s = 0; s < program->switchCount; s++) {
            mark(program->switchDefaultTargets[s]);
            for (u32 c = program->switchCaseOffsets[s]; c < program->switchCaseOffsets[s + 1]; c++) {
                mark(program->switchCaseTargets[c]);
            }
        }
    }
    for (u32 i = before; i-- > 0;) {
        if (sim->targets[i + 1]) return false;
        u16 op = program->opcodes[i];
        if (IsTerminator(static_cast<OpCode>(op))) return false;
        if (!Defines(op) || program->destinations[i] != reg) continue;
        auto read = [&](u16 operand, u32* bits) {
            if (IsConstant(operand)) return ReadConstant(program, operand, bits);
            return KnownBefore(sim, operand, i, depth + 1, bits);
        };
        return Evaluate(program, i, read, value);
    }
    return false;
}

// Runs instructions [first, last). A register read before anything here
// wrote it holds what it held when the loop started.
inline bool Run(Simulation* sim, u32 first, u32 last) {
    const IRProgram* program = sim->program;
    auto read = [&](u16 operand, u32* bits) {
        if (IsConstant(operand)) return ReadConstant(program, operand, bits);
        auto known = sim->values.find(operand);
        if (known != sim->values.end()) {
            *bits = known->second;
            return true;
        }
        if (!KnownBefore(sim, operand, sim->start, 0, bits)) return false;
        sim->values[operand] = *bits;
        return true;
    };
    for (u32 i = first; i < last; i++) {
        u16 op = program->opcodes[i];
        if (op == OP_NOP) continue;
        u32 value;
        if (!Evaluate(program, i, read, &value)) return false;
        sim->values[program->destinations[i]] = value;
    }
    return true;
}

}  // namespace Unroll

inline IRLowering::UnrollCheckpoint IRLowering::BeginUnrollCandidate() const {
    UnrollCheckpoint checkpoint;
    checkpoint.active = Optimizations::IsEnabled(optimizations, OptimizationId::LOOP_UNROLLING) && !hadError;
    checkpoint.instruction = builder.currentInstruction;
    checkpoint.undefRegCount = program.undefRegCount;
    checkpoint.switchCount = program.switchCount;
    checkpoint.localArrayCount = program.localArrayCount;
    checkpoint.sharedVarCount = program.sharedVarCount;
    checkpoint.inlineReturnCounter = inlineReturnCounter;
    checkpoint.inlineReturnFlagReads = inlineReturnFlagReads;
    if (checkpoint.active) checkpoint.nodeRegisters = nodeRegisters;
    return checkpoint;
}

inline bool IRLowering::CountLoopIterations(const UnrollCheckpoint &checkpoint,
                                            const LoopShape &shape, u32 *tripCount,
                                            std::vector<u32> *iteratorValues) {
    if (!checkpoint.active || hadError || recursionDiagnosed) return false;
    // Lowering the body again must not add a second switch table, array
    // declaration or diagnostic, or emit a return the first lowering counted.
    if (program.switchCount != checkpoint.switchCount ||
        program.localArrayCount != checkpoint.localArrayCount ||
        program.sharedVarCount != checkpoint.sharedVarCount ||
        inlineReturnCounter != checkpoint.inlineReturnCounter ||
        builder.currentInstruction != program.instructionCount ||
        program.opcodes[shape.branch] != OP_BRANCH) {
        return false;
    }

    // The body may leave only through its end: a break or skip jumps past it.
    u32 bodySize = 0;
    auto inside = [&](u32 target) { return target >= shape.bodyStart && target < shape.continueTarget; };
    for (u32 i = shape.bodyStart; i < shape.backEdge; i++) {
        u16 op = program.opcodes[i];
        if (op != OP_NOP) bodySize++;
        if (i >= shape.continueTarget) continue;
        if (op == OP_JUMP && !inside(program.metadata[i])) return false;
        if (op == OP_BRANCH && (!inside(program.GetBranchTrueTarget(i)) ||
                                !inside(program.GetBranchFalseTarget(i)))) {
            return false;
        }
    }

    Unroll::Simulation sim{&program, checkpoint.instruction, {}, {}};
    if (!Unroll::Run(&sim, checkpoint.instruction, shape.header)) return false;
    u32 trips = 0;
    while (true) {
        if (!Unroll::Run(&sim, shape.header, shape.branch)) return false;
        u32 condition;
        u16 conditionReg = program.GetOperand(shape.branch, 0);
        if (IsConstant(conditionReg)) {
            if (!Unroll::ReadConstant(&program, conditionReg, &condition)) return false;
        } else {
            auto known = sim.values.find(conditionReg);
            if (known == sim.values.end()) return false;
            condition = known->second;
        }
        if (condition == 0) break;
        if (++trips > MAX_UNROLL_ITERATIONS) return false;
        if (iteratorValues) {
            auto value = sim.values.find(shape.iterator);
            if (value == sim.values.end()) return false;
            iteratorValues->push_back(value->second);
        }
        if (!Unroll::Run(&sim, shape.continueTarget, shape.backEdge)) return false;
    }
    if (trips * bodySize > MAX_UNROLLED_INSTRUCTIONS) return false;

    // The trip count holds only while the body leaves every register it was
    // computed from alone, and nothing reaches them through a pointer.
    for (const auto &entry : sim.values) {
        u16 reg = entry.first;
        if (reg < program.registerCount && program.registerStorageInfo &&
            program.registerStorageInfo[reg] != 0) {
            return false;
        }
    }
    for (u32 i = shape.bodyStart; i < shape.continueTarget; i++) {
        if (Unroll::Defines(program.opcodes[i]) && sim.values.count(program.destinations[i])) {
            return false;
        }
    }
    *tripCount = trips;
    return true;
}

inline void IRLowering::RollBackLoop(const UnrollCheckpoint &checkpoint) {
    // Registers the loop allocated stay allocated; nothing refers to them.
    builder.currentInstruction = checkpoint.instruction;
    program.instructionCount = checkpoint.instruction;
    program.undefRegCount = checkpoint.undefRegCount;
    inlineReturnFlagReads = checkpoint.inlineReturnFlagReads;
    nodeRegisters = checkpoint.nodeRegisters;
}

inline void IRLowering::UnrollForCStyle(NodeRef ref, const UnrollCheckpoint &checkpoint,
                                        const LoopShape &shape) {
    u32 trips;
    if (!CountLoopIterations(checkpoint, shape, &trips, nullptr)) return;
    RollBackLoop(checkpoint);
    // The condition isn't lowered again: it is pure, and true `trips` times.
    const ForCStyleData &forLoop = ast->GetForCStyle(ref);
    auto savedOuterVariableRegisters = variableRegisters;
    if (!forLoop.init.IsNull()) LowerStatement(forLoop.init);
    for (u32 t = 0; t < trips; t++) {
        nodeRegisters = checkpoint.nodeRegisters;
        auto savedVariableRegisters = variableRegisters;
        if (!forLoop.body.IsNull()) LowerStatement(forLoop.body);
        variableRegisters = std::move(savedVariableRegisters);
        if (!forLoop.increment.IsNull()) LowerStatement(forLoop.increment);
    }
    nodeRegisters = checkpoint.nodeRegisters;
    variableRegisters = std::move(savedOuterVariableRegisters);
}

inline void IRLowering::UnrollForRange(NodeRef ref, const UnrollCheckpoint &checkpoint,
                                       const LoopShape &shape, CoreType iterType) {
    u32 trips;
    std::vector<u32> values;
    if (!CountLoopIterations(checkpoint, shape, &trips, &values)) return;
    RollBackLoop(checkpoint);
    // The bounds and step are constants, so they aren't lowered again.
    const ForRangeData &forLoop = ast->GetForRange(ref);
    auto savedOuterVariableRegisters = variableRegisters;
    u16 iterReg = AllocateRegister();
    SetRegisterType(iterReg, iterType);
    if (!forLoop.iteratorVar.IsNull()) {
        const IdentifierData &iter = ast->GetIdentifier(forLoop.iteratorVar);
        variableRegisters[iter.name.nameHash] = iterReg;
        program.registerNameHashes[iterReg] = iter.name.nameHash;
    }
    for (u32 value : values) {
        u16 constant = iterType == CoreType::UINT ? EmitConstantUint(value) : EmitConstantInt(value);
        builder.EmitInstruction(OP_STORE_REG, iterReg, constant);
        nodeRegisters = checkpoint.nodeRegisters;
        if (!forLoop.body.IsNull()) LowerStatement(forLoop.body);
    }
    nodeRegisters = checkpoint.nodeRegisters;
    variableRegisters = std::move(savedOuterVariableRegisters);
}

}  // namespace BWSL::IR
