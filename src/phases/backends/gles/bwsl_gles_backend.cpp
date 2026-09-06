// BWSL Direct GLSL ES 300 Backend - Implementation
// Emits GLSL ES directly from IR, bypassing SPIR-V entirely

#include "bwsl_gles_backend.h"

namespace BWSL {
namespace GLES {

static const char* InterpolationQualifier(InterpolationMode interpolation) {
    switch (interpolation) {
        case InterpolationMode::Flat:
            return "flat ";
        case InterpolationMode::NoPerspective:
            return "noperspective ";
        case InterpolationMode::Default:
        default:
            return "";
    }
}

static bool UsesNoPerspectiveInterpolation(const IR::PassVaryingContext* varyings) {
    if (!varyings) {
        return false;
    }
    for (u32 i = 0; i < varyings->count; i++) {
        if (varyings->varyings[i].interpolation == InterpolationMode::NoPerspective) {
            return true;
        }
    }
    return false;
}

// ============================================================================
// Header Emission
// ============================================================================

void GLESBuilder::EmitHeader() {
    if (stage == ShaderStage::Compute) {
        out.Lit("#version 310 es\n");
    } else {
        out.Lit("#version 300 es\n");
        if (UsesNoPerspectiveInterpolation(varyings)) {
            out.Lit("#extension GL_NV_shader_noperspective_interpolation : require\n");
        }
    }
    out.Lit("precision highp float;\n");
    out.Lit("precision highp int;\n");
    if (stage == ShaderStage::Compute) {
        out.Lit("layout(local_size_x = ");
        out.Uint(workgroupSizeX);
        out.Lit(", local_size_y = ");
        out.Uint(workgroupSizeY);
        out.Lit(", local_size_z = ");
        out.Uint(workgroupSizeZ);
        out.Lit(") in;\n");
    }
    out.NL(0);
    EmitStructDeclarations();

    bool finiteHelper = false, normalHelper = false;
    for (u32 i = 0; i < ir->instructionCount; ++i) {
        finiteHelper |= ir->opcodes[i] == IR::OP_ISFINITE;
        normalHelper |= ir->opcodes[i] == IR::OP_ISNORMAL;
    }
    for (u32 kind = 0; kind < 2; ++kind) {
        if (!(kind == 0 ? finiteHelper : normalHelper)) continue;
        const char* name = kind == 0 ? "bwsl_isfinite" : "bwsl_isnormal";
        out.Lit("bool "); out.Str(name); out.Lit("(float x) { ");
        if (kind == 0) out.Lit("return !isnan(x) && !isinf(x); }\n");
        else out.Lit("uint e = floatBitsToUint(x) & 2139095040u; return e != 0u && e != 2139095040u; }\n");
        for (u32 n = 2; n <= 4; ++n) {
            out.Lit("bvec"); out.Uint(n); out.Chr(' '); out.Str(name);
            out.Lit("(vec"); out.Uint(n); out.Lit(" x) { return bvec"); out.Uint(n); out.Chr('(');
            for (u32 c = 0; c < n; ++c) {
                if (c) out.Lit(", "); out.Str(name); out.Lit("(x."); out.Chr(Str::SWIZZLE[c]); out.Chr(')');
            }
            out.Lit("); }\n");
        }
    }

    bool needsFrexp = false;
    bool needsGatherPolyfill = false;
    for (u32 i = 0; i < ir->instructionCount; i++) {
        if (ir->opcodes[i] == IR::OP_FREXP_STRUCT) {
            needsFrexp = true;
        } else if (ir->opcodes[i] == IR::OP_TEX_GATHER ||
                   ir->opcodes[i] == IR::OP_TEX_GATHER_OFFSET) {
            needsGatherPolyfill = true;
        }
    }
    if (needsFrexp) {
        out.Lit("BwslFrexpResult bwsl_frexp(float x) {\n");
        out.Lit("    int e = (x == 0.0) ? 0 : (int(floor(log2(abs(x)))) + 1);\n");
        out.Lit("    float m = (x == 0.0) ? 0.0 : (x * exp2(float(-e)));\n");
        out.Lit("    return BwslFrexpResult(m, e);\n");
        out.Lit("}\n\n");
    }
    if (needsGatherPolyfill) {
        out.Lit("float bwsl_gather_component(vec4 value, int component) {\n");
        out.Lit("    if (component == 1) return value.y;\n");
        out.Lit("    if (component == 2) return value.z;\n");
        out.Lit("    if (component == 3) return value.w;\n");
        out.Lit("    return value.x;\n");
        out.Lit("}\n\n");
        out.Lit("vec4 bwsl_texture_gather_offset(sampler2D tex, vec2 uv, int component, ivec2 offset) {\n");
        out.Lit("    vec2 size = vec2(textureSize(tex, 0));\n");
        out.Lit("    vec2 invSize = 1.0 / size;\n");
        out.Lit("    ivec2 base = ivec2(floor(uv * size - vec2(0.5))) + offset;\n");
        out.Lit("    vec2 p00 = (vec2(base) + vec2(0.5)) * invSize;\n");
        out.Lit("    vec4 s00 = texture(tex, p00);\n");
        out.Lit("    vec4 s10 = texture(tex, p00 + vec2(invSize.x, 0.0));\n");
        out.Lit("    vec4 s01 = texture(tex, p00 + vec2(0.0, invSize.y));\n");
        out.Lit("    vec4 s11 = texture(tex, p00 + invSize);\n");
        out.Lit("    return vec4(bwsl_gather_component(s00, component), bwsl_gather_component(s10, component), bwsl_gather_component(s11, component), bwsl_gather_component(s01, component));\n");
        out.Lit("}\n\n");
        out.Lit("vec4 bwsl_texture_gather(sampler2D tex, vec2 uv, int component) {\n");
        out.Lit("    return bwsl_texture_gather_offset(tex, uv, component, ivec2(0));\n");
        out.Lit("}\n\n");
    }
}

static bool GLESIsValidIdent(const std::string& s) {
    if (s.empty()) return false;
    char first = s[0];
    if (!((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') || first == '_')) {
        return false;
    }
    for (char c : s) {
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return true;
}

void GLESBuilder::EmitStructTypeName(u32 typeHash) {
    std::string name = ReverseLookup::GetString(typeHash);
    if (GLESIsValidIdent(name)) {
        out.Str(name.c_str());
    } else {
        out.Lit("Struct_");
        out.Uint(typeHash);
    }
}

void GLESBuilder::EmitStructFieldName(u32 fieldHash) {
    std::string name = ReverseLookup::GetString(fieldHash);
    if (GLESIsValidIdent(name)) {
        out.Str(name.c_str());
    } else {
        out.Lit("field_");
        out.Uint(fieldHash);
    }
}

void GLESBuilder::EmitStructFieldNameByIndex(u32 structHash, u16 fieldIdx) {
    if (structHash != 0 && ir->structTypes && ir->structFieldNameHashes) {
        for (u32 s = 0; s < ir->structTypeCount; s++) {
            const IR::IRProgram::StructTypeInfo& info = ir->structTypes[s];
            if (info.nameHash == structHash && fieldIdx < info.fieldCount) {
                EmitStructFieldName(ir->structFieldNameHashes[info.fieldOffset + fieldIdx]);
                return;
            }
        }
    }

    out.Lit("field_");
    out.Uint(fieldIdx);
}

void GLESBuilder::EmitRegisterType(u16 reg) {
    CoreType regType = (ir->registerTypes && reg < ir->registerCount)
                           ? static_cast<CoreType>(ir->registerTypes[reg])
                           : CoreType::INVALID;
    if ((regType == CoreType::CUSTOM || regType == CoreType::ENUM) &&
        ir->registerStructTypes && reg < ir->registerCount &&
        ir->registerStructTypes[reg] != 0) {
        EmitStructTypeName(ir->registerStructTypes[reg]);
        return;
    }
    EmitType(static_cast<u16>(regType));
}

void GLESBuilder::EmitTextureLevelsUniformName(u16 texReg) {
    out.Lit("bwsl_texture_levels_");
    if ((texReg & 0xF000) == 0x2000) {
        out.Uint(texReg & 0x0FFF);
    } else {
        out.Lit("dynamic");
    }
}

void GLESBuilder::EmitStructDeclarations() {
    if (!ir->structTypes || ir->structTypeCount == 0) {
        return;
    }

    for (u32 s = 0; s < ir->structTypeCount; s++) {
        const IR::IRProgram::StructTypeInfo& info = ir->structTypes[s];
        out.Lit("struct ");
        EmitStructTypeName(info.nameHash);
        out.Lit(" {\n");
        for (u32 f = 0; f < info.fieldCount; f++) {
            u32 fieldIdx = info.fieldOffset + f;
            out.Lit("    ");
            CoreType fieldType = static_cast<CoreType>(ir->structFieldTypes[fieldIdx]);
            u32 fieldTypeHash = ir->structFieldTypeHashes[fieldIdx];
            if ((fieldType == CoreType::CUSTOM || fieldType == CoreType::ENUM) &&
                fieldTypeHash != 0) {
                EmitStructTypeName(fieldTypeHash);
            } else {
                EmitType(static_cast<u16>(fieldType));
            }
            out.Chr(' ');
            EmitStructFieldName(ir->structFieldNameHashes[fieldIdx]);
            out.Lit(";\n");
        }
        out.Lit("};\n\n");
    }
}

// ============================================================================
// Interface Emission - Inputs/Outputs/Uniforms
// ============================================================================

void GLESBuilder::EmitInputs() {
    if (stage == ShaderStage::Vertex) {
        // Vertex inputs come from analysis (used attribute mask and types)
        if (analysis) {
            for (u32 i = 0; i < 16; i++) {
                if (analysis->usedAttributeMask & (1 << i)) {
                    CoreType type = static_cast<CoreType>(analysis->attributeTypes[i]);
                    if (type == CoreType::VOID || type == CoreType::INVALID) {
                        type = CoreType::FLOAT4;  // Fallback
                    }

                    out.Lit("layout(location = ");
                    out.Uint(i);
                    out.Lit(") in ");
                    EmitType(static_cast<u16>(type));
                    out.Lit(" attr");
                    out.Uint(i);
                    out.Lit(";\n");
                }
            }
        }
    } else if (stage == ShaderStage::Fragment) {
        // Fragment inputs are varyings from vertex shader
        if (varyings) {
            for (u32 i = 0; i < varyings->count; i++) {
                out.Str(InterpolationQualifier(varyings->varyings[i].interpolation));
                out.Lit("in ");
                EmitType(static_cast<u16>(varyings->varyings[i].type));
                out.Lit(" v_");
                out.Str(varyings->varyings[i].name);
                out.Lit(";\n");
            }
        }

        // Scan IR for any LOAD_INPUT slots not in varyings array
        // IR uses OutputSlot values: VARYING0=2, VARYING1=3, etc.
        // Convert to VaryingInfo slots (0-based) for comparison
        u32 usedSlots = 0;  // Bitmask of used VaryingInfo slots (0-based)
        for (u32 i = 0; i < ir->instructionCount; i++) {
            if (ir->opcodes[i] == IR::OP_LOAD_INPUT) {
                u16 irSlot = ir->GetOperand(i, 0);
                if (irSlot >= 2 && irSlot < 0x80) {  // VARYING0=2 and up, not a builtin
                    u16 varyingSlot = irSlot - 2;  // Convert to 0-based
                    if (varyingSlot < 32) {
                        usedSlots |= (1u << varyingSlot);
                    }
                }
            }
        }

        // Emit declarations for slots not in varyings
        for (u32 slot = 0; slot < 32; slot++) {
            if (!(usedSlots & (1u << slot))) continue;

            // Check if this slot is already in varyings (already 0-based)
            bool inVaryings = false;
            if (varyings) {
                for (u32 i = 0; i < varyings->count; i++) {
                    if (varyings->varyings[i].slot == slot) {
                        inVaryings = true;
                        break;
                    }
                }
            }

            if (!inVaryings) {
                // Emit declaration for this slot - default to float
                out.Lit("in float v_slot");
                out.Uint(slot);
                out.Lit(";\n");
            }
        }
    }
    out.NL(0);
}

void GLESBuilder::EmitOutputs() {
    if (stage == ShaderStage::Vertex) {
        // Vertex outputs are varyings to fragment shader
        if (varyings) {
            for (u32 i = 0; i < varyings->count; i++) {
                out.Str(InterpolationQualifier(varyings->varyings[i].interpolation));
                out.Lit("out ");
                EmitType(static_cast<u16>(varyings->varyings[i].type));
                out.Lit(" v_");
                out.Str(varyings->varyings[i].name);
                out.Lit(";\n");
            }
        }

        // Scan IR for any STORE_OUTPUT slots not in varyings array
        // These need declarations too
        // IR uses OutputSlot values: POSITION=0, VARYING0=2, VARYING1=3, etc.
        // Convert to VaryingInfo slots (0-based) for comparison
        u32 usedSlots = 0;  // Bitmask of used VaryingInfo slots (0-based)
        for (u32 i = 0; i < ir->instructionCount; i++) {
            if (ir->opcodes[i] == IR::OP_STORE_OUTPUT) {
                u16 irSlot = ir->GetOperand(i, 0);
                if (irSlot >= 2 && irSlot < 32) {  // VARYING0=2 and up (slot 0 is gl_Position)
                    u16 varyingSlot = irSlot - 2;  // Convert to 0-based
                    usedSlots |= (1u << varyingSlot);
                }
            }
        }

        // Emit declarations for slots not in varyings
        for (u32 slot = 0; slot < 32; slot++) {
            if (!(usedSlots & (1u << slot))) continue;

            // Check if this slot is already in varyings (already 0-based)
            bool inVaryings = false;
            if (varyings) {
                for (u32 i = 0; i < varyings->count; i++) {
                    if (varyings->varyings[i].slot == slot) {
                        inVaryings = true;
                        break;
                    }
                }
            }

            if (!inVaryings) {
                // Emit declaration for this slot - default to float
                out.Lit("out float v_slot");
                out.Uint(slot);
                out.Lit(";\n");
            }
        }
    } else if (stage == ShaderStage::Fragment) {
        // Fragment color attachments.
        bool emittedColor = false;
        if (analysis) {
            for (u8 location = 0; location < FragmentOutput::MAX_COLOR_ATTACHMENTS; location++) {
                u8 slot = OutputSlot::FragmentColor(location);
                if (!(analysis->usedOutputMask & (1 << slot))) continue;

                CoreType type = static_cast<CoreType>(analysis->outputTypes[slot]);
                if (type == CoreType::VOID || type == CoreType::INVALID) {
                    type = CoreType::FLOAT4;
                }

                out.Lit("layout(location = ");
                out.Uint(location);
                out.Lit(") out ");
                EmitType(static_cast<u16>(type));
                out.Lit(" fragColor");
                out.Uint(location);
                out.Lit(";\n");
                emittedColor = true;
            }
        }

        if (!emittedColor) {
            out.Lit("layout(location = 0) out vec4 fragColor0;\n");
        }
    }
    out.NL(0);
}

void GLESBuilder::EmitUniforms() {
    bool usedTextures[32] = {};
    bool usedTextureLevels[32] = {};
    bool shadowTextures[32] = {};
    for (u32 i = 0; i < ir->instructionCount; i++) {
        if (!IR::IsTextureOp(static_cast<IR::OpCode>(ir->opcodes[i]))) continue;
        u16 texReg = ir->GetOperand(i, 0);
        if ((texReg & 0xF000) != 0x2000) continue;
        u16 texSlot = texReg & 0x0FFF;
        if (texSlot < 32) {
            usedTextures[texSlot] = true;
            if (ir->opcodes[i] == IR::OP_TEX_SAMPLE_CMP) shadowTextures[texSlot] = true;
            if (ir->opcodes[i] == IR::OP_TEX_LEVELS) {
                usedTextureLevels[texSlot] = true;
            }
        }
    }

    bool emittedTextures[32] = {};

    // Emit uniform buffer declarations from render config
    if (renderConfig) {
        // Emit uniform buffers as individual uniforms (GLSL ES 300 style)
        for (const auto& ub : renderConfig->uniformBuffers) {
            // Check if this uniform is used in the current shader stage
            bool isVertex = (stage == ShaderStage::Vertex);
            bool isFragment = (stage == ShaderStage::Fragment);
            bool stageMatch = (isVertex && (ub.stages & 1)) || (isFragment && (ub.stages & 2));

            if (!stageMatch) continue;

            // Emit as std140 uniform block
            out.Lit("layout(std140) uniform UB_");
            out.Str(ub.name.c_str());
            out.Lit(" {\n");
            out.Lit("    ");

            // Map type name to GLSL type
            const char* glslType = "float";
            if (ub.typeName == "mat4") glslType = "mat4";
            else if (ub.typeName == "mat3") glslType = "mat3";
            else if (ub.typeName == "float4" || ub.typeName == "vec4") glslType = "vec4";
            else if (ub.typeName == "float3" || ub.typeName == "vec3") glslType = "vec3";
            else if (ub.typeName == "float2" || ub.typeName == "vec2") glslType = "vec2";
            else if (ub.typeName == "int") glslType = "int";
            else if (ub.typeName == "uint") glslType = "uint";

            out.Str(glslType);
            out.Lit(" u_");
            out.Str(ub.name.c_str());
            out.Lit(";\n} ub_");
            out.Str(ub.name.c_str());
            out.Lit(";\n");
        }

        // Emit samplers
        for (const auto& tex : renderConfig->textures) {
            const u32 texSlot = tex.bindingIndex;
            bool isVertex = (stage == ShaderStage::Vertex);
            bool isFragment = (stage == ShaderStage::Fragment);
            bool stageMatch = (isVertex && (tex.stages & 1)) || (isFragment && (tex.stages & 2));

            if (!stageMatch) {
                continue;
            }

            auto emitSampler = [&](u32 metadata) {
                out.Lit("uniform highp ");
                if (tex.isCubemap) out.Str(shadowTextures[texSlot] ? "samplerCubeShadow" : "samplerCube");
                else if (tex.isArray) out.Str(shadowTextures[texSlot] ? "sampler2DArrayShadow" : "sampler2DArray");
                else if (tex.isVolume) out.Lit("sampler3D");
                else out.Str(shadowTextures[texSlot] ? "sampler2DShadow" : "sampler2D");
                out.Chr(' '); EmitTexture(static_cast<u16>(0x2000 | texSlot), metadata);
                out.Lit(";\n");
            };
            if (tex.separateSampler) {
                // ES combines a texture and sampler into one uniform. Preserve
                // each pair under the same descriptor-based name as SPIRV-Cross.
                std::vector<u16> emittedPairs;
                for (u32 i = 0; i < ir->instructionCount; ++i) {
                    if (!IR::IsTextureOp(static_cast<IR::OpCode>(ir->opcodes[i])) ||
                        (Op(i, 0) & 0x0FFF) != texSlot) continue;
                    u32 metadata = ir->metadata[i];
                    u16 sampler = TextureOpHasExplicitSampler(metadata)
                        ? GetTextureOpExplicitSamplerBinding(metadata) : 0xFFFFu;
                    if (std::find(emittedPairs.begin(), emittedPairs.end(), sampler) != emittedPairs.end()) continue;
                    emittedPairs.push_back(sampler);
                    emitSampler(metadata);
                }
            } else emitSampler(0);
            if (texSlot < 32 && usedTextureLevels[texSlot]) {
                out.Lit("uniform int ");
                EmitTextureLevelsUniformName(static_cast<u16>(0x2000 | texSlot));
                out.Lit(";\n");
            }
            if (texSlot < 32) emittedTextures[texSlot] = true;
        }
        out.NL(0);
    } else {
        // Fallback: scan IR for OP_LOAD_UNIFORM.
        bool hasUniforms = false;
        for (u32 i = 0; i < ir->instructionCount; i++) {
            if (ir->opcodes[i] == IR::OP_LOAD_UNIFORM) {
                hasUniforms = true;
            }
        }

        if (hasUniforms) {
            out.Lit("// TODO: Uniform block declaration (no render config)\n");
            out.NL(0);
        }
    }

    bool emittedFallbackTexture = false;
    for (u32 i = 0; i < 32; i++) {
        if (!usedTextures[i] || emittedTextures[i]) continue;
        out.Lit("uniform sampler2D sampler");
        out.Uint(i);
        out.Lit(";\n");
        if (usedTextureLevels[i]) {
            out.Lit("uniform int ");
            EmitTextureLevelsUniformName(static_cast<u16>(0x2000 | i));
            out.Lit(";\n");
        }
        emittedFallbackTexture = true;
    }
    if (emittedFallbackTexture) {
        out.NL(0);
    }
}

// ============================================================================
// Main Function Emission
// ============================================================================

void GLESBuilder::EmitMain() {
    out.Lit("void main() {\n");
    indent = 1;

    // Array registers carry their element CoreType; declare actual array storage.
    for (u32 i = 0; i < ir->localArrayCount; ++i) {
        u16 reg = ir->localArrayRegisters[i];
        if (reg >= regCount || (regInfo[reg].flags & REG_DECLARED)) continue;
        out.NL(indent);
        if (ir->localArrayStructTypes && ir->localArrayStructTypes[i])
            EmitStructTypeName(ir->localArrayStructTypes[i]);
        else EmitType(ir->localArrayTypes[i]);
        out.Chr(' '); EmitReg(reg); out.Chr('['); out.Uint(ir->localArraySizes[i]); out.Lit("];");
        regInfo[reg].flags |= REG_DECLARED;
    }
    EmitUndefDeclarations();
    if (cfg && cfg->blockCount > 0) {
        EmitControlFlow();
    } else {
        for (u32 i = 0; i < ir->instructionCount; ++i) EmitInstruction(i);
    }
    out.NL(0);
    out.Lit("}\n");
}

bool GLESBuilder::EmitBlockEdge(u32 fromBlock, u32 toBlock, u32 stopBlock,
                                const LoopScope* loop, u32 depth) {
    if (toBlock == NO_BLOCK || toBlock >= cfg->blockCount) {
        out.NL(indent); out.Lit("return;");
        return false;
    }
    // Give each edge's parallel-copy temporaries their own lexical scope.
    out.NL(indent); out.Chr('{'); ++indent;
    EmitPhiAssignments(fromBlock, toBlock);
    --indent; out.NL(indent); out.Chr('}');
    if (loop) {
        if (toBlock == loop->merge) {
            out.NL(indent); out.Lit("break;");
            return false;
        }
        if (toBlock == loop->header) {
            out.NL(indent); out.Lit("continue;");
            return false;
        }
        if (toBlock == loop->continuation && !loop->inContinuation) {
            // A source `skip` still executes the for-loop increment. Emit the
            // continue region on that edge before GLSL's continue statement.
            LoopScope continuing = *loop;
            continuing.inContinuation = true;
            EmitStructuredRegion(toBlock, NO_BLOCK, &continuing, depth + 1);
            return false;
        }
    }
    if (toBlock == stopBlock) return true;
    return EmitStructuredRegion(toBlock, stopBlock, loop, depth + 1);
}

bool GLESBuilder::EmitStructuredRegion(u32 block, u32 stopBlock,
                                       const LoopScope* loop, u32 depth) {
    // A loop header is visited once to open its scope and once for its body.
    if (depth > cfg->blockCount * 2 + 1)
        throw std::runtime_error("Direct GLES cannot structure this control-flow graph");
    u32 visited = 0;
    while (block != stopBlock && block != NO_BLOCK && block < cfg->blockCount) {
        if (++visited > cfg->blockCount)
            throw std::runtime_error("Direct GLES found an unannotated control-flow cycle");
        // CFG construction does not call RecoverStructure. Read the original
        // lowering annotations, whose targets are instruction indices.
        u32 last = cfg->lastInst[block];
        u32 structure = ir->structureInfo ? ir->structureInfo[last] : 0;
        u32 mergeInst = structure & IR::IRProgram::STRUCT_TARGET_MASK;
        u32 merge = structure && mergeInst < ir->instructionCount ? cfg->instToBlock[mergeInst] : NO_BLOCK;
        u32 continueInst = (structure & IR::IRProgram::STRUCT_TYPE_MASK) == IR::IRProgram::STRUCT_LOOP_HEADER &&
                           ir->continueInfo ? ir->continueInfo[last] : NO_BLOCK;
        u32 continuation = continueInst < ir->instructionCount ? cfg->instToBlock[continueInst] : NO_BLOCK;
        if (continuation != NO_BLOCK && (!loop || block != loop->header)) {
            LoopScope inner = {block, merge, continuation, false};
            out.NL(indent); out.Lit("for (;;) {"); ++indent;
            EmitStructuredRegion(block, NO_BLOCK, &inner, depth + 1);
            --indent; out.NL(indent); out.Chr('}');
            block = merge;
            continue;
        }

        u32 terminal = NO_BLOCK;
        for (u32 i = cfg->firstInst[block]; i <= cfg->lastInst[block] && i < ir->instructionCount; ++i) {
            u16 op = ir->opcodes[i];
            if (op == IR::OP_BRANCH || op == IR::OP_JUMP || op == IR::OP_SWITCH || op == IR::OP_RET) {
                terminal = i;
                break;
            }
            if (op == IR::OP_DISCARD) {
                EmitInstruction(i);
                return false;
            }
            if (op != IR::OP_PHI) EmitInstruction(i);
        }
        u16 op = terminal == NO_BLOCK ? IR::OP_NOP : ir->opcodes[terminal];
        if (op == IR::OP_RET || cfg->TotalSuccessorCount(block) == 0) {
            out.NL(indent); out.Lit("return;");
            return false;
        }
        if (op == IR::OP_SWITCH) {
            // Case bodies are mutually exclusive, with no source fallthrough.
            // Using if/else also lets a case's loop break/skip target its loop
            // directly, without an intervening GLSL switch catching the break.
            u32 data = ir->metadata[terminal];
            u32 count = ir->GetSwitchCaseCount(data);
            bool reachesMerge = false;
            for (u32 c = 0; c < count; ++c) {
                out.NL(indent); out.Str(c ? "else if (" : "if (");
                EmitExpr(Op(terminal, 0)); out.Lit(" == ");
                u16 selector = Op(terminal, 0);
                bool unsignedSelector = selector < regCount && ir->registerTypes &&
                    ir->registerTypes[selector] == static_cast<u16>(CoreType::UINT);
                if (unsignedSelector) {
                    out.Uint(static_cast<u32>(ir->GetSwitchCaseValue(data, c))); out.Chr('u');
                } else out.Int(ir->GetSwitchCaseValue(data, c));
                out.Lit(") {"); ++indent;
                u32 target = ir->GetSwitchCaseTarget(data, c);
                reachesMerge |= EmitBlockEdge(block, target < ir->instructionCount ? cfg->instToBlock[target] : NO_BLOCK,
                                               merge, loop, depth);
                --indent; out.NL(indent); out.Chr('}');
            }
            if (count) { out.Lit(" else {"); ++indent; }
            u32 target = ir->GetSwitchDefaultTarget(data);
            reachesMerge |= EmitBlockEdge(block, target < ir->instructionCount ? cfg->instToBlock[target] : NO_BLOCK,
                                           merge, loop, depth);
            if (count) { --indent; out.NL(indent); out.Chr('}'); }
            if (!reachesMerge) return false;
            block = merge;
            continue;
        }
        if (op == IR::OP_BRANCH) {
            // Selection merge code is emitted after both arms, restoring
            // source-level reconvergence for derivatives and implicit LOD.
            // Loop conditions/until branches instead exit via break/continue.
            u32 branchStop = merge != NO_BLOCK ? merge : stopBlock;
            out.NL(indent); out.Lit("if ("); EmitExpr(Op(terminal, 0)); out.Lit(") {"); ++indent;
            bool thenFalls = EmitBlockEdge(block, cfg->GetSuccessor(block, 0), branchStop, loop, depth);
            --indent; out.NL(indent); out.Lit("} else {"); ++indent;
            bool elseFalls = EmitBlockEdge(block, cfg->GetSuccessor(block, 1), branchStop, loop, depth);
            --indent; out.NL(indent); out.Chr('}');
            if (merge == NO_BLOCK) return thenFalls || elseFalls;
            if (!thenFalls && !elseFalls) return false;
            block = merge;
            continue;
        }
        return EmitBlockEdge(block, cfg->GetAnySuccessor(block, 0), stopBlock, loop, depth);
    }
    return block == stopBlock;
}

void GLESBuilder::EmitControlFlow() {
    // Registers must survive loop edges and both arms of a selection.
    for (u32 reg = 0; reg < regCount; ++reg) {
        if (regInfo[reg].flags & REG_DECLARED) continue;
        u16 type = ir->registerTypes ? ir->registerTypes[reg] : 0;
        if (type == 0 || type == static_cast<u16>(CoreType::VOID)) continue;
        out.NL(indent); EmitRegWithDecl(static_cast<u16>(reg)); out.Chr(';');
    }
    EmitStructuredRegion(cfg->entryBlock, NO_BLOCK, nullptr, 0);
}

void GLESBuilder::EmitDefaultValue(u16 type) {
    switch (static_cast<CoreType>(type)) {
        case CoreType::BOOL:   out.Lit("false"); break;
        case CoreType::INT:    out.Lit("0"); break;
        case CoreType::UINT:   out.Lit("0u"); break;
        case CoreType::FLOAT:  out.Lit("0.0"); break;
        case CoreType::BOOL2:  out.Lit("bvec2(false)"); break;
        case CoreType::BOOL3:  out.Lit("bvec3(false)"); break;
        case CoreType::BOOL4:  out.Lit("bvec4(false)"); break;
        case CoreType::INT2:   out.Lit("ivec2(0)"); break;
        case CoreType::INT3:   out.Lit("ivec3(0)"); break;
        case CoreType::INT4:   out.Lit("ivec4(0)"); break;
        case CoreType::UINT2:  out.Lit("uvec2(0u)"); break;
        case CoreType::UINT3:  out.Lit("uvec3(0u)"); break;
        case CoreType::UINT4:  out.Lit("uvec4(0u)"); break;
        case CoreType::FLOAT2: out.Lit("vec2(0.0)"); break;
        case CoreType::FLOAT3: out.Lit("vec3(0.0)"); break;
        case CoreType::FLOAT4: out.Lit("vec4(0.0)"); break;
        case CoreType::MAT2:   out.Lit("mat2(0.0)"); break;
        case CoreType::MAT3:   out.Lit("mat3(0.0)"); break;
        case CoreType::MAT4:   out.Lit("mat4(0.0)"); break;
        default:               out.Lit("0.0"); break;  // Fallback
    }
}

void GLESBuilder::EmitUndefDeclarations() {
    if (!ir->undefRegCount || !ir->undefRegs || !ir->undefRegTypes) return;
    
    // Undef registers are created by SSA when a PHI operand comes from a path
    // where the variable was never defined. We need to declare them with defaults.
    for (u32 i = 0; i < ir->undefRegCount; i++) {
        u16 reg = ir->undefRegs[i];
        u16 type = ir->undefRegTypes[i];
        
        if (reg >= regCount) continue;
        if (regInfo[reg].flags & REG_DECLARED) continue;
        
        // Mark as declared
        regInfo[reg].flags |= REG_DECLARED;
        
        out.NL(indent);
        EmitType(type);
        out.Chr(' ');
        EmitReg(reg);
        out.Lit(" = ");
        EmitDefaultValue(type);
        out.Chr(';');
    }
}


// Emit PHI assignments when transitioning from one block to another
// PHI nodes in toBlock that have values from fromBlock need assignments
void GLESBuilder::EmitPhiAssignments(u32 fromBlock, u32 toBlock) {
    if (!ir->phiCount || !ir->phiBlockIndices) return;
    // Phi assignments are parallel copies: save every source before overwriting
    // any phi destination (for example, swapping two variables in a loop).
    for (u32 phi = 0; phi < ir->phiCount; ++phi) {
        if (ir->phiBlockIndices[phi] != toBlock) continue;
        for (u32 i = 0; i < ir->GetPhiOperandCount(phi); ++i) {
            if (ir->GetPhiOperandBlock(phi, i) != fromBlock) continue;
            out.NL(indent); EmitRegisterType(ir->phiResultRegs[phi]); out.Lit(" bwsl_phi"); out.Uint(phi);
            out.Lit(" = "); EmitExpr(ir->GetPhiOperandValue(phi, i)); out.Chr(';');
        }
    }
    for (u32 phi = 0; phi < ir->phiCount; ++phi) {
        if (ir->phiBlockIndices[phi] != toBlock) continue;
        for (u32 i = 0; i < ir->GetPhiOperandCount(phi); ++i) {
            if (ir->GetPhiOperandBlock(phi, i) != fromBlock) continue;
            out.NL(indent); EmitReg(ir->phiResultRegs[phi]); out.Lit(" = bwsl_phi"); out.Uint(phi); out.Chr(';');
        }
    }
}

// ============================================================================
// Instruction Emission
// ============================================================================

void GLESBuilder::EmitInstruction(u32 instIdx) {
    u16 opcode = ir->opcodes[instIdx];
    u16 dest = ir->destinations[instIdx];

    out.NL(indent);

    // Handle different instruction categories
    switch (opcode) {
        // ===== Control Flow =====
        // These are handled by EmitControlFlow
        case IR::OP_NOP:
        case IR::OP_JUMP:
        case IR::OP_BRANCH:
        case IR::OP_SWITCH:
        case IR::OP_RET:
            // Control flow handled by CFG-based emission
            return;

        case IR::OP_PHI: {
            // SSA phi node - these should be resolved during phi elimination
            // For now, just emit the assignment from the first operand
            // (This is a simplification - proper phi elimination would add
            // copies at predecessor block ends)
            u16 firstVal = Op(instIdx, 0);
            if (firstVal != 0x3FFF) {
                EmitRegWithDecl(dest);
                out.Lit(" = ");
                EmitExpr(firstVal);
                out.Lit(";");
            }
            return;
        }

        case IR::OP_DISCARD:
            out.Lit("discard;");
            return;

        // ===== Memory Operations =====
        case IR::OP_STORE_REG: {
            // Register-to-register copy/store
            u16 srcReg = Op(instIdx, 0);
            EmitRegWithDecl(dest);
            out.Lit(" = ");
            EmitExpr(srcReg);
            out.Lit(";");
            return;
        }

        case IR::OP_STORE_OUTPUT: {
            // Fragment: fragColor = value
            // Vertex: gl_Position = value or varying = value
            // IR encoding: EmitInstruction(OP_STORE_OUTPUT, valueReg, slot)
            // So: dest = valueReg, operand[0] = slot/outputIdx
            u16 outputIdx = Op(instIdx, 0);
            u16 valueReg = dest;  // Value is in destination, not operand

            if (stage == ShaderStage::Fragment) {
                if (outputIdx == OutputSlot::DEPTH) {
                    out.Lit("gl_FragDepth = ");
                } else {
                    out.Lit("fragColor");
                    out.Uint(OutputSlot::FragmentColorLocation((u8)outputIdx));
                    out.Lit(" = ");
                }
            } else if (stage == ShaderStage::Vertex) {
                if (outputIdx == 0) {  // Position output (slot 0)
                    out.Lit("gl_Position = ");
                } else if (varyings) {
                    // Convert IR slot (VARYING0=2, VARYING1=3, etc.) to VaryingInfo slot (0-based)
                    u16 varyingSlot = outputIdx - 2;  // OutputSlot::VARYING0 = 2
                    // Search for varying by converted slot number
                    bool found = false;
                    for (u32 i = 0; i < varyings->count; i++) {
                        if (varyings->varyings[i].slot == varyingSlot) {
                            out.Lit("v_");
                            out.Str(varyings->varyings[i].name);
                            out.Lit(" = ");
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        out.Lit("v_slot");
                        out.Uint(varyingSlot);
                        out.Lit(" = ");
                    }
                } else {
                    out.Lit("v_slot");
                    out.Uint(outputIdx - 2);
                    out.Lit(" = ");
                }
            }
            EmitExpr(valueReg);
            out.Lit(";");
            return;
        }

        case IR::OP_LOAD_CONST:
        case IR::OP_LOAD_REG:
        case IR::OP_LOAD_ATTR:
        case IR::OP_LOAD_INPUT:
        case IR::OP_LOAD_UNIFORM:
            // Materialize loads at their definition, like all other SSA values.
            if (dest < regCount) {
                EmitRegWithDecl(dest);
                out.Lit(" = ");
                EmitLoadExpr(instIdx);
                out.Lit(";");
            }
            return;

        case IR::OP_LOAD_OUTPUT: {
            // Load from a previously written output (rare, for reading gl_Position etc.)
            u16 outputIdx = Op(instIdx, 0);
            EmitRegWithDecl(dest);
            out.Lit(" = ");
            if (stage == ShaderStage::Fragment) {
                if (outputIdx == OutputSlot::DEPTH) {
                    out.Lit("gl_FragDepth");
                } else {
                    out.Lit("fragColor");
                    out.Uint(OutputSlot::FragmentColorLocation((u8)outputIdx));
                }
            } else if (stage == ShaderStage::Vertex && outputIdx == 0) {
                out.Lit("gl_Position");
            } else if (varyings) {
                // Convert IR slot (VARYING0=2, etc.) to VaryingInfo slot (0-based)
                u16 varyingSlot = outputIdx - 2;  // OutputSlot::VARYING0 = 2
                // Search for varying by converted slot number
                bool found = false;
                for (u32 i = 0; i < varyings->count; i++) {
                    if (varyings->varyings[i].slot == varyingSlot) {
                        out.Lit("v_");
                        out.Str(varyings->varyings[i].name);
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    out.Lit("v_slot");
                    out.Uint(varyingSlot);
                }
            } else {
                out.Lit("v_slot");
                out.Uint(outputIdx - 2);
            }
            out.Lit(";");
            return;
        }

        case IR::OP_LOAD_LOCAL:
        case IR::OP_STORE_LOCAL:
            // Thread-local storage - emit as local variable access
            EmitRegWithDecl(dest);
            out.Lit(" = local");
            out.Uint(Op(instIdx, 0));
            out.Lit(";");
            return;

        case IR::OP_LOAD_BUFFER:
        case IR::OP_STORE_BUFFER:
            // Storage buffer access - GLSL ES 300 doesn't have SSBOs
            out.Lit("// Buffer ops require GLSL ES 310+");
            return;

        case IR::OP_LOAD_SHARED:
        case IR::OP_STORE_SHARED:
            // Shared memory - only in compute shaders
            out.Lit("// Shared memory ops require compute shader");
            return;

        // ===== Arithmetic =====
        case IR::OP_FADD: case IR::OP_IADD:
            EmitBinaryAssign(instIdx, dest, "+");
            return;
        case IR::OP_FSUB: case IR::OP_ISUB:
            EmitBinaryAssign(instIdx, dest, "-");
            return;
        case IR::OP_FMUL: case IR::OP_IMUL:
            EmitBinaryAssign(instIdx, dest, "*");
            return;
        case IR::OP_FDIV: case IR::OP_IDIV:
            EmitBinaryAssign(instIdx, dest, "/");
            return;
        case IR::OP_IMOD:
            EmitBinaryAssign(instIdx, dest, "%");
            return;
        case IR::OP_FMOD:
            EmitFuncAssign(instIdx, dest, "mod", 2);
            return;
        case IR::OP_FREM:
            EmitRegWithDecl(dest);
            out.Lit(" = (");
            EmitExpr(Op(instIdx, 0));
            out.Lit(" - ");
            EmitExpr(Op(instIdx, 1));
            out.Lit(" * trunc(");
            EmitExpr(Op(instIdx, 0));
            out.Lit(" / ");
            EmitExpr(Op(instIdx, 1));
            out.Lit("));");
            return;

        case IR::OP_FNEG: case IR::OP_INEG:
            EmitUnaryAssign(instIdx, dest, "-");
            return;

        // ===== Math Functions =====
        case IR::OP_FABS: case IR::OP_IABS:
            EmitFuncAssign(instIdx, dest, "abs", 1);
            return;
        case IR::OP_FMIN: case IR::OP_IMIN: case IR::OP_UMIN:
            EmitFuncAssign(instIdx, dest, "min", 2);
            return;
        case IR::OP_FMAX: case IR::OP_IMAX: case IR::OP_UMAX:
            EmitFuncAssign(instIdx, dest, "max", 2);
            return;
        case IR::OP_FCLAMP: case IR::OP_ICLAMP: case IR::OP_UCLAMP:
            EmitFuncAssign(instIdx, dest, "clamp", 3);
            return;
        case IR::OP_FLOOR:
            EmitFuncAssign(instIdx, dest, "floor", 1);
            return;
        case IR::OP_CEIL:
            EmitFuncAssign(instIdx, dest, "ceil", 1);
            return;
        case IR::OP_ROUND:
            EmitFuncAssign(instIdx, dest, "round", 1);
            return;
        case IR::OP_TRUNC:
            EmitFuncAssign(instIdx, dest, "trunc", 1);
            return;
        case IR::OP_FRACT:
            EmitFuncAssign(instIdx, dest, "fract", 1);
            return;
        case IR::OP_FMA:
            EmitRegWithDecl(dest);
            out.Lit(" = ("); EmitExpr(Op(instIdx, 0)); out.Lit(" * ");
            EmitExpr(Op(instIdx, 1)); out.Lit(" + "); EmitExpr(Op(instIdx, 2));
            out.Lit(");");
            return;

        case IR::OP_SQRT:
            EmitFuncAssign(instIdx, dest, "sqrt", 1);
            return;
        case IR::OP_RSQRT:
            EmitFuncAssign(instIdx, dest, "inversesqrt", 1);
            return;
        case IR::OP_POW:
            EmitFuncAssign(instIdx, dest, "pow", 2);
            return;
        case IR::OP_EXP:
            EmitFuncAssign(instIdx, dest, "exp", 1);
            return;
        case IR::OP_EXP2:
            EmitFuncAssign(instIdx, dest, "exp2", 1);
            return;
        case IR::OP_LOG:
            EmitFuncAssign(instIdx, dest, "log", 1);
            return;
        case IR::OP_LOG2:
            EmitFuncAssign(instIdx, dest, "log2", 1);
            return;
        case IR::OP_LDEXP:
            EmitRegWithDecl(dest);
            out.Lit(" = (");
            EmitExpr(Op(instIdx, 0));
            out.Lit(" * exp2(float(");
            EmitExpr(Op(instIdx, 1));
            out.Lit(")));");
            return;
        case IR::OP_MODF_STRUCT:
            EmitRegWithDecl(dest);
            out.Lit(" = ");
            EmitStructTypeName(ir->metadata[instIdx]);
            out.Chr('(');
            EmitExpr(Op(instIdx, 0));
            out.Lit(" - trunc(");
            EmitExpr(Op(instIdx, 0));
            out.Lit("), trunc(");
            EmitExpr(Op(instIdx, 0));
            out.Lit("));");
            return;
        case IR::OP_FREXP_STRUCT:
            EmitRegWithDecl(dest);
            out.Lit(" = bwsl_frexp(");
            EmitExpr(Op(instIdx, 0));
            out.Lit(");");
            return;
        case IR::OP_SIN:
            EmitFuncAssign(instIdx, dest, "sin", 1);
            return;
        case IR::OP_COS:
            EmitFuncAssign(instIdx, dest, "cos", 1);
            return;
        case IR::OP_TAN:
            EmitFuncAssign(instIdx, dest, "tan", 1);
            return;
        case IR::OP_ASIN:
            EmitFuncAssign(instIdx, dest, "asin", 1);
            return;
        case IR::OP_ACOS:
            EmitFuncAssign(instIdx, dest, "acos", 1);
            return;
        case IR::OP_ATAN:
            EmitFuncAssign(instIdx, dest, "atan", 1);
            return;
        case IR::OP_ATAN2:
            EmitFuncAssign(instIdx, dest, "atan", 2);
            return;
        case IR::OP_SINH:
            EmitFuncAssign(instIdx, dest, "sinh", 1);
            return;
        case IR::OP_COSH:
            EmitFuncAssign(instIdx, dest, "cosh", 1);
            return;
        case IR::OP_TANH:
            EmitFuncAssign(instIdx, dest, "tanh", 1);
            return;

        // ===== Geometric =====
        case IR::OP_DOT:
            EmitFuncAssign(instIdx, dest, "dot", 2);
            return;
        case IR::OP_CROSS:
            EmitFuncAssign(instIdx, dest, "cross", 2);
            return;
        case IR::OP_LENGTH:
            EmitFuncAssign(instIdx, dest, "length", 1);
            return;
        case IR::OP_NORMALIZE:
            EmitFuncAssign(instIdx, dest, "normalize", 1);
            return;
        case IR::OP_DISTANCE:
            EmitFuncAssign(instIdx, dest, "distance", 2);
            return;
        case IR::OP_REFLECT:
            EmitFuncAssign(instIdx, dest, "reflect", 2);
            return;
        case IR::OP_REFRACT:
            EmitFuncAssign(instIdx, dest, "refract", 3);
            return;
        case IR::OP_FACEFORWARD:
            EmitFuncAssign(instIdx, dest, "faceforward", 3);
            return;

        // ===== Interpolation =====
        case IR::OP_LERP:
            EmitFuncAssign(instIdx, dest, "mix", 3);
            return;
        case IR::OP_SMOOTHSTEP:
            EmitFuncAssign(instIdx, dest, "smoothstep", 3);
            return;
        case IR::OP_STEP:
            EmitFuncAssign(instIdx, dest, "step", 2);
            return;
        case IR::OP_SATURATE:
            // GLSL ES doesn't have saturate, use clamp(x, 0.0, 1.0)
            EmitRegWithDecl(dest);
            out.Lit(" = clamp(");
            EmitExpr(Op(instIdx, 0));
            out.Lit(", 0.0, 1.0);");
            return;
        case IR::OP_DEGREES:
            EmitFuncAssign(instIdx, dest, "degrees", 1);
            return;
        case IR::OP_RADIANS:
            EmitFuncAssign(instIdx, dest, "radians", 1);
            return;

        // ===== Comparison =====
        case IR::OP_FEQ: case IR::OP_IEQ:
            EmitBinaryAssign(instIdx, dest, "==");
            return;
        case IR::OP_FNE: case IR::OP_INE:
            EmitBinaryAssign(instIdx, dest, "!=");
            return;
        case IR::OP_FLT: case IR::OP_ILT: case IR::OP_ULT:
            EmitBinaryAssign(instIdx, dest, "<");
            return;
        case IR::OP_FLE: case IR::OP_ILE: case IR::OP_ULE:
            EmitBinaryAssign(instIdx, dest, "<=");
            return;
        case IR::OP_FGT: case IR::OP_IGT: case IR::OP_UGT:
            EmitBinaryAssign(instIdx, dest, ">");
            return;
        case IR::OP_FGE: case IR::OP_IGE: case IR::OP_UGE:
            EmitBinaryAssign(instIdx, dest, ">=");
            return;

        // ===== Bitwise =====
        case IR::OP_AND:
            EmitBinaryAssign(instIdx, dest, "&");
            return;
        case IR::OP_OR:
            EmitBinaryAssign(instIdx, dest, "|");
            return;
        case IR::OP_XOR:
            EmitBinaryAssign(instIdx, dest, "^");
            return;
        case IR::OP_NOT: {
            CoreType type = static_cast<CoreType>(Type(instIdx));
            if (type == CoreType::BOOL)
                EmitUnaryAssign(instIdx, dest, "!");
            else if (type == CoreType::BOOL2 || type == CoreType::BOOL3 || type == CoreType::BOOL4)
                EmitFuncAssign(instIdx, dest, "not", 1);
            else
                EmitUnaryAssign(instIdx, dest, "~");
            return;
        }
        case IR::OP_SHL:
            EmitBinaryAssign(instIdx, dest, "<<");
            return;
        case IR::OP_SHR: case IR::OP_ASR:
            EmitBinaryAssign(instIdx, dest, ">>");
            return;
        case IR::OP_POPCNT:
            // GLSL ES 300 has bitCount
            EmitFuncAssign(instIdx, dest, "bitCount", 1);
            return;
        case IR::OP_CLZ: {
            // GLSL ES 300: use findMSB and compute 31 - findMSB(x)
            EmitRegWithDecl(dest);
            out.Lit(" = (");
            EmitExpr(Op(instIdx, 0));
            out.Lit(" == 0) ? 32 : (31 - findMSB(");
            EmitExpr(Op(instIdx, 0));
            out.Lit("));");
            return;
        }
        case IR::OP_CTZ: {
            // GLSL ES 300: use findLSB
            EmitRegWithDecl(dest);
            out.Lit(" = (");
            EmitExpr(Op(instIdx, 0));
            out.Lit(" == 0) ? 32 : findLSB(");
            EmitExpr(Op(instIdx, 0));
            out.Lit(");");
            return;
        }
        case IR::OP_REVERSE_BITS:
            EmitFuncAssign(instIdx, dest, "bitfieldReverse", 1);
            return;
        case IR::OP_BITFIELD_EXTRACT:
            EmitFuncAssign(instIdx, dest, "bitfieldExtract", 3);
            return;
        case IR::OP_BITFIELD_INSERT:
            EmitFuncAssign(instIdx, dest, "bitfieldInsert", 4);
            return;
        case IR::OP_PACK_UNORM2X16:
            EmitFuncAssign(instIdx, dest, "packUnorm2x16", 1);
            return;
        case IR::OP_UNPACK_UNORM2X16:
            EmitFuncAssign(instIdx, dest, "unpackUnorm2x16", 1);
            return;
        case IR::OP_PACK_UNORM4X8:
            EmitFuncAssign(instIdx, dest, "packUnorm4x8", 1);
            return;
        case IR::OP_UNPACK_UNORM4X8:
            EmitFuncAssign(instIdx, dest, "unpackUnorm4x8", 1);
            return;
        case IR::OP_PACK_SNORM2X16:
            EmitFuncAssign(instIdx, dest, "packSnorm2x16", 1);
            return;
        case IR::OP_UNPACK_SNORM2X16:
            EmitFuncAssign(instIdx, dest, "unpackSnorm2x16", 1);
            return;
        case IR::OP_PACK_SNORM4X8:
            EmitFuncAssign(instIdx, dest, "packSnorm4x8", 1);
            return;
        case IR::OP_UNPACK_SNORM4X8:
            EmitFuncAssign(instIdx, dest, "unpackSnorm4x8", 1);
            return;
        case IR::OP_PACK_HALF2X16:
            EmitFuncAssign(instIdx, dest, "packHalf2x16", 1);
            return;
        case IR::OP_UNPACK_HALF2X16:
            EmitFuncAssign(instIdx, dest, "unpackHalf2x16", 1);
            return;

        // ===== Type Conversion =====
        case IR::OP_F2I:
            EmitRegWithDecl(dest);
            out.Lit(" = int(");
            EmitExpr(Op(instIdx, 0));
            out.Lit(");");
            return;
        case IR::OP_I2F:
            EmitRegWithDecl(dest);
            out.Lit(" = float(");
            EmitExpr(Op(instIdx, 0));
            out.Lit(");");
            return;
        case IR::OP_F2U:
            EmitRegWithDecl(dest);
            out.Lit(" = uint(");
            EmitExpr(Op(instIdx, 0));
            out.Lit(");");
            return;
        case IR::OP_U2F:
            EmitRegWithDecl(dest);
            out.Lit(" = float(");
            EmitExpr(Op(instIdx, 0));
            out.Lit(");");
            return;
        case IR::OP_I2U:
            EmitRegWithDecl(dest);
            out.Lit(" = uint(");
            EmitExpr(Op(instIdx, 0));
            out.Lit(");");
            return;
        case IR::OP_U2I:
            EmitRegWithDecl(dest);
            out.Lit(" = int(");
            EmitExpr(Op(instIdx, 0));
            out.Lit(");");
            return;
        case IR::OP_BITCAST: {
            // Reinterpret bits - use GLSL bitcast functions
            u16 srcReg = Op(instIdx, 0);
            u16 srcType = (ir->registerTypes && srcReg < ir->registerCount) ? ir->registerTypes[srcReg] : 0;
            u16 dstType = Type(instIdx);
            auto scalarFamily = [](u16 type) -> CoreType {
                CoreType t = static_cast<CoreType>(type);
                switch (t) {
                    case CoreType::FLOAT: case CoreType::FLOAT2: case CoreType::FLOAT3: case CoreType::FLOAT4: return CoreType::FLOAT;
                    case CoreType::INT: case CoreType::INT2: case CoreType::INT3: case CoreType::INT4: return CoreType::INT;
                    case CoreType::UINT: case CoreType::UINT2: case CoreType::UINT3: case CoreType::UINT4: return CoreType::UINT;
                    default: return t;
                }
            };
            CoreType srcFamily = scalarFamily(srcType);
            CoreType dstFamily = scalarFamily(dstType);
            if (srcFamily == CoreType::FLOAT && dstFamily == CoreType::INT) {
                EmitRegWithDecl(dest);
                out.Lit(" = floatBitsToInt(");
                EmitExpr(Op(instIdx, 0));
                out.Lit(");");
            } else if (srcFamily == CoreType::FLOAT && dstFamily == CoreType::UINT) {
                EmitRegWithDecl(dest);
                out.Lit(" = floatBitsToUint(");
                EmitExpr(Op(instIdx, 0));
                out.Lit(");");
            } else if (srcFamily == CoreType::INT && dstFamily == CoreType::FLOAT) {
                EmitRegWithDecl(dest);
                out.Lit(" = intBitsToFloat(");
                EmitExpr(Op(instIdx, 0));
                out.Lit(");");
            } else if (srcFamily == CoreType::UINT && dstFamily == CoreType::FLOAT) {
                EmitRegWithDecl(dest);
                out.Lit(" = uintBitsToFloat(");
                EmitExpr(Op(instIdx, 0));
                out.Lit(");");
            } else {
                // Fallback - just copy
                EmitRegWithDecl(dest);
                out.Lit(" = ");
                EmitExpr(Op(instIdx, 0));
                out.Lit(";");
            }
            return;
        }
        case IR::OP_SIGN:
            EmitFuncAssign(instIdx, dest, "sign", 1);
            return;
        case IR::OP_ISNAN:
            EmitFuncAssign(instIdx, dest, "isnan", 1);
            return;
        case IR::OP_ISINF:
            EmitFuncAssign(instIdx, dest, "isinf", 1);
            return;
        case IR::OP_ISFINITE:
            EmitFuncAssign(instIdx, dest, "bwsl_isfinite", 1);
            return;
        case IR::OP_ISNORMAL:
            EmitFuncAssign(instIdx, dest, "bwsl_isnormal", 1);
            return;
        case IR::OP_ANY:
        case IR::OP_ALL:
            if (Op(instIdx, 0) < regCount && ir->registerTypes[Op(instIdx, 0)] == static_cast<u16>(CoreType::BOOL)) {
                EmitRegWithDecl(dest); out.Lit(" = "); EmitExpr(Op(instIdx, 0)); out.Chr(';');
            } else {
                EmitFuncAssign(instIdx, dest, opcode == IR::OP_ANY ? "any" : "all", 1);
            }
            return;

        // ===== Vector Operations =====
        case IR::OP_VEC_CONSTRUCT:
            EmitVecConstruct(instIdx);
            return;

        case IR::OP_VEC_EXTRACT:
            EmitRegWithDecl(dest);
            out.Lit(" = ");
            EmitExpr(Op(instIdx, 0));
            out.Chr('.');
            out.Chr(Str::SWIZZLE[Op(instIdx, 1) & 3]);
            out.Lit(";");
            return;

        case IR::OP_VEC_SHUFFLE:
            EmitSwizzle(instIdx);
            return;

        case IR::OP_VEC_INSERT: {
            // Insert a component into a vector
            // dest = vec with component[index] = value
            u16 vecReg = Op(instIdx, 0);
            u16 componentIdx = Op(instIdx, 1);
            u16 valueReg = Op(instIdx, 2);
            EmitRegWithDecl(dest);
            out.Lit(" = ");
            EmitExpr(vecReg);
            out.Lit(";\n");
            out.NL(indent);
            EmitRegWithDecl(dest);
            out.Chr('['); out.Uint(componentIdx); out.Chr(']');
            out.Lit(" = ");
            EmitExpr(valueReg);
            out.Lit(";");
            return;
        }

        case IR::OP_VEC_INSERT_DYNAMIC:
            EmitRegWithDecl(dest); out.Lit(" = "); EmitExpr(Op(instIdx, 0)); out.Chr(';');
            out.NL(indent); EmitReg(dest); out.Chr('['); EmitExpr(Op(instIdx, 2)); out.Lit("] = ");
            EmitExpr(Op(instIdx, 1)); out.Chr(';');
            return;

        // ===== Texture =====
        case IR::OP_TEX_SAMPLE_CMP: {
            EmitRegWithDecl(dest); out.Lit(" = "); EmitType(Type(instIdx));
            out.Lit("(texture("); EmitTexture(Op(instIdx, 0), ir->metadata[instIdx]);
            u16 coord = Op(instIdx, 1);
            bool threeCoordinates = coord < regCount && ir->registerTypes[coord] == static_cast<u16>(CoreType::FLOAT3);
            out.Str(threeCoordinates ? ", vec4(" : ", vec3(");
            EmitExpr(coord); out.Lit(", "); EmitExpr(Op(instIdx, 2)); out.Lit(")));");
            return;
        }
        case IR::OP_TEX_SAMPLE:
            EmitRegWithDecl(dest);
            out.Lit(" = texture(");
            EmitTexture(Op(instIdx, 0), ir->metadata[instIdx]);  // sampler
            out.Lit(", ");
            EmitExpr(Op(instIdx, 1));  // coord
            out.Lit(");");
            return;

        case IR::OP_TEX_SAMPLE_OFFSET:
            EmitRegWithDecl(dest);
            out.Lit(" = textureOffset(");
            EmitTexture(Op(instIdx, 0), ir->metadata[instIdx]);
            out.Lit(", ");
            EmitExpr(Op(instIdx, 1));
            out.Lit(", ");
            EmitTextureOffset(Op(instIdx, 2));
            out.Lit(");");
            return;

        case IR::OP_TEX_SAMPLE_LOD:
            EmitRegWithDecl(dest);
            out.Lit(" = textureLod(");
            EmitTexture(Op(instIdx, 0), ir->metadata[instIdx]);  // sampler
            out.Lit(", ");
            EmitExpr(Op(instIdx, 1));  // coord
            out.Lit(", ");
            EmitExpr(Op(instIdx, 2));  // lod
            out.Lit(");");
            return;

        case IR::OP_TEX_SAMPLE_LOD_OFFSET:
            EmitRegWithDecl(dest);
            out.Lit(" = textureLodOffset(");
            EmitTexture(Op(instIdx, 0), ir->metadata[instIdx]);
            out.Lit(", ");
            EmitExpr(Op(instIdx, 1));
            out.Lit(", ");
            EmitExpr(Op(instIdx, 2));
            out.Lit(", ");
            EmitTextureOffset(Op(instIdx, 3));
            out.Lit(");");
            return;

        case IR::OP_TEX_SAMPLE_BIAS:
            // GLSL ES 300 has texture with bias
            EmitRegWithDecl(dest);
            out.Lit(" = texture(");
            EmitTexture(Op(instIdx, 0), ir->metadata[instIdx]);  // sampler
            out.Lit(", ");
            EmitExpr(Op(instIdx, 1));  // coord
            out.Lit(", ");
            EmitExpr(Op(instIdx, 2));  // bias
            out.Lit(");");
            return;

        case IR::OP_TEX_SAMPLE_BIAS_OFFSET:
            EmitRegWithDecl(dest);
            out.Lit(" = textureOffset(");
            EmitTexture(Op(instIdx, 0), ir->metadata[instIdx]);
            out.Lit(", ");
            EmitExpr(Op(instIdx, 1));
            out.Lit(", ");
            EmitTextureOffset(Op(instIdx, 3));
            out.Lit(", ");
            EmitExpr(Op(instIdx, 2));
            out.Lit(");");
            return;

        case IR::OP_TEX_SAMPLE_GRAD:
            EmitRegWithDecl(dest);
            out.Lit(" = textureGrad(");
            EmitTexture(Op(instIdx, 0), ir->metadata[instIdx]);  // sampler
            out.Lit(", ");
            EmitExpr(Op(instIdx, 1));  // coord
            out.Lit(", ");
            EmitExpr(Op(instIdx, 2));  // dPdx
            out.Lit(", ");
            EmitExpr(Op(instIdx, 3));  // dPdy
            out.Lit(");");
            return;

        case IR::OP_TEX_FETCH:
            EmitRegWithDecl(dest);
            out.Lit(" = texelFetch(");
            EmitTexture(Op(instIdx, 0), ir->metadata[instIdx]);  // sampler
            out.Lit(", ");
            EmitExpr(Op(instIdx, 1));  // coord (ivec)
            out.Lit(", ");
            EmitExpr(Op(instIdx, 2));  // lod
            out.Lit(");");
            return;

        case IR::OP_TEX_FETCH_OFFSET:
            EmitRegWithDecl(dest);
            out.Lit(" = texelFetchOffset(");
            EmitTexture(Op(instIdx, 0), ir->metadata[instIdx]);
            out.Lit(", ");
            EmitExpr(Op(instIdx, 1));
            out.Lit(", ");
            EmitExpr(Op(instIdx, 2));
            out.Lit(", ");
            EmitTextureOffset(Op(instIdx, 3));
            out.Lit(");");
            return;

        case IR::OP_TEX_SIZE:
            EmitRegWithDecl(dest);
            out.Lit(" = textureSize(");
            EmitTexture(Op(instIdx, 0), ir->metadata[instIdx]);  // sampler
            out.Lit(", ");
            EmitExpr(Op(instIdx, 1));  // lod
            out.Chr(')');
            if (renderConfig && Type(instIdx) == static_cast<u16>(CoreType::INT2)) {
                u16 slot = Op(instIdx, 0) & 0x0FFF;
                for (const auto& texture : renderConfig->textures) {
                    if (texture.bindingIndex == slot && (texture.isVolume || texture.isArray)) {
                        out.Lit(".xy");
                        break;
                    }
                }
            }
            out.Chr(';');
            return;

        case IR::OP_TEX_LEVELS:
            EmitRegWithDecl(dest);
            out.Lit(" = max(");
            EmitTextureLevelsUniformName(Op(instIdx, 0));
            out.Lit(", 1);");
            return;

        case IR::OP_TEX_GATHER:
            EmitRegWithDecl(dest);
            out.Lit(" = bwsl_texture_gather(");
            EmitTexture(Op(instIdx, 0), ir->metadata[instIdx]);  // sampler
            out.Lit(", ");
            EmitExpr(Op(instIdx, 1));  // coord
            out.Lit(", ");
            EmitExpr(Op(instIdx, 2));  // component
            out.Lit(");");
            return;

        case IR::OP_TEX_GATHER_OFFSET:
            EmitRegWithDecl(dest);
            out.Lit(" = bwsl_texture_gather_offset(");
            EmitTexture(Op(instIdx, 0), ir->metadata[instIdx]);
            out.Lit(", ");
            EmitExpr(Op(instIdx, 1));
            out.Lit(", ");
            EmitExpr(Op(instIdx, 2));
            out.Lit(", ");
            EmitExpr(Op(instIdx, 3));
            out.Lit(");");
            return;

        case IR::OP_LOAD_TEX_HANDLE:
            // Bindless textures - emit as sampler reference
            EmitRegWithDecl(dest);
            out.Lit(" = sampler");
            out.Uint(Op(instIdx, 0));
            out.Lit(";");
            return;

        // ===== Derivatives (Fragment only) =====
        case IR::OP_DDX:
            EmitFuncAssign(instIdx, dest, "dFdx", 1);
            return;
        case IR::OP_DDY:
            EmitFuncAssign(instIdx, dest, "dFdy", 1);
            return;
        case IR::OP_DDX_FINE:
            // GLSL ES 300 doesn't have fine derivatives, use regular
            EmitFuncAssign(instIdx, dest, "dFdx", 1);
            return;
        case IR::OP_DDY_FINE:
            EmitFuncAssign(instIdx, dest, "dFdy", 1);
            return;
        case IR::OP_DDX_COARSE:
            EmitFuncAssign(instIdx, dest, "dFdx", 1);
            return;
        case IR::OP_DDY_COARSE:
            EmitFuncAssign(instIdx, dest, "dFdy", 1);
            return;
        case IR::OP_FWIDTH:
            EmitFuncAssign(instIdx, dest, "fwidth", 1);
            return;
        case IR::OP_FWIDTH_FINE:
            EmitFuncAssign(instIdx, dest, "fwidth", 1);
            return;
        case IR::OP_FWIDTH_COARSE:
            EmitFuncAssign(instIdx, dest, "fwidth", 1);
            return;

        // ===== Select (ternary) =====
        case IR::OP_SELECT:
            EmitRegWithDecl(dest);
            out.Lit(" = ");
            EmitSelect(instIdx);
            out.Lit(";");
            return;

        // ===== Matrix Operations =====
        case IR::OP_MAT_MUL:
        case IR::OP_MAT_VEC_MUL:
        case IR::OP_VEC_MAT_MUL:
            EmitRegWithDecl(dest);
            out.Lit(" = ");
            EmitExpr(Op(instIdx, 0));
            out.Lit(" * ");
            EmitExpr(Op(instIdx, 1));
            out.Lit(";");
            return;

        case IR::OP_MAT_TRANSPOSE:
            EmitFuncAssign(instIdx, dest, "transpose", 1);
            return;
        case IR::OP_MAT_INVERSE:
            EmitFuncAssign(instIdx, dest, "inverse", 1);
            return;
        case IR::OP_MAT_DET:
            EmitFuncAssign(instIdx, dest, "determinant", 1);
            return;

        case IR::OP_MAT_CONSTRUCT: {
            // Build matrix from values
            u16 type = Type(instIdx);
            EmitRegWithDecl(dest);
            out.Lit(" = ");
            EmitType(type);
            out.Chr('(');
            // Matrix constructors take columns, count depends on type
            u32 cols = 2;
            if (type == static_cast<u16>(CoreType::MAT3)) cols = 3;
            else if (type == static_cast<u16>(CoreType::MAT4)) cols = 4;
            for (u32 i = 0; i < cols && i < 4; i++) {
                if (i > 0) out.Lit(", ");
                EmitExpr(Op(instIdx, i));
            }
            out.Lit(");");
            return;
        }

        case IR::OP_MAT_SCALE:
            // Matrix * Scalar
            EmitRegWithDecl(dest);
            out.Lit(" = ");
            EmitExpr(Op(instIdx, 0));
            out.Lit(" * ");
            EmitExpr(Op(instIdx, 1));
            out.Lit(";");
            return;

        case IR::OP_MAT_IDENTITY: {
            // Identity matrix
            u16 type = Type(instIdx);
            EmitRegWithDecl(dest);
            out.Lit(" = ");
            EmitType(type);
            out.Lit("(1.0);");
            return;
        }

        case IR::OP_MAT_ZERO: {
            // Zero matrix
            u16 type = Type(instIdx);
            EmitRegWithDecl(dest);
            out.Lit(" = ");
            EmitType(type);
            out.Lit("(0.0);");
            return;
        }

        // ===== Struct Operations =====
        case IR::OP_STRUCT_CONSTRUCT: {
            // Build struct from field values - emit as struct constructor
            EmitRegWithDecl(dest);
            out.Lit(" = ");
            u32 structHash = ir->metadata[instIdx];
            if (structHash == 0 && ir->registerStructTypes && dest < ir->registerCount) {
                structHash = ir->registerStructTypes[dest];
            }
            EmitStructTypeName(structHash);
            out.Chr('(');
            bool first = true;
            for (u32 i = 0; i < 4; i++) {
                u16 valueReg = Op(instIdx, i);
                if (valueReg == 0xFFFF || valueReg == 0x3FFF) continue;
                if (!first) out.Lit(", ");
                EmitExpr(valueReg);
                first = false;
            }
            out.Lit(");");
            return;
        }

        case IR::OP_STRUCT_EXTRACT: {
            // Extract field from struct
            u16 structReg = Op(instIdx, 0);
            u16 fieldIdx = Op(instIdx, 1);
            u32 structHash = (ir->registerStructTypes && structReg < ir->registerCount)
                                 ? ir->registerStructTypes[structReg]
                                 : 0;
            EmitRegWithDecl(dest);
            out.Lit(" = ");
            EmitExpr(structReg);
            out.Chr('.');
            EmitStructFieldNameByIndex(structHash, fieldIdx);
            out.Lit(";");
            return;
        }

        case IR::OP_STRUCT_INSERT: {
            // Insert field into struct
            u16 structReg = Op(instIdx, 0);
            u16 fieldIdx = Op(instIdx, 1);
            u16 valueReg = Op(instIdx, 2);
            u32 structHash = (ir->registerStructTypes && structReg < ir->registerCount)
                                 ? ir->registerStructTypes[structReg]
                                 : 0;
            EmitRegWithDecl(dest);
            out.Lit(" = ");
            EmitExpr(structReg);
            out.Lit(";\n");
            out.NL(indent);
            EmitRegWithDecl(dest);
            out.Chr('.');
            EmitStructFieldNameByIndex(structHash, fieldIdx);
            out.Lit(" = ");
            EmitExpr(valueReg);
            out.Lit(";");
            return;
        }

        case IR::OP_STRUCT_ARRAY_EXTRACT: {
            // Element read from an array field: dest = struct.field[index]
            u16 structReg = Op(instIdx, 0);
            u16 fieldIdx = Op(instIdx, 1);
            u16 indexReg = Op(instIdx, 2);
            u32 structHash = ir->metadata[instIdx];
            if (structHash == 0 && ir->registerStructTypes &&
                structReg < ir->registerCount) {
                structHash = ir->registerStructTypes[structReg];
            }
            EmitRegWithDecl(dest);
            out.Lit(" = ");
            EmitExpr(structReg);
            out.Chr('.');
            EmitStructFieldNameByIndex(structHash, fieldIdx);
            out.Chr('[');
            EmitExpr(indexReg);
            out.Lit("];");
            return;
        }

        case IR::OP_STRUCT_ARRAY_INSERT: {
            // Element write into an array field:
            // dest = struct with field[index] = value
            u16 structReg = Op(instIdx, 0);
            u16 fieldIdx = Op(instIdx, 1);
            u16 indexReg = Op(instIdx, 2);
            u16 valueReg = Op(instIdx, 3);
            u32 structHash = ir->metadata[instIdx];
            if (structHash == 0 && ir->registerStructTypes &&
                structReg < ir->registerCount) {
                structHash = ir->registerStructTypes[structReg];
            }
            EmitRegWithDecl(dest);
            out.Lit(" = ");
            EmitExpr(structReg);
            out.Lit(";\n");
            out.NL(indent);
            EmitRegWithDecl(dest);
            out.Chr('.');
            EmitStructFieldNameByIndex(structHash, fieldIdx);
            out.Chr('[');
            EmitExpr(indexReg);
            out.Lit("] = ");
            EmitExpr(valueReg);
            out.Lit(";");
            return;
        }

        // ===== Array Operations =====
        // Note: OP_ALLOC_ARRAY (0x1C) shares value with OP_LOAD_INPUT (0x1C)
        // OP_LOAD_INPUT is handled above with the other load operations

        case IR::OP_ARRAY_ACCESS: {
            // Get element address - emit as array indexing
            u16 arrayReg = Op(instIdx, 0);
            u16 indexReg = Op(instIdx, 1);
            EmitRegWithDecl(dest);
            out.Lit(" = ");
            EmitExpr(arrayReg);
            out.Chr('[');
            EmitExpr(indexReg);
            out.Lit("];");
            return;
        }

        case IR::OP_ARRAY_LOAD: {
            // Load from array element
            u16 arrayReg = Op(instIdx, 0);
            u16 indexReg = Op(instIdx, 1);
            EmitRegWithDecl(dest);
            out.Lit(" = ");
            EmitExpr(arrayReg);
            out.Chr('[');
            EmitExpr(indexReg);
            out.Lit("];");
            return;
        }

        case IR::OP_ARRAY_STORE: {
            // Store to array element
            u16 arrayReg = dest;
            u16 indexReg = Op(instIdx, 0);
            u16 valueReg = Op(instIdx, 1);
            EmitExpr(arrayReg);
            out.Chr('[');
            EmitExpr(indexReg);
            out.Lit("] = ");
            EmitExpr(valueReg);
            out.Lit(";");
            return;
        }

        case IR::OP_ARRAY_CONSTRUCT: {
            // Build array from elements
            u16 type = Type(instIdx);
            EmitRegWithDecl(dest);
            out.Lit(" = ");
            EmitType(type);
            out.Lit("[](");
            for (u32 i = 0; i < 4; i++) {
                u16 op = Op(instIdx, i);
                if (op == 0x3FFF) break;  // Invalid marker
                if (i > 0) out.Lit(", ");
                EmitExpr(op);
            }
            out.Lit(");");
            return;
        }

        // ===== Synchronization =====
        case IR::OP_BARRIER:
            // GLSL ES 300 compute shaders have barrier()
            out.Lit("barrier();");
            return;

        case IR::OP_MEM_FENCE:
            // GLSL ES 300: memoryBarrier variants
            out.Lit("memoryBarrier();");
            return;

        // ===== Enum Operations (emit as int) =====
        case IR::OP_ENUM_CONSTRUCT:
            EmitRegWithDecl(dest);
            out.Lit(" = ");
            EmitExpr(Op(instIdx, 0));
            out.Lit(";");
            return;

        case IR::OP_ENUM_TAG:
            EmitRegWithDecl(dest);
            out.Lit(" = ");
            EmitExpr(Op(instIdx, 0));
            out.Lit(";");
            return;

        case IR::OP_ENUM_FIELD:
            EmitRegWithDecl(dest);
            out.Lit(" = ");
            EmitExpr(Op(instIdx, 0));
            out.Lit(";");
            return;

        // ===== Storage Buffer Operations =====
        case IR::OP_STORAGE_PTR:
        case IR::OP_STORAGE_FIELD:
        case IR::OP_STORAGE_INDEX:
        case IR::OP_STORAGE_LOAD:
            // These require SSBO support - emit as placeholder
            out.Lit("// Storage buffer op not fully supported in GLES 300");
            return;

        // ===== Atomics (not supported in GLSL ES 300 for non-compute) =====
        case IR::OP_ATOMIC_ADD:
        case IR::OP_ATOMIC_SUB:
        case IR::OP_ATOMIC_MIN:
        case IR::OP_ATOMIC_MAX:
        case IR::OP_ATOMIC_AND:
        case IR::OP_ATOMIC_OR:
        case IR::OP_ATOMIC_XOR:
        case IR::OP_ATOMIC_XCHG:
        case IR::OP_ATOMIC_CMP_XCHG:
            out.Lit("// Atomic ops require compute shader support");
            return;

        // ===== Wave/Subgroup Operations (not in GLSL ES 300) =====
        case IR::OP_WAVE_MIN:
        case IR::OP_WAVE_MAX:
        case IR::OP_WAVE_ALL:
        case IR::OP_WAVE_ANY:
        case IR::OP_WAVE_BALLOT:
        case IR::OP_WAVE_READ_FIRST:
        case IR::OP_WAVE_READ_LANE:
        case IR::OP_WAVE_SUM:
        case IR::OP_WAVE_MUL:
            out.Lit("// Wave ops not supported in GLSL ES 300");
            return;

        // ===== Call (function calls - should be inlined) =====
        case IR::OP_CALL:
            out.Lit("// Function call - should be inlined");
            return;

        default:
            out.Lit("// TODO: opcode 0x");
            // Emit hex
            {
                char hex[3];
                hex[0] = "0123456789ABCDEF"[(opcode >> 4) & 0xF];
                hex[1] = "0123456789ABCDEF"[opcode & 0xF];
                hex[2] = 0;
                out.Str(hex);
            }
            return;
    }
}

// ============================================================================
// Expression Emission Helpers
// ============================================================================

void GLESBuilder::EmitExpr(u16 reg) {
    // Bool must precede float; uint and texture handles share an encoding,
    // so texture instructions explicitly call EmitTexture instead.
    if ((reg & 0xC000) == 0xC000) {
        u16 idx = reg & 0x3FFF;
        out.Str(idx < ir->boolCount && ir->boolConstants[idx] ? "true" : "false");
        return;
    }
    if (reg & 0x8000) {
        u16 idx = reg & 0x7FFF;
        out.Flt(idx < ir->floatCount ? ir->floatConstants[idx] : 0.0f);
        return;
    }
    if (reg & 0x4000) {
        u16 idx = reg & 0x3FFF;
        out.Int(idx < ir->intCount ? static_cast<s32>(ir->intConstants[idx]) : 0);
        return;
    }
    if ((reg & 0xE000) == 0x2000) {
        u16 idx = reg & 0x1FFF;
        out.Uint(idx < ir->uintCount ? ir->uintConstants[idx] : 0u);
        out.Chr('u');
        return;
    }

    // Invalid register marker
    if (reg == 0x3FFF) {
        out.Lit("0.0 /* invalid */");  // Emit placeholder instead of nothing
        return;
    }

    // Keep values at their defining instruction, including mutable reads and
    // values crossing loop edges. The driver can optimize the temporaries.
    EmitReg(reg);
}

void GLESBuilder::EmitLoadExpr(u32 instIdx) {
    u16 opcode = ir->opcodes[instIdx];

    switch (opcode) {
        // ===== Memory/Load Operations =====
        case IR::OP_LOAD_CONST: {
            u16 constReg = Op(instIdx, 0);
            EmitExpr(constReg);
            return;
        }

        case IR::OP_LOAD_ATTR: {
            u16 attrIdx = Op(instIdx, 0);
            out.Lit("attr");
            out.Uint(attrIdx);
            return;
        }

        case IR::OP_LOAD_INPUT: {
            u16 inputIdx = Op(instIdx, 0);

            // Check for built-in inputs (slot >= 0x80)
            if (inputIdx >= 0x80) {
                switch (inputIdx) {
                    case 0x80: out.Lit("uint(gl_VertexID)"); return;
                    case 0x81: out.Lit("uint(gl_InstanceID)"); return;
                    case 0x90: out.Lit("gl_GlobalInvocationID"); return;
                    case 0x91: out.Lit("gl_LocalInvocationID"); return;
                    case 0x92: out.Lit("gl_WorkGroupID"); return;
                    case 0x93: out.Lit("gl_NumWorkGroups"); return;
                    case 0x94: out.Lit("gl_LocalInvocationIndex"); return;
                    default: out.Lit("gl_BuiltIn"); out.Uint(inputIdx); return;
                }
            }

            // Convert IR slot (VARYING0=2, etc.) to VaryingInfo slot (0-based)
            u16 varyingSlot = inputIdx - 2;  // OutputSlot::VARYING0 = 2

            // Fragment/vertex shader inputs: search varyings by converted slot number
            if (varyings) {
                for (u32 i = 0; i < varyings->count; i++) {
                    if (varyings->varyings[i].slot == varyingSlot) {
                        out.Lit("v_");
                        out.Str(varyings->varyings[i].name);
                        return;
                    }
                }
            }

            // Fallback for unmapped inputs
            out.Lit("v_slot");
            out.Uint(varyingSlot);
            return;
        }

        case IR::OP_LOAD_UNIFORM: {
            u16 uniformIdx = Op(instIdx, 0);
            if (renderConfig) {
                for (const auto& ub : renderConfig->uniformBuffers) {
                    if (ub.bindingIndex != uniformIdx) continue;
                    out.Lit("ub_"); out.Str(ub.name.c_str());
                    out.Lit(".u_"); out.Str(ub.name.c_str());
                    return;
                }
            }
            out.Lit("u_uniform"); out.Uint(uniformIdx);
            return;
        }

        case IR::OP_LOAD_REG:
        case IR::OP_STORE_REG:
            EmitExpr(Op(instIdx, 0));
            return;

        default:
            out.Lit("0.0 /* unsupported load */");
            return;
    }
}

void GLESBuilder::EmitBinaryOp(u32 instIdx, const char* op) {
    out.Chr('(');
    EmitExpr(Op(instIdx, 0));
    out.Chr(' ');
    out.Str(op);
    out.Chr(' ');
    EmitExpr(Op(instIdx, 1));
    out.Chr(')');
}

void GLESBuilder::EmitUnaryOp(u32 instIdx, const char* op) {
    out.Str(op);
    out.Chr('(');
    EmitExpr(Op(instIdx, 0));
    out.Chr(')');
}

void GLESBuilder::EmitFuncCall(u32 instIdx, const char* func, u32 arity) {
    out.Str(func);
    out.Chr('(');
    for (u32 i = 0; i < arity; i++) {
        if (i > 0) out.Lit(", ");
        EmitExpr(Op(instIdx, i));
    }
    out.Chr(')');
}

static u32 GLESComponentCount(u16 type) {
    switch (static_cast<CoreType>(type)) {
        case CoreType::BOOL2: case CoreType::INT2: case CoreType::UINT2: case CoreType::FLOAT2: return 2;
        case CoreType::BOOL3: case CoreType::INT3: case CoreType::UINT3: case CoreType::FLOAT3: return 3;
        case CoreType::BOOL4: case CoreType::INT4: case CoreType::UINT4: case CoreType::FLOAT4: return 4;
        default: return 1;
    }
}

bool GLESBuilder::EmitConstantExpr(u16 reg, u32 depth) {
    if (depth > 32 || reg == 0x3FFF) return false;
    if (reg >= 0x2000) { EmitExpr(reg); return true; }
    u32 definition = NO_BLOCK;
    for (u32 i = 0; i < ir->instructionCount; ++i) {
        u16 op = ir->opcodes[i];
        if (Dest(i) != reg || IR::IsOutputOpcode(static_cast<IR::OpCode>(op)) ||
            op == IR::OP_BRANCH || op == IR::OP_JUMP || op == IR::OP_RET ||
            op == IR::OP_NOP) continue;
        if (definition != NO_BLOCK) return false;
        definition = i;
    }
    if (definition == NO_BLOCK) return false;
    u16 op = Opcode(definition);
    u32 start = out.len;
    bool ok = false;
    if (op == IR::OP_LOAD_CONST || op == IR::OP_LOAD_REG || op == IR::OP_STORE_REG) {
        ok = EmitConstantExpr(Op(definition, 0), depth + 1);
    } else if (op == IR::OP_VEC_CONSTRUCT) {
        EmitType(Type(definition)); out.Chr('(');
        ok = true;
        bool first = true;
        const u32 components = GLESComponentCount(Type(definition));
        u32 consumed = 0;
        for (u32 i = 0; i < 4 && consumed < components; ++i) {
            u16 value = Op(definition, i);
            if (value == 0x3FFF) continue;
            if (!first) out.Lit(", ");
            first = false;
            if (!EmitConstantExpr(value, depth + 1)) { ok = false; break; }
            consumed += value < regCount ? GLESComponentCount(ir->registerTypes[value]) : 1;
        }
        out.Chr(')');
    } else if (op >= IR::OP_F2I && op <= IR::OP_U2I) {
        EmitType(Type(definition)); out.Chr('(');
        ok = EmitConstantExpr(Op(definition, 0), depth + 1);
        out.Chr(')');
    } else {
        const char* operation = nullptr;
        bool unary = false;
        switch (op) {
            case IR::OP_INEG: case IR::OP_FNEG: operation = "-"; unary = true; break;
            case IR::OP_IADD: case IR::OP_FADD: operation = " + "; break;
            case IR::OP_ISUB: case IR::OP_FSUB: operation = " - "; break;
            case IR::OP_IMUL: case IR::OP_FMUL: operation = " * "; break;
            case IR::OP_IDIV: case IR::OP_FDIV: operation = " / "; break;
            case IR::OP_IMOD: operation = " % "; break;
            case IR::OP_SHL: operation = " << "; break;
            case IR::OP_SHR: case IR::OP_ASR: operation = " >> "; break;
            case IR::OP_AND: operation = " & "; break;
            case IR::OP_OR: operation = " | "; break;
            case IR::OP_XOR: operation = " ^ "; break;
            case IR::OP_NOT: operation = "~"; unary = true; break;
            default: break;
        }
        if (!operation) return false;
        out.Chr('(');
        if (unary) out.Str(operation);
        ok = EmitConstantExpr(Op(definition, 0), depth + 1);
        if (!unary) {
            out.Str(operation);
            ok = EmitConstantExpr(Op(definition, 1), depth + 1) && ok;
        }
        out.Chr(')');
    }
    if (!ok) out.len = start;
    return ok;
}

void GLESBuilder::EmitTextureOffset(u16 reg) {
    // GLSL offset operands must remain constant expressions, even when the IR
    // materializes a literal vector in an SSA temporary.
    if (!EmitConstantExpr(reg)) EmitExpr(reg);
}

void GLESBuilder::EmitTexture(u16 reg, u32 metadata) {
    u16 slot = reg & 0x0FFF;
    if (renderConfig) {
        for (const auto& texture : renderConfig->textures) {
            if (texture.bindingIndex != slot) continue;
            if (texture.separateSampler) {
                u32 binding = texture.defaultSamplerBinding;
                if (TextureOpHasExplicitSampler(metadata)) {
                    u16 samplerSlot = GetTextureOpExplicitSamplerBinding(metadata);
                    for (const auto& sampler : renderConfig->samplers)
                        if (sampler.bindingIndex == samplerSlot) binding = sampler.descriptorBinding;
                }
                out.Lit("bwsl_tex_0_"); out.Uint(slot);
                out.Lit("_sampler_2_"); out.Uint(binding);
                return;
            }
            out.Lit("u_"); out.Str(texture.name.c_str());
            return;
        }
    }
    out.Lit("sampler"); out.Uint(slot);
}

void GLESBuilder::EmitSelect(u32 instIdx) {
    u16 condition = Op(instIdx, 2);
    u32 count = condition < regCount ? GLESComponentCount(ir->registerTypes[condition]) : 1;
    if (count == 1) {
        out.Chr('('); EmitExpr(condition); out.Lit(" ? ");
        EmitExpr(Op(instIdx, 1)); out.Lit(" : "); EmitExpr(Op(instIdx, 0)); out.Chr(')');
        return;
    }
    // GLSL's ternary takes a scalar condition; construct vector select per lane.
    EmitType(Type(instIdx)); out.Chr('(');
    for (u32 i = 0; i < count; ++i) {
        if (i) out.Lit(", ");
        EmitExpr(condition); out.Chr('.'); out.Chr(Str::SWIZZLE[i]); out.Lit(" ? ");
        EmitExpr(Op(instIdx, 1)); out.Chr('.'); out.Chr(Str::SWIZZLE[i]); out.Lit(" : ");
        EmitExpr(Op(instIdx, 0)); out.Chr('.'); out.Chr(Str::SWIZZLE[i]);
    }
    out.Chr(')');
}

void GLESBuilder::EmitShuffleExpr(u32 instIdx) {
    u16 type = Type(instIdx);
    u32 count = GLESComponentCount(type);
    u16 first = Op(instIdx, 0), second = Op(instIdx, 1);
    u32 firstCount = first < regCount ? GLESComponentCount(ir->registerTypes[first]) : 4;
    if (count > 1) { EmitType(type); out.Chr('('); }
    for (u32 i = 0; i < count; ++i) {
        if (i) out.Lit(", ");
        u32 component = (ir->metadata[instIdx] >> (i * 4)) & 15;
        bool fromFirst = component < firstCount;
        EmitExpr(fromFirst ? first : second);
        out.Chr('['); out.Uint(fromFirst ? component : component - firstCount); out.Chr(']');
    }
    if (count > 1) out.Chr(')');
}

void GLESBuilder::EmitSwizzle(u32 instIdx) {
    EmitRegWithDecl(Dest(instIdx)); out.Lit(" = "); EmitShuffleExpr(instIdx); out.Chr(';');
}

// Helper to check if an operand is a valid value reference
bool GLESBuilder::IsValidOperand(u16 op) const {
    if (op == 0x3FFF) return false;  // Explicit invalid marker

    if ((op & 0xC000) == 0xC000) return (op & 0x3FFF) < ir->boolCount;
    if (op & 0x8000) return (op & 0x7FFF) < ir->floatCount;
    if (op & 0x4000) return (op & 0x3FFF) < ir->intCount;
    if ((op & 0xE000) == 0x2000) return (op & 0x1FFF) < ir->uintCount;

    // Register reference - always valid (r0 is valid!)
    return true;
}

void GLESBuilder::EmitVecConstruct(u32 instIdx) {
    u16 dest = ir->destinations[instIdx];
    u16 type = Type(instIdx);

    EmitRegWithDecl(dest);
    out.Lit(" = ");

    // Priority for determining type:
    // 1. Instruction's stored type (if valid)
    // 2. Destination register's type (if valid)
    // 3. Fall back to operand counting (last resort)

    bool typeValid = (type != 0 &&
                      type != static_cast<u16>(CoreType::VOID) &&
                      type != static_cast<u16>(CoreType::INVALID));

    if (!typeValid && ir->registerTypes && dest < regCount) {
        u16 regType = ir->registerTypes[dest];
        if (regType != 0 &&
            regType != static_cast<u16>(CoreType::VOID) &&
            regType != static_cast<u16>(CoreType::INVALID)) {
            type = regType;
            typeValid = true;
        }
    }

    // Count valid operands (for emission, not type inference)
    u32 operandCount = 0;
    for (u32 i = 0; i < 4; i++) {
        u16 op = Op(instIdx, i);
        if (IsValidOperand(op)) {
            operandCount++;
        } else {
            break;
        }
    }
    if (operandCount == 0) operandCount = 1;

    // If type still invalid, infer from operand count (legacy fallback)
    // Note: This is imprecise for vec4(vec3, float) cases
    if (!typeValid) {
        // Check first operand type to determine base type
        u16 firstOp = Op(instIdx, 0);
        bool isInt = false;
        bool isUint = false;

        // Check if first operand is an int constant (0x4000 prefix)
        if ((firstOp & 0x4000) && !(firstOp & 0x8000)) {
            isInt = true;
        }

        // Default to float vectors based on operand count
        // This is a fallback - prefer using stored types
        if (operandCount == 1) {
            type = isInt ? static_cast<u16>(CoreType::INT) :
                   isUint ? static_cast<u16>(CoreType::UINT) :
                   static_cast<u16>(CoreType::FLOAT);
        } else if (operandCount == 2) {
            type = isInt ? static_cast<u16>(CoreType::INT2) :
                   isUint ? static_cast<u16>(CoreType::UINT2) :
                   static_cast<u16>(CoreType::FLOAT2);
        } else if (operandCount == 3) {
            type = isInt ? static_cast<u16>(CoreType::INT3) :
                   isUint ? static_cast<u16>(CoreType::UINT3) :
                   static_cast<u16>(CoreType::FLOAT3);
        } else {
            type = isInt ? static_cast<u16>(CoreType::INT4) :
                   isUint ? static_cast<u16>(CoreType::UINT4) :
                   static_cast<u16>(CoreType::FLOAT4);
        }
    }

    EmitType(type);
    out.Chr('(');

    // Determine how many components the target type has
    u32 components = 4;
    if (type == static_cast<u16>(CoreType::FLOAT2) ||
        type == static_cast<u16>(CoreType::INT2) ||
        type == static_cast<u16>(CoreType::UINT2)) {
        components = 2;
    } else if (type == static_cast<u16>(CoreType::FLOAT3) ||
               type == static_cast<u16>(CoreType::INT3) ||
               type == static_cast<u16>(CoreType::UINT3)) {
        components = 3;
    } else if (type == static_cast<u16>(CoreType::FLOAT) ||
               type == static_cast<u16>(CoreType::INT) ||
               type == static_cast<u16>(CoreType::UINT) ||
               type == static_cast<u16>(CoreType::BOOL)) {
        components = 1;
    }

    // Check if this is a scalar splat (all operands are the same)
    bool isScalarSplat = operandCount > 1;
    u16 firstOp = Op(instIdx, 0);
    for (u32 i = 1; i < operandCount && isScalarSplat; i++) {
        if (Op(instIdx, i) != firstOp) {
            isScalarSplat = false;
        }
    }

    if (isScalarSplat) {
        // Scalar splat: emit single value, GLSL will broadcast
        EmitExpr(firstOp);
    } else {
        // Normal case: emit minimum of operandCount and components
        u32 emitCount = (operandCount < components) ? operandCount : components;
        bool first = true;
        for (u32 i = 0; i < emitCount; i++) {
            u16 op = Op(instIdx, i);
            if (!IsValidOperand(op)) continue;
            if (!first) out.Lit(", ");
            first = false;
            EmitExpr(op);
        }
    }
    out.Lit(");");
}

// ============================================================================
// Assignment Emission Helpers
// ============================================================================

void GLESBuilder::EmitBinaryAssign(u32 instIdx, u16 dest, const char* op) {
    EmitRegWithDecl(dest);
    out.Lit(" = ");
    EmitBinaryOp(instIdx, op);
    out.Lit(";");
}

void GLESBuilder::EmitUnaryAssign(u32 instIdx, u16 dest, const char* op) {
    EmitRegWithDecl(dest);
    out.Lit(" = ");
    EmitUnaryOp(instIdx, op);
    out.Lit(";");
}

void GLESBuilder::EmitFuncAssign(u32 instIdx, u16 dest, const char* func, u32 arity) {
    EmitRegWithDecl(dest);
    out.Lit(" = ");
    EmitFuncCall(instIdx, func, arity);
    out.Lit(";");
}

// ============================================================================
// Public API
// ============================================================================

std::string_view GLESBuilder::Emit() {
    EmitHeader();
    EmitInputs();
    EmitOutputs();
    EmitUniforms();
    EmitMain();
    return out.View();
}

// ============================================================================
// Debug
// ============================================================================
void GLESBuilder::DebugDumpRegisterInfo() {
    fprintf(stderr, "\n=== REGISTER DEBUG INFO ===\n");
    
    // 1. Dump PHI nodes and their result registers
    fprintf(stderr, "\n--- PHI Nodes (%u total) ---\n", ir->phiCount);
    if (ir->phiCount > 0 && ir->phiResultRegs && ir->phiBlockIndices) {
        for (u32 i = 0; i < ir->phiCount; i++) {
            u16 resultReg = ir->phiResultRegs[i];
            u32 block = ir->phiBlockIndices[i];
            u16 type = ir->phiTypes ? ir->phiTypes[i] : 0;
            
            fprintf(stderr, "  PHI[%u]: result=r%u, block=%u, type=%u\n", 
                    i, resultReg, block, type);
            
            // Show operands
            u32 opCount = ir->GetPhiOperandCount(i);
            for (u32 j = 0; j < opCount; j++) {
                u16 val = ir->GetPhiOperandValue(i, j);
                u32 srcBlock = ir->GetPhiOperandBlock(i, j);
                fprintf(stderr, "    [fromBlock=%u] <- r%u\n", srcBlock, val);
            }
        }
    }
    
    // 2. Dump undef registers
    fprintf(stderr, "\n--- Undef Registers (%u total) ---\n", ir->undefRegCount);
    if (ir->undefRegCount > 0 && ir->undefRegs) {
        for (u32 i = 0; i < ir->undefRegCount; i++) {
            u16 reg = ir->undefRegs[i];
            u16 type = ir->undefRegTypes ? ir->undefRegTypes[i] : 0;
            fprintf(stderr, "  UNDEF r%u (type=%u)\n", reg, type);
        }
    }
    
    // 3. Find referenced but possibly undefined registers
    fprintf(stderr, "\n--- Potentially Undefined References ---\n");
    
    // Track what registers are defined by instructions
    bool* definedByInst = new bool[ir->registerCount]();
    
    for (u32 i = 0; i < ir->instructionCount; i++) {
        u16 op = ir->opcodes[i];
        if (op == IR::OP_STORE_OUTPUT || op == IR::OP_JUMP || 
            op == IR::OP_BRANCH || op == IR::OP_RET || op == IR::OP_NOP) {
            continue;
        }
        u16 dest = ir->destinations[i];
        if ((dest & 0xE000) == 0 && dest < ir->registerCount) {
            definedByInst[dest] = true;
        }
    }
    
    // Add PHI results
    if (ir->phiResultRegs) {
        for (u32 i = 0; i < ir->phiCount; i++) {
            u16 reg = ir->phiResultRegs[i];
            if (reg < ir->registerCount) {
                definedByInst[reg] = true;
            }
        }
    }
    
    // Add undef registers (they're "defined" as undef)
    if (ir->undefRegs) {
        for (u32 i = 0; i < ir->undefRegCount; i++) {
            u16 reg = ir->undefRegs[i];
            if (reg < ir->registerCount) {
                definedByInst[reg] = true;
            }
        }
    }
    
    // Check instruction operands for undefined references
    for (u32 i = 0; i < ir->instructionCount; i++) {
        for (u32 j = 0; j < 4; j++) {
            u16 opReg = ir->GetOperand(i, j);
            if ((opReg & 0xE000) == 0 && opReg < ir->registerCount) {
                if (!definedByInst[opReg]) {
                    fprintf(stderr, "  Inst[%u] operand[%u] uses r%u - NOT DEFINED\n", 
                            i, j, opReg);
                }
            }
        }
    }
    
    // Check PHI operands for undefined references
    if (ir->phiOperandValues) {
        for (u32 i = 0; i < ir->phiCount; i++) {
            u32 opCount = ir->GetPhiOperandCount(i);
            for (u32 j = 0; j < opCount; j++) {
                u16 val = ir->GetPhiOperandValue(i, j);
                if ((val & 0xE000) == 0 && val < ir->registerCount) {
                    if (!definedByInst[val]) {
                        fprintf(stderr, "  PHI[%u] operand[%u] uses r%u - NOT DEFINED\n",
                                i, j, val);
                    }
                }
            }
        }
    }
    
    delete[] definedByInst;
    fprintf(stderr, "\n=== END REGISTER DEBUG ===\n\n");
}


} // namespace GLES
} // namespace BWSL
