#include "phases/control_flow/bwsl_quad_divergence.h"
#include "phases/ir_generation/bwsl_ir_analysis.h"
#include <cstring>

namespace BWSL {

namespace {

constexpr u32 NONE = 0xFFFFFFFFu;
constexpr u32 UNKNOWN_ROOT = 0xFFFFFFFEu;
// A value's mask has a bit per first-level struct field that may differ
// within a quad (fields 31 and up share the top bit); any other value is
// either 0 or ALL_FIELDS.
constexpr u32 ALL_FIELDS = 0xFFFFFFFFu;

u32 FieldBit(u32 field) { return field < 31 ? (1u << field) : (1u << 31); }

struct DivergenceState {
    const IR::IRProgram* ir = nullptr;
    const CFG* cfg = nullptr;
    u32 registerCount = 0;
    u32 blockCount = 0;
    u32* masks = nullptr;             // [registerCount]
    u8* definitionCounts = nullptr;   // [registerCount], saturating
    u8* isPointer = nullptr;          // [registerCount] local pointers
    u32* pointerRoots = nullptr;      // [registerCount] variable a pointer points into
    u32* pointerFields = nullptr;     // [registerCount] fields of it, ALL_FIELDS for all
    u32 outputMasks[32] = {};         // Fragment outputs read back with LOAD_OUTPUT
    bool unknownMemoryVarying = false;
    u32* postorder = nullptr;         // [blockCount + 1] postorder index on the reverse CFG
    u32* reverseOrder = nullptr;      // [blockCount + 1] nodes by postorder index
    u8* visited = nullptr;            // [blockCount + 1] DFS marks
    u32* dfsStack = nullptr;          // [blockCount + 1]
    u32* dfsNext = nullptr;           // [blockCount + 1] next edge per stack entry
    u32* ipdom = nullptr;             // [blockCount + 1]; blockCount is the virtual exit
    u8* divergentBranch = nullptr;    // [blockCount]
    u32* controllingBranch = nullptr; // [blockCount] instruction, NONE when not divergent
    u8* forcedPhis = nullptr;         // [phiCount] phis that choose by a divergent branch
    u32* regionStamp = nullptr;       // [blockCount]
    u32* worklist = nullptr;          // [blockCount + 1]
};

template <typename T>
T* AllocateZeroed(BWSL_Arena* arena, u32 count) {
    void* memory = arena->Allocate(static_cast<size_t>(count == 0 ? 1 : count) * sizeof(T), alignof(T));
    if (memory) {
        std::memset(memory, 0, static_cast<size_t>(count == 0 ? 1 : count) * sizeof(T));
    }
    return static_cast<T*>(memory);
}

bool IsStoreWithoutResult(u16 opcode) {
    switch (opcode) {
        case IR::OP_NOP: case IR::OP_JUMP: case IR::OP_BRANCH: case IR::OP_SWITCH:
        case IR::OP_RET: case IR::OP_DISCARD: case IR::OP_BARRIER: case IR::OP_MEM_FENCE:
        case IR::OP_STORE_OUTPUT: case IR::OP_STORE_BUFFER: case IR::OP_STORE_SHARED:
        case IR::OP_STORE_LOCAL: case IR::OP_LOCAL_STORE: case IR::OP_STRUCT_STORE:
        case IR::OP_IMG_STORE:
            return true;
        default:
            return false;
    }
}

// Operands that are binding slots, interface slots, components or field
// indices rather than registers, though they are encoded like registers.
bool IsLiteralSlot(u16 opcode, u32 slot) {
    switch (opcode) {
        case IR::OP_LOAD_INPUT: case IR::OP_LOAD_ATTR: case IR::OP_LOAD_UNIFORM:
        case IR::OP_LOAD_OUTPUT: case IR::OP_STORE_OUTPUT: case IR::OP_STORAGE_PTR:
            return slot == 0;
        case IR::OP_VEC_INSERT: case IR::OP_VEC_EXTRACT: case IR::OP_ENUM_FIELD:
        case IR::OP_STRUCT_INSERT: case IR::OP_STRUCT_EXTRACT:
        case IR::OP_STRUCT_ARRAY_EXTRACT: case IR::OP_STRUCT_ARRAY_INSERT:
        case IR::OP_LOCAL_FIELD_PTR: case IR::OP_STORAGE_FIELD:
            return slot == 1;
        default:
            return false;
    }
}

bool IsRegisterOperand(const DivergenceState& state, u16 operand) {
    return IR::IsRegister(operand) && operand < state.registerCount;
}

u16 Operand(const DivergenceState& state, u32 instruction, u32 slot) {
    return state.ir->operands[instruction * 4 + slot];
}

u32 OperandMask(const DivergenceState& state, u16 operand) {
    return IsRegisterOperand(state, operand) ? state.masks[operand] : 0;
}

u32 OperandMask(const DivergenceState& state, u32 instruction, u32 slot) {
    return OperandMask(state, Operand(state, instruction, slot));
}

u32 Spread(u32 mask) { return mask != 0 ? ALL_FIELDS : 0; }

// Whether any register operand of the instruction may differ within a quad.
bool HasVaryingOperand(const DivergenceState& state, u32 instruction) {
    const u16 opcode = state.ir->opcodes[instruction];
    for (u32 slot = 0; slot < 4; ++slot) {
        if (!IsLiteralSlot(opcode, slot) && OperandMask(state, instruction, slot) != 0) {
            return true;
        }
    }
    return false;
}

bool Grow(u32& target, u32 bits) {
    if ((target | bits) == target) {
        return false;
    }
    target |= bits;
    return true;
}

bool IsFlatInput(const DivergenceState& state, u32 slot, u16 destination) {
    if (slot < OutputSlot::VARYING0 || slot >= OutputSlot::VARYING0 + 16) {
        return false;
    }
    if (state.ir->inputInterpolations[slot] == static_cast<u8>(InterpolationMode::Flat)) {
        return true;
    }
    // Integer varyings are always flat.
    if (destination < state.registerCount) {
        const CoreType scalar = CoreTypeScalarType(static_cast<CoreType>(state.ir->registerTypes[destination]));
        return scalar == CoreType::INT || scalar == CoreType::UINT;
    }
    return false;
}

// Finds the local pointers and what each points into: a variable register
// and, for a field pointer, its first-level field.
void ResolvePointers(DivergenceState& state) {
    const IR::IRProgram* ir = state.ir;
    for (bool changed = true; changed;) {
        changed = false;
        for (u32 i = 0; i < ir->instructionCount; ++i) {
            const u16 opcode = ir->opcodes[i];
            const u16 destination = ir->destinations[i];
            if (!IsRegisterOperand(state, destination) || state.isPointer[destination]) {
                continue;
            }
            const u16 source = Operand(state, i, 0);
            const bool pointer = opcode == IR::OP_LOCAL_VAR_PTR || opcode == IR::OP_LOCAL_FIELD_PTR ||
                                 opcode == IR::OP_LOCAL_INDEX_PTR ||
                                 ((opcode == IR::OP_LOAD_REG || opcode == IR::OP_STORE_REG) &&
                                  IsRegisterOperand(state, source) && state.isPointer[source]);
            if (pointer) {
                state.isPointer[destination] = 1;
                changed = true;
            }
        }
        for (u32 phi = 0; phi < ir->phiCount; ++phi) {
            const u16 result = ir->phiResultRegs[phi];
            if (!IsRegisterOperand(state, result) || state.isPointer[result]) {
                continue;
            }
            for (u32 k = ir->phiOperandOffsets[phi]; k < ir->phiOperandOffsets[phi + 1]; ++k) {
                const u16 value = ir->phiOperandValues[k];
                if (IsRegisterOperand(state, value) && state.isPointer[value]) {
                    state.isPointer[result] = 1;
                    changed = true;
                    break;
                }
            }
        }
    }

    const auto assign = [&](u16 destination, u32 root, u32 fields) {
        if (state.pointerRoots[destination] == NONE) {
            state.pointerRoots[destination] = root;
            state.pointerFields[destination] = fields;
            return true;
        }
        if (state.pointerRoots[destination] != UNKNOWN_ROOT &&
            (state.pointerRoots[destination] != root || state.pointerFields[destination] != fields)) {
            state.pointerRoots[destination] = UNKNOWN_ROOT;
            return true;
        }
        return false;
    };
    const auto copy = [&](u16 destination, u16 source) {
        if (!IsRegisterOperand(state, source) || state.pointerRoots[source] == NONE) {
            return false;
        }
        return assign(destination, state.pointerRoots[source], state.pointerFields[source]);
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (u32 i = 0; i < ir->instructionCount; ++i) {
            const u16 destination = ir->destinations[i];
            if (!IsRegisterOperand(state, destination) || !state.isPointer[destination]) {
                continue;
            }
            const u16 opcode = ir->opcodes[i];
            const u16 base = Operand(state, i, 0);
            if (opcode == IR::OP_LOCAL_VAR_PTR) {
                changed |= IsRegisterOperand(state, base) ? assign(destination, base, ALL_FIELDS)
                                                          : assign(destination, UNKNOWN_ROOT, ALL_FIELDS);
            } else if (opcode == IR::OP_LOCAL_FIELD_PTR) {
                const u32 field = FieldBit(Operand(state, i, 1));
                if (!IsRegisterOperand(state, base)) {
                    changed |= assign(destination, UNKNOWN_ROOT, ALL_FIELDS);
                } else if (!state.isPointer[base]) {
                    changed |= assign(destination, base, field);
                } else if (state.pointerRoots[base] != NONE) {
                    // Deeper fields share their first-level field's bit.
                    const u32 fields = state.pointerFields[base];
                    changed |= assign(destination, state.pointerRoots[base], fields == ALL_FIELDS ? field : fields);
                }
            } else {
                changed |= copy(destination, base);
            }
        }
        for (u32 phi = 0; phi < ir->phiCount; ++phi) {
            const u16 result = ir->phiResultRegs[phi];
            if (!IsRegisterOperand(state, result) || !state.isPointer[result]) {
                continue;
            }
            for (u32 k = ir->phiOperandOffsets[phi]; k < ir->phiOperandOffsets[phi + 1]; ++k) {
                changed |= copy(result, ir->phiOperandValues[k]);
            }
        }
    }
}

// The varying fields a load through `pointer` reads.
u32 LoadMask(const DivergenceState& state, u16 pointer) {
    if (!IsRegisterOperand(state, pointer)) {
        return ALL_FIELDS;
    }
    if (state.masks[pointer] != 0 || state.unknownMemoryVarying) {
        return ALL_FIELDS;  // The address itself may differ within a quad.
    }
    const u32 root = state.pointerRoots[pointer];
    if (root == NONE || root == UNKNOWN_ROOT) {
        return ALL_FIELDS;
    }
    const u32 fields = state.pointerFields[pointer];
    return fields == ALL_FIELDS ? state.masks[root] : Spread(state.masks[root] & fields);
}

// Records a store of `valueMask` through `pointer`.
bool Store(DivergenceState& state, u16 pointer, u32 valueMask, bool divergentBlock) {
    const u32 root = IsRegisterOperand(state, pointer) ? state.pointerRoots[pointer] : UNKNOWN_ROOT;
    const bool varyingAddress = IsRegisterOperand(state, pointer) && state.masks[pointer] != 0;
    if (root == NONE || root == UNKNOWN_ROOT) {
        if ((valueMask != 0 || varyingAddress || divergentBlock) && !state.unknownMemoryVarying) {
            state.unknownMemoryVarying = true;
            return true;
        }
        return false;
    }
    const u32 fields = state.pointerFields[pointer];
    u32 bits = 0;
    if (fields == ALL_FIELDS) {
        bits = varyingAddress || divergentBlock ? ALL_FIELDS : valueMask;
    } else if (valueMask != 0 || varyingAddress || divergentBlock) {
        bits = fields;
    }
    return Grow(state.masks[root], bits);
}

// The varying fields of an instruction's result, and its stores' effects.
bool VisitInstruction(DivergenceState& state, u32 i, bool divergentBlock) {
    const IR::IRProgram* ir = state.ir;
    const u16 opcode = ir->opcodes[i];
    const u16 destination = ir->destinations[i];
    switch (opcode) {
        case IR::OP_LOCAL_STORE: case IR::OP_STRUCT_STORE:
            return Store(state, Operand(state, i, 0), OperandMask(state, i, 1), divergentBlock);
        case IR::OP_STORE_OUTPUT: {
            const u32 slot = Operand(state, i, 0);
            const u32 mask = divergentBlock ? ALL_FIELDS : OperandMask(state, destination);
            return slot < 32 && Grow(state.outputMasks[slot], Spread(mask));
        }
        default:
            break;
    }
    if (IsStoreWithoutResult(opcode) || !IsRegisterOperand(state, destination)) {
        return false;
    }

    u32 mask = 0;
    switch (opcode) {
        case IR::OP_LOAD_INPUT: {
            const u32 slot = Operand(state, i, 0);
            mask = IsFlatInput(state, slot, destination) ? 0 : ALL_FIELDS;
            break;
        }
        case IR::OP_LOAD_OUTPUT: {
            const u32 slot = Operand(state, i, 0);
            mask = slot < 32 ? state.outputMasks[slot] : ALL_FIELDS;
            break;
        }
        case IR::OP_LOAD_ATTR:
        case IR::OP_ATOMIC_ADD: case IR::OP_ATOMIC_SUB: case IR::OP_ATOMIC_MIN:
        case IR::OP_ATOMIC_MAX: case IR::OP_ATOMIC_AND: case IR::OP_ATOMIC_OR:
        case IR::OP_ATOMIC_XOR: case IR::OP_ATOMIC_XCHG: case IR::OP_ATOMIC_CMP_XCHG:
            mask = ALL_FIELDS;
            break;
        case IR::OP_LOCAL_LOAD: case IR::OP_STRUCT_LOAD:
            mask = LoadMask(state, Operand(state, i, 0));
            break;
        case IR::OP_LOCAL_VAR_PTR:
            mask = 0;
            break;
        case IR::OP_LOCAL_FIELD_PTR: {
            // A field pointer's own mask says whether its address may differ.
            const u16 base = Operand(state, i, 0);
            mask = IsRegisterOperand(state, base) && state.isPointer[base] ? state.masks[base] : 0;
            break;
        }
        case IR::OP_LOAD_REG: case IR::OP_STORE_REG:
            mask = OperandMask(state, i, 0);
            break;
        case IR::OP_SELECT:
            mask = OperandMask(state, i, 0) != 0 ? ALL_FIELDS
                                                 : OperandMask(state, i, 1) | OperandMask(state, i, 2);
            break;
        case IR::OP_STRUCT_CONSTRUCT:
            for (u32 slot = 0; slot < 4; ++slot) {
                if (OperandMask(state, i, slot) != 0) {
                    mask |= FieldBit(slot);
                }
            }
            break;
        case IR::OP_STRUCT_EXTRACT:
            mask = Spread(OperandMask(state, i, 0) & FieldBit(Operand(state, i, 1)));
            break;
        case IR::OP_STRUCT_INSERT: {
            const u32 field = FieldBit(Operand(state, i, 1));
            const u32 kept = field == (1u << 31) ? OperandMask(state, i, 0) : OperandMask(state, i, 0) & ~field;
            mask = kept | (OperandMask(state, i, 2) != 0 ? field : 0);
            break;
        }
        case IR::OP_STRUCT_ARRAY_EXTRACT:
            mask = Spread((OperandMask(state, i, 0) & FieldBit(Operand(state, i, 1))) | OperandMask(state, i, 2));
            break;
        case IR::OP_STRUCT_ARRAY_INSERT:
            mask = OperandMask(state, i, 0) |
                   ((OperandMask(state, i, 2) | OperandMask(state, i, 3)) != 0 ? FieldBit(Operand(state, i, 1)) : 0);
            break;
        case IR::OP_ARRAY_STORE:
            // The destination is the array; operands are the index and value.
            mask = divergentBlock ? ALL_FIELDS : Spread(OperandMask(state, i, 0) | OperandMask(state, i, 1));
            break;
        default:
            mask = HasVaryingOperand(state, i) ? ALL_FIELDS : 0;
            break;
    }
    // A register assigned in more than one place holds what the path taken
    // last wrote, so an assignment under divergent control flow varies.
    if (divergentBlock && state.definitionCounts[destination] > 1) {
        mask = ALL_FIELDS;
    }
    return Grow(state.masks[destination], mask);
}

bool VisitPhis(DivergenceState& state) {
    const IR::IRProgram* ir = state.ir;
    bool changed = false;
    for (u32 phi = 0; phi < ir->phiCount; ++phi) {
        const u16 result = ir->phiResultRegs[phi];
        if (!IsRegisterOperand(state, result)) {
            continue;
        }
        u32 mask = state.forcedPhis[phi] ? ALL_FIELDS : 0;
        for (u32 k = ir->phiOperandOffsets[phi]; k < ir->phiOperandOffsets[phi + 1]; ++k) {
            mask |= OperandMask(state, ir->phiOperandValues[k]);
        }
        changed |= Grow(state.masks[result], mask);
    }
    return changed;
}

bool IsExitBlock(const DivergenceState& state, u32 block) {
    if (state.cfg->TotalSuccessorCount(block) != 0) {
        return false;
    }
    // A discarded pixel stays a helper for its quad's derivatives, so its
    // path rejoins the others as far as divergence goes: leave it out.
    return state.ir->opcodes[state.cfg->lastInst[block]] != IR::OP_DISCARD;
}

u32 IntersectPostDominators(const DivergenceState& state, u32 a, u32 b) {
    while (a != b) {
        while (state.postorder[a] < state.postorder[b]) a = state.ipdom[a];
        while (state.postorder[b] < state.postorder[a]) b = state.ipdom[b];
    }
    return a;
}

// Immediate post-dominators (Cooper, Harvey and Kennedy) on the reverse CFG,
// from a virtual exit joining the blocks that return. Blocks that can't
// reach it (discards, endless loops) keep NONE and don't constrain others.
void ComputePostDominators(DivergenceState& state) {
    const CFG* cfg = state.cfg;
    const u32 exit = state.blockCount;
    const u32 nodeCount = state.blockCount + 1;
    for (u32 node = 0; node < nodeCount; ++node) {
        state.postorder[node] = NONE;
        state.ipdom[node] = NONE;
        state.visited[node] = 0;
    }
    // Reverse edges lead from the exit to the returning blocks and from a
    // block to its CFG predecessors.
    const auto reverseEdgeCount = [&](u32 node) -> u32 {
        return node == exit ? state.blockCount : cfg->PredecessorCount(node);
    };
    const auto reverseEdge = [&](u32 node, u32 index) -> u32 {
        if (node == exit) {
            return IsExitBlock(state, index) ? index : NONE;
        }
        return cfg->GetPredecessor(node, index);
    };
    u32 depth = 0;
    u32 order = 0;
    state.dfsStack[depth] = exit;
    state.dfsNext[depth] = 0;
    state.visited[exit] = 1;
    ++depth;
    while (depth != 0) {
        const u32 node = state.dfsStack[depth - 1];
        if (state.dfsNext[depth - 1] < reverseEdgeCount(node)) {
            const u32 next = reverseEdge(node, state.dfsNext[depth - 1]++);
            if (next < state.blockCount && !state.visited[next]) {
                state.visited[next] = 1;
                state.dfsStack[depth] = next;
                state.dfsNext[depth] = 0;
                ++depth;
            }
            continue;
        }
        state.postorder[node] = order;
        state.reverseOrder[order] = node;
        ++order;
        --depth;
    }

    state.ipdom[exit] = exit;
    for (bool changed = true; changed;) {
        changed = false;
        // Reverse postorder, skipping the exit (last in postorder).
        for (u32 index = order - 1; index-- > 0;) {
            const u32 block = state.reverseOrder[index];
            u32 candidate = NONE;
            const auto consider = [&](u32 successor) {
                if (successor == NONE || state.ipdom[successor] == NONE) {
                    return;
                }
                candidate = candidate == NONE ? successor : IntersectPostDominators(state, successor, candidate);
            };
            if (IsExitBlock(state, block)) {
                consider(exit);
            }
            for (u32 s = 0; s < cfg->TotalSuccessorCount(block); ++s) {
                const u32 successor = cfg->GetAnySuccessor(block, s);
                consider(successor < state.blockCount ? successor : NONE);
            }
            if (candidate != state.ipdom[block]) {
                state.ipdom[block] = candidate;
                changed = true;
            }
        }
    }
}

// Marks the blocks each divergent branch controls: those reachable from it
// before its immediate post-dominator. A phi outside that region with a
// predecessor inside it picks its value by which way the branch went.
void ComputeDivergentRegions(DivergenceState& state) {
    const CFG* cfg = state.cfg;
    const IR::IRProgram* ir = state.ir;
    for (u32 block = 0; block < state.blockCount; ++block) {
        state.controllingBranch[block] = NONE;
        state.regionStamp[block] = NONE;
    }
    for (u32 branch = 0; branch < state.blockCount; ++branch) {
        if (!state.divergentBranch[branch]) {
            continue;
        }
        const u32 stop = state.ipdom[branch];
        u32 count = 0;
        for (u32 s = 0; s < cfg->TotalSuccessorCount(branch); ++s) {
            const u32 successor = cfg->GetAnySuccessor(branch, s);
            if (successor < state.blockCount && successor != stop && state.regionStamp[successor] != branch) {
                state.regionStamp[successor] = branch;
                state.worklist[count++] = successor;
            }
        }
        while (count != 0) {
            const u32 block = state.worklist[--count];
            if (state.controllingBranch[block] == NONE) {
                state.controllingBranch[block] = cfg->lastInst[branch];
            }
            for (u32 s = 0; s < cfg->TotalSuccessorCount(block); ++s) {
                const u32 successor = cfg->GetAnySuccessor(block, s);
                if (successor < state.blockCount && successor != stop && state.regionStamp[successor] != branch) {
                    state.regionStamp[successor] = branch;
                    state.worklist[count++] = successor;
                }
            }
        }
        for (u32 phi = 0; phi < ir->phiCount; ++phi) {
            const u32 block = ir->phiBlockIndices[phi];
            if (block >= state.blockCount || state.regionStamp[block] == branch) {
                continue;
            }
            for (u32 k = ir->phiOperandOffsets[phi]; k < ir->phiOperandOffsets[phi + 1]; ++k) {
                const u32 predecessor = ir->phiOperandBlocks[k];
                if (predecessor == branch ||
                    (predecessor < state.blockCount && state.regionStamp[predecessor] == branch)) {
                    state.forcedPhis[phi] = 1;
                    break;
                }
            }
        }
    }
}

bool MarkDivergentBranches(DivergenceState& state) {
    bool changed = false;
    for (u32 block = 0; block < state.blockCount; ++block) {
        const u32 last = state.cfg->lastInst[block];
        const u16 opcode = state.ir->opcodes[last];
        if (state.divergentBranch[block] || (opcode != IR::OP_BRANCH && opcode != IR::OP_SWITCH) ||
            state.cfg->TotalSuccessorCount(block) < 2) {
            continue;
        }
        if (OperandMask(state, last, 0) != 0) {
            state.divergentBranch[block] = 1;
            changed = true;
        }
    }
    return changed;
}

} // namespace

bool QuadDivergence::IsImplicitDerivative(u16 opcode) {
    return (opcode >= IR::OP_DDX && opcode <= IR::OP_FWIDTH_COARSE) ||
           opcode == IR::OP_TEX_SAMPLE || opcode == IR::OP_TEX_SAMPLE_BIAS ||
           opcode == IR::OP_TEX_SAMPLE_CMP || opcode == IR::OP_TEX_SAMPLE_OFFSET ||
           opcode == IR::OP_TEX_SAMPLE_BIAS_OFFSET;
}

const char* QuadDivergence::IntrinsicName(u16 opcode) {
    switch (opcode) {
        case IR::OP_DDX: return "ddx";
        case IR::OP_DDY: return "ddy";
        case IR::OP_DDX_FINE: return "ddx_fine";
        case IR::OP_DDY_FINE: return "ddy_fine";
        case IR::OP_DDX_COARSE: return "ddx_coarse";
        case IR::OP_DDY_COARSE: return "ddy_coarse";
        case IR::OP_FWIDTH: return "fwidth";
        case IR::OP_FWIDTH_FINE: return "fwidth_fine";
        case IR::OP_FWIDTH_COARSE: return "fwidth_coarse";
        case IR::OP_TEX_SAMPLE: return "sample";
        case IR::OP_TEX_SAMPLE_BIAS: return "sample_bias";
        case IR::OP_TEX_SAMPLE_CMP: return "sample_cmp";
        case IR::OP_TEX_SAMPLE_OFFSET: return "sample_offset";
        case IR::OP_TEX_SAMPLE_BIAS_OFFSET: return "sample_bias_offset";
        default: return "derivative";
    }
}

bool QuadDivergence::FindDivergentDerivatives(QuadDivergenceData* result, const IR::IRProgram* ir,
                                              const CFG* cfg, BWSL_Arena* arena) {
    *result = QuadDivergenceData{};
    if (ir == nullptr || cfg == nullptr || cfg->blockCount < 2) {
        return true;
    }
    bool hasDerivative = false;
    for (u32 i = 0; i < ir->instructionCount && !hasDerivative; ++i) {
        hasDerivative = IsImplicitDerivative(ir->opcodes[i]);
    }
    if (!hasDerivative) {
        return true;
    }

    DivergenceState state;
    state.ir = ir;
    state.cfg = cfg;
    state.registerCount = ir->registerCount < IR::RegisterLimit ? ir->registerCount : IR::RegisterLimit;
    state.blockCount = cfg->blockCount;
    const u32 nodeCount = state.blockCount + 1;
    state.masks = AllocateZeroed<u32>(arena, state.registerCount);
    state.definitionCounts = AllocateZeroed<u8>(arena, state.registerCount);
    state.isPointer = AllocateZeroed<u8>(arena, state.registerCount);
    state.pointerRoots = AllocateZeroed<u32>(arena, state.registerCount);
    state.pointerFields = AllocateZeroed<u32>(arena, state.registerCount);
    state.postorder = AllocateZeroed<u32>(arena, nodeCount);
    state.reverseOrder = AllocateZeroed<u32>(arena, nodeCount);
    state.ipdom = AllocateZeroed<u32>(arena, nodeCount);
    state.visited = AllocateZeroed<u8>(arena, nodeCount);
    state.dfsStack = AllocateZeroed<u32>(arena, nodeCount);
    state.dfsNext = AllocateZeroed<u32>(arena, nodeCount);
    state.divergentBranch = AllocateZeroed<u8>(arena, state.blockCount);
    state.controllingBranch = AllocateZeroed<u32>(arena, state.blockCount);
    state.forcedPhis = AllocateZeroed<u8>(arena, ir->phiCount);
    state.regionStamp = AllocateZeroed<u32>(arena, state.blockCount);
    state.worklist = AllocateZeroed<u32>(arena, nodeCount);
    if (!state.masks || !state.definitionCounts || !state.isPointer || !state.pointerRoots ||
        !state.pointerFields || !state.postorder || !state.reverseOrder || !state.ipdom ||
        !state.visited || !state.dfsStack || !state.dfsNext || !state.divergentBranch ||
        !state.controllingBranch || !state.forcedPhis || !state.regionStamp || !state.worklist) {
        return false;
    }
    for (u32 reg = 0; reg < state.registerCount; ++reg) {
        state.pointerRoots[reg] = NONE;
    }
    for (u32 block = 0; block < state.blockCount; ++block) {
        state.controllingBranch[block] = NONE;
    }
    for (u32 i = 0; i < ir->instructionCount; ++i) {
        const u16 destination = ir->destinations[i];
        if (!IsStoreWithoutResult(ir->opcodes[i]) && IsRegisterOperand(state, destination) &&
            state.definitionCounts[destination] < 255) {
            ++state.definitionCounts[destination];
        }
    }
    for (u32 phi = 0; phi < ir->phiCount; ++phi) {
        const u16 phiResult = ir->phiResultRegs[phi];
        if (IsRegisterOperand(state, phiResult) && state.definitionCounts[phiResult] < 255) {
            ++state.definitionCounts[phiResult];
        }
    }
    ResolvePointers(state);
    ComputePostDominators(state);

    // Values and branches feed each other: a varying value makes a branch
    // divergent, which makes the values it chooses between varying.
    for (bool changed = true; changed;) {
        changed = false;
        for (bool valuesChanged = true; valuesChanged;) {
            valuesChanged = false;
            for (u32 i = 0; i < ir->instructionCount; ++i) {
                const u32 block = cfg->instToBlock[i];
                const bool divergent = block < state.blockCount && state.controllingBranch[block] != NONE;
                valuesChanged |= VisitInstruction(state, i, divergent);
            }
            valuesChanged |= VisitPhis(state);
            changed |= valuesChanged;
        }
        if (MarkDivergentBranches(state)) {
            ComputeDivergentRegions(state);
            changed = true;
        } else {
            changed = false;
        }
    }

    u32 count = 0;
    for (u32 i = 0; i < ir->instructionCount; ++i) {
        const u32 block = cfg->instToBlock[i];
        if (IsImplicitDerivative(ir->opcodes[i]) && block < state.blockCount &&
            state.controllingBranch[block] != NONE) {
            ++count;
        }
    }
    if (count == 0) {
        return true;
    }
    u32* derivatives = AllocateZeroed<u32>(arena, count);
    u32* branches = AllocateZeroed<u32>(arena, count);
    if (!derivatives || !branches) {
        return false;
    }
    for (u32 i = 0; i < ir->instructionCount; ++i) {
        const u32 block = cfg->instToBlock[i];
        if (IsImplicitDerivative(ir->opcodes[i]) && block < state.blockCount &&
            state.controllingBranch[block] != NONE) {
            derivatives[result->count] = i;
            branches[result->count] = state.controllingBranch[block];
            ++result->count;
        }
    }
    result->derivatives = derivatives;
    result->branches = branches;
    return true;
}

} // namespace BWSL
