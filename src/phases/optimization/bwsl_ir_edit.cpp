#include "bwsl_ir_edit.h"
#include <algorithm>
#include <cstring>

namespace BWSL {
namespace Optimization {

using IR::IRProgram;

constexpr u32 REGISTER_BATCH = 64;

// A copy of `count` elements of `values` with room for `capacity`, the rest
// zeroed. Register metadata may come from an arena the pool doesn't own, so
// Reallocate can't copy it.
template <typename T>
static T* GrowArray(IRMemoryPool* pool, T* values, u32 count, u32 capacity) {
    if (!values) return nullptr;
    T* grown = (T*)pool->Allocate(capacity * sizeof(T), 64);
    memcpy(grown, values, count * sizeof(T));
    memset(grown + count, 0, (capacity - count) * sizeof(T));
    return grown;
}

// New registers are numbered after registerCount, past SSA's spare capacity,
// so for a large shader they reached the old 14-bit limit long before its
// registers did, and the rewrites asking for them were skipped. A shader whose
// registers fit below that limit keeps it, so its output doesn't change.
static u32 RegisterCeiling(const IRProgram* ir) {
    u32 used = ir->ssaRegisterEnd != 0 ? ir->ssaRegisterEnd : ir->registerCount;
    return used <= IR::LegacyRegisterLimit ? IR::LegacyRegisterLimit : IR::RegisterLimit;
}

u16 AllocateRegister(IR::IRBuilder* builder, InstructionInsertions* insertions, CoreType type) {
    IRProgram* ir = builder->program;
    if (insertions->nextRegister == insertions->registerEnd) {
        u32 count = ir->registerCount;
        u32 capacity = std::min<u32>(count + REGISTER_BATCH, RegisterCeiling(ir));
        if (capacity <= count) return 0xFFFF;
        ir->registerTypes = GrowArray(builder->pool, ir->registerTypes, count, capacity);
        ir->registerStructTypes = GrowArray(builder->pool, ir->registerStructTypes, count, capacity);
        ir->registerStorageInfo = GrowArray(builder->pool, ir->registerStorageInfo, count, capacity);
        ir->registerNameHashes = GrowArray(builder->pool, ir->registerNameHashes, count, capacity);
        ir->registerCount = capacity;
        insertions->nextRegister = count;
        insertions->registerEnd = capacity;
    }
    u16 reg = static_cast<u16>(insertions->nextRegister++);
    if (ir->registerTypes) ir->registerTypes[reg] = static_cast<u16>(type);
    return reg;
}

void QueueInstruction(IR::IRBuilder* builder, InstructionInsertions* insertions, u32 before,
                      u16 opcode, u16 dest, u16 a, u16 b) {
    if (insertions->count == insertions->capacity) {
        u32 capacity = std::max<u32>(insertions->capacity * 2, 16);
        InsertedInstruction* items =
            (InsertedInstruction*)builder->pool->Allocate(capacity * sizeof(InsertedInstruction), 64);
        if (insertions->count) memcpy(items, insertions->items, insertions->count * sizeof(InsertedInstruction));
        insertions->items = items;
        insertions->capacity = capacity;
    }
    insertions->items[insertions->count++] = {before, opcode, dest, {a, b, 0xFFFF, 0xFFFF}};
}

void ApplyInsertions(IR::IRBuilder* builder, CFG* cfg, InstructionInsertions* insertions) {
    IRProgram* ir = builder->program;
    u32 inserted = insertions->count;
    if (inserted == 0) return;
    u32 oldCount = ir->instructionCount;
    u32 newCount = oldCount + inserted;
    while (ir->instructionCapacity < newCount) builder->GrowInstructionArrays();

    // shift[i]: instructions inserted in front of instructions before i.
    // Instruction i moves to i + shift[i + 1]; a target i becomes
    // i + shift[i], the first instruction inserted in front of it.
    u32* shift = (u32*)builder->pool->Allocate((oldCount + 2) * sizeof(u32), 64);
    memset(shift, 0, (oldCount + 2) * sizeof(u32));
    for (u32 k = 0; k < inserted; k++) shift[std::min(insertions->items[k].before, oldCount) + 1]++;
    for (u32 i = 0; i <= oldCount; i++) shift[i + 1] += shift[i];
    auto target = [&](u32 index) { return index <= oldCount ? index + shift[index] : index; };
    auto moved = [&](u32 index) { return index < oldCount ? index + shift[index + 1] : index; };

    // Back to front, so nothing is overwritten before it moves.
    u32 next = inserted;
    for (u32 i = oldCount; i-- > 0;) {
        u32 to = moved(i);
        ir->opcodes[to] = ir->opcodes[i];
        ir->types[to] = ir->types[i];
        ir->flags[to] = ir->flags[i];
        ir->destinations[to] = ir->destinations[i];
        for (u32 slot = 0; slot < 4; slot++) ir->operands[to * 4 + slot] = ir->operands[i * 4 + slot];
        u32 metadata = ir->metadata[i];
        u32 trueTarget = ir->GetBranchTrueTarget(i), falseTarget = ir->GetBranchFalseTarget(i);
        u32 structure = ir->structureInfo[i];
        u32 continuation = ir->continueInfo[i];
        if (ir->opcodes[to] == IR::OP_JUMP) metadata = target(metadata);
        ir->metadata[to] = metadata;
        if (ir->branchTrueTargets) ir->branchTrueTargets[to] = target(trueTarget);
        if (ir->branchFalseTargets) ir->branchFalseTargets[to] = target(falseTarget);
        if (structure != 0) {
            u32 merge = target(structure & IRProgram::STRUCT_TARGET_MASK);
            structure = (structure & IRProgram::STRUCT_TYPE_MASK) | (merge & IRProgram::STRUCT_TARGET_MASK);
        }
        ir->structureInfo[to] = structure;
        ir->continueInfo[to] = target(continuation);
        if (ir->sourceNodes) ir->sourceNodes[to] = ir->sourceNodes[i];

        // The instructions queued in front of i, in queue order.
        u32 first = next;
        while (first > 0 && insertions->items[first - 1].before == i) first--;
        u32 position = target(i);
        for (u32 k = first; k < next; k++, position++) {
            const InsertedInstruction& item = insertions->items[k];
            ir->opcodes[position] = item.opcode;
            ir->types[position] = 0;
            ir->flags[position] = 0;
            ir->destinations[position] = item.dest;
            for (u32 slot = 0; slot < 4; slot++) ir->operands[position * 4 + slot] = item.operands[slot];
            ir->metadata[position] = 0;
            ir->ClearBranchTargets(position);
            ir->structureInfo[position] = 0;
            ir->continueInfo[position] = NO_BLOCK;
            if (ir->sourceNodes) ir->sourceNodes[position] = ir->sourceNodes[to];
        }
        next = first;
    }
    ir->instructionCount = newCount;

    for (u32 s = 0; s < ir->switchCount; s++) {
        ir->switchInstructionIndices[s] = moved(ir->switchInstructionIndices[s]);
        ir->switchDefaultTargets[s] = target(ir->switchDefaultTargets[s]);
        for (u32 c = ir->switchCaseOffsets[s]; c < ir->switchCaseOffsets[s + 1]; c++) {
            ir->switchCaseTargets[c] = target(ir->switchCaseTargets[c]);
        }
    }
    for (u32 v = 0; v < ir->variantBranchCount; v++) {
        ir->variantInstructionIndices[v] = moved(ir->variantInstructionIndices[v]);
        ir->variantTrueBranches[v] = target(ir->variantTrueBranches[v]);
        ir->variantFalseBranches[v] = target(ir->variantFalseBranches[v]);
    }
    // The call site and resource tables have no initializers and nothing
    // fills them, so they're left alone.

    if (!cfg) return;
    u32* instToBlock = (u32*)builder->pool->Allocate(newCount * sizeof(u32), 64);
    memset(instToBlock, 0xFF, newCount * sizeof(u32));
    for (u32 b = 0; b < cfg->blockCount; b++) {
        cfg->firstInst[b] = target(cfg->firstInst[b]);
        cfg->lastInst[b] = moved(cfg->lastInst[b]);
        for (u32 i = cfg->firstInst[b]; i <= cfg->lastInst[b] && i < newCount; i++) instToBlock[i] = b;
    }
    cfg->instToBlock = instToBlock;
}

}  // namespace Optimization
}  // namespace BWSL
