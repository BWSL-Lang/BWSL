// Included by bwsl_wgsl_backend.cpp inside BWSL::WGSL.

u32 WGSLBuilder::UniformTypeSize(CoreType type, u32 structHash) {
    if (Width(type) != 0) {
        return Width(type) * 4;
    }
    if (type >= CoreType::MAT2 && type <= CoreType::MAT4) {
        return (static_cast<u32>(type) - static_cast<u32>(CoreType::MAT2) + 2) * 16;
    }
    if (type == CoreType::CUSTOM) {
        for (u32 index = 0; index < ir->structTypeCount; ++index) {
            if (ir->structTypes[index].nameHash == structHash) {
                return PrepareUniformLayout(index) ? uniformLayouts.sizes[index] : 0;
            }
        }
    }
    Fail("WGSL uniform has an unsupported type or missing std140 layout");
    return 0;
}

bool WGSLBuilder::PrepareUniformLayout(u32 structId) {
    if (uniformLayouts.states[structId] == 2) {
        return true;
    }
    if (uniformLayouts.states[structId] == 1) {
        return Fail("WGSL uniforms cannot contain recursive structures");
    }
    uniformLayouts.states[structId] = 1;
    const IR::IRProgram::StructTypeInfo& structure = ir->structTypes[structId];
    u32 offset = 0;
    u32 structAlignment = 16;
    for (u32 member = 0; member < structure.fieldCount; ++member) {
        const u32 field = structure.fieldOffset + member;
        const CoreType type = static_cast<CoreType>(ir->structFieldTypes[field]);
        const u32 size = UniformTypeSize(type, ir->structFieldTypeHashes[field]);
        if (size == 0 || size > 65536) {
            return Fail("WGSL uniform member size is invalid");
        }
        u32 alignment = Width(type) == 1 ? 4 : Width(type) == 2 ? 8 : 16;
        if (type == CoreType::CUSTOM) {
            alignment = 16;
        }
        const u32 length = ir->structFieldArraySizes[field];
        if (length != 0) {
            alignment = std::max(alignment, 16u);
        }
        offset = (offset + alignment - 1) & ~(alignment - 1);
        const u32 stride = length ? (size + 15) & ~15u : size;
        if (length > 65536 / stride || offset + stride * (length ? length : 1) > 65536) {
            return Fail("WGSL uniform structure exceeds 64 KiB");
        }
        uniformLayouts.offsets[field] = offset;
        uniformLayouts.strides[field] = stride;
        uniformLayouts.owners[field] = structure.nameHash;
        offset += stride * (length ? length : 1);
        structAlignment = std::max(structAlignment, alignment);
    }
    uniformLayouts.sizes[structId] = (offset + structAlignment - 1) & ~(structAlignment - 1);
    uniformLayouts.alignments[structId] = structAlignment;
    uniformLayouts.states[structId] = 2;
    return true;
}

