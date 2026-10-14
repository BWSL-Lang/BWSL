#include "bwsl_constant_propagation.h"
#include "core/bwsl_arena.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace BWSL {
namespace Optimization {

using IR::IRProgram;

// ============= Lattice =============

enum class ValueState : u8 {
    UNDEFINED,    // Not evaluated yet, or defined only by code that never runs
    CONSTANT,
    OVERDEFINED,  // Varies at run time, or isn't a value this pass folds
};

struct ConstantValue {
    ValueState state;
    CoreType type;
    u32 bits;  // Float bits, two's complement integer, or 0/1 for bool
};

constexpr ConstantValue UNDEFINED_VALUE = {ValueState::UNDEFINED, CoreType::INVALID, 0};
constexpr ConstantValue OVERDEFINED_VALUE = {ValueState::OVERDEFINED, CoreType::INVALID, 0};

// Marks a definitions[] or use-list entry that names a phi, not an instruction.
constexpr u32 PHI_REFERENCE = 0x80000000;
constexpr u32 NO_DEFINITION = 0xFFFFFFFF;

struct ConstantPropagationData {
    IRProgram* ir;
    IR::IRBuilder* builder;
    CFG* cfg;
    u32 regCount;

    ValueState* states;     // [regCount]
    u32* bits;              // [regCount]
    bool* tracked;          // [regCount] single-definition scalar this pass may fold
    bool* undef;            // [regCount] SSA undef register ("not assigned on this path")
    u32* definitions;       // [regCount] instruction, PHI_REFERENCE | phi, or NO_DEFINITION
    u8* definitionCounts;   // [regCount] saturates at 2

    u32* useOffsets;        // [regCount + 1] CSR: instructions and phis reading a register
    u32* useData;

    bool* blockExecutable;  // [blockCount]
    bool* edgeExecutable;   // [predecessorDataCount], parallel to cfg->predecessorData
    u32* blockPhiOffsets;   // [blockCount + 1] CSR: phis by block
    u32* blockPhis;
    bool* phiRemoved;       // [phiCount] phis left with no incoming edge

