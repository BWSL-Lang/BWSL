#pragma once

#include "phases/control_flow/bwsl_cfg.h"
#include "phases/ir_generation/bwsl_ir_gen.h"

namespace BWSL {
namespace Optimization {

// Adds instructions to a program after lowering, CFG construction and SSA.
// A pass queues them while it works, since instruction indices must stay
// put until it's done, and applies them once at the end. Each goes in
// front of an existing instruction, in the same block; several in front of
// the same one keep the order they were queued in. A jump or branch to that
// instruction then lands on the first one inserted before it.

struct InsertedInstruction {
    u32 before;       // The existing instruction it goes in front of
    bool after;       // Goes after instruction before - 1 instead: a jump to `before` skips it
    u16 opcode;
    u16 dest;
    u16 operands[4];
    u32 metadata;
};

struct InstructionInsertions {
    InsertedInstruction* items;
    u32 count;
    u32 capacity;
    u32 nextRegister;   // Spare registers AllocateRegister hands out: [nextRegister, registerEnd)
    u32 registerEnd;
};

// The register count AllocateRegister can grow `ir` to: a pass that must
// not run out halfway checks what it needs against this.
u32 GetRegisterCeiling(const IR::IRProgram* ir);

// A new register of `type`, or 0xFFFF past the register limit. Registers are
// added in batches, like SSA's: the program's registerCount includes spare
// ones of type VOID. Like SSA, it grows the metadata the backends read
// (types, struct types, storage info, names), not registerLifetimes or
// registerSpillSlots, which nothing reads after lowering.
u16 AllocateRegister(IR::IRBuilder* builder, InstructionInsertions* insertions, CoreType type);

// Queues `opcode dest <- a, b` in front of instruction `before`. Queue in
// ascending `before` order.
void QueueInstruction(IR::IRBuilder* builder, InstructionInsertions* insertions, u32 before,
                      u16 opcode, u16 dest, u16 a, u16 b = 0xFFFF);

// The same with all four operands and the instruction's metadata, such as
// the struct type a struct instruction builds.
void QueueInstruction(IR::IRBuilder* builder, InstructionInsertions* insertions, u32 before,
                      u16 opcode, u16 dest, const u16 operands[4], u32 metadata);

// Queues an instruction right after instruction `after`, in its block: a
// jump to the next instruction doesn't run it. Instructions queued after
// one instruction come before those queued in front of the next, so queue
// them first. After the last instruction, they go at the end.
void QueueInstructionAfter(IR::IRBuilder* builder, InstructionInsertions* insertions, u32 after,
                           u16 opcode, u16 dest, const u16 operands[4], u32 metadata);

// Inserts the queued instructions and updates every instruction index the
// program and `cfg` (null for straight-line stages) hold: branch, jump and
// switch targets, structured control flow annotations, block bounds and
// the instruction-to-block map.
void ApplyInsertions(IR::IRBuilder* builder, CFG* cfg, InstructionInsertions* insertions);

}  // namespace Optimization
}  // namespace BWSL