bool WGSLBuilder::PrepareAdditionalResources() {
    u32 uniformMask = 0;
    u32 textureMask = 0;
    u32 depthMask = 0;
    u64 samplerMask = 0;
    u64 comparisonMask = 0;
    u64 ordinaryMask = 0;
    for (u32 instruction = 0; instruction < ir->instructionCount; ++instruction) {
        const u16 opcode = ir->opcodes[instruction];
        const u16 operand = ir->GetOperand(instruction, 0);
        if (opcode == IR::OP_LOAD_UNIFORM) {
            if (operand >= 32) {
                return Fail("WGSL uniform slot is out of bounds");
            }
            uniformMask |= 1u << operand;
        }
        if (!IsSampledTextureOpcode(opcode)) {
            continue;
        }
        const u32 slot = operand & 0x1FFFu;
        if ((operand & 0xE000u) != 0x6000u || slot >= 32) {
            return Fail("WGSL textures require a declared static texture slot");
        }
        textureMask |= 1u << slot;
        const bool comparison = opcode == IR::OP_TEX_SAMPLE_CMP;
        if (comparison) {
            depthMask |= 1u << slot;
        }
        if (opcode == IR::OP_TEX_FETCH || opcode == IR::OP_TEX_SIZE || opcode == IR::OP_TEX_LEVELS) {
            continue;
        }
        const u32 metadata = ir->metadata[instruction];
        const u32 binding = (metadata & 0x80000000u) ? (metadata & 0xFFFFu) : 32 + slot;
        if (binding >= 64) {
            return Fail("WGSL explicit sampler slot is out of bounds");
        }
        const u64 bit = u64(1) << binding;
        samplerMask |= bit;
        (comparison ? comparisonMask : ordinaryMask) |= bit;
    }
    if ((comparisonMask & ordinaryMask) != 0) {
        return Fail("WGSL requires separate ordinary and comparison samplers");
    }
    uniforms.count = std::popcount(uniformMask);
    textures.count = std::popcount(textureMask);
    samplers.count = std::popcount(samplerMask);
    if (uniforms.count != 0 && ir->structTypeCount != 0) {
        for (u32 structure = 0; structure < ir->structTypeCount; ++structure) {
            uniformLayouts.fieldCount = std::max(uniformLayouts.fieldCount,
                ir->structTypes[structure].fieldOffset + ir->structTypes[structure].fieldCount);
        }
        uniformLayouts.sizes = static_cast<u32*>(Allocate(ir->structTypeCount, sizeof(u32)));
        uniformLayouts.alignments = static_cast<u32*>(Allocate(ir->structTypeCount, sizeof(u32)));
        uniformLayouts.states = static_cast<u8*>(Allocate(ir->structTypeCount, sizeof(u8)));
        uniformLayouts.offsets = static_cast<u32*>(Allocate(uniformLayouts.fieldCount, sizeof(u32)));
        uniformLayouts.strides = static_cast<u32*>(Allocate(uniformLayouts.fieldCount, sizeof(u32)));
        uniformLayouts.owners = static_cast<u32*>(Allocate(uniformLayouts.fieldCount, sizeof(u32)));
        if (errorMessage[0] != '\0') {
            return false;
        }
        std::memset(uniformLayouts.states, 0, ir->structTypeCount);
        std::memset(uniformLayouts.owners, 0, uniformLayouts.fieldCount * sizeof(u32));
    }
    uniforms.types = static_cast<u16*>(Allocate(uniforms.count, sizeof(u16)));
    uniforms.slots = static_cast<u32*>(Allocate(uniforms.count, sizeof(u32)));
    uniforms.byteSizes = static_cast<u32*>(Allocate(uniforms.count, sizeof(u32)));
    uniforms.arrayLengths = static_cast<u32*>(Allocate(uniforms.count, sizeof(u32)));
    uniforms.structTypes = static_cast<u32*>(Allocate(uniforms.count, sizeof(u32)));
    textures.types = static_cast<u16*>(Allocate(textures.count, sizeof(u16)));
    textures.slots = static_cast<u32*>(Allocate(textures.count, sizeof(u32)));
    textures.depth = static_cast<u8*>(Allocate(textures.count, sizeof(u8)));
    samplers.bindings = static_cast<u32*>(Allocate(samplers.count, sizeof(u32)));
    samplers.comparison = static_cast<u8*>(Allocate(samplers.count, sizeof(u8)));
    if (errorMessage[0] != '\0') {
        return false;
    }
    u32 uniformId = 0;
    u32 textureId = 0;
    for (u32 slot = 0; slot < 32; ++slot) {
        if (!((uniformMask | textureMask) & (1u << slot))) {
            continue;
        }
        const ResourceData* uniform = nullptr;
        const ResourceData* texture = nullptr;
        if (symbols != nullptr) {
            for (u32 index = 0; index < symbols->resources.count; ++index) {
                const ResourceData& declaration = symbols->resources[index];
                if (declaration.bindingIndex != slot) {
                    continue;
                }
                if (declaration.type == ResourceBinding::UniformBuffer) {
                    uniform = &declaration;
                } else if (declaration.type == ResourceBinding::Texture) {
                    texture = &declaration;
                }
            }
        }
        if (uniformMask & (1u << slot)) {
            if (uniform == nullptr) {
                return Fail("WGSL uniform is missing its declaration");
            }
            const CoreType type = static_cast<CoreType>(uniform->coreType);
            const u32 size = UniformTypeSize(type, uniform->structTypeHash);
            const u32 stride = (size + 15) & ~15u;
            if (size == 0 || (uniform->arraySize != 0 && uniform->arraySize > 65536 / stride)) {
                return Fail("WGSL uniform layout exceeds the 64 KiB portable limit");
            }
            uniformForSlot[slot] = uniformId;
            uniforms.types[uniformId] = static_cast<u16>(type);
            uniforms.slots[uniformId] = slot;
            uniforms.arrayLengths[uniformId] = uniform->arraySize;
            uniforms.structTypes[uniformId] = uniform->structTypeHash;
            uniforms.byteSizes[uniformId] = stride * (uniform->arraySize ? uniform->arraySize : 1);
            ++uniformId;
        }
        if (textureMask & (1u << slot)) {
            if (texture == nullptr) {
                return Fail("WGSL texture is missing its declaration");
            }
            const CoreType type = static_cast<CoreType>(texture->coreType);
            if (type != CoreType::TEXTURE2D && type != CoreType::TEXTURE3D &&
                type != CoreType::TEXTURECUBE && type != CoreType::TEXTURE2DARRAY) {
                return Fail("WGSL supports sampled 2D, 3D, cube and 2D-array textures");
            }
            if ((depthMask & (1u << slot)) && type == CoreType::TEXTURE3D) {
                return Fail("WGSL has no 3D depth texture type");
            }
            textureForSlot[slot] = textureId;
            textures.types[textureId] = static_cast<u16>(type);
            textures.slots[textureId] = slot;
            textures.depth[textureId] = (depthMask >> slot) & 1;
            ++textureId;
        }
    }
    u32 samplerId = 0;
    for (u32 binding = 0; binding < 64; ++binding) {
        if (!(samplerMask & (u64(1) << binding))) {
            continue;
        }
        samplers.bindings[samplerId] = binding;
        samplers.comparison[samplerId] = (comparisonMask >> binding) & 1;
        ++samplerId;
    }
    return true;
}

