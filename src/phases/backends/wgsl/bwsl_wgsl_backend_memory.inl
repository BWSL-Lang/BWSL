// Included by bwsl_wgsl_backend.cpp inside BWSL::WGSL.
//
// Variables, references and aggregates. SSA leaves some registers in
// memory: locals whose address is taken (^x, or a struct written field by
// field) and local arrays. Each becomes one WGSL `var`, and every
// instruction that defines the register assigns it. Pointers into such a
// variable (LOCAL_*_PTR, and STORAGE_INDEX/FIELD on a local array) are
// Reference values: a parent and one accessor, written as the access path
// where they are loaded or stored. Struct values use declared WGSL structs.

namespace {

enum RegisterFlag : u8 {
    RegisterPointer = 1,   // Holds a pointer to a variable
    RegisterVariable = 2,  // Lives in a WGSL var
    RegisterDefined = 4,
    RegisterRedefined = 8,
    RegisterTracked = 16,  // A pointer whose target is a run-time tag
};

bool IsAggregate(CoreType type, u32 arrayLength) {
    return arrayLength != 0 || type == CoreType::CUSTOM || type == CoreType::ENUM ||
           MatrixDimension(type) != 0;
}

} // namespace

u32 WGSLBuilder::StructIndex(u32 structHash) const {
    for (u32 index = 0; structHash != 0 && index < ir->structTypeCount; ++index) {
        if (ir->structTypes[index].nameHash == structHash) {
            return index;
        }
    }
    return InvalidId;
}

bool WGSLBuilder::FieldShape(u32 structHash, u32 field, CoreType& type, u32& structType,
                             u32& arrayLength) {
    const u32 index = StructIndex(structHash);
    if (index == InvalidId || field >= ir->structTypes[index].fieldCount) {
        return Fail("WGSL struct field is missing its type metadata");
    }
    const u32 member = ir->structTypes[index].fieldOffset + field;
    type = static_cast<CoreType>(ir->structFieldTypes[member]);
    structType = type == CoreType::CUSTOM || type == CoreType::ENUM ? ir->structFieldTypeHashes[member] : 0;
    arrayLength = ir->structFieldArraySizes != nullptr ? ir->structFieldArraySizes[member] : 0;
    return true;
}

u32 WGSLBuilder::NewValue(u16 reg, CoreType type, u32 structType, u32 arrayLength, ValueKind kind) {
    const u32 id = values.count++;
    registerToValue[reg] = id;
    values.types[id] = static_cast<u16>(type);
    values.producerInstructions[id] = InvalidId;
    values.kinds[id] = static_cast<u8>(kind);
    values.resourceIds[id] = InvalidId;
    values.indexOperands[id] = 0xFFFF;
    values.aliasOperands[id] = reg;
    values.structTypes[id] = structType;
    values.arrayLengths[id] = arrayLength;
    values.parents[id] = InvalidId;
    values.accessors[id] = static_cast<u8>(Accessor::Whole);
    values.variables[id] = 0;
    valueOrigins.registers[id] = reg;
    if (uniforms.count != 0) {
        uniformAddresses.bases[id] = InvalidId;
        uniformAddresses.byteOffsets[id] = 0;
        uniformAddresses.strides[id] = 0;
        uniformAddresses.lengths[id] = 0;
        uniformAddresses.structTypes[id] = 0;
    }
    lastDefined = id;
    return id;
}

void WGSLBuilder::CopyValueShape(u32 target, u32 from) {
    values.types[target] = values.types[from];
    values.structTypes[target] = values.structTypes[from];
    values.arrayLengths[target] = values.arrayLengths[from];
}

bool WGSLBuilder::IsMutable(u16 operand) const {
    if (IR::IsConstant(operand) || operand >= ir->registerCount || operand >= IR::RegisterLimit) {
        return false;
    }
    const u32 value = registerToValue[operand];
    return value != InvalidId && (values.variables[value] & VariableMutable) != 0;
}

// Counts the registers that live in WGSL variables; their values are made
// once the value table exists.
bool WGSLBuilder::PrepareRegisters() {
    const u32 registers = ir->registerCount < IR::RegisterLimit ? ir->registerCount : IR::RegisterLimit;
    registerFlags = static_cast<u8*>(Allocate(registers, sizeof(u8)));
    if (registers == 0) {
        return true;
    }
    if (registerFlags == nullptr) {
        return false;
    }
    std::memset(registerFlags, 0, registers);
    const auto valid = [&](u16 reg) { return !IR::IsConstant(reg) && reg < registers; };
    for (u32 array = 0; array < ir->localArrayCount; ++array) {
        if (!valid(ir->localArrayRegisters[array])) {
            return Fail("WGSL local array register is out of bounds");
        }
        registerFlags[ir->localArrayRegisters[array]] |= RegisterVariable;
    }
    for (u32 shared = 0; shared < ir->sharedVarCount; ++shared) {
        if (!valid(ir->sharedRegisters[shared])) {
            return Fail("WGSL workgroup variable register is out of bounds");
        }
        registerFlags[ir->sharedRegisters[shared]] |= RegisterVariable;
    }
    for (u32 instruction = 0; instruction < ir->instructionCount; ++instruction) {
        const u16 opcode = ir->opcodes[instruction];
        const u16 destination = ir->destinations[instruction];
        if (!HasResult(opcode) || opcode == IR::OP_JUMP || opcode == IR::OP_BRANCH ||
            opcode == IR::OP_SWITCH || !valid(destination)) {
            continue;
        }
        registerFlags[destination] |= (registerFlags[destination] & RegisterDefined) ? RegisterRedefined
                                                                                      : RegisterDefined;
        if (opcode == IR::OP_LOCAL_VAR_PTR || opcode == IR::OP_LOCAL_FIELD_PTR ||
            opcode == IR::OP_LOCAL_INDEX_PTR) {
            registerFlags[destination] |= RegisterPointer;
        }
    }
    // Pointers: into a local array, and copies of pointers. A pointer
    // assigned more than once, or copied from such a pointer, has its
    // target chosen at run time and is tracked.
    for (bool changed = true; changed;) {
        changed = false;
        for (u32 instruction = 0; instruction < ir->instructionCount; ++instruction) {
            const u16 opcode = ir->opcodes[instruction];
            const u16 destination = ir->destinations[instruction];
            const u16 base = ir->GetOperand(instruction, 0);
            if (!valid(destination) || !valid(base)) {
                continue;
            }
            u8 flags = registerFlags[destination];
            if ((opcode == IR::OP_STORAGE_INDEX || opcode == IR::OP_STORAGE_FIELD) &&
                (registerFlags[base] & (RegisterVariable | RegisterPointer))) {
                flags |= RegisterPointer;
            }
            if (IsCopy(opcode) && (registerFlags[base] & RegisterPointer)) {
                flags |= RegisterPointer;
                if ((registerFlags[base] & RegisterTracked) || (flags & RegisterRedefined)) {
                    flags |= RegisterTracked;
                }
            }
            changed |= flags != registerFlags[destination];
            registerFlags[destination] = flags;
        }
    }
    for (u32 instruction = 0; instruction < ir->instructionCount; ++instruction) {
        const u16 opcode = ir->opcodes[instruction];
        const u16 base = ir->GetOperand(instruction, 0);
        if ((opcode == IR::OP_LOCAL_VAR_PTR || opcode == IR::OP_LOCAL_FIELD_PTR ||
             opcode == IR::OP_LOCAL_INDEX_PTR) && valid(base) && !(registerFlags[base] & RegisterPointer)) {
            registerFlags[base] |= RegisterVariable;
        }
    }
    for (u32 reg = 0; reg < registers; ++reg) {
        if ((registerFlags[reg] & RegisterRedefined) && !(registerFlags[reg] & RegisterPointer)) {
            registerFlags[reg] |= RegisterVariable;
        }
    }
    return true;
}

