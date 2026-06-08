#pragma once

#include <cstdio>
#include <string>
#include <utility>

#include "bwsl_ast_soa.h"
#include "bwsl_ir_gen.h"
#include "bwsl_render_config.h"
#include "bwsl_resource_reflection.h"

namespace BWSL::ToolCommon {

inline const char* OpCodeToString(IR::OpCode op) {
    switch (op) {
        case IR::OP_NOP:           return "NOP";
        case IR::OP_JUMP:          return "JUMP";
        case IR::OP_BRANCH:        return "BRANCH";
        case IR::OP_CALL:          return "CALL";
        case IR::OP_RET:           return "RET";
        case IR::OP_SELECT:        return "SELECT";
        case IR::OP_PHI:           return "PHI";
        case IR::OP_SWITCH:        return "SWITCH";

        case IR::OP_LOAD_CONST:    return "LOAD_CONST";
        case IR::OP_LOAD_REG:      return "LOAD_REG";
        case IR::OP_STORE_REG:     return "STORE_REG";
        case IR::OP_LOAD_ATTR:     return "LOAD_ATTR";
        case IR::OP_STORE_OUTPUT:  return "STORE_OUTPUT";
        case IR::OP_LOAD_OUTPUT:   return "LOAD_OUTPUT";
        case IR::OP_LOAD_UNIFORM:  return "LOAD_UNIFORM";
        case IR::OP_LOAD_BUFFER:   return "LOAD_BUFFER";
        case IR::OP_STORE_BUFFER:  return "STORE_BUFFER";
        case IR::OP_LOAD_LOCAL:    return "LOAD_LOCAL";
        case IR::OP_STORE_LOCAL:   return "STORE_LOCAL";
        case IR::OP_LOAD_SHARED:   return "LOAD_SHARED";
        case IR::OP_STORE_SHARED:  return "STORE_SHARED";
        case IR::OP_LOAD_INPUT:    return "LOAD_INPUT";
        case IR::OP_ARRAY_LOAD:    return "ARRAY_LOAD";
        case IR::OP_ARRAY_STORE:   return "ARRAY_STORE";
        case IR::OP_STORAGE_PTR:   return "STORAGE_PTR";
        case IR::OP_STORAGE_FIELD: return "STORAGE_FIELD";
        case IR::OP_STORAGE_INDEX: return "STORAGE_INDEX";
        case IR::OP_STORAGE_LOAD:  return "STORAGE_LOAD";

        case IR::OP_FADD:          return "FADD";
        case IR::OP_FSUB:          return "FSUB";
        case IR::OP_FMUL:          return "FMUL";
        case IR::OP_FDIV:          return "FDIV";
        case IR::OP_FMOD:          return "FMOD";
        case IR::OP_FREM:          return "FREM";
        case IR::OP_LDEXP:         return "LDEXP";
        case IR::OP_FNEG:          return "FNEG";
        case IR::OP_FABS:          return "FABS";
        case IR::OP_FMIN:          return "FMIN";
        case IR::OP_FMAX:          return "FMAX";
        case IR::OP_FCLAMP:        return "FCLAMP";
        case IR::OP_FLOOR:         return "FLOOR";
        case IR::OP_CEIL:          return "CEIL";
        case IR::OP_ROUND:         return "ROUND";
        case IR::OP_TRUNC:         return "TRUNC";
        case IR::OP_FRACT:         return "FRACT";
        case IR::OP_FMA:           return "FMA";

        case IR::OP_IADD:          return "IADD";
        case IR::OP_ISUB:          return "ISUB";
        case IR::OP_IMUL:          return "IMUL";
        case IR::OP_IDIV:          return "IDIV";
        case IR::OP_IMOD:          return "IMOD";
        case IR::OP_INEG:          return "INEG";
        case IR::OP_IABS:          return "IABS";
        case IR::OP_IMIN:          return "IMIN";
        case IR::OP_IMAX:          return "IMAX";
        case IR::OP_ICLAMP:        return "ICLAMP";
        case IR::OP_UMIN:          return "UMIN";
        case IR::OP_UMAX:          return "UMAX";
        case IR::OP_UCLAMP:        return "UCLAMP";

        case IR::OP_AND:           return "AND";
        case IR::OP_OR:            return "OR";
        case IR::OP_XOR:           return "XOR";
        case IR::OP_NOT:           return "NOT";
        case IR::OP_SHL:           return "SHL";
        case IR::OP_SHR:           return "SHR";
        case IR::OP_ASR:           return "ASR";
        case IR::OP_POPCNT:        return "POPCNT";
        case IR::OP_CLZ:           return "CLZ";
        case IR::OP_CTZ:           return "CTZ";
        case IR::OP_REVERSE_BITS:  return "REVERSE_BITS";
        case IR::OP_BITFIELD_EXTRACT: return "BITFIELD_EXTRACT";
        case IR::OP_BITFIELD_INSERT:  return "BITFIELD_INSERT";
        case IR::OP_PACK_UNORM2X16:   return "PACK_UNORM2X16";
        case IR::OP_UNPACK_UNORM2X16: return "UNPACK_UNORM2X16";
        case IR::OP_PACK_UNORM4X8:    return "PACK_UNORM4X8";
        case IR::OP_UNPACK_UNORM4X8:  return "UNPACK_UNORM4X8";
        case IR::OP_PACK_SNORM2X16:   return "PACK_SNORM2X16";
        case IR::OP_UNPACK_SNORM2X16: return "UNPACK_SNORM2X16";
        case IR::OP_PACK_SNORM4X8:    return "PACK_SNORM4X8";

        case IR::OP_FEQ:           return "FEQ";
        case IR::OP_FNE:           return "FNE";
        case IR::OP_FLT:           return "FLT";
        case IR::OP_FLE:           return "FLE";
        case IR::OP_FGT:           return "FGT";
        case IR::OP_FGE:           return "FGE";
        case IR::OP_IEQ:           return "IEQ";
        case IR::OP_INE:           return "INE";
        case IR::OP_ILT:           return "ILT";
        case IR::OP_ILE:           return "ILE";
        case IR::OP_IGT:           return "IGT";
        case IR::OP_IGE:           return "IGE";
        case IR::OP_ULT:           return "ULT";
        case IR::OP_ULE:           return "ULE";
        case IR::OP_UGT:           return "UGT";
        case IR::OP_UGE:           return "UGE";

        case IR::OP_F2I:           return "F2I";
        case IR::OP_I2F:           return "I2F";
        case IR::OP_F2U:           return "F2U";
        case IR::OP_U2F:           return "U2F";
        case IR::OP_I2U:           return "I2U";
        case IR::OP_U2I:           return "U2I";
        case IR::OP_F2F16:         return "F2F16";
        case IR::OP_F162F:         return "F162F";
        case IR::OP_BITCAST:       return "BITCAST";
        case IR::OP_SIGN:          return "SIGN";

        case IR::OP_VEC_EXTRACT:   return "VEC_EXTRACT";
        case IR::OP_VEC_INSERT:    return "VEC_INSERT";
        case IR::OP_VEC_SHUFFLE:   return "VEC_SHUFFLE";
        case IR::OP_VEC_CONSTRUCT: return "VEC_CONSTRUCT";

        case IR::OP_STRUCT_CONSTRUCT: return "STRUCT_CONSTRUCT";
        case IR::OP_STRUCT_EXTRACT:   return "STRUCT_EXTRACT";
        case IR::OP_STRUCT_INSERT:    return "STRUCT_INSERT";
        case IR::OP_STRUCT_LOAD:      return "STRUCT_LOAD";
        case IR::OP_STRUCT_STORE:     return "STRUCT_STORE";
        case IR::OP_STRUCT_GEP:       return "STRUCT_GEP";

        case IR::OP_SQRT:          return "SQRT";
        case IR::OP_RSQRT:         return "RSQRT";
        case IR::OP_POW:           return "POW";
        case IR::OP_EXP:           return "EXP";
        case IR::OP_EXP2:          return "EXP2";
        case IR::OP_LOG:           return "LOG";
        case IR::OP_LOG2:          return "LOG2";
        case IR::OP_SIN:           return "SIN";
        case IR::OP_COS:           return "COS";
        case IR::OP_TAN:           return "TAN";
        case IR::OP_ASIN:          return "ASIN";
        case IR::OP_ACOS:          return "ACOS";
        case IR::OP_ATAN:          return "ATAN";
        case IR::OP_ATAN2:         return "ATAN2";
        case IR::OP_SINH:          return "SINH";
        case IR::OP_COSH:          return "COSH";
        case IR::OP_TANH:          return "TANH";
        case IR::OP_UNPACK_SNORM4X8: return "UNPACK_SNORM4X8";
        case IR::OP_PACK_HALF2X16: return "PACK_HALF2X16";
        case IR::OP_UNPACK_HALF2X16: return "UNPACK_HALF2X16";
        case IR::OP_MODF_STRUCT: return "MODF_STRUCT";
        case IR::OP_FREXP_STRUCT: return "FREXP_STRUCT";
        case IR::OP_ISNORMAL:      return "ISNORMAL";
        case IR::OP_ISNAN:         return "ISNAN";
        case IR::OP_ISINF:         return "ISINF";
        case IR::OP_ISFINITE:      return "ISFINITE";

        case IR::OP_DOT:           return "DOT";
        case IR::OP_CROSS:         return "CROSS";
        case IR::OP_LENGTH:        return "LENGTH";
        case IR::OP_NORMALIZE:     return "NORMALIZE";
        case IR::OP_DISTANCE:      return "DISTANCE";
        case IR::OP_REFLECT:       return "REFLECT";
        case IR::OP_REFRACT:       return "REFRACT";
        case IR::OP_FACEFORWARD:   return "FACEFORWARD";

        case IR::OP_MAT_MUL:       return "MAT_MUL";
        case IR::OP_MAT_TRANSPOSE: return "MAT_TRANSPOSE";
        case IR::OP_MAT_INVERSE:   return "MAT_INVERSE";
        case IR::OP_MAT_DET:       return "MAT_DET";
        case IR::OP_MAT_CONSTRUCT: return "MAT_CONSTRUCT";
        case IR::OP_MAT_IDENTITY:  return "MAT_IDENTITY";
        case IR::OP_MAT_ZERO:      return "MAT_ZERO";

        case IR::OP_TEX_SAMPLE:      return "TEX_SAMPLE";
        case IR::OP_TEX_SAMPLE_LOD:  return "TEX_SAMPLE_LOD";
        case IR::OP_TEX_SAMPLE_BIAS: return "TEX_SAMPLE_BIAS";
        case IR::OP_TEX_SAMPLE_GRAD: return "TEX_SAMPLE_GRAD";
        case IR::OP_TEX_SAMPLE_CMP:  return "TEX_SAMPLE_CMP";
        case IR::OP_TEX_SAMPLE_OFFSET:      return "TEX_SAMPLE_OFFSET";
        case IR::OP_TEX_SAMPLE_LOD_OFFSET:  return "TEX_SAMPLE_LOD_OFFSET";
        case IR::OP_TEX_SAMPLE_BIAS_OFFSET: return "TEX_SAMPLE_BIAS_OFFSET";
        case IR::OP_TEX_GATHER:      return "TEX_GATHER";
        case IR::OP_TEX_GATHER_OFFSET: return "TEX_GATHER_OFFSET";
        case IR::OP_TEX_FETCH:       return "TEX_FETCH";
        case IR::OP_TEX_FETCH_OFFSET: return "TEX_FETCH_OFFSET";
        case IR::OP_TEX_SIZE:        return "TEX_SIZE";
        case IR::OP_TEX_LEVELS:      return "TEX_LEVELS";
        case IR::OP_IMG_LOAD:        return "IMG_LOAD";
        case IR::OP_IMG_STORE:       return "IMG_STORE";
        case IR::OP_LOAD_TEX_HANDLE: return "LOAD_TEX_HANDLE";

        case IR::OP_DDX:           return "DDX";
        case IR::OP_DDY:           return "DDY";
        case IR::OP_DDX_FINE:      return "DDX_FINE";
        case IR::OP_DDY_FINE:      return "DDY_FINE";
        case IR::OP_DDX_COARSE:    return "DDX_COARSE";
        case IR::OP_DDY_COARSE:    return "DDY_COARSE";
        case IR::OP_FWIDTH:        return "FWIDTH";
        case IR::OP_FWIDTH_FINE:   return "FWIDTH_FINE";
        case IR::OP_FWIDTH_COARSE: return "FWIDTH_COARSE";

        case IR::OP_LERP:          return "LERP";
        case IR::OP_SMOOTHSTEP:    return "SMOOTHSTEP";
        case IR::OP_STEP:          return "STEP";
        case IR::OP_SATURATE:      return "SATURATE";
        case IR::OP_DEGREES:       return "DEGREES";
        case IR::OP_RADIANS:       return "RADIANS";

        case IR::OP_ATOMIC_ADD:      return "ATOMIC_ADD";
        case IR::OP_ATOMIC_SUB:      return "ATOMIC_SUB";
        case IR::OP_ATOMIC_MIN:      return "ATOMIC_MIN";
        case IR::OP_ATOMIC_MAX:      return "ATOMIC_MAX";
        case IR::OP_ATOMIC_AND:      return "ATOMIC_AND";
        case IR::OP_ATOMIC_OR:       return "ATOMIC_OR";
        case IR::OP_ATOMIC_XOR:      return "ATOMIC_XOR";
        case IR::OP_ATOMIC_XCHG:     return "ATOMIC_XCHG";
        case IR::OP_ATOMIC_CMP_XCHG: return "ATOMIC_CMP_XCHG";
        case IR::OP_ANY:             return "ANY";
        case IR::OP_ALL:             return "ALL";

        case IR::OP_LOCAL_VAR_PTR:   return "LOCAL_VAR_PTR";
        case IR::OP_LOCAL_LOAD:      return "LOCAL_LOAD";
        case IR::OP_LOCAL_STORE:     return "LOCAL_STORE";
        case IR::OP_LOCAL_FIELD_PTR: return "LOCAL_FIELD_PTR";

        case IR::OP_BARRIER:         return "BARRIER";
        case IR::OP_MEM_FENCE:       return "MEM_FENCE";
        default:                     return "UNKNOWN";
    }
}

inline const char* CoreTypeToString(CoreType type) {
    switch (type) {
        case CoreType::INVALID: return "INVALID";
        case CoreType::BOOL:    return "BOOL";
        case CoreType::INT:     return "INT";
        case CoreType::UINT:    return "UINT";
        case CoreType::FLOAT:   return "FLOAT";
        case CoreType::INT2:    return "INT2";
        case CoreType::INT3:    return "INT3";
        case CoreType::INT4:    return "INT4";
        case CoreType::UINT2:   return "UINT2";
        case CoreType::UINT3:   return "UINT3";
        case CoreType::UINT4:   return "UINT4";
        case CoreType::FLOAT2:  return "FLOAT2";
        case CoreType::FLOAT3:  return "FLOAT3";
        case CoreType::FLOAT4:  return "FLOAT4";
        case CoreType::MAT2:    return "MAT2";
        case CoreType::MAT3:    return "MAT3";
        case CoreType::MAT4:    return "MAT4";
        case CoreType::VOID:    return "VOID";
        case CoreType::STRING:  return "STRING";
        case CoreType::CUSTOM:  return "CUSTOM";
        default:                return "?";
    }
}

inline std::string FormatOperand(const IR::IRProgram& prog, u16 op, bool allowZeroReg = true) {
    if (op & 0x8000) {
        u16 idx = op & 0x7FFF;
        if (idx < prog.floatCount) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.4g", prog.floatConstants[idx]);
            return buf;
        }
        return "f?";
    }
    if (op & 0x4000) {
        u16 idx = op & 0x3FFF;
        if (idx < prog.intCount) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%d", static_cast<int>(prog.intConstants[idx]));
            return buf;
        }
        return "i?";
    }
    if (op == 0 && !allowZeroReg) return "_";
    char buf[16];
    snprintf(buf, sizeof(buf), "r%u", op);
    return buf;
}