bool WGSLBuilder::PrepareUniformInstruction(u32 instruction, bool& handled) {
    handled = true;
    const u16 opcode = ir->opcodes[instruction];
    const u16 first = ir->GetOperand(instruction, 0);
    const u16 second = ir->GetOperand(instruction, 1);
    const u16 third = ir->GetOperand(instruction, 2);
    const u16 destination = ir->destinations[instruction];
    const CoreType type = static_cast<CoreType>(ir->registerTypes[destination]);
    if (opcode == IR::OP_LOAD_UNIFORM) {
        const u32 uniform = uniformForSlot[first];
        const bool address = uniforms.arrayLengths[uniform] != 0 || type == CoreType::CUSTOM;
        if (!DefineValue(instruction, type, address ? ValueKind::UniformAddress : ValueKind::Value)) {
            return false;
        }
        const u32 value = values.count - 1;
        values.resourceIds[value] = uniform;
        uniformAddresses.lengths[value] = uniforms.arrayLengths[uniform];
        uniformAddresses.structTypes[value] = uniforms.structTypes[uniform];
        return true;
    }
    const bool field = opcode == IR::OP_STRUCT_EXTRACT || opcode == IR::OP_STORAGE_FIELD ||
                       opcode == IR::OP_STRUCT_ARRAY_EXTRACT;
    const bool index = opcode == IR::OP_STORAGE_INDEX || opcode == IR::OP_ARRAY_LOAD;
    const bool load = opcode == IR::OP_STORAGE_LOAD;
    if (!(field || index || load || IsCopy(opcode)) || IR::IsConstant(first) ||
        first >= ir->registerCount || registerToValue[first] == InvalidId ||
        values.kinds[registerToValue[first]] != static_cast<u8>(ValueKind::UniformAddress)) {
        handled = false;
        return true;
    }
    const u32 base = registerToValue[first];
    u32 byteOffset = 0;
    u32 length = uniformAddresses.lengths[base];
    u32 structHash = uniformAddresses.structTypes[base];
    u32 stride = 0;
    u16 subscript = 0xFFFF;
    bool address = IsCopy(opcode) || opcode == IR::OP_STORAGE_FIELD || opcode == IR::OP_STORAGE_INDEX;
    if (field) {
        u32 structId = 0;
        while (structId < ir->structTypeCount && ir->structTypes[structId].nameHash != structHash) {
            ++structId;
        }
        if (structId == ir->structTypeCount || second >= ir->structTypes[structId].fieldCount) {
            return Fail("WGSL uniform field is missing layout metadata");
        }
        const u32 fieldId = ir->structTypes[structId].fieldOffset + second;
        byteOffset = uniformLayouts.offsets[fieldId];
        length = ir->structFieldArraySizes[fieldId];
        structHash = ir->structFieldTypeHashes[fieldId];
        if (opcode == IR::OP_STRUCT_ARRAY_EXTRACT) {
            if (length == 0 || !RequireIndex(third)) {
                return Fail("WGSL uniform array field requires an integer index");
            }
            subscript = third;
            stride = uniformLayouts.strides[fieldId];
        } else {
            address |= length != 0 || type == CoreType::CUSTOM;
        }
    } else if (index) {
        if (length == 0 || !RequireIndex(second)) {
            return Fail("WGSL uniform indexing requires a fixed-size array");
        }
        subscript = second;
        stride = (UniformTypeSize(type, structHash) + 15) & ~15u;
        address |= type == CoreType::CUSTOM;
    } else if (load) {
        address = type == CoreType::CUSTOM;
    }
    if (!DefineValue(instruction, type, address ? ValueKind::UniformAddress : ValueKind::Value)) {
        return false;
    }
    const u32 value = values.count - 1;
    values.resourceIds[value] = values.resourceIds[base];
    uniformAddresses.bases[value] = base;
    uniformAddresses.byteOffsets[value] = byteOffset;
    uniformAddresses.strides[value] = stride;
    uniformAddresses.lengths[value] = subscript == 0xFFFF ? length : 0;
    uniformAddresses.structTypes[value] = structHash;
    values.indexOperands[value] = subscript;
    return true;
}