bool WGSLBuilder::PrepareMemoryInstruction(u32 instruction, bool& handled) {
    handled = true;
    const u16 opcode = ir->opcodes[instruction];
    const u16 first = ir->GetOperand(instruction, 0);
    const u16 second = ir->GetOperand(instruction, 1);
    const u16 destination = ir->destinations[instruction];
    const auto valueOf = [&](u16 reg) -> u32 {
        return !IR::IsConstant(reg) && reg < ir->registerCount && reg < IR::RegisterLimit
            ? registerToValue[reg] : InvalidId;
    };
    // A variable or a reference into one: something with an access path.
    const auto addressable = [&](u32 value) {
        return value != InvalidId &&
               (values.kinds[value] == static_cast<u8>(ValueKind::Reference) ||
                values.kinds[value] == static_cast<u8>(ValueKind::StorageElement) ||
                (values.kinds[value] == static_cast<u8>(ValueKind::Value) &&
                 (values.variables[value] & VariableMutable) != 0));
    };
    const auto defineReference = [&](u32 parent, Accessor accessor, u16 operand, CoreType type,
                                     u32 structType, u32 arrayLength) {
        if (destination >= ir->registerCount || destination >= IR::RegisterLimit) {
            return Fail("WGSL pointer register is out of bounds");
        }
        if (registerToValue[destination] != InvalidId) {
            return Fail("WGSL pointer register is defined by more than one address computation");
        }
        const u32 value = NewValue(destination, type, structType, arrayLength, ValueKind::Reference);
        values.producerInstructions[value] = instruction;
        values.parents[value] = parent;
        values.accessors[value] = static_cast<u8>(accessor);
        values.indexOperands[value] = operand;
        return true;
    };
    // The shape of one element of an indexable value.
    const auto elementOf = [&](u32 base, u16 index) {
        if (!RequireIndex(index)) {
            return false;
        }
        const CoreType type = static_cast<CoreType>(values.types[base]);
        if (values.arrayLengths[base] != 0) {
            return defineReference(base, Accessor::Index, index, type, values.structTypes[base], 0);
        }
        if (MatrixDimension(type) != 0) {
            return defineReference(base, Accessor::Index, index, Shape(CoreType::FLOAT, MatrixDimension(type)), 0, 0);
        }
        if (Width(type) >= 2) {
            return defineReference(base, Accessor::Index, index, Scalar(type), 0, 0);
        }
        return Fail("WGSL indexed pointer requires an array, matrix or vector");
    };

    switch (opcode) {
        case IR::OP_LOCAL_VAR_PTR: {
            const u32 base = valueOf(first);
            if (!addressable(base)) {
                return Fail("WGSL pointer target is not a local variable");
            }
            return defineReference(base, Accessor::Whole, 0xFFFF, static_cast<CoreType>(values.types[base]),
                                   values.structTypes[base], values.arrayLengths[base]);
        }
        case IR::OP_LOCAL_FIELD_PTR:
        case IR::OP_STORAGE_FIELD: {
            const u32 base = valueOf(first);
            if (!addressable(base)) {
                if (opcode == IR::OP_STORAGE_FIELD) {
                    handled = false;
                    return true;
                }
                return Fail("WGSL field pointer target is not a local variable");
            }
            const CoreType baseType = static_cast<CoreType>(values.types[base]);
            if ((baseType != CoreType::CUSTOM && baseType != CoreType::ENUM) || values.arrayLengths[base] != 0) {
                return Fail("WGSL field pointer target is not a struct");
            }
            CoreType type = CoreType::INVALID;
            u32 structType = 0;
            u32 arrayLength = 0;
            return FieldShape(values.structTypes[base], second, type, structType, arrayLength) &&
                   defineReference(base, Accessor::Field, second, type, structType, arrayLength);
        }
        case IR::OP_LOCAL_INDEX_PTR:
        case IR::OP_STORAGE_INDEX: {
            const u32 base = valueOf(first);
            if (!addressable(base)) {
                if (opcode == IR::OP_STORAGE_INDEX) {
                    handled = false;
                    return true;
                }
                return Fail("WGSL index pointer target is not a local variable");
            }
            return elementOf(base, second);
        }
        case IR::OP_LOCAL_LOAD:
        case IR::OP_STORAGE_LOAD: {
            const u32 base = valueOf(first);
            if (base == InvalidId || values.kinds[base] != static_cast<u8>(ValueKind::Reference)) {
                if (opcode == IR::OP_STORAGE_LOAD) {
                    handled = false;
                    return true;
                }
                return Fail("WGSL load through a pointer requires a local pointer");
            }
            MarkAccess(base, AccessRead);
            return DefineValue(instruction, static_cast<CoreType>(values.types[base]), ValueKind::Value,
                               values.structTypes[base], values.arrayLengths[base]);
        }
        case IR::OP_LOCAL_STORE: {
            const u32 base = valueOf(first);
            CoreType type = CoreType::INVALID;
            if (base == InvalidId || values.kinds[base] != static_cast<u8>(ValueKind::Reference)) {
                return Fail("WGSL store through a pointer requires a local pointer");
            }
            if (!OperandType(second, type)) {
                return false;
            }
            MarkAccess(base, AccessWrite);
            const u32 stored = IR::IsConstant(second) ? InvalidId : registerToValue[second];
            if (type != static_cast<CoreType>(values.types[base]) ||
                (stored != InvalidId && (values.structTypes[stored] != values.structTypes[base] ||
                                         values.arrayLengths[stored] != values.arrayLengths[base])) ||
                (stored == InvalidId && values.arrayLengths[base] != 0)) {
                return Fail("WGSL stored value does not match its pointer's type");
            }
            return true;
        }
        case IR::OP_ARRAY_LOAD: {
            const u32 base = valueOf(first);
            if (!addressable(base) || values.arrayLengths[base] == 0) {
                handled = false;
                return true;
            }
            if (!RequireIndex(second)) {
                return false;
            }
            MarkAccess(base, AccessRead);
            return DefineValue(instruction, static_cast<CoreType>(values.types[base]), ValueKind::Value,
                               values.structTypes[base], 0);
        }
        case IR::OP_ARRAY_STORE: {
            const u32 base = valueOf(destination);
            if (!addressable(base) || values.arrayLengths[base] == 0) {
                handled = false;
                return true;
            }
            CoreType type = CoreType::INVALID;
            if (!RequireIndex(first) || !OperandType(second, type)) {
                return false;
            }
            MarkAccess(base, AccessWrite);
            const u32 stored = IR::IsConstant(second) ? InvalidId : registerToValue[second];
            if (type != static_cast<CoreType>(values.types[base]) ||
                (stored != InvalidId && values.structTypes[stored] != values.structTypes[base])) {
                return Fail("WGSL stored array element does not match the array's type");
            }
            return true;
        }
        case IR::OP_LOAD_CONST:
        case IR::OP_LOAD_REG:
        case IR::OP_STORE_REG: {
            const u32 copied = valueOf(first);
            if (destination < ir->registerCount && destination < IR::RegisterLimit &&
                (registerFlags[destination] & RegisterTracked)) {
                // Selects the target; targets are gathered afterwards.
                const u32 tracked = registerToValue[destination];
                if (copied == InvalidId || values.kinds[copied] != static_cast<u8>(ValueKind::Reference)) {
                    return Fail("WGSL pointer is assigned something that is not a local pointer");
                }
                if (static_cast<CoreType>(values.types[tracked]) == CoreType::INVALID) {
                    CopyValueShape(tracked, copied);
                } else if (values.types[tracked] != values.types[copied] ||
                           values.structTypes[tracked] != values.structTypes[copied] ||
                           values.arrayLengths[tracked] != values.arrayLengths[copied]) {
                    return Fail("WGSL pointer is reassigned to a target of another type");
                }
                return true;
            }
            if (copied != InvalidId && values.kinds[copied] == static_cast<u8>(ValueKind::UniformAddress)) {
                if (!(registerFlags[destination] & RegisterVariable)) {
                    return Fail("WGSL uniform address copy was not prepared as a uniform access");
                }
                return DefineValue(instruction, static_cast<CoreType>(values.types[copied]), ValueKind::Value,
                                   uniformAddresses.structTypes[copied], uniformAddresses.lengths[copied]);
            }
            if (copied == InvalidId || values.kinds[copied] != static_cast<u8>(ValueKind::Reference)) {
                handled = false;
                return true;
            }
            return defineReference(copied, Accessor::Whole, 0xFFFF, static_cast<CoreType>(values.types[copied]),
                                   values.structTypes[copied], values.arrayLengths[copied]);
        }
        default:
            handled = false;
            return true;
    }
}