inline std::string DumpIRToString(const IR::IRProgram& prog) {
    std::string out;
    char buf[256];

    snprintf(buf, sizeof(buf), "Instructions: %u, Registers: %u\n", prog.instructionCount, prog.registerCount);
    out += buf;
    snprintf(buf, sizeof(buf), "Float constants: %u, Int constants: %u\n\n", prog.floatCount, prog.intCount);
    out += buf;

    if (prog.floatCount > 0) {
        out += "Float Constants:\n";
        for (u32 i = 0; i < prog.floatCount; i++) {
            snprintf(buf, sizeof(buf), "  [%u] = %.6g\n", i, prog.floatConstants[i]);
            out += buf;
        }
        out += "\n";
    }

    if (prog.intCount > 0) {
        out += "Int Constants:\n";
        for (u32 i = 0; i < prog.intCount; i++) {
            snprintf(buf, sizeof(buf), "  [%u] = %d\n", i, static_cast<int>(prog.intConstants[i]));
            out += buf;
        }
        out += "\n";
    }

    out += "Instructions:\n";
    for (u32 i = 0; i < prog.instructionCount; i++) {
        IR::OpCode op = static_cast<IR::OpCode>(prog.opcodes[i]);
        u16 dest = prog.destinations[i];
        CoreType type = prog.types ? static_cast<CoreType>(prog.types[i]) : CoreType::INVALID;

        snprintf(buf, sizeof(buf), "  [%3u] %-16s", i, OpCodeToString(op));
        out += buf;

        if (dest != 0 || op == IR::OP_STORE_REG || op == IR::OP_STORE_OUTPUT) {
            if (type != CoreType::INVALID) {
                snprintf(buf, sizeof(buf), " r%-3u:%-7s <-", dest, CoreTypeToString(type));
            } else {
                snprintf(buf, sizeof(buf), " r%-3u         <-", dest);
            }
            out += buf;
        } else {
            out += "                ";
        }

        u16 op0 = prog.GetOperand(i, 0);
        u16 op1 = prog.GetOperand(i, 1);
        u16 op2 = prog.GetOperand(i, 2);
        u16 op3 = prog.GetOperand(i, 3);

        switch (op) {
            case IR::OP_NOP:
            case IR::OP_RET:
                break;
            case IR::OP_JUMP:
                snprintf(buf, sizeof(buf), " -> %u", prog.metadata[i]);
                out += buf;
                break;
            case IR::OP_BRANCH:
                snprintf(buf, sizeof(buf), " %s ? -> %u : %u",
                    FormatOperand(prog, op0).c_str(),
                    prog.GetBranchTrueTarget(i), prog.GetBranchFalseTarget(i));
                out += buf;
                break;
            case IR::OP_LOAD_CONST:
                out += " " + FormatOperand(prog, op0);
                break;
            case IR::OP_LOAD_ATTR:
                snprintf(buf, sizeof(buf), " attr[%u]", op0);
                out += buf;
                break;
            case IR::OP_LOAD_OUTPUT:
                snprintf(buf, sizeof(buf), " output[%u]", op0);
                out += buf;
                break;
            case IR::OP_STORE_OUTPUT:
                snprintf(buf, sizeof(buf), " output[%u] = %s", op0, FormatOperand(prog, op1).c_str());
                out += buf;
                break;
            case IR::OP_STORE_REG:
                out += " " + FormatOperand(prog, op0);
                break;
            case IR::OP_VEC_CONSTRUCT:
                snprintf(buf, sizeof(buf), " (%s, %s, %s, %s)",
                    FormatOperand(prog, op0).c_str(),
                    FormatOperand(prog, op1).c_str(),
                    FormatOperand(prog, op2).c_str(),
                    FormatOperand(prog, op3).c_str());
                out += buf;
                break;
            case IR::OP_FADD: case IR::OP_FSUB: case IR::OP_FMUL: case IR::OP_FDIV: case IR::OP_FREM:
            case IR::OP_IADD: case IR::OP_ISUB: case IR::OP_IMUL: case IR::OP_IDIV:
            case IR::OP_AND: case IR::OP_OR: case IR::OP_XOR:
            case IR::OP_FLT: case IR::OP_FLE: case IR::OP_FGT: case IR::OP_FGE: case IR::OP_FEQ: case IR::OP_FNE:
            case IR::OP_ILT: case IR::OP_ILE: case IR::OP_IGT: case IR::OP_IGE: case IR::OP_IEQ: case IR::OP_INE:
                snprintf(buf, sizeof(buf), " %s, %s", FormatOperand(prog, op0).c_str(), FormatOperand(prog, op1).c_str());
                out += buf;
                break;
            default:
                out += " " + FormatOperand(prog, op0);
                if (op1 || op2 || op3) out += ", " + FormatOperand(prog, op1);
                if (op2 || op3) out += ", " + FormatOperand(prog, op2);
                if (op3) out += ", " + FormatOperand(prog, op3);
                break;
        }

        out += "\n";
    }

    if (prog.phiCount > 0) {
        out += "\nPHI Nodes:\n";
        for (u32 i = 0; i < prog.phiCount; i++) {
            snprintf(buf, sizeof(buf), "  PHI r%u (block %u): ", prog.phiResultRegs[i], prog.phiBlockIndices[i]);
            out += buf;
            u32 start = prog.phiOperandOffsets[i];
            u32 end = prog.phiOperandOffsets[i + 1];
            for (u32 j = start; j < end; j++) {
                if (j > start) out += ", ";
                snprintf(buf, sizeof(buf), "[b%u: %s]", prog.phiOperandBlocks[j],
                         FormatOperand(prog, prog.phiOperandValues[j]).c_str());
                out += buf;
            }
            out += "\n";
        }
    }

    return out;
}