bool WGSLBuilder::WriteUniformOffset(TextWriter& writer, u32 value) {
    const u32 base = uniformAddresses.bases[value];
    if (base != InvalidId) {
        if (!WriteUniformOffset(writer, base)) {
            return false;
        }
        writer.Text(" + ");
    }
    writer.Number(uniformAddresses.byteOffsets[value] / 4);
    writer.Text("u");
    if (values.indexOperands[value] != 0xFFFF) {
        writer.Text(" + u32(");
        if (!WriteOperand(writer, values.indexOperands[value])) {
            return false;
        }
        writer.Text(") * ");
        writer.Number(uniformAddresses.strides[value] / 4);
        writer.Text("u");
    }
    return true;
}

bool WGSLBuilder::WriteUniformLoad(TextWriter& writer, u32 value) {
    const CoreType type = static_cast<CoreType>(values.types[value]);
    const bool matrix = type >= CoreType::MAT2 && type <= CoreType::MAT4;
    const u32 columns = matrix ? static_cast<u32>(type) - static_cast<u32>(CoreType::MAT2) + 2 : 1;
    const u32 width = matrix ? columns : Width(type);
    const CoreType scalar = matrix ? CoreType::FLOAT : Scalar(type);
    u32 staticOffset = 0;
    for (u32 address = value; address != InvalidId; address = uniformAddresses.bases[address]) {
        staticOffset += uniformAddresses.byteOffsets[address];
        if ((uniformAddresses.strides[address] & 15u) != 0) {
            return Fail("WGSL uniform dynamic stride is not aligned to 16 bytes");
        }
    }
    const u32 component = (staticOffset / 4) % 4;
    if (component + width > 4) {
        return Fail("WGSL uniform value crosses an aligned 16-byte row");
    }
    if (matrix) {
        WriteType(writer, type);
        writer.Text("(");
    }
    for (u32 column = 0; column < columns; ++column) {
        if (column != 0) {
            writer.Text(", ");
        }
        if (scalar != CoreType::UINT && scalar != CoreType::BOOL) {
            writer.Text("bitcast<");
            WriteType(writer, matrix ? Shape(CoreType::FLOAT, width) : type);
            writer.Text(">(");
        }
        writer.Text("bwsl_uniform_");
        writer.Number(uniforms.slots[values.resourceIds[value]]);
        writer.Text("[(");
        if (!WriteUniformOffset(writer, value)) {
            return false;
        }
        writer.Text(") / 4u");
        if (column != 0) {
            writer.Text(" + ");
            writer.Number(column);
            writer.Text("u");
        }
        writer.Text("]");
        if (width != 4) {
            writer.Text(".");
            writer.Bytes(&"xyzw"[component], width);
        }
        if (scalar != CoreType::UINT && scalar != CoreType::BOOL) {
            writer.Text(")");
        }
        if (scalar == CoreType::BOOL) {
            writer.Text(" != ");
            WriteType(writer, Shape(CoreType::UINT, width));
            writer.Text("(0u)");
        }
    }
    if (matrix) {
        writer.Text(")");
    }
    return true;
}