bool WGSLBuilder::PrepareStructInstruction(u32 instruction, bool& handled) {
    handled = true;
    const u16 opcode = ir->opcodes[instruction];
    const u16 first = ir->GetOperand(instruction, 0);
    const u16 second = ir->GetOperand(instruction, 1);
    const u16 third = ir->GetOperand(instruction, 2);
    const u16 destination = ir->destinations[instruction];
    const auto structOf = [&](u16 operand, u32& value) {
        value = !IR::IsConstant(operand) && operand < ir->registerCount ? registerToValue[operand] : InvalidId;
        CoreType type = CoreType::INVALID;
        if (!OperandType(operand, type)) {
            return false;
        }
        if ((type != CoreType::CUSTOM && type != CoreType::ENUM) || values.arrayLengths[value] != 0) {
            return Fail("WGSL struct operation requires a struct value");
        }
        return true;
    };
    // The value stored into a field must have the field's whole shape.
    const auto matches = [&](u16 operand, CoreType type, u32 structType, u32 arrayLength) {
        CoreType operandType = CoreType::INVALID;
        if (!OperandType(operand, operandType)) {
            return false;
        }
        const u32 value = IR::IsConstant(operand) ? InvalidId : registerToValue[operand];
        const u32 operandStruct = value != InvalidId ? values.structTypes[value] : 0;
        const u32 operandLength = value != InvalidId ? values.arrayLengths[value] : 0;
        if (operandType != type || operandStruct != structType || operandLength != arrayLength) {
            return Fail("WGSL struct field value has an incompatible type");
        }
        return true;
    };
    switch (opcode) {
        case IR::OP_STRUCT_EXTRACT:
        case IR::OP_STRUCT_ARRAY_EXTRACT:
        case IR::OP_STRUCT_INSERT:
        case IR::OP_STRUCT_ARRAY_INSERT: {
            u32 base = InvalidId;
            if (!structOf(first, base)) {
                return false;
            }
            CoreType type = CoreType::INVALID;
            u32 structType = 0;
            u32 arrayLength = 0;
            if (!FieldShape(values.structTypes[base], second, type, structType, arrayLength)) {
                return false;
            }
            const bool element = opcode == IR::OP_STRUCT_ARRAY_EXTRACT || opcode == IR::OP_STRUCT_ARRAY_INSERT;
            if (element) {
                if (arrayLength == 0 || !RequireIndex(third)) {
                    return Fail("WGSL struct array field access requires an array field and an index");
                }
                arrayLength = 0;
            }
            if (opcode == IR::OP_STRUCT_EXTRACT || opcode == IR::OP_STRUCT_ARRAY_EXTRACT) {
                return DefineValue(instruction, type, ValueKind::Value, structType, arrayLength);
            }
            if (!matches(ir->GetOperand(instruction, element ? 3 : 2), type, structType, arrayLength)) {
                return false;
            }
            return DefineValue(instruction, static_cast<CoreType>(values.types[base]), ValueKind::Value,
                               values.structTypes[base], 0);
        }
        case IR::OP_STRUCT_CONSTRUCT: {
            const u32 structType = destination < ir->registerCount ? ir->registerStructTypes[destination] : 0;
            const u32 index = StructIndex(structType);
            if (index == InvalidId || ir->structTypes[index].fieldCount > 4) {
                return Fail("WGSL struct constructor is missing its struct type");
            }
            for (u32 field = 0; field < ir->structTypes[index].fieldCount; ++field) {
                CoreType type = CoreType::INVALID;
                u32 fieldStruct = 0;
                u32 arrayLength = 0;
                if (!FieldShape(structType, field, type, fieldStruct, arrayLength)) {
                    return false;
                }
                const u16 operand = ir->GetOperand(instruction, field);
                CoreType operandType = CoreType::INVALID;
                if (!OperandType(operand, operandType)) {
                    return false;
                }
                // Scalars and vectors convert as in a vector constructor.
                const bool numeric = arrayLength == 0 && Width(type) != 0 && Width(operandType) == Width(type);
                if (!numeric && !matches(operand, type, fieldStruct, arrayLength)) {
                    return false;
                }
            }
            return DefineValue(instruction, CoreType::CUSTOM, ValueKind::Value, structType, 0);
        }
        case IR::OP_MODF_STRUCT:
        case IR::OP_FREXP_STRUCT: {
            // { fraction, whole } and { mantissa, exponent }.
            const u32 structType = destination < ir->registerCount ? ir->registerStructTypes[destination] : 0;
            const u32 index = StructIndex(structType);
            CoreType argumentType = CoreType::INVALID;
            if (!OperandType(first, argumentType)) {
                return false;
            }
            CoreType secondField = CoreType::INVALID;
            u32 fieldStruct = 0;
            u32 fieldLength = 0;
            if (index == InvalidId || ir->structTypes[index].fieldCount != 2 ||
                !FieldShape(structType, 1, secondField, fieldStruct, fieldLength) ||
                Scalar(argumentType) != CoreType::FLOAT ||
                static_cast<CoreType>(ir->structFieldTypes[ir->structTypes[index].fieldOffset]) != argumentType ||
                secondField != (opcode == IR::OP_MODF_STRUCT ? argumentType : Shape(CoreType::INT, Width(argumentType)))) {
                return Fail("WGSL modf/frexp result struct does not match its argument");
            }
            return DefineValue(instruction, CoreType::CUSTOM, ValueKind::Value, structType, 0);
        }
        case IR::OP_ENUM_TAG:
        case IR::OP_ENUM_FIELD: {
            u32 base = InvalidId;
            if (!structOf(first, base)) {
                return false;
            }
            if (opcode == IR::OP_ENUM_TAG) {
                return DefineValue(instruction, CoreType::INT, ValueKind::Value, 0, 0);
            }
            if (!IR::IsIntConstant(second)) {
                return Fail("WGSL enum payload field index must be constant");
            }
            CoreType type = CoreType::INVALID;
            u32 structType = 0;
            u32 arrayLength = 0;
            const u32 field = 1 + ir->intConstants[IR::IntConstantIndex(second)];
            return FieldShape(values.structTypes[base], field, type, structType, arrayLength) &&
                   DefineValue(instruction, type, ValueKind::Value, structType, arrayLength);
        }
        case IR::OP_ENUM_CONSTRUCT: {
            const u32 structType = destination < ir->registerCount ? ir->registerStructTypes[destination] : 0;
            if (StructIndex(structType) == InvalidId) {
                return Fail("WGSL enum value is missing its struct type");
            }
            // The payload's fit is checked as it is written.
            for (u32 argument = 0; argument < 4; ++argument) {
                const u16 operand = ir->GetOperand(instruction, argument);
                CoreType operandType = CoreType::INVALID;
                if (operand != IR::InvalidOperand && operand != IR::UnusedOperand &&
                    !OperandType(operand, operandType)) {
                    return false;
                }
            }
            return DefineValue(instruction, CoreType::CUSTOM, ValueKind::Value, structType, 0);
        }
        case IR::OP_SELECT: {
            CoreType type = CoreType::INVALID;
            CoreType condition = CoreType::INVALID;
            if (!OperandType(first, type) || !OperandType(third, condition)) {
                return false;
            }
            const u32 whenFalse = IR::IsConstant(first) ? InvalidId : registerToValue[first];
            const u32 arrayLength = whenFalse != InvalidId ? values.arrayLengths[whenFalse] : 0;
            if (!IsAggregate(type, arrayLength)) {
                handled = false;
                return true;
            }
            const u32 structType = whenFalse != InvalidId ? values.structTypes[whenFalse] : 0;
            if (condition != CoreType::BOOL || !matches(second, type, structType, arrayLength)) {
                return Fail("WGSL select of aggregates requires matching values and a bool condition");
            }
            return DefineValue(instruction, type, ValueKind::Value, structType, arrayLength);
        }
        default:
            handled = false;
            return true;
    }
}