inline std::string ResolveArenaString(const ArenaString& value,
                                      const char* sourceBase,
                                      const std::string& fallback = "") {
    if (!value.isHashOnly() && sourceBase) {
        return std::string(value.view(sourceBase));
    }

    std::string reversed = ReverseLookup::GetString(value.nameHash);
    if (!reversed.empty()) {
        return reversed;
    }

    std::string resolved = value.ToString(sourceBase);
    if (resolved.find("<hash:") == std::string::npos) {
        return resolved;
    }

    return fallback;
}

inline RenderConfig CreateSyntheticRenderConfig(const AST& ast,
                                                const PipelineData& pipeline,
                                                const SymbolTableData& symbols,
                                                const char* sourceBase,
                                                const std::string& pipelineFallbackName) {
    RenderConfig config;
    config.name = ResolveArenaString(pipeline.name, sourceBase, pipelineFallbackName);

    auto appendResource = [&](const std::string& resourceName,
                              const std::string& typeName,
                              const ResourceData& resource) {
        switch (resource.type) {
            case ResourceBinding::UniformBuffer: {
                UniformBufferBinding binding;
                binding.name = resourceName;
                binding.typeName = typeName;
                binding.bindingIndex = resource.bindingIndex;
                binding.stages = resource.stageFlags;
                config.uniformBuffers.push_back(std::move(binding));
                break;
            }
            case ResourceBinding::Texture: {
                TextureBinding binding;
                binding.name = resourceName;
                binding.bindingIndex = resource.bindingIndex;
                binding.isArray = resource.isArrayTexture;
                binding.isCubemap = resource.isCubemapTexture;
                binding.stages = resource.stageFlags;
                config.textures.push_back(std::move(binding));
                break;
            }
            case ResourceBinding::Sampler: {
                SamplerBinding binding;
                binding.name = resourceName;
                binding.bindingIndex = resource.bindingIndex;
                binding.stages = resource.stageFlags;
                config.samplers.push_back(std::move(binding));
                break;
            }
            case ResourceBinding::StorageBuffer: {
                StorageBufferBinding binding;
                binding.name = resourceName;
                binding.typeName = typeName;
                binding.bindingIndex = resource.bindingIndex;
                binding.readOnly = true;
                binding.stages = resource.stageFlags;
                config.storageBuffers.push_back(std::move(binding));
                break;
            }
            case ResourceBinding::StorageImage: {
                StorageImageBinding binding;
                binding.name = resourceName;
                binding.bindingIndex = resource.bindingIndex;
                binding.accessMode = ResourceAccessMode::ReadWrite;
                binding.stages = resource.stageFlags;
                config.storageImages.push_back(std::move(binding));
                break;
            }
            default:
                break;
        }
    };

    if (pipeline.resources.count > 0) {
        for (u32 i = 0; i < pipeline.resources.count; i++) {
            const ResourceDeclData& decl = ast.GetResourceDecl(pipeline.resources[i]);
            Symbol* sym = SymbolTable::LookupResource(const_cast<SymbolTableData*>(&symbols), decl.name);
            if (!sym || sym->index >= symbols.resources.count) {
                continue;
            }

            const ResourceData& resource = symbols.resources[sym->index];
            std::string resourceName = ResolveArenaString(
                decl.name,
                sourceBase,
                GetResourceNameByIndex(symbols, sym->index, sourceBase)
            );
            if (resourceName.empty()) {
                continue;
            }

            appendResource(resourceName,
                           ResolveArenaString(decl.typeName, sourceBase),
                           resource);
        }
    } else {
        for (u32 resourceIndex = 0; resourceIndex < symbols.resources.count; resourceIndex++) {
            const ResourceData& resource = symbols.resources[resourceIndex];
            std::string resourceName = GetResourceNameByIndex(symbols, resourceIndex, sourceBase);
            if (resourceName.empty()) {
                continue;
            }

            appendResource(resourceName,
                           ResolveArenaString(resource.typeName, sourceBase),
                           resource);
        }
    }

    for (u32 i = 0; i < pipeline.passes.count; i++) {
        const PassData& pass = ast.GetPass(pipeline.passes[i]);
        RenderConfig::PassData passConfig;
        passConfig.name = ResolveArenaString(pass.name, sourceBase, "pass" + std::to_string(i));
        passConfig.type = pass.computeShader.IsNull() ? PassType::Standard : PassType::Compute;
        passConfig.descriptor.name = passConfig.name;
        passConfig.descriptor.pipelineName = config.name;
        config.passes.push_back(std::move(passConfig));
    }

    return config;
}

} // namespace BWSL::ToolCommon
