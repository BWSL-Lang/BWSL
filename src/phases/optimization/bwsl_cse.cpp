#include "bwsl_cse.h"
#include "core/bwsl_arena.h"
#include <algorithm>
#include <cstring>
#include <vector>

namespace BWSL {
namespace Optimization {

// The unity build puts this next to constant propagation's helpers.
namespace Cse {

using IR::IRProgram;

// Value numbers: registers get them counting up from 1. A tagged constant
// or a literal operand stands for itself, with a tag bit, so equal ones
// agree without a register.
constexpr u32 NO_VALUE = 0;
constexpr u32 CONSTANT_VALUE = 0x80000000;
constexpr u32 LITERAL_VALUE = 0x40000000;

struct CseEntry {
    u32 hash;
    u16 opcode;
    u16 type;            // The result register's type
    u16 instructionType;
    u32 metadata;
    u32 operands[4];     // Value numbers
    u16 reg;             // Register holding the result
    u32 version;         // Straight-line: the register's assignment count when it got the result
    u32 value;
    bool used;
};

struct CseData {
    IRProgram* ir;
    CFG* cfg;
    u32 regCount;
    bool straightLine;

    u32* values;           // [regCount] a register's current value number, NO_VALUE before it has one
    u32* versions;         // [regCount] straight-line: assignments so far
    u8* definitionCounts;  // [regCount] saturates at 2
    u32 nextValue;