bool WGSLBuilder::WriteFullType(TextWriter& writer, CoreType type, u32 structType, u32 arrayLength) {
    if (arrayLength != 0) {
        writer.Text("array<");
    }
    if (type == CoreType::CUSTOM || type == CoreType::ENUM) {
        const u32 index = StructIndex(structType);
        if (index == InvalidId) {
            return Fail("WGSL struct value is missing its struct type");
        }
        writer.Text("BWSLStruct");
        writer.Number(index);
    } else {
        WriteType(writer, type);
    }
    if (arrayLength != 0) {
        writer.Text(", ");
        writer.Number(arrayLength);
        writer.Text(">");
    }
    return true;
}

bool WGSLBuilder::WriteValueType(TextWriter& writer, u32 valueId) {
    return WriteFullType(writer, static_cast<CoreType>(values.types[valueId]), values.structTypes[valueId],
                         values.arrayLengths[valueId]);
}

bool WGSLBuilder::WriteReference(TextWriter& writer, u32 valueId) {
    const ValueKind kind = static_cast<ValueKind>(values.kinds[valueId]);
    if (kind == ValueKind::StorageRoot || kind == ValueKind::StorageElement) {
        return WriteStorageAddress(writer, valueId);
    }
    if (kind == ValueKind::Value) {
        WriteValueName(writer, valueId);
        return true;
    }
    const Accessor accessor = static_cast<Accessor>(values.accessors[valueId]);
    if (kind == ValueKind::Reference && accessor == Accessor::Tracked) {
        // Inside a switch on the tag: the case's target, with the index
        // the pointer kept when it was assigned.
        const u32 target = dispatchTarget;
        if (valueId != dispatchNode || target == InvalidId) {
            return Fail("WGSL tracked pointer is used outside its target dispatch");
        }
        dispatchNode = InvalidId;
        const bool snapshot = (values.variables[valueId] & VariableSnapshot) != 0 &&
            static_cast<Accessor>(values.accessors[target]) == Accessor::Index &&
            !IR::IsConstant(values.indexOperands[target]);
        const bool written = snapshot ? WriteReference(writer, values.parents[target]) : WriteReference(writer, target);
        dispatchNode = valueId;
        if (!written) {
            return false;
        }
        if (snapshot) {
            writer.Text("[");
            WriteValueName(writer, valueId);
            writer.Text("_i]");
        }
        return true;
    }
    if (kind != ValueKind::Reference || values.parents[valueId] == InvalidId ||
        !WriteReference(writer, values.parents[valueId])) {
        return Fail("WGSL pointer has no valid access path");
    }
    if (accessor == Accessor::Field) {
        writer.Text(".f");
        writer.Number(values.indexOperands[valueId]);
    } else if (accessor == Accessor::Index) {
        writer.Text("[");
        if (!WriteOperand(writer, values.indexOperands[valueId])) {
            return false;
        }
        writer.Text("]");
    }
    return true;
}

// Every struct type in the program, nested structs first.
bool WGSLBuilder::WriteStructDeclarations(TextWriter& writer) {
    if (ir->structTypeCount == 0) {
        return true;
    }
    u8* states = static_cast<u8*>(Allocate(ir->structTypeCount, sizeof(u8)));
    u32* stack = static_cast<u32*>(Allocate(ir->structTypeCount, sizeof(u32)));
    if (states == nullptr || stack == nullptr) {
        return false;
    }
    std::memset(states, 0, ir->structTypeCount);
    // Only the structs values and buffers use, and the structs they
    // contain.
    for (u32 item = 0; item < values.count + resources.count; ++item) {
        u32 root = InvalidId;
        if (item < values.count) {
            const CoreType valueType = static_cast<CoreType>(values.types[item]);
            if ((valueType == CoreType::CUSTOM || valueType == CoreType::ENUM) &&
                values.kinds[item] != static_cast<u8>(ValueKind::UniformAddress)) {
                root = StructIndex(values.structTypes[item]);
            }
        } else {
            root = StructIndex(resources.structTypes[item - values.count]);
        }
        if (root == InvalidId || states[root] != 0) {
            continue;
        }
        u32 depth = 0;
        stack[depth++] = root;
        states[root] = 1;
        while (depth != 0) {
            const u32 index = stack[depth - 1];
            const IR::IRProgram::StructTypeInfo& info = ir->structTypes[index];
            bool pushed = false;
            for (u32 field = 0; field < info.fieldCount && !pushed; ++field) {
                const u32 nested = StructIndex(ir->structFieldTypeHashes[info.fieldOffset + field]);
                const CoreType type = static_cast<CoreType>(ir->structFieldTypes[info.fieldOffset + field]);
                if ((type != CoreType::CUSTOM && type != CoreType::ENUM) || nested == InvalidId) {
                    continue;
                }
                if (states[nested] == 1) {
                    return Fail("WGSL structs cannot contain themselves");
                }
                if (states[nested] == 0) {
                    states[nested] = 1;
                    stack[depth++] = nested;
                    pushed = true;
                }
            }
            if (pushed) {
                continue;
            }
            --depth;
            states[index] = 2;
            writer.Text("struct BWSLStruct");
            writer.Number(index);
            writer.Text(" {\n");
            // WGSL structs need a member. Buffers can't hold an empty
            // struct (lowering reports it), so only locals see this one.
            if (info.fieldCount == 0) {
                writer.Text("    bwsl_empty: u32,\n");
            }
            for (u32 field = 0; field < info.fieldCount; ++field) {
                CoreType type = CoreType::INVALID;
                u32 structType = 0;
                u32 arrayLength = 0;
                if (!FieldShape(info.nameHash, field, type, structType, arrayLength)) {
                    return false;
                }
                writer.Text("    ");
                // Host-shareable structs keep the BWSL buffer layout.
                if (storageStates != nullptr && storageStates[index] == 2) {
                    writer.Text("@size(");
                    writer.Number(storageMemberSizes[info.fieldOffset + field]);
                    writer.Text(") ");
                }
                writer.Text("f");
                writer.Number(field);
                writer.Text(": ");
                if (!WriteFullType(writer, type, structType, arrayLength)) {
                    return false;
                }
                writer.Text(",\n");
            }
            writer.Text("}\n\n");
        }
    }
    return true;
}

bool WGSLBuilder::WriteStructExpression(TextWriter& writer, u32 instruction, u32 valueId, bool& handled) {
    handled = true;
    const u16 opcode = ir->opcodes[instruction];
    const u16 first = ir->GetOperand(instruction, 0);
    const u16 second = ir->GetOperand(instruction, 1);
    switch (opcode) {
        case IR::OP_LOCAL_LOAD:
            return WriteReference(writer, registerToValue[first]);
        case IR::OP_STRUCT_EXTRACT:
        case IR::OP_STRUCT_ARRAY_EXTRACT:
            if (!WriteOperand(writer, first)) {
                return false;
            }
            writer.Text(".f");
            writer.Number(second);
            if (opcode == IR::OP_STRUCT_ARRAY_EXTRACT) {
                writer.Text("[");
                if (!WriteOperand(writer, ir->GetOperand(instruction, 2))) {
                    return false;
                }
                writer.Text("]");
            }
            return true;
        case IR::OP_MODF_STRUCT:
        case IR::OP_FREXP_STRUCT: {
            const bool modf = opcode == IR::OP_MODF_STRUCT;
            if (!WriteValueType(writer, valueId)) {
                return false;
            }
            for (u32 field = 0; field < 2; ++field) {
                writer.Text(field == 0 ? (modf ? "(modf(" : "(frexp(") : (modf ? ", modf(" : ", frexp("));
                if (!WriteOperand(writer, first)) {
                    return false;
                }
                writer.Text(field == 0 ? ").fract" : modf ? ").whole" : ").exp");
            }
            writer.Text(")");
            return true;
        }
        case IR::OP_ENUM_TAG:
            if (!WriteOperand(writer, first)) {
                return false;
            }
            writer.Text(".f0");
            return true;
        case IR::OP_ENUM_FIELD:
            if (!WriteOperand(writer, first)) {
                return false;
            }
            writer.Text(".f");
            writer.Number(1 + ir->intConstants[IR::IntConstantIndex(second)]);
            return true;
        case IR::OP_ENUM_CONSTRUCT:
            return WriteEnumConstruct(writer, instruction, values.structTypes[valueId]);
        case IR::OP_STRUCT_CONSTRUCT: {
            if (!WriteValueType(writer, valueId)) {
                return false;
            }
            writer.Text("(");
            const u32 structType = values.structTypes[valueId];
            const u32 fieldCount = ir->structTypes[StructIndex(structType)].fieldCount;
            for (u32 field = 0; field < fieldCount; ++field) {
                CoreType type = CoreType::INVALID;
                u32 fieldStruct = 0;
                u32 arrayLength = 0;
                if (!FieldShape(structType, field, type, fieldStruct, arrayLength)) {
                    return false;
                }
                writer.Text(field != 0 ? ", " : "");
                const u16 operand = ir->GetOperand(instruction, field);
                const bool numeric = arrayLength == 0 && Width(type) != 0;
                if (!(numeric ? WriteConvertedOperand(writer, operand, type) : WriteOperand(writer, operand))) {
                    return false;
                }
            }
            writer.Text(")");
            return true;
        }
        default:
            handled = false;
            return true;
    }
}

