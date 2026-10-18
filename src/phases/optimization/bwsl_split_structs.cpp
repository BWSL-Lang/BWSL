#include "bwsl_split_structs.h"
#include "bwsl_ir_edit.h"
#include <algorithm>
#include <cstring>
#include <string>

namespace BWSL {
namespace Optimization {
namespace Split {

using IR::IRProgram;

constexpr u16 NO_REGISTER = 0xFFFF;
constexpr u32 MAX_FIELDS = 32;
constexpr u32 MAX_ROUNDS = 8;  // Struct fields nested this deep are split

// A candidate's state while a round checks its uses.
constexpr u8 NOT_SPLIT = 0;
constexpr u8 SPLIT = 1;

struct SplitData {
    IR::IRBuilder* builder;
    IRProgram* ir;
    u32 regCount;
    u8* state;                    // [regCount] SPLIT or NOT_SPLIT
    const IRProgram::StructTypeInfo** layouts;  // [regCount] a candidate's struct type
    u16* pointerStruct;           // [regCount] the struct a field pointer points into, or NO_REGISTER
    u8* pointerField;             // [regCount] its field
    u8* definitionCounts;         // [regCount], saturating
    u32* definedBy;               // [regCount] a register's last definition
    u32* fieldOffsets;            // [regCount] where a split struct's field registers start in fields
    u16* fields;
    u32 fieldCount;
    InstructionInsertions insertions;
};

static const IRProgram::StructTypeInfo* FindStructType(const IRProgram* ir, u32 hash) {
    for (u32 s = 0; hash && s < ir->structTypeCount; s++) {
        if (ir->structTypes[s].nameHash == hash) return &ir->structTypes[s];
    }
    return nullptr;
}

static bool IsRegister(const SplitData* data, u16 operand) {
    return operand != 0xFFFF && !IR::IsConstant(operand) && operand < data->regCount;
}

// Instructions whose destination slot isn't a register they define.
static bool Defines(u16 op) {
    switch (op) {
        case IR::OP_NOP: case IR::OP_JUMP: case IR::OP_BRANCH: case IR::OP_RET: case IR::OP_SWITCH:
        case IR::OP_DISCARD: case IR::OP_BARRIER: case IR::OP_MEM_FENCE:
        case IR::OP_LOCAL_STORE: case IR::OP_ARRAY_STORE: case IR::OP_STORE_OUTPUT: case IR::OP_STORE_BUFFER:
        case IR::OP_STORE_SHARED: case IR::OP_STORE_LOCAL: case IR::OP_IMG_STORE: case IR::OP_STRUCT_STORE:
            return false;
        default:
            return true;
    }
}

// Stores that carry the value they store in the destination slot.
static bool StoresDestination(u16 op) {
    switch (op) {
        case IR::OP_STORE_OUTPUT: case IR::OP_STORE_BUFFER: case IR::OP_STORE_SHARED: case IR::OP_STORE_LOCAL:
        case IR::OP_IMG_STORE: case IR::OP_STRUCT_STORE:
            return true;
        default:
            return false;
    }
}

// Operand slots that hold a literal: a field or component index, or an
// attribute, input, output or constant slot.
static bool IsLiteralSlot(u16 op, u32 slot) {
    switch (op) {
        case IR::OP_LOAD_ATTR: case IR::OP_LOAD_INPUT: case IR::OP_LOAD_CONST: case IR::OP_LOAD_OUTPUT:
        case IR::OP_STORE_OUTPUT: case IR::OP_JUMP: case IR::OP_NOP: case IR::OP_RET:
            return true;
        case IR::OP_VEC_INSERT: case IR::OP_VEC_EXTRACT: case IR::OP_ENUM_FIELD: case IR::OP_STRUCT_INSERT:
        case IR::OP_STRUCT_EXTRACT: case IR::OP_STRUCT_ARRAY_EXTRACT: case IR::OP_STRUCT_ARRAY_INSERT:
        case IR::OP_LOCAL_FIELD_PTR:
            return slot == 1;
        default:
            return false;
    }
}

// Instructions that may read a whole struct from a register. Any other
// instruction's operand that names a candidate's register is a literal
// (a binding, an index) that happens to have its number.
static bool MayReadStruct(u16 op) {
    switch (op) {
        case IR::OP_LOAD_REG: case IR::OP_STORE_REG: case IR::OP_SELECT: case IR::OP_CALL: case IR::OP_RET:
        case IR::OP_STRUCT_CONSTRUCT: case IR::OP_STRUCT_EXTRACT: case IR::OP_STRUCT_INSERT:
        case IR::OP_STRUCT_LOAD: case IR::OP_STRUCT_STORE: case IR::OP_STRUCT_GEP:
        case IR::OP_STRUCT_ARRAY_EXTRACT: case IR::OP_STRUCT_ARRAY_INSERT:
        case IR::OP_ARRAY_STORE: case IR::OP_STORE_BUFFER: case IR::OP_STORE_SHARED: case IR::OP_STORE_LOCAL:
        case IR::OP_LOCAL_STORE: case IR::OP_LOCAL_LOAD: case IR::OP_LOCAL_VAR_PTR: case IR::OP_LOCAL_FIELD_PTR:
        case IR::OP_LOCAL_INDEX_PTR: case IR::OP_ENUM_CONSTRUCT:
        case IR::OP_WAVE_READ_FIRST: case IR::OP_WAVE_READ_LANE:
            return true;
        default:
            return false;
    }
}

// Slots that read a whole struct this pass rebuilds from its fields.
static bool IsWholeRead(u16 op, u32 slot) {
    switch (op) {
        case IR::OP_STORE_REG: case IR::OP_LOAD_REG: return slot == 0;
        case IR::OP_SELECT: return slot == 1 || slot == 2;
        case IR::OP_STRUCT_INSERT: return slot == 0 || slot == 2;
        case IR::OP_STRUCT_CONSTRUCT: return true;
        case IR::OP_ARRAY_STORE: case IR::OP_LOCAL_STORE: return slot == 1;
        default: return false;
    }
}

static bool IsCandidate(const SplitData* data, u16 reg) {
    const IRProgram* ir = data->ir;
    if (static_cast<CoreType>(ir->registerTypes[reg]) != CoreType::CUSTOM) return false;
    if (!ir->registerNameHashes || ir->registerNameHashes[reg] == 0) return false;  // Only variables
    u32 storage = ir->registerStorageInfo ? ir->registerStorageInfo[reg] : 0;
    if ((storage & ~IRProgram::STORAGE_IS_ADDRESS_TAKEN) != 0) return false;
    const IRProgram::StructTypeInfo* layout = FindStructType(ir, ir->registerStructTypes[reg]);
    if (!layout || layout->fieldCount == 0 || layout->fieldCount > MAX_FIELDS) return false;
    for (u32 f = 0; f < layout->fieldCount; f++) {
        u32 field = layout->fieldOffset + f;
        if (ir->structFieldArraySizes && ir->structFieldArraySizes[field] != 0) return false;
        CoreType type = static_cast<CoreType>(ir->structFieldTypes[field]);
        if (type == CoreType::INVALID || type == CoreType::VOID) return false;
    }
    return true;
}

static CoreType FieldType(const IRProgram* ir, const IRProgram::StructTypeInfo* layout, u32 field) {
    return static_cast<CoreType>(ir->structFieldTypes[layout->fieldOffset + field]);
}

// Finds the variables to split this round: candidates all of whose uses
// this pass can rewrite. Returns whether there are any.
static bool ChooseStructs(SplitData* data) {
    IRProgram* ir = data->ir;
    memset(data->definitionCounts, 0, data->regCount);
    for (u32 i = 0; i < ir->instructionCount; i++) {
        u16 dest = ir->destinations[i];
        if (!Defines(ir->opcodes[i]) || !IsRegister(data, dest)) continue;
        if (data->definitionCounts[dest] < 255) data->definitionCounts[dest]++;
        data->definedBy[dest] = i;
    }
    // Enums are CUSTOM registers too, with a layout of their own that enum
    // instructions build and read: the types those instructions use aren't
    // split.
    u32 enumTypes[64];
    u32 enumTypeCount = 0;
    bool tooManyEnums = false;
    for (u32 i = 0; i < ir->instructionCount; i++) {
        u16 op = ir->opcodes[i];
        u16 reg = op == IR::OP_ENUM_CONSTRUCT ? ir->destinations[i]
                : op == IR::OP_ENUM_TAG || op == IR::OP_ENUM_FIELD ? ir->GetOperand(i, 0) : NO_REGISTER;
        if (!IsRegister(data, reg)) continue;
        u32 hash = ir->registerStructTypes[reg];
        bool known = false;
        for (u32 e = 0; e < enumTypeCount && !known; e++) known = enumTypes[e] == hash;
        if (known) continue;
        if (enumTypeCount == 64) {
            tooManyEnums = true;
            break;
        }
        enumTypes[enumTypeCount++] = hash;
    }
    if (tooManyEnums) return false;
    auto isEnumType = [&](u32 hash) {
        for (u32 e = 0; e < enumTypeCount; e++) {
            if (enumTypes[e] == hash) return true;
        }
        return false;
    };
    // A variable: its fields are written in place (its address is taken
    // for them), or it's assigned more than once. A struct computed once,
    // such as an initializer the variable is copied from, is a value.
    bool any = false;
    for (u32 reg = 0; reg < data->regCount; reg++) {
        bool addressed = ir->registerStorageInfo &&
                         (ir->registerStorageInfo[reg] & IRProgram::STORAGE_IS_ADDRESS_TAKEN);
        bool variable = addressed || data->definitionCounts[reg] > 1;
        data->state[reg] = variable && !isEnumType(ir->registerStructTypes[reg]) &&
                                   IsCandidate(data, static_cast<u16>(reg))
                               ? SPLIT
                               : NOT_SPLIT;
        data->layouts[reg] = data->state[reg] == SPLIT ? FindStructType(ir, ir->registerStructTypes[reg]) : nullptr;
        data->pointerStruct[reg] = NO_REGISTER;
        any |= data->state[reg] == SPLIT;
    }
    if (!any) return false;
    auto reject = [&](u16 reg) {
        if (IsRegister(data, reg)) data->state[reg] = NOT_SPLIT;
    };
    // Field pointers into candidates.
    for (u32 i = 0; i < ir->instructionCount; i++) {
        u16 op = ir->opcodes[i];
        u16 base = ir->GetOperand(i, 0);
        if (!IsRegister(data, base)) continue;
        if ((op == IR::OP_LOCAL_VAR_PTR || op == IR::OP_LOCAL_INDEX_PTR) && data->state[base] == SPLIT) reject(base);
        if (op != IR::OP_LOCAL_FIELD_PTR || data->state[base] != SPLIT) continue;
        u16 pointer = ir->destinations[i];
        u32 field = ir->GetOperand(i, 1);
        if (!IsRegister(data, pointer) || data->definitionCounts[pointer] != 1 ||
            field >= data->layouts[base]->fieldCount) {
            reject(base);
            continue;
        }
        data->pointerStruct[pointer] = base;
        data->pointerField[pointer] = static_cast<u8>(field);
    }
    // Every use of a candidate or of a field pointer into one.
    for (u32 i = 0; i < ir->instructionCount; i++) {
        u16 op = ir->opcodes[i];
        u16 dest = ir->destinations[i];
        if (StoresDestination(op) && IsRegister(data, dest)) {
            // A store's value in the destination slot: a struct there isn't rewritten.
            reject(dest);
            if (data->pointerStruct[dest] != NO_REGISTER) reject(data->pointerStruct[dest]);
        }
        for (u32 slot = 0; slot < 4; slot++) {
            u16 value = ir->GetOperand(i, slot);
            if (!IsRegister(data, value) || IsLiteralSlot(op, slot)) continue;
            u16 pointed = data->pointerStruct[value];
            if (pointed != NO_REGISTER) {
                bool access = slot == 0 && (op == IR::OP_LOCAL_LOAD || op == IR::OP_LOCAL_STORE);
                // A field pointer of a struct field: the field is split next round.
                bool nested = slot == 0 && op == IR::OP_LOCAL_FIELD_PTR &&
                              FieldType(ir, data->layouts[pointed], data->pointerField[value]) == CoreType::CUSTOM;
                if (!access && !nested) reject(pointed);
                continue;
            }
            if (data->state[value] != SPLIT) continue;
            if (op == IR::OP_LOCAL_FIELD_PTR && slot == 0) continue;
            if (op == IR::OP_STRUCT_EXTRACT && slot == 0) {
                if (ir->GetOperand(i, 1) >= data->layouts[value]->fieldCount) reject(value);
                continue;
            }
            if (IsWholeRead(op, slot)) continue;
            if (MayReadStruct(op)) reject(value);
        }
    }
    // A field pointer whose struct isn't split stays a pointer.
    any = false;
    for (u32 reg = 0; reg < data->regCount; reg++) {
        u16 pointed = data->pointerStruct[reg];
        if (pointed != NO_REGISTER && data->state[pointed] != SPLIT) data->pointerStruct[reg] = NO_REGISTER;
        any |= data->state[reg] == SPLIT;
    }
    return any;
}

static void AddUndefRegister(SplitData* data, u16 reg, CoreType type) {
    IRProgram* ir = data->ir;
    if (ir->undefRegCount == ir->undefRegCapacity) {
        u32 capacity = std::max<u32>(ir->undefRegCapacity * 2, 16);
        u16* regs = (u16*)data->builder->pool->Allocate(capacity * sizeof(u16), 64);
        u16* types = (u16*)data->builder->pool->Allocate(capacity * sizeof(u16), 64);
        if (ir->undefRegCount) {
            memcpy(regs, ir->undefRegs, ir->undefRegCount * sizeof(u16));
            memcpy(types, ir->undefRegTypes, ir->undefRegCount * sizeof(u16));
        }
        ir->undefRegs = regs;
        ir->undefRegTypes = types;
        ir->undefRegCapacity = capacity;
    }
    ir->undefRegs[ir->undefRegCount] = reg;
    ir->undefRegTypes[ir->undefRegCount] = static_cast<u16>(type);
    ir->undefRegCount++;
}

// Registers for the fields of every struct split this round, named
// `<variable>_<field>`.
static bool AllocateFields(SplitData* data) {
    IRProgram* ir = data->ir;
    u32 total = 0;
    for (u32 reg = 0; reg < data->regCount; reg++) {
        if (data->state[reg] == SPLIT) total += data->layouts[reg]->fieldCount;
    }
    data->fields = (u16*)data->builder->pool->Allocate(std::max<u32>(total, 1) * sizeof(u16), 64);
    data->fieldCount = 0;
    for (u32 reg = 0; reg < data->regCount; reg++) {
        if (data->state[reg] != SPLIT) continue;
        const IRProgram::StructTypeInfo* layout = data->layouts[reg];
        data->fieldOffsets[reg] = data->fieldCount;
        std::string variable = ReverseLookup::GetString(ir->registerNameHashes[reg]);
        for (u32 f = 0; f < layout->fieldCount; f++) {
            u32 field = layout->fieldOffset + f;
            CoreType type = FieldType(ir, layout, f);
            u16 fieldReg = AllocateRegister(data->builder, &data->insertions, type);
            if (fieldReg == NO_REGISTER) return false;
            if (type == CoreType::CUSTOM || type == CoreType::ENUM) {
                ir->registerStructTypes[fieldReg] = ir->structFieldTypeHashes ? ir->structFieldTypeHashes[field] : 0;
            }
            std::string name = variable + "_" + ReverseLookup::GetString(ir->structFieldNameHashes[field]);
            ir->registerNameHashes[fieldReg] = ReverseLookup::Intern(name);
            data->fields[data->fieldCount++] = fieldReg;
        }
    }
    return true;
}

static u16 FieldRegister(const SplitData* data, u16 structReg, u32 field) {
    return data->fields[data->fieldOffsets[structReg] + field];
}

// The struct's value rebuilt from its fields, in front of `before`.
static u16 Rebuild(SplitData* data, u16 structReg, u32 before) {
    IRProgram* ir = data->ir;
    const IRProgram::StructTypeInfo* layout = data->layouts[structReg];
    u32 hash = ir->registerStructTypes[structReg];
    auto newStruct = [&]() {
        u16 reg = AllocateRegister(data->builder, &data->insertions, CoreType::CUSTOM);
        if (reg != NO_REGISTER) ir->registerStructTypes[reg] = hash;
        return reg;
    };
    if (layout->fieldCount <= 4) {
        u16 result = newStruct();
        if (result == NO_REGISTER) return NO_REGISTER;
        u16 operands[4] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
        for (u32 f = 0; f < layout->fieldCount; f++) operands[f] = FieldRegister(data, structReg, f);
        QueueInstruction(data->builder, &data->insertions, before, IR::OP_STRUCT_CONSTRUCT, result, operands, hash);
        return result;
    }
    // Wider than OP_STRUCT_CONSTRUCT: inserts into an undefined struct.
    u16 current = newStruct();
    if (current == NO_REGISTER) return NO_REGISTER;
    AddUndefRegister(data, current, CoreType::CUSTOM);
    for (u32 f = 0; f < layout->fieldCount; f++) {
        u16 next = newStruct();
        if (next == NO_REGISTER) return NO_REGISTER;
        u16 operands[4] = {current, static_cast<u16>(f), FieldRegister(data, structReg, f), 0xFFFF};
        QueueInstruction(data->builder, &data->insertions, before, IR::OP_STRUCT_INSERT, next, operands, hash);
        current = next;
    }
    return current;
}

static void SetOperands(IRProgram* ir, u32 inst, u16 a, u16 b = 0xFFFF) {
    ir->SetOperand(inst, 0, a);
    ir->SetOperand(inst, 1, b);
    ir->SetOperand(inst, 2, 0xFFFF);
    ir->SetOperand(inst, 3, 0xFFFF);
    ir->metadata[inst] = 0;
}

static void MakeNop(IRProgram* ir, u32 inst) {
    ir->opcodes[inst] = IR::OP_NOP;
    ir->destinations[inst] = 0;
    SetOperands(ir, inst, 0xFFFF);
}

// A register nothing refers to any more: no backend declares it.
static void Retire(IRProgram* ir, u16 reg) {
    ir->registerTypes[reg] = static_cast<u16>(CoreType::VOID);
    ir->registerStructTypes[reg] = 0;
    if (ir->registerStorageInfo) ir->registerStorageInfo[reg] = 0;
}

// Queues `dest = source` right after instruction `after`.
static void QueueCopyAfter(SplitData* data, u32 after, u16 dest, u16 source) {
    const u16 operands[4] = {source, 0xFFFF, 0xFFFF, 0xFFFF};
    QueueInstructionAfter(data->builder, &data->insertions, after, IR::OP_STORE_REG, dest, operands, 0);
}

// An insert into a split struct itself: it assigns the one field.
static bool IsSelfInsert(const SplitData* data, u32 inst) {
    const IRProgram* ir = data->ir;
    u16 dest = ir->destinations[inst];
    return ir->opcodes[inst] == IR::OP_STRUCT_INSERT && IsRegister(data, dest) && data->state[dest] == SPLIT &&
           ir->GetOperand(inst, 0) == dest;
}

static bool IsFieldCopy(const SplitData* data, u32 inst) {
    const IRProgram* ir = data->ir;
    u16 dest = ir->destinations[inst];
    u16 source = ir->GetOperand(inst, 0);
    return ir->opcodes[inst] == IR::OP_STORE_REG && IsRegister(data, dest) && data->state[dest] == SPLIT &&
           IsRegister(data, source) && data->state[source] == SPLIT;
}

// Registers Rewrite allocates: fields, rebuilt structs and the temporaries
// definitions compute into.
static u32 RegistersNeeded(const SplitData* data) {
    const IRProgram* ir = data->ir;
    u32 needed = 0;
    for (u32 reg = 0; reg < data->regCount; reg++) {
        if (data->state[reg] == SPLIT) needed += data->layouts[reg]->fieldCount;
    }
    for (u32 i = 0; i < ir->instructionCount; i++) {
        u16 op = ir->opcodes[i];
        u16 dest = ir->destinations[i];
        bool fieldCopy = IsFieldCopy(data, i), selfInsert = IsSelfInsert(data, i);
        if (op != IR::OP_STRUCT_EXTRACT && op != IR::OP_LOCAL_FIELD_PTR && !fieldCopy) {
            for (u32 slot = selfInsert ? 1 : 0; slot < 4; slot++) {
                u16 value = ir->GetOperand(i, slot);
                if (!IsRegister(data, value) || data->state[value] != SPLIT || !IsWholeRead(op, slot)) continue;
                u32 fields = data->layouts[value]->fieldCount;
                needed += fields <= 4 ? 1 : fields + 1;
            }
        }
        if (Defines(op) && IsRegister(data, dest) && data->state[dest] == SPLIT && !fieldCopy && !selfInsert) {
            // The temporary it computes into, and one per field it extracts.
            needed += 1 + data->layouts[dest]->fieldCount;
        }
    }
    return needed;
}

// The value field `field` of `value` holds where it's read, from the
// inserts and the constructor it was built with: a constant or a register
// assigned once, so it still holds that. NO_REGISTER when it can't be
// told, UNDEFINED_FIELD when the struct was built without it.
constexpr u16 UNDEFINED_FIELD = 0xFFFE;
static u16 ResolveField(const SplitData* data, u16 value, u32 field) {
    const IRProgram* ir = data->ir;
    auto stable = [&](u16 operand) {
        return IR::IsConstant(operand) || (IsRegister(data, operand) && data->definitionCounts[operand] == 1);
    };
    for (u32 step = 0; step < MAX_FIELDS * 2; step++) {
        if (!IsRegister(data, value)) return NO_REGISTER;
        for (u32 u = 0; u < ir->undefRegCount; u++) {
            if (ir->undefRegs[u] == value) return UNDEFINED_FIELD;
        }
        if (data->definitionCounts[value] != 1) return NO_REGISTER;
        u32 definition = data->definedBy[value];
        u16 op = ir->opcodes[definition];
        if (op == IR::OP_STRUCT_INSERT) {
            if (ir->GetOperand(definition, 1) == field) {
                u16 inserted = ir->GetOperand(definition, 2);
                return stable(inserted) ? inserted : NO_REGISTER;
            }
            value = ir->GetOperand(definition, 0);
            continue;
        }
        if (op == IR::OP_STRUCT_CONSTRUCT) {
            u16 argument = field < 4 ? ir->GetOperand(definition, field) : 0xFFFF;
            return argument != 0xFFFF && stable(argument) ? argument : NO_REGISTER;
        }
        return NO_REGISTER;
    }
    return NO_REGISTER;
}

// Rewrites the program for the structs chosen this round.
static bool Rewrite(SplitData* data) {
    IRProgram* ir = data->ir;
    u32 count = ir->instructionCount;
    for (u32 i = 0; i < count; i++) {
        u16 op = ir->opcodes[i];
        u16 dest = ir->destinations[i];
        // Whole reads see the struct rebuilt in front of the instruction,
        // except a copy into another split struct, which copies the fields,
        // and an insert into the struct itself, which assigns one.
        bool fieldCopy = IsFieldCopy(data, i), selfInsert = IsSelfInsert(data, i);
        if (op != IR::OP_STRUCT_EXTRACT && op != IR::OP_LOCAL_FIELD_PTR && !fieldCopy) {
            for (u32 slot = selfInsert ? 1 : 0; slot < 4; slot++) {
                u16 value = ir->GetOperand(i, slot);
                if (!IsRegister(data, value) || data->state[value] != SPLIT || !IsWholeRead(op, slot)) continue;
                u16 rebuilt = Rebuild(data, value, i);
                if (rebuilt == NO_REGISTER) return false;
                ir->SetOperand(i, slot, rebuilt);
            }
        }
        u16 base = ir->GetOperand(i, 0);
        switch (op) {
            case IR::OP_STRUCT_EXTRACT:
                if (IsRegister(data, base) && data->state[base] == SPLIT) {
                    ir->opcodes[i] = IR::OP_STORE_REG;
                    SetOperands(ir, i, FieldRegister(data, base, ir->GetOperand(i, 1)));
                }
                break;
            case IR::OP_LOCAL_FIELD_PTR:
                if (IsRegister(data, base) && data->state[base] == SPLIT) {
                    MakeNop(ir, i);
                    Retire(ir, dest);
                } else if (IsRegister(data, base) && data->pointerStruct[base] != NO_REGISTER) {
                    // A field of a struct field: the field's register is now
                    // the struct it points into.
                    u16 field = FieldRegister(data, data->pointerStruct[base], data->pointerField[base]);
                    ir->SetOperand(i, 0, field);
                    if (ir->registerStorageInfo) {
                        ir->registerStorageInfo[field] |= IRProgram::STORAGE_IS_ADDRESS_TAKEN;
                        u32& storage = ir->registerStorageInfo[dest];
                        storage = (storage & 0xFFFFu) | (static_cast<u32>(field) << 16);
                    }
                }
                break;
            case IR::OP_LOCAL_LOAD:
                if (IsRegister(data, base) && data->pointerStruct[base] != NO_REGISTER) {
                    ir->opcodes[i] = IR::OP_STORE_REG;
                    SetOperands(ir, i, FieldRegister(data, data->pointerStruct[base], data->pointerField[base]));
                }
                break;
            case IR::OP_LOCAL_STORE:
                if (IsRegister(data, base) && data->pointerStruct[base] != NO_REGISTER) {
                    u16 value = ir->GetOperand(i, 1);
                    ir->opcodes[i] = IR::OP_STORE_REG;
                    ir->destinations[i] = FieldRegister(data, data->pointerStruct[base], data->pointerField[base]);
                    SetOperands(ir, i, value);
                }
                break;
            default:
                break;
        }
        if (!Defines(op) || !IsRegister(data, dest) || data->state[dest] != SPLIT) continue;
        // A definition of a split struct assigns its fields, right after it.
        const IRProgram::StructTypeInfo* layout = data->layouts[dest];
        u16 source = ir->GetOperand(i, 0);
        if (fieldCopy) {
            for (u32 f = 0; f < layout->fieldCount; f++) {
                QueueCopyAfter(data, i, FieldRegister(data, dest, f), FieldRegister(data, source, f));
            }
            MakeNop(ir, i);
            continue;
        }
        if (selfInsert) {
            u16 value = ir->GetOperand(i, 2);
            u32 field = ir->GetOperand(i, 1);
            ir->opcodes[i] = IR::OP_STORE_REG;
            ir->destinations[i] = FieldRegister(data, dest, field);
            SetOperands(ir, i, value);
            continue;
        }
        u16 value = source;
        if (op != IR::OP_STORE_REG && op != IR::OP_LOAD_REG) {
            // Anything else computes the struct into a temporary.
            value = AllocateRegister(data->builder, &data->insertions, CoreType::CUSTOM);
            if (value == NO_REGISTER) return false;
            ir->registerStructTypes[value] = ir->registerStructTypes[dest];
            ir->destinations[i] = value;
        } else {
            MakeNop(ir, i);
        }
        for (u32 f = 0; f < layout->fieldCount; f++) {
            // A struct built from known values gives the fields those.
            u16 known = value == source ? ResolveField(data, value, f) : NO_REGISTER;
            if (known == UNDEFINED_FIELD) continue;
            if (known != NO_REGISTER) {
                QueueCopyAfter(data, i, FieldRegister(data, dest, f), known);
                continue;
            }
            // A variable is only assigned by copies, as lowering does: the
            // SPIR-V backend aliases a copy's destination to its source.
            u16 fieldReg = FieldRegister(data, dest, f);
            u16 extracted = AllocateRegister(data->builder, &data->insertions,
                                             static_cast<CoreType>(ir->registerTypes[fieldReg]));
            if (extracted == NO_REGISTER) return false;
            ir->registerStructTypes[extracted] = ir->registerStructTypes[fieldReg];
            u16 operands[4] = {value, static_cast<u16>(f), 0xFFFF, 0xFFFF};
            QueueInstructionAfter(data->builder, &data->insertions, i, IR::OP_STRUCT_EXTRACT, extracted, operands,
                                  ir->registerStructTypes[dest]);
            QueueCopyAfter(data, i, fieldReg, extracted);
        }
    }
    // A field no instruction assigns holds what the struct never got.
    for (u32 k = 0; k < data->fieldCount; k++) {
        u16 field = data->fields[k];
        bool assigned = false;
        for (u32 i = 0; i < ir->instructionCount && !assigned; i++) {
            assigned = Defines(ir->opcodes[i]) && ir->destinations[i] == field;
        }
        for (u32 q = 0; q < data->insertions.count && !assigned; q++) {
            assigned = data->insertions.items[q].dest == field;
        }
        if (!assigned) AddUndefRegister(data, field, static_cast<CoreType>(ir->registerTypes[field]));
    }
    for (u32 reg = 0; reg < data->regCount; reg++) {
        if (data->state[reg] == SPLIT) Retire(ir, static_cast<u16>(reg));
    }
    return true;
}

}  // namespace Split

void SplitLocalStructs(IR::IRBuilder* builder) {
    using namespace Split;
    IRProgram* ir = builder->program;
    if (ir->instructionCount == 0 || !ir->registerTypes || !ir->registerStructTypes || !ir->structTypes) return;
    for (u32 round = 0; round < MAX_ROUNDS; round++) {
        SplitData data = {};
        data.builder = builder;
        data.ir = ir;
        data.regCount = std::min<u32>(ir->registerCount, IR::RegisterLimit);
        IRMemoryPool* pool = builder->pool;
        u32 n = std::max<u32>(data.regCount, 1);
        data.state = (u8*)pool->Allocate(n, 64);
        data.layouts = (const IRProgram::StructTypeInfo**)pool->Allocate(n * sizeof(void*), 64);
        data.pointerStruct = (u16*)pool->Allocate(n * sizeof(u16), 64);
        data.pointerField = (u8*)pool->Allocate(n, 64);
        data.definitionCounts = (u8*)pool->Allocate(n, 64);
        data.definedBy = (u32*)pool->Allocate(n * sizeof(u32), 64);
        data.fieldOffsets = (u32*)pool->Allocate(n * sizeof(u32), 64);
        if (!data.state || !data.layouts || !data.pointerStruct || !data.pointerField || !data.definitionCounts ||
            !data.definedBy || !data.fieldOffsets) {
            return;
        }
        if (!ChooseStructs(&data)) return;
        // Rewriting can't stop halfway, so the registers it needs must exist.
        if (data.regCount + RegistersNeeded(&data) > IR::RegisterLimit) return;
        if (!AllocateFields(&data) || !Rewrite(&data)) return;
        ApplyInsertions(builder, nullptr, &data.insertions);
    }
}

}  // namespace Optimization
}  // namespace BWSL