    CseEntry* table;       // Open addressing, linear probing; size is a power of two
    u32 tableMask;
    u32* undo;             // CFG: slots filled in the blocks being walked, innermost last
    u32 undoCount;
};

// Operations whose result depends only on their operands, so two with the
// same operands give the same value wherever they run.
static bool IsCseOp(u16 op) {
    if ((op >= IR::OP_FADD && op <= IR::OP_FMA) || (op >= IR::OP_IADD && op <= IR::OP_UCLAMP) ||
        (op >= IR::OP_AND && op <= IR::OP_BITFIELD_INSERT) || (op >= IR::OP_FEQ && op <= IR::OP_UGE) ||
        (op >= IR::OP_F2I && op <= IR::OP_U2I) || (op >= IR::OP_SQRT && op <= IR::OP_COSH) ||
        (op >= IR::OP_DOT && op <= IR::OP_FACEFORWARD) || (op >= IR::OP_LERP && op <= IR::OP_RADIANS)) {
        return true;
    }
    switch (op) {
        case IR::OP_BITCAST: case IR::OP_SIGN: case IR::OP_FREM: case IR::OP_LDEXP:
        case IR::OP_TANH: case IR::OP_ISNORMAL:
        case IR::OP_VEC_EXTRACT: case IR::OP_VEC_INSERT: case IR::OP_VEC_SHUFFLE: case IR::OP_VEC_CONSTRUCT:
        case IR::OP_MAT_MUL: case IR::OP_MAT_TRANSPOSE: case IR::OP_MAT_INVERSE: case IR::OP_MAT_DET:
        case IR::OP_MAT_CONSTRUCT: case IR::OP_MAT_VEC_MUL: case IR::OP_VEC_MAT_MUL: case IR::OP_MAT_SCALE:
        case IR::OP_ANY: case IR::OP_ALL: case IR::OP_ISNAN: case IR::OP_ISINF: case IR::OP_ISFINITE:
        case IR::OP_SELECT:
            return true;
        default:
            return false;
    }
}

// Operations whose first two operands can swap.
static bool IsCommutative(u16 op) {
    switch (op) {
        case IR::OP_FADD: case IR::OP_FMUL: case IR::OP_IADD: case IR::OP_IMUL:
        case IR::OP_AND: case IR::OP_OR: case IR::OP_XOR:
        case IR::OP_FMIN: case IR::OP_FMAX: case IR::OP_IMIN: case IR::OP_IMAX:
        case IR::OP_UMIN: case IR::OP_UMAX:
        case IR::OP_FEQ: case IR::OP_FNE: case IR::OP_IEQ: case IR::OP_INE:
            return true;
        default:
            return false;
    }
}

// The operand slots that hold a component index rather than a register.
static bool IsLiteralSlot(u16 op, u32 slot) {
    return slot == 1 && (op == IR::OP_VEC_INSERT || op == IR::OP_VEC_EXTRACT);
}

static bool IsMemoryBacked(const IRProgram* ir, u16 reg) {
    return ir->registerStorageInfo && ir->registerStorageInfo[reg] != 0;
}

static bool DefinesRegister(const IRProgram* ir, u32 inst) {
    u16 op = ir->opcodes[inst];
    if (op == IR::OP_NOP || IR::IsTerminator(static_cast<IR::OpCode>(op))) return false;
    // These carry a value register in `dest`, not a result.
    return op != IR::OP_LOCAL_STORE && op != IR::OP_ARRAY_STORE && op != IR::OP_STORE_OUTPUT &&
           op != IR::OP_STORE_BUFFER;
}

static u32 NewValue(CseData* data) {
    return data->nextValue < LITERAL_VALUE ? data->nextValue++ : NO_VALUE;
}

// The value an operand holds where it's read, or NO_VALUE when it can't
// be named: a register a pointer can reach, or, with a CFG, one assigned
// more than once (SSA leaves those for variables it doesn't rename).
static u32 GetOperandValue(CseData* data, u16 op, u32 slot, u16 operand) {
    if (IsLiteralSlot(op, slot)) return LITERAL_VALUE | operand;
    if (operand == 0xFFFF) return LITERAL_VALUE | 0xFFFF;
    if (IR::IsConstant(operand)) return CONSTANT_VALUE | operand;
    if (operand >= data->regCount || IsMemoryBacked(data->ir, operand)) return NO_VALUE;
    if (!data->straightLine && data->definitionCounts[operand] > 1) return NO_VALUE;
    // A register no instruction has assigned yet holds what the stage
    // started with, the same for every read until it's assigned.
    if (data->values[operand] == NO_VALUE) data->values[operand] = NewValue(data);
    return data->values[operand];
}

static void AssignValue(CseData* data, u16 reg, u32 value) {
    if (reg >= data->regCount) return;
    data->values[reg] = value == NO_VALUE ? NewValue(data) : value;
    data->versions[reg]++;
}

static u32 HashEntry(const CseEntry& entry) {
    u32 hash = 2166136261u;
    auto mix = [&](u32 value) { hash = (hash ^ value) * 16777619u; };
    mix(entry.opcode);
    mix(entry.type);
    mix(entry.instructionType);
    mix(entry.metadata);
    for (u32 slot = 0; slot < 4; slot++) mix(entry.operands[slot]);
    return hash;
}

static bool SameKey(const CseEntry& a, const CseEntry& b) {
    return a.hash == b.hash && a.opcode == b.opcode && a.type == b.type &&
           a.instructionType == b.instructionType && a.metadata == b.metadata &&
           memcmp(a.operands, b.operands, sizeof(a.operands)) == 0;
}

// Copies get their source's value; a value instruction whose key is
// already available becomes a copy of the register that has it; every
// other assignment gets a new value.
static void VisitInstruction(CseData* data, u32 inst) {
    IRProgram* ir = data->ir;
    if (!DefinesRegister(ir, inst)) return;
    u16 op = ir->opcodes[inst];
    u16 dest = ir->destinations[inst];
    if (dest >= data->regCount) return;
    bool memory = IsMemoryBacked(ir, dest);

    if (op == IR::OP_LOAD_REG || op == IR::OP_STORE_REG) {
        u32 value = memory ? NO_VALUE : GetOperandValue(data, op, 0, ir->GetOperand(inst, 0));
        AssignValue(data, dest, value);
        return;
    }
    if (!IsCseOp(op) || memory || (!data->straightLine && data->definitionCounts[dest] > 1)) {
        AssignValue(data, dest, NO_VALUE);
        return;
    }

    CseEntry key = {};
    key.opcode = op;
    key.type = ir->registerTypes ? ir->registerTypes[dest] : 0;
    key.instructionType = ir->types[inst];
    key.metadata = ir->metadata[inst];
    for (u32 slot = 0; slot < 4; slot++) {
        key.operands[slot] = GetOperandValue(data, op, slot, ir->GetOperand(inst, slot));
        if (key.operands[slot] == NO_VALUE) {
            AssignValue(data, dest, NO_VALUE);
            return;
        }
    }
    if (IsCommutative(op) && key.operands[0] > key.operands[1]) std::swap(key.operands[0], key.operands[1]);
    key.hash = HashEntry(key);

    u32 slot = key.hash & data->tableMask;
    while (data->table[slot].used && !SameKey(data->table[slot], key)) slot = (slot + 1) & data->tableMask;
    CseEntry& entry = data->table[slot];
    bool available = entry.used && (!data->straightLine || data->versions[entry.reg] == entry.version);
    bool sameStruct = !ir->registerStructTypes || ir->registerStructTypes[dest] == ir->registerStructTypes[entry.reg];
    if (available && entry.reg != dest && sameStruct) {
        ir->opcodes[inst] = IR::OP_LOAD_REG;
        ir->metadata[inst] = 0;
        ir->SetOperand(inst, 0, entry.reg);
        for (u32 s = 1; s < 4; s++) ir->SetOperand(inst, s, 0xFFFF);
        AssignValue(data, dest, entry.value);
        return;
    }

    AssignValue(data, dest, NO_VALUE);
    if (data->values[dest] == NO_VALUE) return;
    bool fresh = !entry.used;
    key.reg = dest;
    key.version = data->versions[dest];
    key.value = data->values[dest];
    key.used = true;
    entry = key;
    if (fresh && !data->straightLine) data->undo[data->undoCount++] = slot;
}

static void VisitBlock(CseData* data, u32 block) {
    const IRProgram* ir = data->ir;
    for (u32 p = 0; p < ir->phiCount; p++) {
        if (ir->phiBlockIndices[p] == block) AssignValue(data, ir->phiResultRegs[p], NO_VALUE);
    }
    for (u32 i = data->cfg->firstInst[block]; i <= data->cfg->lastInst[block]; i++) VisitInstruction(data, i);
}

}  // namespace Cse

void EliminateCommonSubexpressions(IR::IRBuilder* builder, CFG* cfg) {
    using namespace Cse;
    IR::IRProgram* ir = builder->program;
    if (ir->instructionCount == 0 || !ir->registerTypes) return;
    if (cfg && cfg->blockCount == 0) cfg = nullptr;
    bool straightLine = !cfg || cfg->blockCount == 1;
    if (!straightLine && !cfg->domChildrenComputed) return;

    u32 regCount = std::min<u32>(ir->registerCount, IR::RegisterLimit);
    u32 tableSize = 16;
    while (tableSize < ir->instructionCount * 2) tableSize *= 2;
    u32 blockCount = cfg ? cfg->blockCount : 0;
    size_t bytes = size_t(regCount) * (sizeof(u32) * 2 + 1) + size_t(tableSize) * sizeof(CseEntry) +
                   size_t(ir->instructionCount) * sizeof(u32) + size_t(blockCount) * sizeof(u32) * 3 + 8 * 64;
    std::vector<char> memory(bytes);
    BWSL_Arena arena;
    arena.Initialize(memory.data(), memory.size());
    auto allocate = [&](size_t size) {
        void* pointer = arena.Allocate(std::max<size_t>(size, 1), 64);
        if (pointer) memset(pointer, 0, std::max<size_t>(size, 1));
        return pointer;
    };

    CseData data = {};
    data.ir = ir;
    data.cfg = cfg;
    data.regCount = regCount;
    data.straightLine = straightLine;
    data.values = (u32*)allocate(regCount * sizeof(u32));
    data.versions = (u32*)allocate(regCount * sizeof(u32));
    data.definitionCounts = (u8*)allocate(regCount);
    data.table = (CseEntry*)allocate(tableSize * sizeof(CseEntry));
    data.undo = (u32*)allocate(ir->instructionCount * sizeof(u32));
    u32* stack = (u32*)allocate(blockCount * sizeof(u32));
    u32* childCursor = (u32*)allocate(blockCount * sizeof(u32));
    u32* undoMarks = (u32*)allocate(blockCount * sizeof(u32));
    if (!data.values || !data.versions || !data.definitionCounts || !data.table || !data.undo ||
        (cfg && (!stack || !childCursor || !undoMarks))) {
        return;
    }
    data.tableMask = tableSize - 1;
    data.nextValue = 1;

    for (u32 i = 0; i < ir->instructionCount; i++) {
        u16 dest = ir->destinations[i];
        if (DefinesRegister(ir, i) && dest < regCount && data.definitionCounts[dest] < 2) {
            data.definitionCounts[dest]++;
        }
    }
    for (u32 p = 0; p < ir->phiCount; p++) {
        u16 result = ir->phiResultRegs[p];
        if (result < regCount && data.definitionCounts[result] < 2) data.definitionCounts[result]++;
    }

    if (straightLine) {
        for (u32 i = 0; i < ir->instructionCount; i++) VisitInstruction(&data, i);
        return;
    }

    // Down the dominator tree: what a block computes is available in the
    // blocks it dominates, and forgotten on the way back up.
    u32 top = 0;
    stack[top++] = cfg->entryBlock;
    undoMarks[cfg->entryBlock] = 0;
    VisitBlock(&data, cfg->entryBlock);
    while (top > 0) {
        u32 block = stack[top - 1];
        u32 child = cfg->domChildOffsets[block] + childCursor[block];
        if (child < cfg->domChildOffsets[block + 1]) {
            childCursor[block]++;
            u32 next = cfg->domChildData[child];
            if (next >= cfg->blockCount || next == block) continue;
            undoMarks[next] = data.undoCount;
            stack[top++] = next;
            VisitBlock(&data, next);
            continue;
        }
        while (data.undoCount > undoMarks[block]) data.table[data.undo[--data.undoCount]].used = false;
        top--;
    }
}

}  // namespace Optimization
}  // namespace BWSL