// Statements: stores through pointers and into variables, and aggregate
// updates and selects, which have no WGSL expression form.
bool WGSLBuilder::WriteMemoryInstruction(TextWriter& writer, u32 instruction, bool& handled) {
    handled = true;
    const u16 opcode = ir->opcodes[instruction];
    const u16 first = ir->GetOperand(instruction, 0);
    const u16 second = ir->GetOperand(instruction, 1);
    const u16 destination = ir->destinations[instruction];
    const auto valueOf = [&](u16 reg) -> u32 {
        return !IR::IsConstant(reg) && reg < ir->registerCount && reg < IR::RegisterLimit
            ? registerToValue[reg] : InvalidId;
    };
    if (IsCopy(opcode) && destination < ir->registerCount && destination < IR::RegisterLimit &&
        (registerFlags[destination] & RegisterTracked)) {
        // tag = target (or the copied pointer's tag), and its index.
        const u32 tracked = registerToValue[destination];
        const u32 copied = valueOf(first);
        const bool copiedTracked = static_cast<Accessor>(values.accessors[copied]) == Accessor::Tracked;
        writer.Indent();
        WriteValueName(writer, tracked);
        writer.Text(" = ");
        if (copiedTracked) {
            WriteValueName(writer, copied);
        } else {
            writer.Number(copied);
            writer.Text("u");
        }
        writer.Text(";\n");
        if (values.variables[tracked] & VariableSnapshot) {
            const bool index = !copiedTracked && static_cast<Accessor>(values.accessors[copied]) == Accessor::Index &&
                               !IR::IsConstant(values.indexOperands[copied]);
            if (index || (copiedTracked && (values.variables[copied] & VariableSnapshot))) {
                writer.Indent();
                WriteValueName(writer, tracked);
                writer.Text("_i = ");
                if (copiedTracked) {
                    WriteValueName(writer, copied);
                    writer.Text("_i");
                } else if (!WriteConvertedOperand(writer, values.indexOperands[copied], CoreType::INT)) {
                    return false;
                }
                writer.Text(";\n");
            }
        }
        return true;
    }
    {
        // Loads and stores through a pointer whose target is a tag.
        const u16 pointer = opcode == IR::OP_LOCAL_STORE || opcode == IR::OP_LOCAL_LOAD ||
                            opcode == IR::OP_STORAGE_LOAD || opcode == IR::OP_ARRAY_LOAD ? first :
                            opcode == IR::OP_ARRAY_STORE ? destination : 0xFFFF;
        const u32 reference = pointer != 0xFFFF ? valueOf(pointer) : InvalidId;
        if (reference != InvalidId && TrackedNode(reference) != InvalidId) {
            return WriteTrackedAccess(writer, instruction, reference);
        }
    }
    if (opcode >= IR::OP_ATOMIC_ADD && opcode <= IR::OP_ATOMIC_CMP_XCHG) {
        return WriteAtomicInstruction(writer, instruction);
    }
    if (opcode == IR::OP_LOCAL_STORE) {
        const bool atomic = IsAtomicReference(registerToValue[first]);
        writer.Indent();
        writer.Text(atomic ? "atomicStore(&" : "");
        if (!WriteReference(writer, registerToValue[first])) {
            return false;
        }
        writer.Text(atomic ? ", " : " = ");
        if (!WriteOperand(writer, second)) {
            return false;
        }
        writer.Text(atomic ? ");\n" : ";\n");
        return true;
    }
    if (opcode == IR::OP_ARRAY_STORE && destination < ir->registerCount &&
        registerToValue[destination] != InvalidId &&
        values.kinds[registerToValue[destination]] != static_cast<u8>(ValueKind::StorageRoot)) {
        const bool atomic = IsAtomicReference(registerToValue[destination]);
        writer.Indent();
        writer.Text(atomic ? "atomicStore(&" : "");
        if (!WriteReference(writer, registerToValue[destination])) {
            return false;
        }
        writer.Text("[");
        if (!WriteOperand(writer, first)) {
            return false;
        }
        writer.Text(atomic ? "], " : "] = ");
        if (!WriteOperand(writer, second)) {
            return false;
        }
        writer.Text(atomic ? ");\n" : ";\n");
        return true;
    }
    if (IsCopy(opcode) && !IR::IsConstant(first) && first < ir->registerCount &&
        registerToValue[first] != InvalidId &&
        values.kinds[registerToValue[first]] == static_cast<u8>(ValueKind::UniformAddress)) {
        if (!(registerFlags[destination] & RegisterVariable)) {
            return true; // Uniform accesses are written where they are loaded.
        }
        writer.Indent();
        WriteValueName(writer, registerToValue[destination]);
        writer.Text(" = ");
        if (!WriteUniformLoad(writer, registerToValue[first])) {
            return false;
        }
        writer.Text(";\n");
        return true;
    }
    const bool insert = opcode == IR::OP_STRUCT_INSERT || opcode == IR::OP_STRUCT_ARRAY_INSERT;
    const u32 valueId = HasResult(opcode) && destination < ir->registerCount ? registerToValue[destination] : InvalidId;
    const bool select = opcode == IR::OP_SELECT && valueId != InvalidId &&
        IsAggregate(static_cast<CoreType>(values.types[valueId]), values.arrayLengths[valueId]);
    if (!insert && !select) {
        handled = false;
        return true;
    }
    // var v = base; v.f = value;  /  var v = whenFalse; if (c) { v = whenTrue; }
    writer.Indent();
    if (!values.functionScope[valueId]) {
        writer.Text("var ");
    }
    WriteValueName(writer, valueId);
    writer.Text(" = ");
    if (!WriteOperand(writer, first)) {
        return false;
    }
    writer.Text(";\n");
    writer.Indent();
    if (select) {
        writer.Text("if (");
        if (!WriteOperand(writer, ir->GetOperand(instruction, 2))) {
            return false;
        }
        writer.Text(") {\n");
        ++writer.indentation;
        writer.Indent();
        WriteValueName(writer, valueId);
        writer.Text(" = ");
        if (!WriteOperand(writer, second)) {
            return false;
        }
        writer.Text(";\n");
        --writer.indentation;
        writer.Indent();
        writer.Text("}\n");
        return true;
    }
    WriteValueName(writer, valueId);
    writer.Text(".f");
    writer.Number(second);
    const bool element = opcode == IR::OP_STRUCT_ARRAY_INSERT;
    if (element) {
        writer.Text("[");
        if (!WriteOperand(writer, ir->GetOperand(instruction, 2))) {
            return false;
        }
        writer.Text("]");
    }
    writer.Text(" = ");
    if (!WriteOperand(writer, ir->GetOperand(instruction, element ? 3 : 2))) {
        return false;
    }
    writer.Text(";\n");
    return true;
}

u32 WGSLBuilder::TrackedNode(u32 reference) const {
    for (u32 node = reference; node != InvalidId; node = values.parents[node]) {
        if (values.kinds[node] != static_cast<u8>(ValueKind::Reference)) {
            return InvalidId;
        }
        if (static_cast<Accessor>(values.accessors[node]) == Accessor::Tracked) {
            return node;
        }
    }
    return InvalidId;
}