bool WGSLBuilder::PrepareTextureInstruction(u32 instruction) {
    const u16 opcode = ir->opcodes[instruction];
    const u32 texture = textureForSlot[ir->GetOperand(instruction, 0) & 0x1FFFu];
    const CoreType textureType = static_cast<CoreType>(textures.types[texture]);
    const u16 destination = ir->destinations[instruction];
    const CoreType type = static_cast<CoreType>(ir->registerTypes[destination]);
    const CoreType expectedResult = opcode == IR::OP_TEX_SIZE ? StdLib::TextureSizeType(textureType) :
                                    opcode == IR::OP_TEX_LEVELS ? CoreType::INT : CoreType::FLOAT4;
    if (type != expectedResult) {
        return Fail("WGSL texture operation has an incompatible result type");
    }
    if (opcode != IR::OP_TEX_LEVELS) {
        CoreType argumentType = CoreType::INVALID;
        if (!OperandType(ir->GetOperand(instruction, 1), argumentType)) {
            return false;
        }
        if (opcode == IR::OP_TEX_SIZE) {
            if (!RequireIndex(ir->GetOperand(instruction, 1))) {
                return false;
            }
        } else {
            const u32 width = textureType == CoreType::TEXTURE2D ? 2 : 3;
            const CoreType expected = Shape(opcode == IR::OP_TEX_FETCH ? CoreType::INT : CoreType::FLOAT, width);
            if (argumentType != expected) {
                return Fail("WGSL texture coordinate has the wrong shape or scalar type");
            }
        }
    }
    if ((opcode == IR::OP_TEX_SAMPLE || opcode == IR::OP_TEX_SAMPLE_BIAS ||
         opcode == IR::OP_TEX_SAMPLE_CMP) && stage != ShaderStage::Fragment) {
        return Fail("WGSL implicit-LOD texture sampling requires a fragment shader");
    }
    if (opcode == IR::OP_TEX_SAMPLE_LOD || opcode == IR::OP_TEX_SAMPLE_BIAS ||
        opcode == IR::OP_TEX_SAMPLE_CMP || opcode == IR::OP_TEX_FETCH) {
        CoreType extra = CoreType::INVALID;
        if (!OperandType(ir->GetOperand(instruction, 2), extra) || Width(extra) != 1 || extra == CoreType::BOOL) {
            return Fail("WGSL texture level, bias or comparison must be scalar");
        }
        if (opcode == IR::OP_TEX_FETCH && !RequireIndex(ir->GetOperand(instruction, 2))) {
            return false;
        }
    }
    if (opcode == IR::OP_TEX_GATHER && !textures.depth[texture]) {
        const u16 component = ir->GetOperand(instruction, 2);
        CoreType componentType = CoreType::INVALID;
        if (!OperandType(component, componentType) ||
            !(IR::IsIntConstant(component) || IR::IsUintConstant(component))) {
            return Fail("WGSL textureGather component must be a literal integer");
        }
        const u32 index = component & 0x1FFFu;
        const u32 value = componentType == CoreType::INT ? ir->intConstants[index] : ir->uintConstants[index];
        if (value > 3) {
            return Fail("WGSL textureGather component must be in the range 0 through 3");
        }
    }
    if (opcode == IR::OP_TEX_SAMPLE_GRAD) {
        CoreType dx = CoreType::INVALID;
        CoreType dy = CoreType::INVALID;
        const CoreType expected = textureType == CoreType::TEXTURE3D || textureType == CoreType::TEXTURECUBE
            ? CoreType::FLOAT3 : CoreType::FLOAT2;
        if (!OperandType(ir->GetOperand(instruction, 2), dx) || !OperandType(ir->GetOperand(instruction, 3), dy) ||
            dx != expected || dy != expected) {
            return Fail("WGSL texture gradients have an incompatible type");
        }
    }
    if (textureType == CoreType::TEXTURECUBE && opcode == IR::OP_TEX_FETCH) {
        return Fail("WGSL textureLoad does not accept cube textures");
    }
    return DefineValue(instruction, type, ValueKind::Value);
}