    u32* blockWork;         // A block is queued once, when it first becomes executable
    u32 blockWorkHead;
    u32 blockWorkTail;
    u32* regWork;           // A register is queued each time it drops a level: at most twice
    u32 regWorkHead;
    u32 regWorkTail;
};

static ConstantValue MakeConstant(CoreType type, u32 bits) {
    return {ValueState::CONSTANT, type, bits};
}

static ConstantValue Meet(ConstantValue a, ConstantValue b) {
    if (a.state == ValueState::UNDEFINED) return b;
    if (b.state == ValueState::UNDEFINED) return a;
    if (a.state == ValueState::OVERDEFINED || b.state == ValueState::OVERDEFINED) {
        return OVERDEFINED_VALUE;
    }
    return a.type == b.type && a.bits == b.bits ? a : OVERDEFINED_VALUE;
}

static bool IsFoldedType(CoreType type) {
    return type == CoreType::BOOL || type == CoreType::INT || type == CoreType::UINT ||
           type == CoreType::FLOAT;
}

static bool IsIntegerType(CoreType type) {
    return type == CoreType::INT || type == CoreType::UINT;
}

static CoreType GetRegisterType(const IRProgram* ir, u16 reg) {
    return ir->registerTypes ? static_cast<CoreType>(ir->registerTypes[reg]) : CoreType::INVALID;
}

static void* AllocateZeroed(BWSL_Arena* arena, size_t bytes) {
    void* memory = arena->Allocate(std::max<size_t>(bytes, 1), 64);
    if (memory) memset(memory, 0, std::max<size_t>(bytes, 1));
    return memory;
}

static bool IsRegisterOperand(const ConstantPropagationData* data, u16 operand) {
    return operand != 0xFFFF && !IR::IsConstant(operand) && operand < data->regCount;
}

// ============= Floats =============

static float BitsToFloat(u32 bits) {
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static u32 FloatToBits(float value) {
    u32 bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// Folding stays away from values a device may compute differently: NaN and
// infinities, denormals (which GPUs may flush to zero) and -0.0 (which the
// constant pool merges with +0.0).
static bool IsFoldableFloat(float value) {
    return std::isfinite(value) && std::fpclassify(value) != FP_SUBNORMAL &&
           !(value == 0.0f && std::signbit(value));
}

static bool MakeFloatResult(float value, ConstantValue* out) {
    if (!IsFoldableFloat(value)) return false;
    *out = MakeConstant(CoreType::FLOAT, FloatToBits(value));
    return true;
}

static ConstantValue DecodeConstant(const IRProgram* ir, u16 operand) {
    if (IR::IsBoolConstant(operand)) {
        u32 index = operand & 0x3FFF;
        if (index < ir->boolCount) {
            return MakeConstant(CoreType::BOOL, ir->boolConstants[index] ? 1 : 0);
        }
    } else if (IR::IsFloatConstant(operand)) {
        u32 index = operand & 0x3FFF;
        if (index < ir->floatCount && IsFoldableFloat(ir->floatConstants[index])) {
            return MakeConstant(CoreType::FLOAT, FloatToBits(ir->floatConstants[index]));
        }
    } else if (IR::IsIntConstant(operand)) {
        u32 index = operand & 0x1FFF;
        if (index < ir->intCount) return MakeConstant(CoreType::INT, ir->intConstants[index]);
    } else if (IR::IsUintConstant(operand)) {
        u32 index = operand & 0x1FFF;
        if (index < ir->uintCount) return MakeConstant(CoreType::UINT, ir->uintConstants[index]);
    }
    return OVERDEFINED_VALUE;
}

static ConstantValue GetValue(const ConstantPropagationData* data, u16 operand) {
    if (operand == 0xFFFF) return OVERDEFINED_VALUE;
    if (IR::IsConstant(operand)) return DecodeConstant(data->ir, operand);
    if (operand >= data->regCount || !data->tracked[operand]) return OVERDEFINED_VALUE;
    return {data->states[operand], GetRegisterType(data->ir, operand), data->bits[operand]};
}

// ============= Folding =============

// Operand count of the scalar operations FoldOperation evaluates; 0 for
// everything else.
static u32 GetFoldArity(u16 op) {
    switch (op) {
        case IR::OP_LOAD_REG: case IR::OP_STORE_REG:
        case IR::OP_FNEG: case IR::OP_FABS: case IR::OP_FLOOR: case IR::OP_CEIL:
        case IR::OP_TRUNC: case IR::OP_INEG: case IR::OP_IABS: case IR::OP_NOT:
        case IR::OP_F2I: case IR::OP_I2F: case IR::OP_F2U: case IR::OP_U2F:
        case IR::OP_I2U: case IR::OP_U2I:
            return 1;
        case IR::OP_FADD: case IR::OP_FSUB: case IR::OP_FMUL: case IR::OP_FDIV:
        case IR::OP_FMIN: case IR::OP_FMAX:
        case IR::OP_IADD: case IR::OP_ISUB: case IR::OP_IMUL: case IR::OP_IDIV:
        case IR::OP_IMOD: case IR::OP_IMIN: case IR::OP_IMAX: case IR::OP_UMIN:
        case IR::OP_UMAX:
        case IR::OP_AND: case IR::OP_OR: case IR::OP_XOR:
        case IR::OP_SHL: case IR::OP_SHR: case IR::OP_ASR:
        case IR::OP_FEQ: case IR::OP_FNE: case IR::OP_FLT: case IR::OP_FLE:
        case IR::OP_FGT: case IR::OP_FGE:
        case IR::OP_IEQ: case IR::OP_INE: case IR::OP_ILT: case IR::OP_ILE:
        case IR::OP_IGT: case IR::OP_IGE:
        case IR::OP_ULT: case IR::OP_ULE: case IR::OP_UGT: case IR::OP_UGE:
            return 2;
        case IR::OP_FCLAMP: case IR::OP_ICLAMP: case IR::OP_UCLAMP:
            return 3;
        default:
            return 0;
    }
}

// Evaluates `op` on constant operands. Returns false when the result isn't
// one every target computes the same way (division by zero, overflowing
// conversions, negative integer division, ...), or the operand types don't
// match the operation.
static bool FoldOperation(u16 op, const ConstantValue* in, ConstantValue* out) {
    CoreType type = in[0].type;
    bool sameTypes2 = in[1].type == type;
    bool sameTypes3 = sameTypes2 && in[2].type == type;
    float fa = BitsToFloat(in[0].bits), fb = BitsToFloat(in[1].bits), fc = BitsToFloat(in[2].bits);
    s32 sa = static_cast<s32>(in[0].bits), sb = static_cast<s32>(in[1].bits);
    s32 sc = static_cast<s32>(in[2].bits);
    u32 ua = in[0].bits, ub = in[1].bits, uc = in[2].bits;
    bool isFloat2 = type == CoreType::FLOAT && sameTypes2;
    bool isInt2 = type == CoreType::INT && sameTypes2;
    bool isUint2 = type == CoreType::UINT && sameTypes2;

    switch (op) {
        case IR::OP_LOAD_REG:
        case IR::OP_STORE_REG:
            *out = in[0];
            return true;

        // Float arithmetic. Division may be less precise on the GPU; lowering
        // already folds it, so the IR's answer wins everywhere.
        case IR::OP_FADD: return isFloat2 && MakeFloatResult(fa + fb, out);
        case IR::OP_FSUB: return isFloat2 && MakeFloatResult(fa - fb, out);
        case IR::OP_FMUL: return isFloat2 && MakeFloatResult(fa * fb, out);
        case IR::OP_FDIV: return isFloat2 && fb != 0.0f && MakeFloatResult(fa / fb, out);
        case IR::OP_FMIN: return isFloat2 && MakeFloatResult(std::fmin(fa, fb), out);
        case IR::OP_FMAX: return isFloat2 && MakeFloatResult(std::fmax(fa, fb), out);
        case IR::OP_FCLAMP:
            return type == CoreType::FLOAT && sameTypes3 && fb <= fc &&
                   MakeFloatResult(std::fmin(std::fmax(fa, fb), fc), out);
        case IR::OP_FNEG: return type == CoreType::FLOAT && MakeFloatResult(-fa, out);
        case IR::OP_FABS: return type == CoreType::FLOAT && MakeFloatResult(std::fabs(fa), out);
        case IR::OP_FLOOR: return type == CoreType::FLOAT && MakeFloatResult(std::floor(fa), out);
        case IR::OP_CEIL: return type == CoreType::FLOAT && MakeFloatResult(std::ceil(fa), out);
        case IR::OP_TRUNC: return type == CoreType::FLOAT && MakeFloatResult(std::trunc(fa), out);

        // Integer arithmetic wraps, as it does on the GPU.
        case IR::OP_IADD:
        case IR::OP_ISUB:
        case IR::OP_IMUL: {
            if (!IsIntegerType(type) || !sameTypes2) return false;
            u32 result = op == IR::OP_IADD ? ua + ub : op == IR::OP_ISUB ? ua - ub : ua * ub;
            *out = MakeConstant(type, result);
            return true;
        }
        case IR::OP_IDIV:
        case IR::OP_IMOD:
            // Signed division and remainder of negative operands differ
            // between GLSL, HLSL and MSL, so only non-negative ones fold.
            if (!IsIntegerType(type) || !sameTypes2 || ub == 0) return false;
            if (type == CoreType::INT && (sa < 0 || sb < 0)) return false;
            *out = MakeConstant(type, op == IR::OP_IDIV ? ua / ub : ua % ub);
            return true;
        case IR::OP_INEG:
            if (type != CoreType::INT) return false;
            *out = MakeConstant(type, 0u - ua);
            return true;
        case IR::OP_IABS:
            if (type != CoreType::INT || sa == INT32_MIN) return false;
            *out = MakeConstant(type, sa < 0 ? 0u - ua : ua);
            return true;
        case IR::OP_IMIN:
            if (!isInt2) return false;
            *out = MakeConstant(type, static_cast<u32>(std::min(sa, sb)));
            return true;
        case IR::OP_IMAX:
            if (!isInt2) return false;
            *out = MakeConstant(type, static_cast<u32>(std::max(sa, sb)));
            return true;
        case IR::OP_ICLAMP:
            if (type != CoreType::INT || !sameTypes3 || sb > sc) return false;
            *out = MakeConstant(type, static_cast<u32>(std::min(std::max(sa, sb), sc)));
            return true;
        case IR::OP_UMIN:
            if (!isUint2) return false;
            *out = MakeConstant(type, std::min(ua, ub));
            return true;
        case IR::OP_UMAX:
            if (!isUint2) return false;
            *out = MakeConstant(type, std::max(ua, ub));
            return true;
        case IR::OP_UCLAMP:
            if (type != CoreType::UINT || !sameTypes3 || ub > uc) return false;
            *out = MakeConstant(type, std::min(std::max(ua, ub), uc));
            return true;

        // Bitwise operations are logical ones on bools, whose bits are 0 or 1.
        case IR::OP_AND:
        case IR::OP_OR:
        case IR::OP_XOR: {
            if (!sameTypes2 || !(IsIntegerType(type) || type == CoreType::BOOL)) return false;
            u32 result = op == IR::OP_AND ? ua & ub : op == IR::OP_OR ? ua | ub : ua ^ ub;
            *out = MakeConstant(type, result);
            return true;
        }
        case IR::OP_NOT:
            if (type == CoreType::BOOL) *out = MakeConstant(type, ua ^ 1u);
            else if (IsIntegerType(type)) *out = MakeConstant(type, ~ua);
            else return false;
            return true;
        case IR::OP_SHL:
        case IR::OP_SHR:
        case IR::OP_ASR: {
            // Shifting by the bit width or more is undefined.
            if (!IsIntegerType(type) || !IsIntegerType(in[1].type) || ub >= 32) return false;
            u32 result = op == IR::OP_SHL ? ua << ub
                       : op == IR::OP_SHR ? ua >> ub
                                          : static_cast<u32>(sa >> ub);
            *out = MakeConstant(type, result);
            return true;
        }

        case IR::OP_FEQ: case IR::OP_FNE: case IR::OP_FLT:
        case IR::OP_FLE: case IR::OP_FGT: case IR::OP_FGE: {
            if (!isFloat2) return false;
            bool result = op == IR::OP_FEQ ? fa == fb : op == IR::OP_FNE ? fa != fb
                        : op == IR::OP_FLT ? fa < fb : op == IR::OP_FLE ? fa <= fb
                        : op == IR::OP_FGT ? fa > fb : fa >= fb;
            *out = MakeConstant(CoreType::BOOL, result ? 1 : 0);
            return true;
        }
        case IR::OP_IEQ:
        case IR::OP_INE:
            if (!sameTypes2 || !(IsIntegerType(type) || type == CoreType::BOOL)) return false;
            *out = MakeConstant(CoreType::BOOL, (ua == ub) == (op == IR::OP_IEQ) ? 1 : 0);
            return true;
        case IR::OP_ILT: case IR::OP_ILE: case IR::OP_IGT: case IR::OP_IGE: {
            if (!isInt2) return false;
            bool result = op == IR::OP_ILT ? sa < sb : op == IR::OP_ILE ? sa <= sb
                        : op == IR::OP_IGT ? sa > sb : sa >= sb;
            *out = MakeConstant(CoreType::BOOL, result ? 1 : 0);
            return true;
        }
        case IR::OP_ULT: case IR::OP_ULE: case IR::OP_UGT: case IR::OP_UGE: {
            if (!isUint2) return false;
            bool result = op == IR::OP_ULT ? ua < ub : op == IR::OP_ULE ? ua <= ub
                        : op == IR::OP_UGT ? ua > ub : ua >= ub;
            *out = MakeConstant(CoreType::BOOL, result ? 1 : 0);
            return true;
        }

        // Conversions fold only when the result is exact and in range.
        case IR::OP_F2I: {
            if (type != CoreType::FLOAT) return false;
            double truncated = std::trunc(static_cast<double>(fa));
            if (truncated < -2147483648.0 || truncated > 2147483647.0) return false;
            *out = MakeConstant(CoreType::INT, static_cast<u32>(static_cast<s32>(truncated)));
            return true;
        }
        case IR::OP_F2U: {
            if (type != CoreType::FLOAT || fa < 0.0f) return false;
            double truncated = std::trunc(static_cast<double>(fa));
            if (truncated > 4294967295.0) return false;
            *out = MakeConstant(CoreType::UINT, static_cast<u32>(truncated));
            return true;
        }
        case IR::OP_I2F: {
            if (type != CoreType::INT) return false;
            float value = static_cast<float>(sa);
            return static_cast<double>(value) == static_cast<double>(sa) &&
                   MakeFloatResult(value, out);
        }
        case IR::OP_U2F: {
            if (type != CoreType::UINT) return false;
            float value = static_cast<float>(ua);
            return static_cast<double>(value) == static_cast<double>(ua) &&
                   MakeFloatResult(value, out);
        }
        case IR::OP_I2U:
            if (type != CoreType::INT) return false;
            *out = MakeConstant(CoreType::UINT, ua);
            return true;
        case IR::OP_U2I:
            if (type != CoreType::UINT) return false;
            *out = MakeConstant(CoreType::INT, ua);
            return true;

        default:
            return false;
    }
}

static ConstantValue EvaluateInstruction(const ConstantPropagationData* data, u32 inst) {
    const IRProgram* ir = data->ir;
    u16 op = ir->opcodes[inst];

    // SELECT(falseValue, trueValue, condition)
    if (op == IR::OP_SELECT) {
        ConstantValue condition = GetValue(data, ir->GetOperand(inst, 2));
        if (condition.state == ValueState::UNDEFINED) return UNDEFINED_VALUE;
        if (condition.state == ValueState::CONSTANT && condition.type == CoreType::BOOL) {
            return GetValue(data, ir->GetOperand(inst, condition.bits ? 1 : 0));
        }
        return Meet(GetValue(data, ir->GetOperand(inst, 0)), GetValue(data, ir->GetOperand(inst, 1)));
    }

    u32 arity = GetFoldArity(op);
    if (arity == 0) return OVERDEFINED_VALUE;
    ConstantValue in[3] = {UNDEFINED_VALUE, UNDEFINED_VALUE, UNDEFINED_VALUE};
    bool waiting = false;
    for (u32 k = 0; k < arity; k++) {
        in[k] = GetValue(data, ir->GetOperand(inst, k));
        if (in[k].state == ValueState::OVERDEFINED) return OVERDEFINED_VALUE;
        if (in[k].state == ValueState::UNDEFINED) waiting = true;
    }
    if (waiting) return UNDEFINED_VALUE;

    ConstantValue result;
    if (!FoldOperation(op, in, &result)) return OVERDEFINED_VALUE;
    return result;
}

// ============= Propagation =============

static void UpdateRegister(ConstantPropagationData* data, u16 reg, ConstantValue value) {
    CoreType type = GetRegisterType(data->ir, reg);
    if (value.state == ValueState::CONSTANT && value.type != type) value = OVERDEFINED_VALUE;
    ConstantValue current = {data->states[reg], type, data->bits[reg]};
    ConstantValue next = Meet(current, value);
    if (next.state == current.state && next.bits == current.bits) return;
    data->states[reg] = next.state;
    data->bits[reg] = next.state == ValueState::CONSTANT ? next.bits : 0;
    data->regWork[data->regWorkTail++] = reg;
}

static bool IsEdgeExecutable(const ConstantPropagationData* data, u32 from, u32 to) {
    const CFG* cfg = data->cfg;
    for (u32 k = cfg->predecessorOffsets[to]; k < cfg->predecessorOffsets[to + 1]; k++) {
        if (cfg->predecessorData[k] == from && data->edgeExecutable[k]) return true;
    }
    return false;
}

static void EvaluatePhi(ConstantPropagationData* data, u32 phi) {
    const IRProgram* ir = data->ir;
    u16 result = ir->phiResultRegs[phi];
    if (result >= data->regCount || !data->tracked[result]) return;
    u32 block = ir->phiBlockIndices[phi];
    ConstantValue value = UNDEFINED_VALUE;
    for (u32 j = 0; j < ir->GetPhiOperandCount(phi); j++) {
        if (!IsEdgeExecutable(data, ir->GetPhiOperandBlock(phi, j), block)) continue;
        u16 operand = ir->GetPhiOperandValue(phi, j);
        // An undef operand stands for a path that never assigned the
        // variable, so any value will do for it.
        if (IsRegisterOperand(data, operand) && data->undef[operand]) continue;
        value = Meet(value, GetValue(data, operand));
    }
    UpdateRegister(data, result, value);
}

static void MarkEdgeExecutable(ConstantPropagationData* data, u32 from, u32 to) {
    const CFG* cfg = data->cfg;
    if (to >= cfg->blockCount) return;
    bool found = false;
    bool added = false;
    for (u32 k = cfg->predecessorOffsets[to]; k < cfg->predecessorOffsets[to + 1]; k++) {
        if (cfg->predecessorData[k] != from) continue;
        found = true;
        if (!data->edgeExecutable[k]) {
            data->edgeExecutable[k] = true;
            added = true;
        }
    }
    if (found && !added) return;
    if (!data->blockExecutable[to]) {
        data->blockExecutable[to] = true;
        data->blockWork[data->blockWorkTail++] = to;
        return;
    }
    // A new incoming edge can change the phis of a block already visited.
    for (u32 k = data->blockPhiOffsets[to]; k < data->blockPhiOffsets[to + 1]; k++) {
        EvaluatePhi(data, data->blockPhis[k]);
    }
}

static void VisitTerminator(ConstantPropagationData* data, u32 block) {
    const IRProgram* ir = data->ir;
    const CFG* cfg = data->cfg;
    u32 last = cfg->lastInst[block];
    u16 op = ir->opcodes[last];
    if (op == IR::OP_BRANCH) {
        ConstantValue condition = GetValue(data, ir->GetOperand(last, 0));
        if (condition.state == ValueState::UNDEFINED) return;
        if (condition.state == ValueState::CONSTANT) {
            // Bits are zero exactly when the value is: -0.0 never folds.
            u32 target = condition.bits != 0 ? ir->GetBranchTrueTarget(last)
                                             : ir->GetBranchFalseTarget(last);
            if (target < ir->instructionCount) {
                MarkEdgeExecutable(data, block, cfg->instToBlock[target]);
                return;
            }
        }
    } else if (op == IR::OP_SWITCH) {
        ConstantValue selector = GetValue(data, ir->GetOperand(last, 0));
        if (selector.state == ValueState::UNDEFINED) return;
        u32 switchId = ir->metadata[last];
        if (selector.state == ValueState::CONSTANT && IsIntegerType(selector.type) &&
            switchId < ir->switchCount) {
            u32 target = ir->GetSwitchDefaultTarget(switchId);
            for (u32 c = 0; c < ir->GetSwitchCaseCount(switchId); c++) {
                if (static_cast<u32>(ir->GetSwitchCaseValue(switchId, c)) == selector.bits) {
                    target = ir->GetSwitchCaseTarget(switchId, c);
                    break;
                }
            }
            if (target < ir->instructionCount) {
                MarkEdgeExecutable(data, block, cfg->instToBlock[target]);
                return;
            }
        }
    }
    for (u32 s = 0; s < cfg->TotalSuccessorCount(block); s++) {
        MarkEdgeExecutable(data, block, cfg->GetAnySuccessor(block, s));
    }
}

static void VisitInstruction(ConstantPropagationData* data, u32 inst) {
    const IRProgram* ir = data->ir;
    u16 op = ir->opcodes[inst];
    if (op == IR::OP_NOP) return;
    if (op == IR::OP_BRANCH || op == IR::OP_SWITCH) {
        VisitTerminator(data, data->cfg->instToBlock[inst]);
        return;
    }
    u16 dest = ir->destinations[inst];
    if (dest < data->regCount && data->tracked[dest] && data->definitions[dest] == inst) {
        UpdateRegister(data, dest, EvaluateInstruction(data, inst));
    }
}

static void VisitBlock(ConstantPropagationData* data, u32 block) {
    const CFG* cfg = data->cfg;
    for (u32 k = data->blockPhiOffsets[block]; k < data->blockPhiOffsets[block + 1]; k++) {
        EvaluatePhi(data, data->blockPhis[k]);
    }
    for (u32 i = cfg->firstInst[block]; i <= cfg->lastInst[block]; i++) {
        u16 op = data->ir->opcodes[i];
        if (op != IR::OP_BRANCH && op != IR::OP_SWITCH) VisitInstruction(data, i);
    }
    VisitTerminator(data, block);
}

static void Propagate(ConstantPropagationData* data) {
    u32 entry = data->cfg->entryBlock;
    data->blockExecutable[entry] = true;
    data->blockWork[data->blockWorkTail++] = entry;

    while (data->blockWorkHead < data->blockWorkTail || data->regWorkHead < data->regWorkTail) {
        if (data->blockWorkHead < data->blockWorkTail) {
            VisitBlock(data, data->blockWork[data->blockWorkHead++]);
            continue;
        }
        u16 reg = static_cast<u16>(data->regWork[data->regWorkHead++]);
        for (u32 k = data->useOffsets[reg]; k < data->useOffsets[reg + 1]; k++) {
            u32 user = data->useData[k];
            if (user & PHI_REFERENCE) {
                u32 phi = user & ~PHI_REFERENCE;
                if (data->blockExecutable[data->ir->phiBlockIndices[phi]]) EvaluatePhi(data, phi);
            } else if (data->blockExecutable[data->cfg->instToBlock[user]]) {
                VisitInstruction(data, user);
            }
        }
    }
}

// ============= Rewriting =============

static bool IsUseInDestination(u16 op) {
    // Same list as SSA: these carry a value register in `dest`, not a result.
    return op == IR::OP_LOCAL_STORE || op == IR::OP_ARRAY_STORE ||
           op == IR::OP_STORE_OUTPUT || op == IR::OP_STORE_BUFFER;
}

// Same list as SSA: these hold an attribute, input or output slot in their
// operands, never a register.
static bool HasSlotOperands(u16 op) {
    return op == IR::OP_LOAD_ATTR || op == IR::OP_LOAD_INPUT || op == IR::OP_LOAD_CONST ||
           op == IR::OP_LOAD_OUTPUT || op == IR::OP_STORE_OUTPUT;
}

// Operand slots that hold a literal index rather than a register.
static bool IsLiteralSlot(u16 op, u32 slot) {
    return slot == 1 &&
           (op == IR::OP_VEC_INSERT || op == IR::OP_VEC_EXTRACT || op == IR::OP_ENUM_FIELD ||
            op == IR::OP_STRUCT_INSERT || op == IR::OP_STRUCT_EXTRACT ||
            op == IR::OP_STRUCT_ARRAY_EXTRACT || op == IR::OP_STRUCT_ARRAY_INSERT ||
            op == IR::OP_LOCAL_FIELD_PTR);
}

// Value operations without side effects, beyond the ones this pass folds.
static bool IsPureValueOp(u16 op) {
    return op == IR::OP_SELECT || op == IR::OP_VEC_CONSTRUCT || op == IR::OP_VEC_INSERT ||
           (op >= IR::OP_SQRT && op <= IR::OP_COSH) || op == IR::OP_TANH ||
           op == IR::OP_FMA || op == IR::OP_FRACT || op == IR::OP_ROUND ||
           op == IR::OP_FMOD || op == IR::OP_FREM || op == IR::OP_SIGN ||
           (op >= IR::OP_LERP && op <= IR::OP_RADIANS);
}

// Instructions whose operands may become tagged constants. The backends
// read the type of a shift's operands from the register table, so shifts
// only ever fold whole.
static bool IsRewritableOp(u16 op) {
    if (op == IR::OP_SHL || op == IR::OP_SHR || op == IR::OP_ASR) return false;
    return GetFoldArity(op) != 0 || IsPureValueOp(op) || op == IR::OP_BRANCH ||
           op == IR::OP_SWITCH;
}

// Returns 0xFFFF when the constant pool is full.
static u16 InternConstant(IR::IRBuilder* builder, CoreType type, u32 bits) {
    switch (type) {
        case CoreType::BOOL:
            return builder->EmitConstantBool(bits != 0);
        case CoreType::INT:
        case CoreType::UINT:
            return builder->EmitConstantInteger(bits, type == CoreType::UINT);
        case CoreType::FLOAT: {
            bool wasExceeded = builder->floatConstantLimitExceeded;
            u16 constant = builder->EmitConstant(BitsToFloat(bits));
            if (builder->floatConstantLimitExceeded && !wasExceeded) {
                builder->floatConstantLimitExceeded = false;
                return 0xFFFF;
            }
            return constant;
        }
        default:
            return 0xFFFF;
    }
}

static u16 GetConstantOperand(ConstantPropagationData* data, u16 reg) {
    if (data->states[reg] != ValueState::CONSTANT) return 0xFFFF;
    return InternConstant(data->builder, GetRegisterType(data->ir, reg), data->bits[reg]);
}

static void RewriteOperands(ConstantPropagationData* data, u32 inst) {
    IRProgram* ir = data->ir;
    u16 op = ir->opcodes[inst];
    if (!IsRewritableOp(op)) return;
    for (u32 slot = 0; slot < 4; slot++) {
        u16 operand = ir->GetOperand(inst, slot);
        if (IsLiteralSlot(op, slot) || !IsRegisterOperand(data, operand) ||
            !data->tracked[operand]) {
            continue;
        }
        u16 constant = GetConstantOperand(data, operand);
        if (constant != 0xFFFF) ir->SetOperand(inst, slot, constant);
    }
}

// A constant result can still be read where an operand can't be a constant
// (a phi, an output store, a texture operation), so its definition becomes
// a plain constant copy.
static void MakeConstantCopy(ConstantPropagationData* data, u32 inst, u16 constant) {
    IRProgram* ir = data->ir;
    u16 op = ir->opcodes[inst];
    if (op == IR::OP_LOAD_REG || op == IR::OP_STORE_REG) return;
    ir->opcodes[inst] = IR::OP_LOAD_REG;
    ir->metadata[inst] = 0;
    ir->SetOperand(inst, 0, constant);
    for (u32 slot = 1; slot < 4; slot++) ir->SetOperand(inst, slot, 0xFFFF);
}

static void RewriteConstants(ConstantPropagationData* data) {
    IRProgram* ir = data->ir;
    for (u32 i = 0; i < ir->instructionCount; i++) RewriteOperands(data, i);

    // A phi with a constant result gets the constant on every edge, which
    // the backends treat as a copy of it.
    for (u32 reg = 0; reg < data->regCount; reg++) {
        if (!data->tracked[reg] || data->states[reg] != ValueState::CONSTANT) continue;
        u16 constant = GetConstantOperand(data, static_cast<u16>(reg));
        if (constant == 0xFFFF) continue;
        u32 definition = data->definitions[reg];
        if (definition & PHI_REFERENCE) {
            u32 phi = definition & ~PHI_REFERENCE;
            for (u32 j = 0; j < ir->GetPhiOperandCount(phi); j++) {
                ir->SetPhiOperandValue(phi, j, constant);
            }
            continue;
        }
        MakeConstantCopy(data, definition, constant);
    }
}

static void AddUndefRegister(ConstantPropagationData* data, u16 reg, u16 type) {
    IRProgram* ir = data->ir;
    if (ir->undefRegCount == ir->undefRegCapacity) {
        u32 capacity = std::max<u32>(ir->undefRegCapacity * 2, 16);
        u16* regs = (u16*)data->builder->pool->Allocate(capacity * sizeof(u16), 64);
        u16* types = (u16*)data->builder->pool->Allocate(capacity * sizeof(u16), 64);
        if (ir->undefRegCount > 0) {
            memcpy(regs, ir->undefRegs, ir->undefRegCount * sizeof(u16));
            memcpy(types, ir->undefRegTypes, ir->undefRegCount * sizeof(u16));
        }
        ir->undefRegs = regs;
        ir->undefRegTypes = types;
        ir->undefRegCapacity = capacity;
    }
    ir->undefRegs[ir->undefRegCount] = reg;
    ir->undefRegTypes[ir->undefRegCount] = type;
    ir->undefRegCount++;
    data->undef[reg] = true;
}

// The backends emit a branch with a constant condition as a jump (see
// SPIRVBuilder::EmitBranch), so the edge to the arm it skips disappears.
// Phis lose their operand for that edge. One left with none is in a block
// nothing branches to any more, and its result becomes an undef register.
static void DropFoldedBranchEdges(ConstantPropagationData* data, BWSL_Arena* arena) {
    IRProgram* ir = data->ir;
    const CFG* cfg = data->cfg;
    if (ir->phiCount == 0 || !ir->structureInfo) return;
    bool* dropped = (bool*)AllocateZeroed(arena, ir->phiOperandOffsets[ir->phiCount] * sizeof(bool));
    bool anyDropped = false;
    for (u32 block = 0; block < cfg->blockCount; block++) {
        u32 last = cfg->lastInst[block];
        bool condition;
        if (ir->opcodes[last] != IR::OP_BRANCH ||
            (ir->structureInfo[last] & IRProgram::STRUCT_TYPE_MASK) == IRProgram::STRUCT_LOOP_HEADER ||
            !IR::TryGetConstantCondition(ir, ir->GetOperand(last, 0), &condition)) {
            continue;
        }
        u32 taken = condition ? ir->GetBranchTrueTarget(last) : ir->GetBranchFalseTarget(last);
        u32 skipped = condition ? ir->GetBranchFalseTarget(last) : ir->GetBranchTrueTarget(last);
        if (taken == skipped || skipped >= ir->instructionCount) continue;
        u32 skippedBlock = cfg->instToBlock[skipped];
        if (taken < ir->instructionCount && cfg->instToBlock[taken] == skippedBlock) continue;
        for (u32 k = data->blockPhiOffsets[skippedBlock]; k < data->blockPhiOffsets[skippedBlock + 1]; k++) {
            u32 phi = data->blockPhis[k];
            for (u32 j = ir->phiOperandOffsets[phi]; j < ir->phiOperandOffsets[phi + 1]; j++) {
                if (ir->phiOperandBlocks[j] != block) continue;
                dropped[j] = true;
                anyDropped = true;
            }
        }
    }
    if (!anyDropped) return;

    u32 kept = 0;
    for (u32 p = 0; p < ir->phiCount; p++) {
        u32 start = ir->phiOperandOffsets[p];
        u32 end = ir->phiOperandOffsets[p + 1];
        ir->phiOperandOffsets[p] = kept;
        for (u32 j = start; j < end; j++) {
            if (dropped[j]) continue;
            ir->phiOperandValues[kept] = ir->phiOperandValues[j];
            ir->phiOperandBlocks[kept] = ir->phiOperandBlocks[j];
            kept++;
        }
        if (kept == ir->phiOperandOffsets[p]) {
            data->phiRemoved[p] = true;
            AddUndefRegister(data, ir->phiResultRegs[p], ir->phiTypes[p]);
        }
    }
    ir->phiOperandOffsets[ir->phiCount] = kept;
}

// A phi whose emitted incoming edges all carry one value is a copy of it.
// Giving it that value on every edge lets the backends alias the two, so no
// phi is emitted. The value reaches the phi's block on every emitted path,
// so it dominates every use. Edges count by what the backends emit, not by
// what can run: a loop header or switch keeps every edge, even when its
// condition is constant.
static void CollapseSingleValuePhis(ConstantPropagationData* data, BWSL_Arena* arena) {
    IRProgram* ir = data->ir;
    const CFG* cfg = data->cfg;
    if (ir->phiCount == 0) return;
    bool* reachable = (bool*)AllocateZeroed(arena, cfg->blockCount * sizeof(bool));
    bool* edgeEmitted = (bool*)AllocateZeroed(arena, cfg->predecessorDataCount * sizeof(bool));
    u32* queue = (u32*)AllocateZeroed(arena, cfg->blockCount * sizeof(u32));
    u32 head = 0, tail = 0;
    auto emitEdge = [&](u32 from, u32 to) {
        if (to >= cfg->blockCount) return;
        for (u32 k = cfg->predecessorOffsets[to]; k < cfg->predecessorOffsets[to + 1]; k++) {
            if (cfg->predecessorData[k] == from) edgeEmitted[k] = true;
        }
        if (!reachable[to]) {
            reachable[to] = true;
            queue[tail++] = to;
        }
    };
    auto isEdgeEmitted = [&](u32 from, u32 to) {
        for (u32 k = cfg->predecessorOffsets[to]; k < cfg->predecessorOffsets[to + 1]; k++) {
            if (cfg->predecessorData[k] == from && edgeEmitted[k]) return true;
        }
        return false;
    };

    // Mirrors SPIRVBuilder::EmitBranch and the direct GLES emitter.
    reachable[cfg->entryBlock] = true;
    queue[tail++] = cfg->entryBlock;
    while (head < tail) {
        u32 block = queue[head++];
        u32 last = cfg->lastInst[block];
        bool condition;
        if (ir->opcodes[last] == IR::OP_BRANCH && ir->structureInfo &&
            (ir->structureInfo[last] & IRProgram::STRUCT_TYPE_MASK) != IRProgram::STRUCT_LOOP_HEADER &&
            IR::TryGetConstantCondition(ir, ir->GetOperand(last, 0), &condition)) {
            u32 target = condition ? ir->GetBranchTrueTarget(last) : ir->GetBranchFalseTarget(last);
            if (target < ir->instructionCount) {
                emitEdge(block, cfg->instToBlock[target]);
                continue;
            }
        }
        for (u32 s = 0; s < cfg->TotalSuccessorCount(block); s++) {
            emitEdge(block, cfg->GetAnySuccessor(block, s));
        }
    }

    // Collapsing one phi can make another single-valued.
    bool changed = true;
    while (changed) {
        changed = false;
        for (u32 p = 0; p < ir->phiCount; p++) {
            u16 result = ir->phiResultRegs[p];
            u32 block = ir->phiBlockIndices[p];
            u16 value = 0xFFFF;
            bool single = true;
            for (u32 j = 0; j < ir->GetPhiOperandCount(p) && single; j++) {
                u16 operand = ir->GetPhiOperandValue(p, j);
                if (operand == result || !isEdgeEmitted(ir->GetPhiOperandBlock(p, j), block)) continue;
                if (value == 0xFFFF) value = operand;
                else single = operand == value;
            }
            if (!single || value == 0xFFFF) continue;
            for (u32 j = 0; j < ir->GetPhiOperandCount(p); j++) {
                if (ir->GetPhiOperandValue(p, j) == value) continue;
                ir->SetPhiOperandValue(p, j, value);
                changed = true;
            }
        }
    }
}

// A single-block stage skips SSA, so a local assigned twice keeps one
// register. One forward walk tracks each register's current value instead,
// rewriting operands as it goes.
static void PropagateStraightLine(ConstantPropagationData* data) {
    IRProgram* ir = data->ir;
    // A register read before any write holds whatever the backend set up.
    for (u32 reg = 0; reg < data->regCount; reg++) data->states[reg] = ValueState::OVERDEFINED;

    for (u32 i = 0; i < ir->instructionCount; i++) {
        u16 op = ir->opcodes[i];
        if (op == IR::OP_NOP) continue;
        RewriteOperands(data, i);
        u16 dest = ir->destinations[i];
        if (!IsRegisterOperand(data, dest) || !data->tracked[dest] || IsUseInDestination(op) ||
            op == IR::OP_JUMP || op == IR::OP_BRANCH || op == IR::OP_SWITCH || op == IR::OP_RET) {
            continue;
        }
        ConstantValue value = EvaluateInstruction(data, i);
        bool isConstant = value.state == ValueState::CONSTANT &&
                          value.type == GetRegisterType(ir, dest);
        data->states[dest] = isConstant ? ValueState::CONSTANT : ValueState::OVERDEFINED;
        data->bits[dest] = isConstant ? value.bits : 0;
        if (!isConstant) continue;
        u16 constant = GetConstantOperand(data, dest);
        if (constant != 0xFFFF) MakeConstantCopy(data, i, constant);
    }
}

// ============= Dead code =============

static bool IsRemovableDefinition(const ConstantPropagationData* data, u16 reg) {
    if (data->definitionCounts[reg] != 1) return false;
    if (data->ir->registerStorageInfo && data->ir->registerStorageInfo[reg] != 0) return false;
    u32 definition = data->definitions[reg];
    if (definition & PHI_REFERENCE) return true;
    u16 op = data->ir->opcodes[definition];
    return GetFoldArity(op) != 0 || IsPureValueOp(op);
}

// Removes the value instructions and phis that nothing reads any more,
// including ones that only fed instructions removed here.
static void RemoveDeadValues(ConstantPropagationData* data, BWSL_Arena* arena) {
    IRProgram* ir = data->ir;
    u32 regCount = data->regCount;
    u32* useCounts = (u32*)arena->Allocate(regCount * sizeof(u32), 64);
    bool* queued = (bool*)arena->Allocate(regCount * sizeof(bool), 64);
    u32* work = (u32*)arena->Allocate(regCount * sizeof(u32), 64);
    bool* phiDead = (bool*)arena->Allocate(std::max<u32>(ir->phiCount, 1) * sizeof(bool), 64);
    memset(useCounts, 0, regCount * sizeof(u32));
    memset(queued, 0, regCount * sizeof(bool));
    memset(phiDead, 0, std::max<u32>(ir->phiCount, 1) * sizeof(bool));
    if (data->phiRemoved) memcpy(phiDead, data->phiRemoved, ir->phiCount * sizeof(bool));

    // Count operands as uses, literal indices included: an over-count only
    // keeps a value alive. A destination counts too unless it's the
    // register's recorded definition: STORE_OUTPUT and LOCAL_STORE carry a
    // value register there.
    for (u32 i = 0; i < ir->instructionCount; i++) {
        if (ir->opcodes[i] == IR::OP_NOP) continue;
        for (u32 slot = 0; slot < 4 && !HasSlotOperands(ir->opcodes[i]); slot++) {
            u16 operand = ir->GetOperand(i, slot);
            if (IsRegisterOperand(data, operand)) useCounts[operand]++;
        }
        u16 dest = ir->destinations[i];
        if (IsRegisterOperand(data, dest) && data->definitions[dest] != i) useCounts[dest]++;
    }
    for (u32 p = 0; p < ir->phiCount; p++) {
        for (u32 j = 0; j < ir->GetPhiOperandCount(p); j++) {
            u16 operand = ir->GetPhiOperandValue(p, j);
            if (IsRegisterOperand(data, operand)) useCounts[operand]++;
        }
    }

    u32 workCount = 0;
    for (u32 reg = 0; reg < regCount; reg++) {
        if (useCounts[reg] == 0 && IsRemovableDefinition(data, static_cast<u16>(reg))) {
            queued[reg] = true;
            work[workCount++] = reg;
        }
    }
    auto release = [&](u16 operand) {
        if (!IsRegisterOperand(data, operand) || useCounts[operand] == 0) return;
        if (--useCounts[operand] == 0 && !queued[operand] && IsRemovableDefinition(data, operand)) {
            queued[operand] = true;
            work[workCount++] = operand;
        }
    };

    while (workCount > 0) {
        u32 reg = work[--workCount];
        u32 definition = data->definitions[reg];
        if (definition & PHI_REFERENCE) {
            u32 phi = definition & ~PHI_REFERENCE;
            phiDead[phi] = true;
            for (u32 j = 0; j < ir->GetPhiOperandCount(phi); j++) release(ir->GetPhiOperandValue(phi, j));
            continue;
        }
        for (u32 slot = 0; slot < 4; slot++) release(ir->GetOperand(definition, slot));
        ir->opcodes[definition] = IR::OP_NOP;
        ir->destinations[definition] = 0;
        for (u32 slot = 0; slot < 4; slot++) ir->SetOperand(definition, slot, 0xFFFF);
    }

    // Compact the phi arrays in place, keeping their order.
    u32 keptPhis = 0;
    u32 keptOperands = 0;
    for (u32 p = 0; p < ir->phiCount; p++) {
        if (phiDead[p]) continue;
        u32 opStart = ir->phiOperandOffsets[p];
        u32 opEnd = ir->phiOperandOffsets[p + 1];
        ir->phiBlockIndices[keptPhis] = ir->phiBlockIndices[p];
        ir->phiResultRegs[keptPhis] = ir->phiResultRegs[p];
        ir->phiTypes[keptPhis] = ir->phiTypes[p];
        ir->phiOperandOffsets[keptPhis] = keptOperands;
        for (u32 k = opStart; k < opEnd; k++) {
            ir->phiOperandValues[keptOperands] = ir->phiOperandValues[k];
            ir->phiOperandBlocks[keptOperands] = ir->phiOperandBlocks[k];
            keptOperands++;
        }
        keptPhis++;
    }
    if (ir->phiCount > 0) {
        ir->phiOperandOffsets[keptPhis] = keptOperands;
        ir->phiCount = keptPhis;
    }
}

// ============= Setup =============

// Straight-line stages track registers assigned more than once, too: the
// forward walk knows which assignment is current.
static void FindDefinitions(ConstantPropagationData* data, bool straightLine) {
    IRProgram* ir = data->ir;
    for (u32 i = 0; i < ir->instructionCount; i++) {
        u16 op = ir->opcodes[i];
        if (op == IR::OP_NOP || op == IR::OP_JUMP || op == IR::OP_BRANCH ||
            op == IR::OP_SWITCH || op == IR::OP_RET || IsUseInDestination(op)) {
            continue;
        }
        u16 dest = ir->destinations[i];
        if (!IsRegisterOperand(data, dest)) continue;
        if (data->definitionCounts[dest] < 2) data->definitionCounts[dest]++;
        data->definitions[dest] = i;
    }
    for (u32 p = 0; p < ir->phiCount; p++) {
        u16 result = ir->phiResultRegs[p];
        if (!IsRegisterOperand(data, result)) continue;
        if (data->definitionCounts[result] < 2) data->definitionCounts[result]++;
        data->definitions[result] = PHI_REFERENCE | p;
    }
    for (u32 i = 0; ir->undefRegs && i < ir->undefRegCount; i++) {
        if (ir->undefRegs[i] < data->regCount) data->undef[ir->undefRegs[i]] = true;
    }
    for (u32 reg = 0; reg < data->regCount; reg++) {
        data->tracked[reg] = (straightLine || data->definitionCounts[reg] == 1) && !data->undef[reg] &&
            IsFoldedType(GetRegisterType(ir, static_cast<u16>(reg))) &&
            !(ir->registerStorageInfo && ir->registerStorageInfo[reg] != 0);
    }
}

static void BuildUseLists(ConstantPropagationData* data) {
    IRProgram* ir = data->ir;
    u32* offsets = data->useOffsets;
    for (u32 i = 0; i < ir->instructionCount; i++) {
        if (ir->opcodes[i] == IR::OP_NOP) continue;
        for (u32 slot = 0; slot < 4; slot++) {
            u16 operand = ir->GetOperand(i, slot);
            if (IsRegisterOperand(data, operand)) offsets[operand + 1]++;
        }
    }
    for (u32 p = 0; p < ir->phiCount; p++) {
        for (u32 j = 0; j < ir->GetPhiOperandCount(p); j++) {
            u16 operand = ir->GetPhiOperandValue(p, j);
            if (IsRegisterOperand(data, operand)) offsets[operand + 1]++;
        }
    }
    for (u32 reg = 0; reg < data->regCount; reg++) offsets[reg + 1] += offsets[reg];

    // Fill using the offsets as cursors, then shift them back.
    for (u32 i = 0; i < ir->instructionCount; i++) {
        if (ir->opcodes[i] == IR::OP_NOP) continue;
        for (u32 slot = 0; slot < 4; slot++) {
            u16 operand = ir->GetOperand(i, slot);
            if (IsRegisterOperand(data, operand)) data->useData[offsets[operand]++] = i;
        }
    }
    for (u32 p = 0; p < ir->phiCount; p++) {
        for (u32 j = 0; j < ir->GetPhiOperandCount(p); j++) {
            u16 operand = ir->GetPhiOperandValue(p, j);
            if (IsRegisterOperand(data, operand)) data->useData[offsets[operand]++] = PHI_REFERENCE | p;
        }
    }
    for (u32 reg = data->regCount; reg > 0; reg--) offsets[reg] = offsets[reg - 1];
    offsets[0] = 0;
}

static void GroupPhisByBlock(ConstantPropagationData* data) {
    const IRProgram* ir = data->ir;
    u32 blockCount = data->cfg->blockCount;
    u32* offsets = data->blockPhiOffsets;
    for (u32 p = 0; p < ir->phiCount; p++) {
        if (ir->phiBlockIndices[p] < blockCount) offsets[ir->phiBlockIndices[p] + 1]++;
    }
    for (u32 b = 0; b < blockCount; b++) offsets[b + 1] += offsets[b];
    for (u32 p = 0; p < ir->phiCount; p++) {
        u32 block = ir->phiBlockIndices[p];
        if (block < blockCount) data->blockPhis[offsets[block]++] = p;
    }
    for (u32 b = blockCount; b > 0; b--) offsets[b] = offsets[b - 1];
    offsets[0] = 0;
}

void PropagateConstants(IR::IRBuilder* builder, CFG* cfg) {
    IRProgram* ir = builder->program;
    if (ir->instructionCount == 0 || !ir->registerTypes) return;
    if (cfg && cfg->blockCount == 0) cfg = nullptr;
    bool straightLine = !cfg || cfg->blockCount == 1;
    if (straightLine && ir->phiCount > 0) return;

    u32 regCount = std::min<u32>(ir->registerCount, IR::RegisterLimit);
    u32 phiOperandCount = ir->phiCount > 0 ? ir->phiOperandOffsets[ir->phiCount] : 0;
    u32 maxUses = ir->instructionCount * 4 + phiOperandCount;
    u32 blockCount = cfg ? cfg->blockCount : 0;
    u32 edgeCount = cfg ? cfg->predecessorDataCount : 0;

    // Everything is sized up front, so the arena can't run out.
    size_t bytes = size_t(regCount) * (sizeof(ValueState) + sizeof(u32) * 4 + sizeof(bool) * 3 + 1) +
                   size_t(regCount) * 2 * sizeof(u32) +  // regWork
                   size_t(regCount + 1) * sizeof(u32) + size_t(maxUses) * sizeof(u32) +
                   size_t(blockCount) * (sizeof(bool) * 2 + sizeof(u32) * 3) + 2 * sizeof(u32) +
                   size_t(edgeCount) * sizeof(bool) * 2 + size_t(phiOperandCount) * sizeof(bool) +
                   size_t(ir->phiCount) * (sizeof(u32) + sizeof(bool) * 2) + 32 * 64;
    std::vector<char> memory(bytes);
    BWSL_Arena arena;
    arena.Initialize(memory.data(), memory.size());

    ConstantPropagationData data = {};
    data.ir = ir;
    data.builder = builder;
    data.cfg = cfg;
    data.regCount = regCount;
    data.states = (ValueState*)AllocateZeroed(&arena, regCount * sizeof(ValueState));
    data.bits = (u32*)AllocateZeroed(&arena, regCount * sizeof(u32));
    data.tracked = (bool*)AllocateZeroed(&arena, regCount * sizeof(bool));
    data.undef = (bool*)AllocateZeroed(&arena, regCount * sizeof(bool));
    data.definitions = (u32*)AllocateZeroed(&arena, regCount * sizeof(u32));
    data.definitionCounts = (u8*)AllocateZeroed(&arena, regCount);
    data.useOffsets = (u32*)AllocateZeroed(&arena, (regCount + 1) * sizeof(u32));
    data.useData = (u32*)AllocateZeroed(&arena, maxUses * sizeof(u32));
    data.regWork = (u32*)AllocateZeroed(&arena, regCount * 2 * sizeof(u32));
    data.blockExecutable = (bool*)AllocateZeroed(&arena, blockCount * sizeof(bool));
    data.edgeExecutable = (bool*)AllocateZeroed(&arena, edgeCount * sizeof(bool));
    data.blockPhiOffsets = (u32*)AllocateZeroed(&arena, (blockCount + 1) * sizeof(u32));
    data.blockPhis = (u32*)AllocateZeroed(&arena, ir->phiCount * sizeof(u32));
    data.phiRemoved = (bool*)AllocateZeroed(&arena, ir->phiCount * sizeof(bool));
    data.blockWork = (u32*)AllocateZeroed(&arena, blockCount * sizeof(u32));
    if (!data.states || !data.bits || !data.tracked || !data.undef || !data.definitions ||
        !data.definitionCounts || !data.useOffsets || !data.useData || !data.regWork ||
        !data.blockExecutable || !data.edgeExecutable || !data.blockPhiOffsets ||
        !data.blockPhis || !data.phiRemoved || !data.blockWork) {
        return;
    }
    memset(data.definitions, 0xFF, regCount * sizeof(u32));

    FindDefinitions(&data, straightLine);
    if (straightLine) {
        PropagateStraightLine(&data);
    } else {
        BuildUseLists(&data);
        GroupPhisByBlock(&data);
        Propagate(&data);
        RewriteConstants(&data);
        DropFoldedBranchEdges(&data, &arena);
        CollapseSingleValuePhis(&data, &arena);
    }
    RemoveDeadValues(&data, &arena);
}

}  // namespace Optimization
}  // namespace BWSL