u32 WGSLBuilder::TrackedIndex(u32 valueId) const {
    for (u32 index = 0; index < trackedPointers.count; ++index) {
        if (trackedPointers.values[index] == valueId) {
            return index;
        }
    }
    return InvalidId;
}

// Gathers each tracked pointer's possible targets from the assignments to
// it, through copies of other tracked pointers.
bool WGSLBuilder::PrepareTrackedPointers() {
    for (u32 value = 0; value < values.count; ++value) {
        trackedPointers.count += values.kinds[value] == static_cast<u8>(ValueKind::Reference) &&
                                 static_cast<Accessor>(values.accessors[value]) == Accessor::Tracked;
    }
    if (trackedPointers.count == 0) {
        return true;
    }
    const u32 words = (values.count + 63) / 64;
    trackedPointers.values = static_cast<u32*>(Allocate(trackedPointers.count, sizeof(u32)));
    trackedPointers.offsets = static_cast<u32*>(Allocate(trackedPointers.count + 1, sizeof(u32)));
    u64* sets = static_cast<u64*>(Allocate(static_cast<size_t>(trackedPointers.count) * words, sizeof(u64)));
    if (errorMessage[0] != '\0') {
        return false;
    }
    std::memset(sets, 0, static_cast<size_t>(trackedPointers.count) * words * sizeof(u64));
    for (u32 value = 0, index = 0; value < values.count; ++value) {
        if (values.kinds[value] == static_cast<u8>(ValueKind::Reference) &&
            static_cast<Accessor>(values.accessors[value]) == Accessor::Tracked) {
            trackedPointers.values[index++] = value;
        }
    }
    for (bool changed = true; changed;) {
        changed = false;
        for (u32 instruction = 0; instruction < ir->instructionCount; ++instruction) {
            const u16 destination = ir->destinations[instruction];
            const u16 copied = ir->GetOperand(instruction, 0);
            if (!IsCopy(ir->opcodes[instruction]) || IR::IsConstant(copied) || destination >= ir->registerCount ||
                destination >= IR::RegisterLimit || !(registerFlags[destination] & RegisterTracked) ||
                (structured && !control.reachable[cfg->instToBlock[instruction]])) {
                continue;
            }
            u64* set = sets + static_cast<size_t>(TrackedIndex(registerToValue[destination])) * words;
            const u32 sourceValue = registerToValue[copied];
            const u32 sourceIndex = TrackedIndex(sourceValue);
            if (sourceIndex == InvalidId) {
                const u64 bit = u64(1) << (sourceValue % 64);
                changed |= (set[sourceValue / 64] & bit) == 0;
                set[sourceValue / 64] |= bit;
                continue;
            }
            const u64* other = sets + static_cast<size_t>(sourceIndex) * words;
            for (u32 word = 0; word < words; ++word) {
                changed |= (set[word] | other[word]) != set[word];
                set[word] |= other[word];
            }
        }
    }
    u32 total = 0;
    for (size_t word = 0; word < static_cast<size_t>(trackedPointers.count) * words; ++word) {
        total += static_cast<u32>(std::popcount(sets[word]));
    }
    trackedPointers.targets = static_cast<u32*>(Allocate(total, sizeof(u32)));
    if (errorMessage[0] != '\0') {
        return false;
    }
    u32 cursor = 0;
    for (u32 index = 0; index < trackedPointers.count; ++index) {
        const u32 tracked = trackedPointers.values[index];
        trackedPointers.offsets[index] = cursor;
        for (u32 target = 0; target < values.count; ++target) {
            if (!(sets[static_cast<size_t>(index) * words + target / 64] & (u64(1) << (target % 64)))) {
                continue;
            }
            // A target's own path is fixed; only its last index is kept
            // per assignment.
            if (TrackedNode(target) != InvalidId) {
                return Fail("WGSL pointer targets cannot themselves be reassigned pointers");
            }
            if (static_cast<Accessor>(values.accessors[target]) == Accessor::Index &&
                !IR::IsConstant(values.indexOperands[target])) {
                values.variables[tracked] |= VariableSnapshot;
            }
            trackedPointers.targets[cursor++] = target;
        }
        if (cursor == trackedPointers.offsets[index]) {
            return Fail("WGSL pointer is used without being assigned a target");
        }
    }
    trackedPointers.offsets[trackedPointers.count] = cursor;
    return true;
}

// Counts what each target's path reads where the pointer is used.
void WGSLBuilder::CountTrackedTargets(u32 tracked) {
    const u32 index = TrackedIndex(tracked);
    if (index == InvalidId) {
        return;
    }
    for (u32 item = trackedPointers.offsets[index]; item < trackedPointers.offsets[index + 1]; ++item) {
        const u32 target = trackedPointers.targets[item];
        const bool snapshot = (values.variables[tracked] & VariableSnapshot) != 0 &&
            static_cast<Accessor>(values.accessors[target]) == Accessor::Index;
        CountUse(valueOrigins.registers[snapshot ? values.parents[target] : target]);
    }
}

// switch (tag) { case target: { access through target; } ... }
bool WGSLBuilder::WriteTrackedAccess(TextWriter& writer, u32 instruction, u32 reference) {
    const u16 opcode = ir->opcodes[instruction];
    const u32 tracked = TrackedNode(reference);
    const u32 index = TrackedIndex(tracked);
    const bool load = opcode == IR::OP_LOCAL_LOAD || opcode == IR::OP_STORAGE_LOAD || opcode == IR::OP_ARRAY_LOAD;
    const u32 result = load ? registerToValue[ir->destinations[instruction]] : InvalidId;
    if (index == InvalidId) {
        return Fail("WGSL tracked pointer has no targets");
    }
    if (load && !values.functionScope[result]) {
        writer.Indent();
        writer.Text("var ");
        WriteValueName(writer, result);
        writer.Text(": ");
        if (!WriteValueType(writer, result)) {
            return false;
        }
        writer.Text(";\n");
    }
    writer.Indent();
    writer.Text("switch (");
    WriteValueName(writer, tracked);
    writer.Text(") {\n");
    ++writer.indentation;
    const u32 begin = trackedPointers.offsets[index];
    const u32 end = trackedPointers.offsets[index + 1];
    for (u32 item = begin; item < end; ++item) {
        writer.Indent();
        if (item + 1 == end) {
            writer.Text("default: {\n");
        } else {
            writer.Text("case ");
            writer.Number(trackedPointers.targets[item]);
            writer.Text("u: {\n");
        }
        ++writer.indentation;
        writer.Indent();
        dispatchNode = tracked;
        dispatchTarget = trackedPointers.targets[item];
        if (load) {
            WriteValueName(writer, result);
            writer.Text(" = ");
        }
        bool written = WriteReference(writer, reference);
        if (written && (opcode == IR::OP_ARRAY_LOAD || opcode == IR::OP_ARRAY_STORE)) {
            writer.Text("[");
            written = WriteOperand(writer, ir->GetOperand(instruction, opcode == IR::OP_ARRAY_LOAD ? 1 : 0));
            writer.Text("]");
        }
        if (written && !load) {
            writer.Text(" = ");
            written = WriteOperand(writer, ir->GetOperand(instruction, 1));
        }
        dispatchNode = InvalidId;
        dispatchTarget = InvalidId;
        if (!written) {
            return false;
        }
        writer.Text(";\n");
        --writer.indentation;
        writer.Indent();
        writer.Text("}\n");
    }
    --writer.indentation;
    writer.Indent();
    writer.Text("}\n");
    return true;
}

void WGSLBuilder::MarkAccess(u32 reference, u8 access) {
    for (u32 node = reference; node != InvalidId; node = values.parents[node]) {
        const ValueKind kind = static_cast<ValueKind>(values.kinds[node]);
        if (kind == ValueKind::StorageRoot || kind == ValueKind::StorageElement) {
            resources.access[values.resourceIds[node]] |= access;
            return;
        }
        if (kind != ValueKind::Reference) {
            return;
        }
    }
}