bool WGSLBuilder::WriteTextureExpression(TextWriter& writer, u32 instruction) {
    const u16 opcode = ir->opcodes[instruction];
    const u32 texture = textureForSlot[ir->GetOperand(instruction, 0) & 0x1FFFu];
    const CoreType textureType = static_cast<CoreType>(textures.types[texture]);
    const bool array = textureType == CoreType::TEXTURE2DARRAY;
    const bool depth = textures.depth[texture] != 0;
    const bool query = opcode == IR::OP_TEX_SIZE || opcode == IR::OP_TEX_LEVELS;
    const bool load = opcode == IR::OP_TEX_FETCH;
    const bool comparison = opcode == IR::OP_TEX_SAMPLE_CMP;
    const u16 coords = ir->GetOperand(instruction, 1);
    if (query || depth) {
        writer.Text(opcode == IR::OP_TEX_LEVELS ? "i32(" :
                    !query ? "vec4<f32>(" :
                    StdLib::TextureSizeType(textureType) == CoreType::INT3 ? "vec3<i32>(" : "vec2<i32>(");
    }
    // textureDimensions of a 2D array is its layer size; the layer count
    // comes from textureNumLayers.
    if (opcode == IR::OP_TEX_SIZE && array) {
        writer.Text("vec3<u32>(");
    }
    const char* function = opcode == IR::OP_TEX_SAMPLE ? "textureSample" :
                           opcode == IR::OP_TEX_SAMPLE_LOD ? "textureSampleLevel" :
                           opcode == IR::OP_TEX_SAMPLE_BIAS ? "textureSampleBias" :
                           opcode == IR::OP_TEX_SAMPLE_GRAD ? "textureSampleGrad" :
                           comparison ? "textureSampleCompare" :
                           opcode == IR::OP_TEX_GATHER ? "textureGather" :
                           load ? "textureLoad" : opcode == IR::OP_TEX_SIZE ? "textureDimensions" : "textureNumLevels";
    writer.Text(function);
    writer.Text("(");
    if (opcode == IR::OP_TEX_GATHER && !depth) {
        if (!WriteConvertedOperand(writer, ir->GetOperand(instruction, 2), CoreType::INT)) {
            return false;
        }
        writer.Text(", ");
    }
    writer.Text("bwsl_texture_");
    writer.Number(textures.slots[texture]);
    if (!query && !load) {
        const u32 metadata = ir->metadata[instruction];
        writer.Text(", bwsl_sampler_");
        writer.Number((metadata & 0x80000000u) ? (metadata & 0xFFFFu) : 32 + textures.slots[texture]);
    }
    if (!query) {
        writer.Text(", ");
        if (!WriteOperand(writer, coords)) {
            return false;
        }
        if (array) {
            writer.Text(".xy, i32(");
            if (!load) {
                // BWSL/SPIR-V sampling selects the nearest array layer.
                // Vulkan permits floor(layer + 0.5); integer texel fetches
                // already carry a discrete layer and require no rounding.
                writer.Text("floor(");
            }
            if (!WriteOperand(writer, coords)) {
                return false;
            }
            writer.Text(load ? ".z)" : ".z + 0.5f))");
        }
    }
    if (opcode == IR::OP_TEX_SAMPLE_LOD || opcode == IR::OP_TEX_SAMPLE_BIAS || comparison || load) {
        writer.Text(", ");
        if (!WriteConvertedOperand(writer, ir->GetOperand(instruction, 2),
                                   load || (depth && !comparison) ? CoreType::INT : CoreType::FLOAT)) {
            return false;
        }
    } else if (opcode == IR::OP_TEX_SAMPLE_GRAD) {
        for (u32 argument = 2; argument < 4; ++argument) {
            writer.Text(", ");
            if (!WriteOperand(writer, ir->GetOperand(instruction, argument))) {
                return false;
            }
        }
    } else if (opcode == IR::OP_TEX_SIZE) {
        writer.Text(", ");
        if (!WriteConvertedOperand(writer, coords, CoreType::INT)) {
            return false;
        }
    }
    writer.Text(")");
    if (opcode == IR::OP_TEX_SIZE && array) {
        writer.Text(", textureNumLayers(bwsl_texture_");
        writer.Number(textures.slots[texture]);
        writer.Text("))");
    }
    if (query || depth) {
        writer.Text(")");
    }
    return true;
}

