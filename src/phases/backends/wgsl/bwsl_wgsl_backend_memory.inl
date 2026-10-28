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

void WGSLBuilder::CopyValueShape(u32 target, u32 source) {
    values.types[target] = values.types[source];
    values.structTypes[target] = values.structTypes[source];
    values.arrayLengths[target] = values.arrayLengths[source];
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
    for (bool changed = true; changed;) {
        changed = false;
        for (u32 instruction = 0; instruction < ir->instructionCount; ++instruction) {
            const u16 destination = ir->destinations[instruction];
            const u16 source = ir->GetOperand(instruction, 0);
            if (IsCopy(ir->opcodes[instruction]) && valid(destination) && valid(source) &&
                (registerFlags[source] & RegisterPointer) && !(registerFlags[destination] & RegisterPointer)) {
                registerFlags[destination] |= RegisterPointer;
                changed = true;
            }
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
    for (u32 array = 0; array < ir->localArrayCount; ++array) {
        if (!valid(ir->localArrayRegisters[array])) {
            return Fail("WGSL local array register is out of bounds");
        }
        registerFlags[ir->localArrayRegisters[array]] |= RegisterVariable;
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
                (values.kinds[value] == static_cast<u8>(ValueKind::Value) &&
                 (values.variables[value] & VariableMutable) != 0));
    };
    const auto defineReference = [&](u32 parent, Accessor accessor, u16 operand, CoreType type,
                                     u32 structType, u32 arrayLength) {
        if (destination >= ir->registerCount || destination >= IR::RegisterLimit) {
            return Fail("WGSL pointer register is out of bounds");
        }
        if (registerToValue[destination] != InvalidId) {
            return Fail("WGSL pointers that are reassigned to another variable are not supported");
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
            const u32 source = valueOf(first);
            if (source != InvalidId && values.kinds[source] == static_cast<u8>(ValueKind::UniformAddress)) {
                if (!(registerFlags[destination] & RegisterVariable)) {
                    return Fail("WGSL uniform address copy was not prepared as a uniform access");
                }
                return DefineValue(instruction, static_cast<CoreType>(values.types[source]), ValueKind::Value,
                                   uniformAddresses.structTypes[source], uniformAddresses.lengths[source]);
            }
            if (source == InvalidId || values.kinds[source] != static_cast<u8>(ValueKind::Reference)) {
                handled = false;
                return true;
            }
            return defineReference(source, Accessor::Whole, 0xFFFF, static_cast<CoreType>(values.types[source]),
                                   values.structTypes[source], values.arrayLengths[source]);
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
    if (kind != ValueKind::Reference || values.parents[valueId] == InvalidId ||
        !WriteReference(writer, values.parents[valueId])) {
        return Fail("WGSL pointer has no valid access path");
    }
    const Accessor accessor = static_cast<Accessor>(values.accessors[valueId]);
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
    // Only the structs values use, and the structs they contain.
    for (u32 value = 0; value < values.count; ++value) {
        const CoreType valueType = static_cast<CoreType>(values.types[value]);
        const u32 root = valueType == CoreType::CUSTOM || valueType == CoreType::ENUM
            ? StructIndex(values.structTypes[value]) : InvalidId;
        if (root == InvalidId || states[root] != 0 ||
            values.kinds[value] == static_cast<u8>(ValueKind::UniformAddress)) {
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
            if (info.fieldCount == 0) {
                continue;
            }
            writer.Text("struct BWSLStruct");
            writer.Number(index);
            writer.Text(" {\n");
            for (u32 field = 0; field < info.fieldCount; ++field) {
                CoreType type = CoreType::INVALID;
                u32 structType = 0;
                u32 arrayLength = 0;
                if (!FieldShape(info.nameHash, field, type, structType, arrayLength)) {
                    return false;
                }
                writer.Text("    f");
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
    if (opcode == IR::OP_LOCAL_STORE) {
        writer.Indent();
        if (!WriteReference(writer, registerToValue[first])) {
            return false;
        }
        writer.Text(" = ");
        if (!WriteOperand(writer, second)) {
            return false;
        }
        writer.Text(";\n");
        return true;
    }
    if (opcode == IR::OP_ARRAY_STORE && destination < ir->registerCount &&
        registerToValue[destination] != InvalidId &&
        values.kinds[registerToValue[destination]] != static_cast<u8>(ValueKind::StorageRoot)) {
        writer.Indent();
        if (!WriteReference(writer, registerToValue[destination])) {
            return false;
        }
        writer.Text("[");
        if (!WriteOperand(writer, first)) {
            return false;
        }
        writer.Text("] = ");
        if (!WriteOperand(writer, second)) {
            return false;
        }
        writer.Text(";\n");
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