// WGSL alignment and size of a storage-buffer member type.
bool WGSLBuilder::StorageShape(CoreType type, u32 structType, u32& alignment, u32& size,
                               bool std430Matrices) {
    if (type == CoreType::CUSTOM || type == CoreType::ENUM) {
        const u32 index = StructIndex(structType);
        if (index == InvalidId || !PrepareStorageStruct(index)) {
            return errorMessage[0] == '\0' ? Fail("WGSL storage struct is missing its type") : false;
        }
        alignment = storageAlignments[index];
        size = storageSizes[index];
        return true;
    }
    if (Scalar(type) == CoreType::BOOL) {
        return Fail("WGSL storage buffers cannot hold bool, which is not host-shareable");
    }
    if (type == CoreType::MAT2) {
        if (!std430Matrices) {
            return Fail("WGSL mat2x2 has an 8-byte column stride; std140 structs and mat2 buffers use 16");
        }
        alignment = 8;
        size = 16;
        return true;
    }
    if (MatrixDimension(type) != 0) {
        alignment = 16;
        size = 16 * MatrixDimension(type);
        return true;
    }
    const u32 width = Width(type);
    if (width == 0) {
        return Fail("WGSL storage buffers hold numbers, vectors, matrices and structs");
    }
    alignment = width == 1 ? 4 : width == 2 ? 8 : 16;
    size = 4 * width;
    return true;
}

// Checks that WGSL can give a buffer struct the BWSL member offsets, and
// sizes each member up to the next one with @size. A struct's size is its
// BWSL array stride: its own size up to 8 bytes, else rounded to 16.
bool WGSLBuilder::PrepareStorageStruct(u32 structId) {
    if (storageStates == nullptr) {
        u32 fields = 0;
        for (u32 index = 0; index < ir->structTypeCount; ++index) {
            fields = std::max(fields, ir->structTypes[index].fieldOffset + ir->structTypes[index].fieldCount);
        }
        storageStates = static_cast<u8*>(Allocate(ir->structTypeCount, sizeof(u8)));
        storageAlignments = static_cast<u32*>(Allocate(ir->structTypeCount, sizeof(u32)));
        storageSizes = static_cast<u32*>(Allocate(ir->structTypeCount, sizeof(u32)));
        storageMemberSizes = static_cast<u32*>(Allocate(fields, sizeof(u32)));
        if (errorMessage[0] != '\0') {
            return false;
        }
        std::memset(storageStates, 0, ir->structTypeCount);
    }
    if (storageStates[structId] == 2) {
        return true;
    }
    if (storageStates[structId] == 1) {
        return Fail("WGSL structs cannot contain themselves");
    }
    storageStates[structId] = 1;
    const IR::IRProgram::StructTypeInfo& info = ir->structTypes[structId];
    // Lowering laid the struct out; its size includes the end padding.
    const u32 structSize = info.totalSize;
    u32 structAlignment = 4;
    for (u32 field = 0; field < info.fieldCount; ++field) {
        CoreType type = CoreType::INVALID;
        u32 fieldStruct = 0;
        u32 arrayLength = 0;
        u32 alignment = 0;
        u32 size = 0;
        if (!FieldShape(info.nameHash, field, type, fieldStruct, arrayLength) ||
            !StorageShape(type, fieldStruct, alignment, size, info.std140 == 0)) {
            return false;
        }
        if (arrayLength != 0) {
            const u32 bwslStride = ir->structFieldArrayStrides[info.fieldOffset + field];
            const u32 stride = (size + alignment - 1) & ~(alignment - 1);
            if (stride != bwslStride) {
                return Fail("WGSL cannot give a storage array field its BWSL stride");
            }
            size = stride * arrayLength;
        }
        const u32 offset = ir->structFieldByteOffsets[info.fieldOffset + field];
        const u32 next = field + 1 < info.fieldCount ? ir->structFieldByteOffsets[info.fieldOffset + field + 1]
                                                     : structSize;
        if ((field == 0 && offset != 0) || offset % alignment != 0 || next < offset || next - offset < size) {
            return Fail("WGSL cannot give a storage struct member its BWSL offset");
        }
        storageMemberSizes[info.fieldOffset + field] = next - offset;
        structAlignment = std::max(structAlignment, alignment);
    }
    storageAlignments[structId] = structAlignment;
    storageSizes[structId] = structSize;
    storageStates[structId] = 2;
    return true;
}

// Atomic elements: the root is a workgroup variable or a primitive buffer.
bool WGSLBuilder::IsAtomicReference(u32 reference) const {
    for (u32 node = reference; node != InvalidId; node = values.parents[node]) {
        const ValueKind kind = static_cast<ValueKind>(values.kinds[node]);
        if (kind == ValueKind::StorageRoot || kind == ValueKind::StorageElement) {
            return (resources.access[values.resourceIds[node]] & AccessAtomic) != 0;
        }
        if (kind == ValueKind::Value) {
            return (values.variables[node] & VariableAtomic) != 0;
        }
        if (kind != ValueKind::Reference) {
            return false;
        }
    }
    return false;
}

bool WGSLBuilder::WriteAtomicType(TextWriter& writer, CoreType type, u32 arrayLength) {
    if (arrayLength != 0) {
        writer.Text("array<");
    }
    writer.Text(type == CoreType::INT ? "atomic<i32>" : "atomic<u32>");
    if (arrayLength != 0) {
        writer.Text(", ");
        writer.Number(arrayLength);
        writer.Text(">");
    }
    return true;
}

// WGSL atomics are typed: the workgroup variable or buffer an atomic
// operation targets holds atomic<i32>/atomic<u32> elements, and its other
// loads and stores become atomicLoad and atomicStore.
bool WGSLBuilder::PrepareAtomicInstruction(u32 instruction) {
    const u16 opcode = ir->opcodes[instruction];
    const u16 pointer = ir->GetOperand(instruction, 0);
    const u32 reference = !IR::IsConstant(pointer) && pointer < ir->registerCount ? registerToValue[pointer] : InvalidId;
    if (reference == InvalidId) {
        return Fail("WGSL atomic operation requires a pointer");
    }
    const CoreType type = static_cast<CoreType>(values.types[reference]);
    if ((type != CoreType::INT && type != CoreType::UINT) || values.arrayLengths[reference] != 0) {
        return Fail("WGSL atomics operate on i32 and u32 only");
    }
    // The pointer must be the whole root or one element of an array root.
    u32 node = reference;
    while (values.kinds[node] == static_cast<u8>(ValueKind::Reference) &&
           static_cast<Accessor>(values.accessors[node]) == Accessor::Whole) {
        node = values.parents[node];
    }
    if (values.kinds[node] == static_cast<u8>(ValueKind::Reference) &&
        static_cast<Accessor>(values.accessors[node]) == Accessor::Index) {
        node = values.parents[node];
        if (values.kinds[node] != static_cast<u8>(ValueKind::Value) || values.arrayLengths[node] == 0) {
            return Fail("WGSL atomics require a workgroup or storage-buffer integer, or an array of them");
        }
    }
    const ValueKind kind = static_cast<ValueKind>(values.kinds[node]);
    if (kind == ValueKind::StorageElement) {
        resources.access[values.resourceIds[node]] |= AccessAtomic | AccessRead | AccessWrite;
    } else if (kind == ValueKind::Value && (values.variables[node] & VariableWorkgroup) &&
               values.structTypes[node] == 0) {
        values.variables[node] |= VariableAtomic;
    } else {
        return Fail("WGSL atomics require a workgroup or storage-buffer integer, or an array of them");
    }
    for (u32 argument = 1; argument < (opcode == IR::OP_ATOMIC_CMP_XCHG ? 3u : 2u); ++argument) {
        CoreType argumentType = CoreType::INVALID;
        if (!OperandType(ir->GetOperand(instruction, argument), argumentType)) {
            return false;
        }
        if (argumentType != CoreType::INT && argumentType != CoreType::UINT) {
            return Fail("WGSL atomic operands must be i32 or u32");
        }
    }
    return DefineValue(instruction, type, ValueKind::Value);
}