bool WGSLBuilder::WriteAdditionalResources(TextWriter& writer) {
    for (u32 uniform = 0; uniform < uniforms.count; ++uniform) {
        writer.Text("@group(1) @binding(");
        writer.Number(uniforms.slots[uniform]);
        writer.Text(") var<uniform> bwsl_uniform_");
        writer.Number(uniforms.slots[uniform]);
        writer.Text(": array<vec4<u32>, ");
        writer.Number(uniforms.byteSizes[uniform] / 16);
        writer.Text(">;\n\n");
    }
    for (u32 texture = 0; texture < textures.count; ++texture) {
        writer.Text("@group(2) @binding(");
        writer.Number(textures.slots[texture]);
        writer.Text(") var bwsl_texture_");
        writer.Number(textures.slots[texture]);
        writer.Text(": texture_");
        if (textures.depth[texture]) {
            writer.Text("depth_");
        }
        const CoreType type = static_cast<CoreType>(textures.types[texture]);
        writer.Text(type == CoreType::TEXTURE3D ? "3d" : type == CoreType::TEXTURECUBE ? "cube" :
                    type == CoreType::TEXTURE2DARRAY ? "2d_array" : "2d");
        if (!textures.depth[texture]) {
            writer.Text("<f32>");
        }
        writer.Text(";\n\n");
    }
    for (u32 sampler = 0; sampler < samplers.count; ++sampler) {
        writer.Text("@group(3) @binding(");
        writer.Number(samplers.bindings[sampler]);
        writer.Text(") var bwsl_sampler_");
        writer.Number(samplers.bindings[sampler]);
        writer.Text(samplers.comparison[sampler] ? ": sampler_comparison;\n\n" : ": sampler;\n\n");
    }
    return true;
}
