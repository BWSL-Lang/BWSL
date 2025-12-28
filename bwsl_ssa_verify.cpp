#include "bwsl_ssa.h"
#include <cstdio>

namespace BWSL {
namespace SSA {

// Helper to determine if an opcode defines a register or just uses its destination slot
// (e.g., Stores use dest as an address/target, not a definition)
static bool IsDefinitionOp(u16 op) {
    using namespace IR;
    switch (op) {
        // These ops use 'dest' as a value sink or address, not a register definition
        case OP_STORE_REG:
        case OP_STORE_OUTPUT:
        case OP_STORE_BUFFER:
        case OP_STORE_LOCAL:
        case OP_STORE_SHARED:
        case OP_IMG_STORE:
        case OP_ARRAY_STORE:
        case OP_JUMP:
        case OP_BRANCH:
        case OP_RET:
        case OP_SWITCH:
        case OP_NOP:
        case OP_BARRIER:
        case OP_MEM_FENCE:
        case OP_DISCARD:
            return false;
        default:
            return true;
    }
}

bool VerifySSA(const IR::IRProgram* ir) {
    bool isValid = true;

    // Allocate tracking arrays
    int* defCount = new int[ir->registerCount]();
    int* firstDefInst = new int[ir->registerCount];
    for (u32 i = 0; i < ir->registerCount; i++) {
        firstDefInst[i] = -1;
    }

    // 1. Mark Undef Registers as defined
    // These represent uninitialized variables and are valid roots
    for (u32 i = 0; i < ir->undefRegCount; i++) {
        u16 reg = ir->undefRegs[i];
        if (reg < ir->registerCount) {
            defCount[reg]++;
            firstDefInst[reg] = -2; // Special marker for UNDEF
        }
    }

    // 2. Count PHI Definitions
    // PHI nodes define their result register
    for (u32 i = 0; i < ir->phiCount; i++) {
        u16 reg = ir->phiResultRegs[i];
        if (reg < ir->registerCount) {
            if (defCount[reg] > 0) {
                fprintf(stderr, "[SSA Error] Register R%u defined by PHI %u but already defined elsewhere.\n", reg, i);
                isValid = false;
            }
            defCount[reg]++;
            firstDefInst[reg] = -3; // Special marker for PHI
        }
    }

    // 3. Scan Instructions for Definitions
    for (u32 i = 0; i < ir->instructionCount; i++) {
        u16 op = ir->opcodes[i];

        if (IsDefinitionOp(op)) {
            u16 dest = ir->destinations[i];

            // Ignore constants and invalid registers
            if ((dest & 0xC000) == 0 && dest < ir->registerCount) {
                if (defCount[dest] > 0) {
                    fprintf(stderr, "[SSA Error] Register R%u defined at Inst %u (Op 0x%X), but already defined at ", dest, i, op);
                    if (firstDefInst[dest] == -2) fprintf(stderr, "UNDEF\n");
                    else if (firstDefInst[dest] == -3) fprintf(stderr, "PHI\n");
                    else fprintf(stderr, "Inst %d\n", firstDefInst[dest]);
                    isValid = false;
                }
                defCount[dest]++;
                firstDefInst[dest] = static_cast<int>(i);
            }
        }
    }

    // 4. Scan for Uses of Undefined Registers
    // This checks operands of Instructions AND PHIs

    // Check Instructions
    for (u32 i = 0; i < ir->instructionCount; i++) {
        u16 opCode = ir->opcodes[i];

        // Check all 4 operands
        for (u32 opIdx = 0; opIdx < 4; opIdx++) {
            // Skip non-register operands based on opcode
            // (VEC_INSERT/EXTRACT operand 1 is an immediate index)
            if ((opCode == IR::OP_VEC_INSERT || opCode == IR::OP_VEC_EXTRACT || opCode == IR::OP_ENUM_FIELD) && opIdx == 1) continue;

            u16 reg = ir->GetOperand(i, opIdx);

            // Check if it's a valid register reference
            if ((reg & 0xC000) == 0 && reg != 0xFFFF && reg != 0 && reg < ir->registerCount) {
                if (defCount[reg] == 0) {
                    fprintf(stderr, "[SSA Error] Inst %u (Op 0x%X) uses R%u which is never defined.\n", i, opCode, reg);
                    isValid = false;
                }
            }
        }
    }

    // Check PHI Operands
    for (u32 i = 0; i < ir->phiCount; i++) {
        u32 count = ir->GetPhiOperandCount(i);
        for (u32 j = 0; j < count; j++) {
            u16 reg = ir->GetPhiOperandValue(i, j);

            if ((reg & 0xC000) == 0 && reg != 0xFFFF && reg != 0 && reg < ir->registerCount) {
                if (defCount[reg] == 0) {
                    fprintf(stderr, "[SSA Error] PHI %u operand %u uses R%u which is never defined.\n", i, j, reg);
                    isValid = false;
                }
            }
        }
    }

    if (isValid) {
        fprintf(stderr, "[SSA] Verification Passed: All %u registers valid.\n", ir->registerCount);
    }

    delete[] defCount;
    delete[] firstDefInst;

    return isValid;
}

}  // namespace SSA
}  // namespace BWSL