bool WGSLBuilder::WriteAtomicInstruction(TextWriter& writer, u32 instruction) {
    const u16 opcode = ir->opcodes[instruction];
    const u32 reference = registerToValue[ir->GetOperand(instruction, 0)];
    const u32 result = registerToValue[ir->destinations[instruction]];
    const CoreType type = static_cast<CoreType>(values.types[reference]);
    const u16 value = ir->GetOperand(instruction, 1);
    if (opcode == IR::OP_ATOMIC_CMP_XCHG) {
        // atomic_cmp_exchange(pointer, compare, value) is strong; WGSL's
        // may fail spuriously, so retry until it exchanges or sees
        // another value.
        const u16 compare = value;
        const u16 replacement = ir->GetOperand(instruction, 2);
        if (!values.functionScope[result]) {
            writer.Indent();
            writer.Text("var ");
            WriteValueName(writer, result);
            writer.Text(": ");
            WriteType(writer, type);
            writer.Text(";\n");
        }
        writer.Indent();
        writer.Text("loop {\n");
        ++writer.indentation;
        writer.Indent();
        writer.Text("let bwsl_exchange = atomicCompareExchangeWeak(&");
        if (!WriteReference(writer, reference)) {
            return false;
        }
        writer.Text(", ");
        if (!WriteConvertedOperand(writer, compare, type)) {
            return false;
        }
        writer.Text(", ");
        if (!WriteConvertedOperand(writer, replacement, type)) {
            return false;
        }
        writer.Text(");\n");
        writer.Indent();
        WriteValueName(writer, result);
        writer.Text(" = bwsl_exchange.old_value;\n");
        writer.Indent();
        writer.Text("if (bwsl_exchange.exchanged || bwsl_exchange.old_value != ");
        if (!WriteConvertedOperand(writer, compare, type)) {
            return false;
        }
        writer.Text(") {\n");
        ++writer.indentation;
        writer.Indent();
        writer.Text("break;\n");
        --writer.indentation;
        writer.Indent();
        writer.Text("}\n");
        --writer.indentation;
        writer.Indent();
        writer.Text("}\n");
        return true;
    }
    const char* function = opcode == IR::OP_ATOMIC_ADD ? "atomicAdd(&" : opcode == IR::OP_ATOMIC_SUB ? "atomicSub(&" :
                           opcode == IR::OP_ATOMIC_MIN ? "atomicMin(&" : opcode == IR::OP_ATOMIC_MAX ? "atomicMax(&" :
                           opcode == IR::OP_ATOMIC_AND ? "atomicAnd(&" : opcode == IR::OP_ATOMIC_OR ? "atomicOr(&" :
                           opcode == IR::OP_ATOMIC_XOR ? "atomicXor(&" : "atomicExchange(&";
    writer.Indent();
    if (!values.functionScope[result]) {
        writer.Text("let ");
    }
    WriteValueName(writer, result);
    writer.Text(" = ");
    writer.Text(function);
    if (!WriteReference(writer, reference)) {
        return false;
    }
    writer.Text(", ");
    if (!WriteConvertedOperand(writer, value, type)) {
        return false;
    }
    writer.Text(");\n");
    return true;
}

// An enum value is a struct: an int tag (the variant), then the payload
// fields. Arguments fill the payload in order, as in the SPIR-V backend:
// a vector into scalar fields is split into components and a struct into
// non-struct fields is spread over its fields; unfilled fields are zero.
bool WGSLBuilder::WriteEnumConstruct(TextWriter& writer, u32 instruction, u32 structType) {
    const u32 index = StructIndex(structType);
    const IR::IRProgram::StructTypeInfo& info = ir->structTypes[index];
    if (info.fieldCount == 0 || static_cast<CoreType>(ir->structFieldTypes[info.fieldOffset]) != CoreType::INT) {
        return Fail("WGSL enum struct must start with an int tag");
    }
    if (!WriteFullType(writer, CoreType::CUSTOM, structType, 0)) {
        return false;
    }
    writer.Text("(");
    writer.Number((ir->metadata[instruction] >> 16) & 0xFFFF);
    writer.Text("i");
    u32 field = 1;
    // Writes `expression` (of `type`) into the next fields.
    const auto fill = [&](const auto& writeValue, CoreType type, u32 valueStruct, u32 valueLength) -> bool {
        CoreType fieldType = CoreType::INVALID;
        u32 fieldStruct = 0;
        u32 fieldLength = 0;
        if (field >= info.fieldCount || !FieldShape(structType, field, fieldType, fieldStruct, fieldLength)) {
            return field >= info.fieldCount || false;
        }
        if (fieldType == type && fieldStruct == valueStruct && fieldLength == valueLength) {
            writer.Text(", ");
            if (!writeValue(-1)) {
                return false;
            }
            ++field;
            return true;
        }
        if (Width(type) > 1 && valueLength == 0 && fieldLength == 0 && Width(fieldType) == 1) {
            for (u32 component = 0; component < Width(type) && field < info.fieldCount; ++component) {
                if (!FieldShape(structType, field, fieldType, fieldStruct, fieldLength) || Width(fieldType) != 1) {
                    return Fail("WGSL enum payload does not fit its fields");
                }
                const bool convert = fieldType != Scalar(type);
                writer.Text(", ");
                if (convert) {
                    WriteType(writer, fieldType);
                    writer.Text("(");
                }
                if (!writeValue(static_cast<s32>(component))) {
                    return false;
                }
                writer.Text(convert ? ")" : "");
                ++field;
            }
            return true;
        }
        if (Width(type) == 1 && Width(fieldType) == 1 && valueLength == 0 && fieldLength == 0) {
            writer.Text(", ");
            WriteType(writer, fieldType);
            writer.Text("(");
            if (!writeValue(-1)) {
                return false;
            }
            writer.Text(")");
            ++field;
            return true;
        }
        return Fail("WGSL enum payload does not fit its fields");
    };
    for (u32 argument = 0; argument < 4; ++argument) {
        const u16 operand = ir->GetOperand(instruction, argument);
        if (operand == IR::InvalidOperand || operand == IR::UnusedOperand || field >= info.fieldCount) {
            continue;
        }
        CoreType type = CoreType::INVALID;
        if (!OperandType(operand, type)) {
            return false;
        }
        const u32 value = IR::IsConstant(operand) ? InvalidId : registerToValue[operand];
        const u32 valueStruct = value != InvalidId ? values.structTypes[value] : 0;
        const u32 valueLength = value != InvalidId ? values.arrayLengths[value] : 0;
        CoreType fieldType = CoreType::INVALID;
        u32 fieldStruct = 0;
        u32 fieldLength = 0;
        if (!FieldShape(structType, field, fieldType, fieldStruct, fieldLength)) {
            return false;
        }
        const auto component = [&](s32 item) {
            if (!WriteOperand(writer, operand)) {
                return false;
            }
            if (item >= 0) {
                writer.Bytes(&".x.y.z.w"[item * 2], 2);
            }
            return true;
        };
        if ((type == CoreType::CUSTOM || type == CoreType::ENUM) && fieldType != CoreType::CUSTOM &&
            fieldType != CoreType::ENUM && valueLength == 0) {
            // Spread a struct argument over the following fields.
            const u32 argumentStruct = StructIndex(valueStruct);
            for (u32 member = 0; argumentStruct != InvalidId &&
                 member < ir->structTypes[argumentStruct].fieldCount; ++member) {
                CoreType memberType = CoreType::INVALID;
                u32 memberStruct = 0;
                u32 memberLength = 0;
                if (!FieldShape(valueStruct, member, memberType, memberStruct, memberLength)) {
                    return false;
                }
                const auto memberValue = [&](s32 item) {
                    if (!WriteOperand(writer, operand)) {
                        return false;
                    }
                    writer.Text(".f");
                    writer.Number(member);
                    if (item >= 0) {
                        writer.Bytes(&".x.y.z.w"[item * 2], 2);
                    }
                    return true;
                };
                if (!fill(memberValue, memberType, memberStruct, memberLength)) {
                    return false;
                }
            }
            continue;
        }
        if (!fill(component, type, valueStruct, valueLength)) {
            return false;
        }
    }
    for (; field < info.fieldCount; ++field) {
        CoreType fieldType = CoreType::INVALID;
        u32 fieldStruct = 0;
        u32 fieldLength = 0;
        if (!FieldShape(structType, field, fieldType, fieldStruct, fieldLength)) {
            return false;
        }
        writer.Text(", ");
        if (!WriteFullType(writer, fieldType, fieldStruct, fieldLength)) {
            return false;
        }
        writer.Text("()");
    }
    writer.Text(")");
    return true;
}
