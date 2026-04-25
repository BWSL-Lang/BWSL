// BWSL WebAssembly Compiler Entry Point
// Made by Alexander Presthus
// https://github.com/apresthus/bwsl
// Compiles BWSL source to GLSL ES for browser-based shader playground
//
// Build with Emscripten:
//   emcc tools/bwsl_wasm.cpp tools/spirv_cross_wrapper.cpp \
//     -DUSE_SPIRV_CROSS_LIB -DBWSL_WASM \
//     -I. -I.. \
//     -s WASM=1 -s MODULARIZE=1 -s EXPORT_ES6=1 \
//     -s EXPORTED_FUNCTIONS='["_compile", "_getSymbols", "_getVersion", "_malloc", "_free"]' \
//     -s EXPORTED_RUNTIME_METHODS='["ccall", "cwrap", "UTF8ToString", "stringToUTF8", "lengthBytesUTF8", "FS"]' \
//     -s ALLOW_MEMORY_GROWTH=1 \
//     -O3 \
//     -o bwsl.js
//
// Usage from JavaScript:
//   const bwsl = await import('./bwsl.js');
//   const module = await bwsl.default();
//   const compile = module.cwrap('compile', 'string', ['string', 'string', 'string']);
//   const result = compile(bwslSource, "", flags);  // flags: "-internals", "-modules /path"
//   const json = JSON.parse(result);
//   const getSymbols = module.cwrap('getSymbols', 'string', ['string', 'string', 'string']);
//   const symbols = getSymbols(bwslSource, "", flags);  // flags: "-modules /path"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sstream>

// SPIRV-Cross wrapper (compiled separately to avoid macro conflicts)
#ifdef USE_SPIRV_CROSS_LIB
#include <unordered_map>
namespace spirv_cross_wrapper {
    std::string CompileToMSL(const std::vector<uint32_t>& spirv);
    std::string CompileToHLSL(const std::vector<uint32_t>& spirv, int shaderModel);
    std::string CompileToGLSL(const std::vector<uint32_t>& spirv, int glslVersion, bool es);
    std::string CompileToGLSLWithVaryings(
        const std::vector<uint32_t>& spirv,
        int glslVersion,
        bool es,
        const std::unordered_map<uint32_t, std::string>& varyingNames,
        bool isVertex);
}
#endif

// ============= Direct GLES Toggle =============
// Set to true to use direct IR->GLES backend (faster, bypasses SPIRV-Cross)
// Set to false to use SPIR-V + SPIRV-Cross path (more mature)
static constexpr bool USE_DIRECT_GLES = false;

// BWSL compiler includes (all standalone)
#include "../bwsl_spirv_backend.h"
#include "../bwsl_ir_gen.h"
#include "../bwsl_ir_lowering.h"
#include "../bwsl_ir_analysis.h"
#include "../bwsl_cfg.h"
#include "../bwsl_ssa.h"
#include "../bwsl_parser_soa.h"
#include "../bwsl_resource_reflection.h"
#include "../bwsl_lexer.h"
#include "../bwsl_eval_soa.h"
#include "../bwsl_variant_system.h"
#include "../bwsl_arena.h"
#include "../bwsl_mem_pool.h"
#include "../bwsl_render_config.h"
#include "../bwsl_compute_graph.h"
#include "../bwsl_gles_backend.h"

// Unity build: include all implementation files
#include "../bwsl_lexer.cpp"
#include "../bwsl_parser_soa.cpp"
#include "../bwsl_eval_soa.cpp"
#include "../bwsl_module_cache.cpp"
#include "../bwsl_ir_gen.cpp"
#include "../bwsl_ir_analysis.cpp"
#include "../bwsl_cfg.cpp"
#include "../bwsl_ssa.cpp"
#include "../bwsl_spirv_backend.cpp"
#include "../bwsl_compute_graph.cpp"
#include "../bwsl_variant_system.cpp"
#include "../bwsl_custom_type_registry.cpp"
#include "../bwsl_gles_backend.cpp"

using namespace BWSL;
using namespace BWSL::IR;

// ============= Helper Functions =============

static const char* PixelFormatToString(PixelFormat format) {
    switch (format) {
        case PixelFormat::RGBA8Unorm: return "RGBA8Unorm";
        case PixelFormat::RGBA16Float: return "RGBA16Float";
        case PixelFormat::RGBA32Float: return "RGBA32Float";
        case PixelFormat::Depth32Float: return "Depth32Float";
        case PixelFormat::RG16Float: return "RG16Float";
        default: return "RGBA8Unorm";
    }
}

static const char* AccessModeToString(BWSL::ResourceAccessMode mode) {
    switch (mode) {
        case BWSL::ResourceAccessMode::ReadOnly: return "readonly";
        case BWSL::ResourceAccessMode::ReadWrite: return "readwrite";
        case BWSL::ResourceAccessMode::WriteOnly: return "writeonly";
        default: return "readonly";
    }
}

static const char* BarrierTypeToString(BarrierType type) {
    switch (type) {
        case BarrierType::BufferWriteToRead: return "BufferWriteToRead";
        case BarrierType::BufferWriteToVertex: return "BufferWriteToVertex";
        case BarrierType::ImageWriteToSample: return "ImageWriteToSample";
        case BarrierType::ImageWriteToStorage: return "ImageWriteToStorage";
        default: return "None";
    }
}

static std::string EscapeJsonString(const std::string& str) {
    std::string result;
    result.reserve(str.length());
    for (char c : str) {
        switch (c) {
            case '"':  result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:   result += c; break;
        }
    }
    return result;
}

// ============= IR Dump Functions =============

static const char* OpCodeToString(IR::OpCode op) {
    switch (op) {
        case IR::OP_NOP: return "NOP";
        case IR::OP_JUMP: return "JUMP";
        case IR::OP_BRANCH: return "BRANCH";
        case IR::OP_CALL: return "CALL";
        case IR::OP_RET: return "RET";
        case IR::OP_SELECT: return "SELECT";
        case IR::OP_PHI: return "PHI";
        case IR::OP_LOAD_CONST: return "LOAD_CONST";
        case IR::OP_LOAD_REG: return "LOAD_REG";
        case IR::OP_STORE_REG: return "STORE_REG";
        case IR::OP_LOAD_ATTR: return "LOAD_ATTR";
        case IR::OP_STORE_OUTPUT: return "STORE_OUTPUT";
        case IR::OP_LOAD_OUTPUT: return "LOAD_OUTPUT";
        case IR::OP_LOAD_UNIFORM: return "LOAD_UNIFORM";
        case IR::OP_LOAD_INPUT: return "LOAD_INPUT";
        case IR::OP_FADD: return "FADD";
        case IR::OP_FSUB: return "FSUB";
        case IR::OP_FMUL: return "FMUL";
        case IR::OP_FDIV: return "FDIV";
        case IR::OP_FREM: return "FREM";
        case IR::OP_LDEXP: return "LDEXP";
        case IR::OP_FNEG: return "FNEG";
        case IR::OP_IADD: return "IADD";
        case IR::OP_ISUB: return "ISUB";
        case IR::OP_IMUL: return "IMUL";
        case IR::OP_IDIV: return "IDIV";
        case IR::OP_DOT: return "DOT";
        case IR::OP_CROSS: return "CROSS";
        case IR::OP_LENGTH: return "LENGTH";
        case IR::OP_NORMALIZE: return "NORMALIZE";
        case IR::OP_SQRT: return "SQRT";
        case IR::OP_RSQRT: return "RSQRT";
        case IR::OP_FWIDTH_FINE: return "FWIDTH_FINE";
        case IR::OP_FWIDTH_COARSE: return "FWIDTH_COARSE";
        case IR::OP_SIN: return "SIN";
        case IR::OP_COS: return "COS";
        case IR::OP_TAN: return "TAN";
        case IR::OP_POW: return "POW";
        case IR::OP_TANH: return "TANH";
        case IR::OP_UNPACK_SNORM4X8: return "UNPACK_SNORM4X8";
        case IR::OP_PACK_HALF2X16: return "PACK_HALF2X16";
        case IR::OP_UNPACK_HALF2X16: return "UNPACK_HALF2X16";
        case IR::OP_MODF_STRUCT: return "MODF_STRUCT";
        case IR::OP_FREXP_STRUCT: return "FREXP_STRUCT";
        case IR::OP_ISNORMAL: return "ISNORMAL";
        case IR::OP_EXP: return "EXP";
        case IR::OP_LOG: return "LOG";
        case IR::OP_FLOOR: return "FLOOR";
        case IR::OP_CEIL: return "CEIL";
        case IR::OP_FRACT: return "FRACT";
        case IR::OP_LERP: return "LERP";
        case IR::OP_SMOOTHSTEP: return "SMOOTHSTEP";
        case IR::OP_STEP: return "STEP";
        case IR::OP_SATURATE: return "SATURATE";
        case IR::OP_FMIN: return "FMIN";
        case IR::OP_FMAX: return "FMAX";
        case IR::OP_FCLAMP: return "FCLAMP";
        case IR::OP_FABS: return "FABS";
        case IR::OP_BITFIELD_EXTRACT: return "BITFIELD_EXTRACT";
        case IR::OP_BITFIELD_INSERT: return "BITFIELD_INSERT";
        case IR::OP_PACK_UNORM2X16: return "PACK_UNORM2X16";
        case IR::OP_UNPACK_UNORM2X16: return "UNPACK_UNORM2X16";
        case IR::OP_PACK_UNORM4X8: return "PACK_UNORM4X8";
        case IR::OP_UNPACK_UNORM4X8: return "UNPACK_UNORM4X8";
        case IR::OP_PACK_SNORM2X16: return "PACK_SNORM2X16";
        case IR::OP_UNPACK_SNORM2X16: return "UNPACK_SNORM2X16";
        case IR::OP_PACK_SNORM4X8: return "PACK_SNORM4X8";
        case IR::OP_ISNAN: return "ISNAN";
        case IR::OP_ISINF: return "ISINF";
        case IR::OP_ISFINITE: return "ISFINITE";
        case IR::OP_TEX_LEVELS: return "TEX_LEVELS";
        case IR::OP_TEX_SAMPLE_OFFSET: return "TEX_SAMPLE_OFFSET";
        case IR::OP_TEX_SAMPLE_LOD_OFFSET: return "TEX_SAMPLE_LOD_OFFSET";
        case IR::OP_TEX_SAMPLE_BIAS_OFFSET: return "TEX_SAMPLE_BIAS_OFFSET";
        case IR::OP_TEX_GATHER_OFFSET: return "TEX_GATHER_OFFSET";
        case IR::OP_TEX_FETCH_OFFSET: return "TEX_FETCH_OFFSET";
        case IR::OP_VEC_CONSTRUCT: return "VEC_CONSTRUCT";
        case IR::OP_VEC_EXTRACT: return "VEC_EXTRACT";
        case IR::OP_MAT_MUL: return "MAT_MUL";
        case IR::OP_TEX_SAMPLE: return "TEX_SAMPLE";
        case IR::OP_FLT: return "FLT";
        case IR::OP_FLE: return "FLE";
        case IR::OP_FGT: return "FGT";
        case IR::OP_FGE: return "FGE";
        case IR::OP_FEQ: return "FEQ";
        case IR::OP_FNE: return "FNE";
        default: return "OP_UNKNOWN";
    }
}

static const char* CoreTypeToString(CoreType type) {
    switch (type) {
        case CoreType::INVALID: return "INVALID";
        case CoreType::BOOL: return "BOOL";
        case CoreType::INT: return "INT";
        case CoreType::UINT: return "UINT";
        case CoreType::FLOAT: return "FLOAT";
        case CoreType::INT2: return "INT2";
        case CoreType::INT3: return "INT3";
        case CoreType::INT4: return "INT4";
        case CoreType::UINT2: return "UINT2";
        case CoreType::UINT3: return "UINT3";
        case CoreType::UINT4: return "UINT4";
        case CoreType::FLOAT2: return "FLOAT2";
        case CoreType::FLOAT3: return "FLOAT3";
        case CoreType::FLOAT4: return "FLOAT4";
        case CoreType::MAT2: return "MAT2";
        case CoreType::MAT3: return "MAT3";
        case CoreType::MAT4: return "MAT4";
        case CoreType::VOID: return "VOID";
        default: return "?";
    }
}

static std::string FormatOperand(const IRProgram& prog, u16 op) {
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
            snprintf(buf, sizeof(buf), "%d", (int)prog.intConstants[idx]);
            return buf;
        }
        return "i?";
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "r%u", op);
    return buf;
}

static std::string DumpIRToString(const IRProgram& prog) {
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
            snprintf(buf, sizeof(buf), "  [%u] = %d\n", i, (int)prog.intConstants[i]);
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
                snprintf(buf, sizeof(buf), " -> %u", op0);
                out += buf;
                break;
            case IR::OP_BRANCH:
                snprintf(buf, sizeof(buf), " %s ? -> %u : %u", FormatOperand(prog, op0).c_str(), op1, op2);
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
            case IR::OP_VEC_CONSTRUCT:
                snprintf(buf, sizeof(buf), " (%s, %s, %s, %s)",
                    FormatOperand(prog, op0).c_str(),
                    FormatOperand(prog, op1).c_str(),
                    FormatOperand(prog, op2).c_str(),
                    FormatOperand(prog, op3).c_str());
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

    return out;
}

static bool CanUseDirectGLESFallback(const IRProgram& program, ShaderStage stage) {
    for (u32 i = 0; i < program.instructionCount; i++) {
        IR::OpCode op = static_cast<IR::OpCode>(program.opcodes[i]);
        switch (op) {
            case IR::OP_LOAD_BUFFER:
            case IR::OP_STORE_BUFFER:
            case IR::OP_LOAD_SHARED:
            case IR::OP_STORE_SHARED:
            case IR::OP_BARRIER:
            case IR::OP_MEM_FENCE:
                return false;
            case IR::OP_ATOMIC_ADD:
            case IR::OP_ATOMIC_SUB:
            case IR::OP_ATOMIC_MIN:
            case IR::OP_ATOMIC_MAX:
            case IR::OP_ATOMIC_AND:
            case IR::OP_ATOMIC_OR:
            case IR::OP_ATOMIC_XOR:
            case IR::OP_ATOMIC_XCHG:
            case IR::OP_ATOMIC_CMP_XCHG:
                return false;
            default:
                break;
        }
    }
    (void)stage;
    return true;
}

static std::string EmitDirectGLES(IRProgram& program,
                                  CFG* cfgPtr,
                                  ShaderStage stage,
                                  const PassData& pass,
                                  const RenderConfig& renderConfig,
                                  IRAnalysis& analysis,
                                  IR::PassVaryingContext* varyingContext,
                                  const ShaderStageData* shaderStageData,
                                  const char* sourceBase) {
    if (!CanUseDirectGLESFallback(program, stage)) {
        return {};
    }

    Memory::BWEMemoryArena glesArena;
    char glesMem[128 * 1024];
    glesArena.Initialize(glesMem, sizeof(glesMem));

    GLES::GLESBuilder glesBuilder;
    glesBuilder.Initialize(&glesArena, sourceBase, &program, cfgPtr, stage,
                           &pass, &renderConfig, &analysis, varyingContext);
    if (stage == ShaderStage::Compute && shaderStageData) {
        glesBuilder.SetComputeWorkgroupSize(shaderStageData->workgroupSizeX,
                                            shaderStageData->workgroupSizeY,
                                            shaderStageData->workgroupSizeZ);
    }
    std::string_view glesOutput = glesBuilder.Emit();
    return std::string(glesOutput);
}

// Simple SPIR-V text dump (since we can't run spirv-dis in WASM)
static std::string DumpSpirvToString(const std::vector<u32>& spirv) {
    std::ostringstream out;
    out << "; SPIR-V Binary (" << spirv.size() << " words)\n";
    out << "; Magic: 0x" << std::hex << spirv[0] << std::dec << "\n";
    if (spirv.size() > 1) out << "; Version: " << ((spirv[1] >> 16) & 0xFF) << "." << ((spirv[1] >> 8) & 0xFF) << "\n";
    if (spirv.size() > 2) out << "; Generator: 0x" << std::hex << spirv[2] << std::dec << "\n";
    if (spirv.size() > 3) out << "; Bound: " << spirv[3] << "\n";
    out << "\n; Raw words (first 100):\n";
    for (size_t i = 0; i < std::min(spirv.size(), (size_t)100); i++) {
        out << "  [" << i << "] 0x" << std::hex << spirv[i] << std::dec << "\n";
    }
    if (spirv.size() > 100) {
        out << "  ... (" << (spirv.size() - 100) << " more words)\n";
    }
    return out.str();
}

static const char* ResourceTypeToString(::ResourceBinding::Type type) {
    switch (type) {
        case ::ResourceBinding::UniformBuffer: return "uniform";
        case ::ResourceBinding::StorageBuffer: return "storage_buffer";
        case ::ResourceBinding::Texture: return "texture";
        case ::ResourceBinding::Sampler: return "sampler";
        case ::ResourceBinding::StorageImage: return "storage_image";
        default: return "buffer";
    }
}

static std::string StageFlagsToJsonArray(u8 stageFlags) {
    std::string json = "[";
    bool first = true;
    auto appendStage = [&](const char* stageName, ShaderStage stage) {
        if ((stageFlags & SymbolTable::ShaderStageToBit(stage)) == 0) return;
        if (!first) json += ",";
        json += "\"";
        json += stageName;
        json += "\"";
        first = false;
    };

    appendStage("vertex", ShaderStage::Vertex);
    appendStage("fragment", ShaderStage::Fragment);
    appendStage("compute", ShaderStage::Compute);

    json += "]";
    return json;
}

static const char* ResourceAccessToString(BWSL::ResourceAccessMode access) {
    switch (access) {
        case BWSL::ResourceAccessMode::ReadOnly: return "readonly";
        case BWSL::ResourceAccessMode::WriteOnly: return "writeonly";
        case BWSL::ResourceAccessMode::ReadWrite: return "readwrite";
        default: return "readonly";
    }
}

static std::string ResolveArenaString(const ArenaString& value,
                                      const char* sourceBase,
                                      const std::string& fallback = {}) {
    if (!value.isHashOnly() && sourceBase) {
        return std::string(value.view(sourceBase));
    }

    std::string reversed = ReverseLookup::GetString(value.nameHash);
    if (!reversed.empty()) {
        return reversed;
    }

    return fallback;
}

static RenderConfig CreateSyntheticRenderConfig(const AST& ast,
                                                const PipelineData& pipeline,
                                                const SymbolTableData& symbols,
                                                const char* sourceBase) {
    RenderConfig config;
    config.name = ResolveArenaString(pipeline.name, sourceBase, "Demo");

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
                binding.accessMode = ::ResourceAccessMode::ReadWrite;
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

using NameBindingEntry = std::pair<std::string, u32>;

static void SortNameBindingEntries(std::vector<NameBindingEntry>& entries) {
    std::sort(entries.begin(), entries.end(),
              [](const NameBindingEntry& lhs, const NameBindingEntry& rhs) {
                  if (lhs.second != rhs.second) return lhs.second < rhs.second;
                  return lhs.first < rhs.first;
              });
}

static std::vector<NameBindingEntry> CollectWebGLUniformEntries(
    const std::vector<ReflectedResourceBinding>& resources,
    ShaderStage stage) {
    std::vector<NameBindingEntry> entries;
    const u8 stageMask = SymbolTable::ShaderStageToBit(stage);
    for (const ReflectedResourceBinding& resource : resources) {
        if (resource.type != ResourceBinding::UniformBuffer ||
            (resource.stages & stageMask) == 0 ||
            resource.set != 0) {
            continue;
        }
        entries.emplace_back(resource.name, resource.binding);
    }
    SortNameBindingEntries(entries);
    return entries;
}

static std::vector<NameBindingEntry> CollectWebGLSamplerEntries(
    const std::vector<ReflectedResourceBinding>& resources,
    ShaderStage stage) {
    std::vector<NameBindingEntry> entries;
    const u8 stageMask = SymbolTable::ShaderStageToBit(stage);
    for (const ReflectedResourceBinding& resource : resources) {
        if (resource.type != ResourceBinding::Texture ||
            (resource.stages & stageMask) == 0 ||
            resource.set != 0) {
            continue;
        }
        entries.emplace_back(resource.name, resource.binding);
    }
    SortNameBindingEntries(entries);
    return entries;
}

static void AppendNameBindingMap(std::ostringstream& json,
                                 const char* label,
                                 const std::vector<NameBindingEntry>& entries) {
    json << "\"" << label << "\":{";
    for (size_t i = 0; i < entries.size(); i++) {
        if (i > 0) json << ",";
        json << "\"" << EscapeJsonString(entries[i].first) << "\":" << entries[i].second;
    }
    json << "}";
}

static void AppendResourceReflectionJson(std::ostringstream& json,
                                         const std::vector<ReflectedResourceBinding>& resources) {
    json << "\"resources\":[";
    for (size_t i = 0; i < resources.size(); i++) {
        const ReflectedResourceBinding& resource = resources[i];
        if (i > 0) json << ",";
        json << "{";
        json << "\"name\":\"" << EscapeJsonString(resource.name) << "\",";
        json << "\"type\":\"" << ResourceTypeToString(resource.type) << "\",";
        json << "\"set\":" << resource.set << ",";
        json << "\"binding\":" << resource.binding << ",";
        json << "\"stages\":" << StageFlagsToJsonArray(resource.stages) << ",";
        json << "\"access\":\"" << ResourceAccessToString(resource.access) << "\"";
        if (resource.combinedSampledImage) {
            json << ",\"abi\":\"combined_sampled_image\"";
            if (!resource.combinedWith.empty()) {
                json << ",\"combinedWith\":\"" << EscapeJsonString(resource.combinedWith) << "\"";
            }
        }
        json << "}";
    }
    json << "]";
}

static void AppendWebGLStageJson(std::ostringstream& json,
                                 ShaderStage stage,
                                 const AST* ast,
                                 const PipelineData* pipeline,
                                 const PassData* pass,
                                 const char* sourceBase,
                                 const std::vector<ReflectedResourceBinding>& resources) {
    if (stage == ShaderStage::Vertex) {
        json << "\"vertex\":{";
        json << "\"attributes\":{";
        if (ast && pipeline && pass) {
            bool firstAttribute = true;
            for (u32 i = 0; i < pipeline->attributes.count; i++) {
                const AttributeDeclData& attributeDecl = ast->GetAttributeDecl(pipeline->attributes[i]);
                bool isUsedByPass = false;
                for (u32 usedIdx = 0; usedIdx < pass->usedAttributes.count; usedIdx++) {
                    if (pass->usedAttributes[usedIdx].nameHash == attributeDecl.name.nameHash) {
                        isUsedByPass = true;
                        break;
                    }
                }

                if (!isUsedByPass) continue;

                if (!firstAttribute) json << ",";
                json << "\""
                     << EscapeJsonString(ResolveArenaString(attributeDecl.name, sourceBase))
                     << "\":" << static_cast<u32>(attributeDecl.attributeIndex);
                firstAttribute = false;
            }
        }
        json << "},";
        AppendNameBindingMap(json, "uniforms", CollectWebGLUniformEntries(resources, stage));
        json << ",";
        AppendNameBindingMap(json, "samplers", CollectWebGLSamplerEntries(resources, stage));
        json << "}";
        return;
    }

    if (stage == ShaderStage::Fragment) {
        json << "\"fragment\":{";
        AppendNameBindingMap(json, "uniforms", CollectWebGLUniformEntries(resources, stage));
        json << ",";
        AppendNameBindingMap(json, "samplers", CollectWebGLSamplerEntries(resources, stage));
        json << "}";
        return;
    }

    json << "\"compute\":{";
    AppendNameBindingMap(json, "uniforms", CollectWebGLUniformEntries(resources, stage));
    json << ",";
    AppendNameBindingMap(json, "samplers", CollectWebGLSamplerEntries(resources, stage));
    json << "}";
}

// ============= Shader Compilation =============

struct ShaderOutput {
    bool success = false;
    std::string vertexGlsl;
    std::string fragmentGlsl;
    std::string computeGlsl;
    std::string error;
    IRAnalysis analysis{};
    std::vector<ExplicitSamplerUse> explicitSamplerUses;

    // Internals for -internals flag
    std::string irDump;
    std::string spirvDis;
};

static ShaderOutput CompileShaderStage(
    const CompilationContext& context,
    const Parser& parser,
    const PassData& pass,
    ShaderStage stage,
    const RenderConfig& renderConfig,
    NodeRef pipelineRef,
    PassVaryingContext* varyingContext,
    const char* sourceBase,
    bool captureInternals = false
) {
    ShaderOutput output;

    // Get shader body
    NodeRef shaderBody;
    const ShaderStageData* shaderStageData = nullptr;
    if (stage == ShaderStage::Vertex) {
        if (pass.vertexShader.IsNull()) {
            output.error = "No vertex shader in pass";
            return output;
        }
        shaderStageData = &context.ast.GetShaderStage(pass.vertexShader);
        shaderBody = shaderStageData->body;
    } else if (stage == ShaderStage::Fragment) {
        if (pass.fragmentShader.IsNull()) {
            output.error = "No fragment shader in pass";
            return output;
        }
        shaderStageData = &context.ast.GetShaderStage(pass.fragmentShader);
        shaderBody = shaderStageData->body;
    } else if (stage == ShaderStage::Compute) {
        if (pass.computeShader.IsNull()) {
            output.error = "No compute shader in pass";
            return output;
        }
        shaderStageData = &context.ast.GetShaderStage(pass.computeShader);
        shaderBody = shaderStageData->body;
    } else {
        output.error = "Unsupported shader stage";
        return output;
    }

    // Lower to IR
    IRMemoryPool irPool;
    IRLowering lowering;
    lowering.Initialize(&irPool, const_cast<SymbolTableData*>(&parser.symbolTable),
                        const_cast<AST*>(&context.ast), sourceBase);
    lowering.currentStage = stage;
    lowering.currentPipeline = pipelineRef;
    lowering.currentPassVaryings = varyingContext;

    for (u32 i = 0; i < pass.consts.count; i++) {
        lowering.LowerStatement(pass.consts[i]);
    }

    const BlockData& block = context.ast.GetBlock(shaderBody);
    for (u32 i = 0; i < block.statements.count; i++) {
        lowering.LowerStatement(block.statements[i]);
    }

    // Ensure return
    if (lowering.program.instructionCount == 0 ||
        lowering.program.opcodes[lowering.program.instructionCount - 1] != OP_RET) {
        lowering.builder.EmitInstruction(OP_RET, 0, 0);
    }

    // CFG/SSA for control flow
    Memory::BWEMemoryArena cfgArena;
    char cfgMem[128 * 1024];
    cfgArena.Initialize(cfgMem, sizeof(cfgMem));

    CFGBuilder cfgBuilder;
    CFG* cfgPtr = nullptr;

    bool hasControlFlow = false;
    for (u32 i = 0; i < lowering.program.instructionCount; i++) {
        u16 op = lowering.program.opcodes[i];
        if (op == OP_BRANCH || op == OP_JUMP || op == OP_SWITCH) {
            hasControlFlow = true;
            break;
        }
    }

    if (hasControlFlow) {
        cfgBuilder.Init(&lowering.program, &cfgArena);
        cfgBuilder.Build();
        cfgPtr = &cfgBuilder.cfg;

        if (cfgBuilder.cfg.blockCount > 1) {
            SSA::ConvertToSSA(&lowering.program, &cfgBuilder.cfg, &cfgBuilder, &cfgArena);
        }
    }

    // Generate SPIR-V
    Memory::BWEMemoryArena spirvArena;
    char spirvMem[512 * 1024];
    spirvArena.Initialize(spirvMem, sizeof(spirvMem));

    SPIRVBuilder builder;
    builder.Initialize(&spirvArena, &lowering.program, stage,
                       const_cast<SymbolTableData*>(&parser.symbolTable), cfgPtr);
    if (stage == ShaderStage::Compute && shaderStageData) {
        builder.SetComputeWorkgroupSize(shaderStageData->workgroupSizeX,
                                        shaderStageData->workgroupSizeY,
                                        shaderStageData->workgroupSizeZ);
    }

    // Use interleaved vertex inputs for WebGL compatibility
    SPIRVBuilder::VertexPullingConfig vpConfig;
    vpConfig.mode = SPIRVBuilder::VertexInputMode::Interleaved;
    vpConfig.baseBufferBinding = 0;
    vpConfig.descriptorSet = 0;
    builder.SetVertexPullingConfig(vpConfig);
    builder.SetUseStd430Padding(true);  // For GLSL ES compatibility

    builder.EmitFunction();
    std::vector<u32> spirv = builder.Finalize();
    AnalyzeIR(&output.analysis, &lowering.program);
    output.explicitSamplerUses = CollectExplicitSamplerUses(lowering.program, stage);

    if (spirv.size() <= 5) {
        output.error = "SPIR-V generation failed";
        return output;
    }

    // Capture internals if requested (after SSA for final IR form)
    if (captureInternals) {
        output.irDump = DumpIRToString(lowering.program);
        output.spirvDis = DumpSpirvToString(spirv);
    }

    // Cross-compile to GLSL ES 300
    std::string glslSource;

    // Use direct GLES backend when requested, or as a fallback for ES constructs
    // SPIRV-Cross cannot emit in the no-exceptions WASM build.
    if constexpr (USE_DIRECT_GLES) {
        glslSource = EmitDirectGLES(lowering.program, cfgPtr, stage, pass,
                                    renderConfig, output.analysis, varyingContext,
                                    shaderStageData, sourceBase);
        if (glslSource.empty()) {
            output.error = "Direct GLES generation failed";
            return output;
        }
    } else {
        // Original SPIRV-Cross path
#ifdef USE_SPIRV_CROSS_LIB
        std::vector<uint32_t> spirvData(spirv.begin(), spirv.end());

        if (stage == ShaderStage::Vertex || stage == ShaderStage::Fragment) {
            // Build varying name map for clean output names
            std::unordered_map<uint32_t, std::string> varyingNames;
            if (varyingContext) {
                for (u32 i = 0; i < varyingContext->count; i++) {
                    const auto& varying = varyingContext->varyings[i];
                    if (varying.name[0] != '\0') {
                        varyingNames[varying.slot] = std::string("v_") + varying.name;
                    }
                }
            }

            bool isVertex = (stage == ShaderStage::Vertex);
            glslSource = spirv_cross_wrapper::CompileToGLSLWithVaryings(
                spirvData, 300, true, varyingNames, isVertex);
        } else if (stage == ShaderStage::Compute) {
            glslSource = spirv_cross_wrapper::CompileToGLSL(spirvData, 310, true);
        }

        if (glslSource.empty() || glslSource.find("error:") == 0) {
            std::string fallback = EmitDirectGLES(lowering.program, cfgPtr, stage, pass,
                                                  renderConfig, output.analysis, varyingContext,
                                                  shaderStageData, sourceBase);
            if (!fallback.empty()) {
                glslSource = fallback;
            }
        }
#else
        output.error = "SPIRV-Cross not available (USE_SPIRV_CROSS_LIB not defined)";
        return output;
#endif
    }

    if (glslSource.empty() || glslSource.find("error:") == 0) {
        output.error = glslSource.empty() ? "GLSL ES compilation failed" : glslSource;
        return output;
    }

    if (stage == ShaderStage::Vertex) {
        output.vertexGlsl = glslSource;
    } else if (stage == ShaderStage::Fragment) {
        output.fragmentGlsl = glslSource;
    } else if (stage == ShaderStage::Compute) {
        output.computeGlsl = glslSource;
    }

    output.success = true;
    return output;
}

// ============= Main Compile Function =============

static std::string CompileToJson(const char* bwslSource,
                                 const char* rcfgSource,
                                 bool emitInternals = false,
                                 const std::vector<std::string>& modulePaths = {},
                                 const std::vector<VariantOverride>& variantOverrides = {},
                                 bool dumpVariantSpace = false) {
    (void)rcfgSource;
    std::string source(bwslSource);

    // Set up module search paths from flags
    BWSL::ClearModuleSearchPaths();
    for (const auto& path : modulePaths) {
        BWSL::AddModuleSearchPath(path);
    }

    // Parse BWSL source
    CompilationContext context;
    TokenStream stream;
    stream.Init(&context.arena, source.c_str(), source.length());
    Lexer lexer(source, stream);
    lexer.Tokenize();
    Parser parser;
    parser.Init(&lexer, &stream, &context);

    // Parse as pipeline
    parser.ParsePipeline();

    if (parser.hadError) {
        std::string errorJson = "{\"success\":false,\"errors\":[";
        for (u32 i = 0; i < parser.errors.count && i < 10; i++) {
            if (i > 0) errorJson += ",";
            const ParseError& err = parser.errors[i];
            std::string msg = err.message ? err.message : "Parse error";

            // Get token text for the error
            std::string tokenText;
            if (err.token != INVALID_TOKEN) {
                std::string_view tokenView = stream.GetValue(err.token);
                tokenText = std::string(tokenView);
            }

            // Get context lines (1 before, error line, 1 after)
            std::string lineBefore = err.line > 1 ? lexer.GetLine(err.line - 1) : "";
            std::string sourceLine = lexer.GetLine(err.line);
            std::string lineAfter = lexer.GetLine(err.line + 1);

            errorJson += "{\"line\":" + std::to_string(err.line) +
                         ",\"column\":" + std::to_string(err.column) +
                         ",\"message\":\"" + EscapeJsonString(msg) + "\"";
            if (!tokenText.empty()) {
                errorJson += ",\"token\":\"" + EscapeJsonString(tokenText) + "\"";
            }

            // Add context array with line before, error line, and line after
            errorJson += ",\"context\":[";
            if (!lineBefore.empty()) {
                errorJson += "\"" + EscapeJsonString(lineBefore) + "\",";
            }
            errorJson += "\"" + EscapeJsonString(sourceLine) + "\"";
            if (!lineAfter.empty()) {
                errorJson += ",\"" + EscapeJsonString(lineAfter) + "\"";
            }
            errorJson += "]";

            errorJson += "}";
        }
        errorJson += "]}";
        return errorJson;
    }

    if (context.ast.pipelines.count == 0) {
        return "{\"success\":false,\"errors\":[\"No pipeline found in source\"]}";
    }

    NodeRef originalPipelineRef = context.root;
    VariantSelectionData variantSelection;
    VariantReflectionData variantReflection;
    std::string variantError;

    if (!parser.BuildVariantSelection(originalPipelineRef, nullptr, 0, false,
                                      variantOverrides, &variantSelection, &variantError)) {
        return "{\"success\":false,\"errors\":[\"" + EscapeJsonString(variantError) + "\"]}";
    }
    if (!parser.BuildVariantReflection(originalPipelineRef, &variantSelection,
                                       &variantReflection, &variantError)) {
        return "{\"success\":false,\"errors\":[\"" + EscapeJsonString(variantError) + "\"]}";
    }
    if (dumpVariantSpace) {
        return SerializeVariantReflectionJson(variantReflection);
    }

    NodeRef specializedPipelineRef = parser.SpecializePipelineForVariants(originalPipelineRef,
                                                                         variantSelection,
                                                                         &variantError);
    if (specializedPipelineRef.IsNull()) {
        return "{\"success\":false,\"errors\":[\"" + EscapeJsonString(variantError) + "\"]}";
    }

    const PipelineData& pipeline = context.ast.GetPipeline(specializedPipelineRef);
    const char* sourceBase = lexer.GetSourceBase();
    RenderConfig renderConfig = CreateSyntheticRenderConfig(context.ast,
                                                            pipeline,
                                                            parser.symbolTable,
                                                            sourceBase);
    const ComputeGraphData* graphData = nullptr;
    ComputeGraphCompileResult graphResult = CompileComputeGraph(context.ast,
                                                               pipeline,
                                                               renderConfig,
                                                               sourceBase);
    if (!graphResult.success) {
        return "{\"success\":false,\"errors\":[\"" + EscapeJsonString(graphResult.error) + "\"]}";
    }
    if (!pipeline.computeGraph.IsNull()) {
        graphData = &context.ast.GetComputeGraph(pipeline.computeGraph);
    }

    // Build result JSON
    std::ostringstream json;
    json << "{\"success\":true,\"shaders\":{";

    bool firstPass = true;
    for (u32 passIdx = 0; passIdx < pipeline.passes.count; passIdx++) {
        const PassData& pass = context.ast.GetPass(pipeline.passes[passIdx]);

        // Get pass name
        std::string passName;
        if (!pass.name.isHashOnly() && sourceBase) {
            passName = std::string(pass.name.view(sourceBase));
        } else {
            passName = "pass" + std::to_string(passIdx);
        }

        bool isComputePass = !pass.computeShader.IsNull();

        // Create varying context for vertex->fragment data flow
        PassVaryingContext passVaryings;

        ShaderOutput vertResult;
        ShaderOutput fragResult;
        ShaderOutput compResult;

        if (isComputePass) {
            compResult = CompileShaderStage(
                context, parser, pass, ShaderStage::Compute,
                renderConfig, specializedPipelineRef, nullptr, sourceBase, emitInternals
            );
            if (!compResult.success) {
                return "{\"success\":false,\"errors\":[\"Compute shader: " + EscapeJsonString(compResult.error) + "\"]}";
            }
        } else {
            // Compile vertex shader
            vertResult = CompileShaderStage(
                context, parser, pass, ShaderStage::Vertex,
                renderConfig, specializedPipelineRef, &passVaryings, sourceBase, emitInternals
            );

            if (!vertResult.success) {
                return "{\"success\":false,\"errors\":[\"Vertex shader: " + EscapeJsonString(vertResult.error) + "\"]}";
            }

            // Compile fragment shader
            fragResult = CompileShaderStage(
                context, parser, pass, ShaderStage::Fragment,
                renderConfig, specializedPipelineRef, &passVaryings, sourceBase, emitInternals
            );

            if (!fragResult.success) {
                return "{\"success\":false,\"errors\":[\"Fragment shader: " + EscapeJsonString(fragResult.error) + "\"]}";
            }
        }

        std::vector<ExplicitSamplerUse> reflectionSamplerUses;
        if (!vertResult.explicitSamplerUses.empty()) {
            reflectionSamplerUses.insert(reflectionSamplerUses.end(),
                                         vertResult.explicitSamplerUses.begin(),
                                         vertResult.explicitSamplerUses.end());
        }
        if (!fragResult.explicitSamplerUses.empty()) {
            reflectionSamplerUses.insert(reflectionSamplerUses.end(),
                                         fragResult.explicitSamplerUses.begin(),
                                         fragResult.explicitSamplerUses.end());
        }
        if (!compResult.explicitSamplerUses.empty()) {
            reflectionSamplerUses.insert(reflectionSamplerUses.end(),
                                         compResult.explicitSamplerUses.begin(),
                                         compResult.explicitSamplerUses.end());
        }

        ResourceReflectionConfig reflectionConfig;
        reflectionConfig.vertexPullingMode = ResourceReflectionConfig::VertexPullingMode::Disabled;
        reflectionConfig.descriptorSet = 0;

        const std::vector<ReflectedResourceBinding> reflectedResources =
            BuildResolvedResourceReflection(&context.ast,
                                           &pipeline,
                                           &pass,
                                           parser.symbolTable,
                                           sourceBase,
                                           isComputePass ? nullptr : &vertResult.analysis,
                                           isComputePass ? nullptr : &fragResult.analysis,
                                           isComputePass ? &compResult.analysis : nullptr,
                                           reflectionConfig,
                                           reflectionSamplerUses.empty() ? nullptr : &reflectionSamplerUses);

        // Add to JSON
        if (!firstPass) json << ",";
        firstPass = false;

        json << "\"" << passName << "\":{";
        if (isComputePass) {
            const ShaderStageData& computeStage = context.ast.GetShaderStage(pass.computeShader);
            json << "\"compute\":\"" << EscapeJsonString(compResult.computeGlsl) << "\",";
            json << "\"workgroupSize\":[" << computeStage.workgroupSizeX << ","
                 << computeStage.workgroupSizeY << "," << computeStage.workgroupSizeZ << "],";
            AppendResourceReflectionJson(json, reflectedResources);
            json << ",\"webgl\":{";
            AppendWebGLStageJson(json, ShaderStage::Compute, &context.ast, &pipeline, nullptr, sourceBase, reflectedResources);
            json << "}";
        } else {
            json << "\"vertex\":\"" << EscapeJsonString(vertResult.vertexGlsl) << "\",";
            json << "\"fragment\":\"" << EscapeJsonString(fragResult.fragmentGlsl) << "\",";
            AppendResourceReflectionJson(json, reflectedResources);
            json << ",\"webgl\":{";
            AppendWebGLStageJson(json, ShaderStage::Vertex, &context.ast, &pipeline, &pass, sourceBase, reflectedResources);
            json << ",";
            AppendWebGLStageJson(json, ShaderStage::Fragment, &context.ast, &pipeline, nullptr, sourceBase, reflectedResources);
            json << "},";

            // Add varyings metadata (vertex output -> fragment input mappings)
            json << "\"varyings\":{";
            bool firstVarying = true;
            for (u32 i = 0; i < passVaryings.count; i++) {
                const auto& varying = passVaryings.varyings[i];
                if (varying.name[0] == '\0') continue;  // Skip if name wasn't stored
                if (!firstVarying) json << ",";
                json << "\"" << varying.name << "\":" << varying.slot;
                firstVarying = false;
            }
            json << "}";
        }

        json << "}";
    }

    json << "},\"variants\":" << SerializeVariantReflectionJson(variantReflection);

    if (graphData) {
        json << ",\"computeGraph\":{";
        json << "\"executionOrder\":[";
        for (size_t i = 0; i < graphResult.graph.executionOrder.size(); i++) {
            if (i > 0) json << ",";
            json << "\"" << graphResult.graph.executionOrder[i].ToString(sourceBase) << "\"";
        }
        json << "],";

        json << "\"barriers\":[";
        for (size_t i = 0; i < graphResult.graph.barriers.size(); i++) {
            if (i > 0) json << ",";
            const auto& barrier = graphResult.graph.barriers[i];
            json << "{\"before\":\"" << barrier.beforeNode.ToString(sourceBase) << "\",";
            json << "\"after\":\"" << barrier.afterNode.ToString(sourceBase) << "\",";
            json << "\"type\":\"" << BarrierTypeToString(barrier.type) << "\",";
            json << "\"resource\":\"" << barrier.resourceName.ToString(sourceBase) << "\"}";
        }
        json << "],";

        json << "\"nodes\":{";
        for (u32 i = 0; i < graphData->nodes.count; i++) {
            if (i > 0) json << ",";
            const ComputeGraphNode& node = graphData->nodes[i];
            json << "\"" << node.passName.ToString(sourceBase) << "\":{";

            json << "\"inputs\":[";
            for (u32 in = 0; in < node.inputs.count; in++) {
                if (in > 0) json << ",";
                const auto& input = node.inputs[in];
                json << "{\"name\":\"" << input.name.ToString(sourceBase) << "\",";
                json << "\"access\":\"" << AccessModeToString(input.access) << "\"}";
            }
            json << "],";

            json << "\"outputs\":[";
            for (u32 out = 0; out < node.outputs.count; out++) {
                if (out > 0) json << ",";
                json << "\"" << node.outputs[out].ToString(sourceBase) << "\"";
            }
            json << "]";

            json << "}";
        }
        json << "}";
        json << "}";
    }

    // Add internals if requested (must iterate passes again to get the data)
    if (emitInternals) {
        json << ",\"internals\":[";
        bool firstInternal = true;
        for (u32 passIdx = 0; passIdx < pipeline.passes.count; passIdx++) {
            const PassData& pass = context.ast.GetPass(pipeline.passes[passIdx]);
            std::string passName;
            if (!pass.name.isHashOnly() && sourceBase) {
                passName = std::string(pass.name.view(sourceBase));
            } else {
                passName = "pass" + std::to_string(passIdx);
            }

            // Recompile with internals capture (we need the IR/SPIR-V dumps)
            PassVaryingContext passVaryings;
            bool isComputePass = !pass.computeShader.IsNull();

            if (isComputePass) {
                ShaderOutput compResult = CompileShaderStage(
                    context, parser, pass, ShaderStage::Compute,
                    renderConfig, specializedPipelineRef, nullptr, sourceBase, true
                );
                if (compResult.success) {
                    if (!firstInternal) json << ",";
                    firstInternal = false;
                    json << "{\"pass\":\"" << passName << "\",\"stage\":\"compute\",";
                    json << "\"ir\":\"" << EscapeJsonString(compResult.irDump) << "\",";
                    json << "\"spirv_dis\":\"" << EscapeJsonString(compResult.spirvDis) << "\"}";
                }
            } else {
                ShaderOutput vertResult = CompileShaderStage(
                    context, parser, pass, ShaderStage::Vertex,
                    renderConfig, specializedPipelineRef, &passVaryings, sourceBase, true
                );
                if (vertResult.success) {
                    if (!firstInternal) json << ",";
                    firstInternal = false;
                    json << "{\"pass\":\"" << passName << "\",\"stage\":\"vertex\",";
                    json << "\"ir\":\"" << EscapeJsonString(vertResult.irDump) << "\",";
                    json << "\"spirv_dis\":\"" << EscapeJsonString(vertResult.spirvDis) << "\"}";
                }

                ShaderOutput fragResult = CompileShaderStage(
                    context, parser, pass, ShaderStage::Fragment,
                    renderConfig, specializedPipelineRef, &passVaryings, sourceBase, true
                );
                if (fragResult.success) {
                    if (!firstInternal) json << ",";
                    firstInternal = false;
                    json << "{\"pass\":\"" << passName << "\",\"stage\":\"fragment\",";
                    json << "\"ir\":\"" << EscapeJsonString(fragResult.irDump) << "\",";
                    json << "\"spirv_dis\":\"" << EscapeJsonString(fragResult.spirvDis) << "\"}";
                }
            }
        }
        json << "]";
    }

    json << "}";

    return json.str();
}

// ============= C API for Emscripten =============

// Global buffer for return value (Emscripten needs stable memory)
static std::string g_resultBuffer;

extern "C" {

// Main compile function - takes BWSL source, a reserved/ignored compatibility string,
// and optional flags.
// flags: "-internals" to include IR dump and SPIR-V disassembly in output
//        "-modules <path>" to add module search path (can be used multiple times)
//        "-variant name=value" to specialize one named variant (can be used multiple times)
//        "-dump-variant-space" to return variant reflection JSON without shader output
// Returns JSON string with compiled shaders and metadata
const char* compile(const char* bwslSource, const char* rcfgSource, const char* flags) {
    bool emitInternals = flags && strstr(flags, "-internals") != nullptr;
    bool dumpVariantSpace = flags && strstr(flags, "-dump-variant-space") != nullptr;

    std::vector<std::string> modulePaths;
    std::vector<VariantOverride> variantOverrides;
    if (flags) {
        const char* p = flags;
        while ((p = strstr(p, "-modules ")) != nullptr) {
            p += 9;  // Skip "-modules "
            while (*p == ' ') p++;  // Skip extra spaces
            const char* end = p;
            while (*end && *end != ' ' && *end != '\0') end++;
            if (end > p) {
                modulePaths.push_back(std::string(p, end - p));
            }
            p = end;
        }

        p = flags;
        while ((p = strstr(p, "-variant ")) != nullptr) {
            p += 9;  // Skip "-variant "
            while (*p == ' ') p++;
            const char* end = p;
            while (*end && *end != ' ' && *end != '\0') end++;
            if (end > p) {
                std::string assignment(p, end - p);
                size_t equals = assignment.find('=');
                if (equals != std::string::npos && equals > 0 && equals + 1 < assignment.size()) {
                    VariantOverride overrideValue;
                    overrideValue.name = assignment.substr(0, equals);
                    overrideValue.value = assignment.substr(equals + 1);
                    variantOverrides.push_back(std::move(overrideValue));
                }
            }
            p = end;
        }
    }

    g_resultBuffer = CompileToJson(bwslSource,
                                   rcfgSource ? rcfgSource : "",
                                   emitInternals,
                                   modulePaths,
                                   variantOverrides,
                                   dumpVariantSpace);
    return g_resultBuffer.c_str();
}

// Version info
const char* getVersion() {
    return "BWSL Compiler 1.0.0 (WASM)";
}

// ============= Symbol Export for Autocomplete =============

static const char* SymbolKindToString(SymbolKind kind) {
    switch (kind) {
        case SymbolKind::VARIABLE: return "variable";
        case SymbolKind::FUNCTION: return "function";
        case SymbolKind::ATTRIBUTE: return "attribute";
        case SymbolKind::RESOURCE: return "resource";
        case SymbolKind::SHADER_STAGE: return "stage";
        case SymbolKind::PASS: return "pass";
        case SymbolKind::RENDER_TARGET: return "renderTarget";
        case SymbolKind::BUFFER_GROUP: return "bufferGroup";
        case SymbolKind::UNIFORM_BUFFER: return "uniform";
        case SymbolKind::EVAL_CONSTANT: return "constant";
        case SymbolKind::EVAL_FUNCTION: return "evalFunction";
        case SymbolKind::ENUM_SYMBOL: return "enumValue";
        case SymbolKind::ENUM: return "enum";
        case SymbolKind::CONSTRAINT: return "constraint";
        case SymbolKind::GENERIC_FUNCTION: return "genericFunction";
        case SymbolKind::CUSTOM_TYPE: return "struct";
        default: return "unknown";
    }
}

static std::string GetSymbolsJson(const char* bwslSource, const char* rcfgSource, const std::vector<std::string>& modulePaths = {}) {
    (void)rcfgSource;
    std::string source(bwslSource);
    std::ostringstream json;

    // Set up module search paths from flags
    BWSL::ClearModuleSearchPaths();
    for (const auto& path : modulePaths) {
        BWSL::AddModuleSearchPath(path);
    }

    // Parse BWSL source
    CompilationContext context;
    TokenStream stream;
    stream.Init(&context.arena, source.c_str(), source.length());
    Lexer lexer(source, stream);
    lexer.Tokenize();
    Parser parser;
    parser.Init(&lexer, &stream, &context);

    // Try to parse - continue even if there are errors
    parser.ParsePipeline();

    const char* sourceBase = lexer.GetSourceBase();
    const SymbolTableData& symTable = parser.symbolTable;

    json << "{\"symbols\":[";

    bool first = true;

    // Export symbols from the symbol table
    for (u32 i = 0; i < symTable.symbols.count; i++) {
        const Symbol& sym = symTable.symbols[i];

        // Skip internal/hash-only symbols that have no readable name
        std::string name;
        if (!sym.name.isHashOnly() && sourceBase) {
            name = std::string(sym.name.view(sourceBase));
        } else {
            // Try reverse lookup for hash-only names
            std::string reversed = ReverseLookup::GetString(sym.name.nameHash);
            if (!reversed.empty()) {
                name = reversed;
            } else {
                continue;  // Skip unnamed symbols
            }
        }

        // Skip symbols with 'resources.' prefix for cleaner autocomplete
        if (name.find("resources.") == 0) {
            name = name.substr(10);  // Remove prefix for display
        }

        if (!first) json << ",";
        first = false;

        json << "{\"name\":\"" << EscapeJsonString(name) << "\"";
        json << ",\"kind\":\"" << SymbolKindToString(sym.kind) << "\"";
        json << ",\"scope\":" << sym.scopeLevel;

        // Add type info for variables
        if (sym.kind == SymbolKind::VARIABLE && sym.index < symTable.variables.count) {
            const VariableData& var = symTable.variables[sym.index];
            json << ",\"type\":\"" << CoreTypeToString(var.typeInfo.coreType) << "\"";
            if (var.isConst) json << ",\"const\":true";
        }

        // Add type info for attributes
        if (sym.kind == SymbolKind::ATTRIBUTE && sym.index < symTable.attributes.count) {
            const AttributeData& attr = symTable.attributes[sym.index];
            json << ",\"type\":\"" << CoreTypeToString(attr.typeInfo.coreType) << "\"";
        }

        // Add type info for resources (uniforms)
        if (sym.kind == SymbolKind::RESOURCE && sym.index < symTable.resources.count) {
            const ResourceData& res = symTable.resources[sym.index];
            CoreType coreType = static_cast<CoreType>(res.coreType);
            if (coreType != CoreType::INVALID && coreType != CoreType::CUSTOM) {
                json << ",\"type\":\"" << CoreTypeToString(coreType) << "\"";
            }
        }

        // Add return type for functions
        if (sym.kind == SymbolKind::FUNCTION && sym.index < symTable.functions.count) {
            const FunctionData& func = symTable.functions[sym.index];
            json << ",\"returnType\":\"" << CoreTypeToString(func.returnType) << "\"";
        }

        json << "}";
    }

    json << "],";

    // Add built-in types
    json << "\"types\":[";
    json << "\"float\",\"float2\",\"float3\",\"float4\",";
    json << "\"int\",\"int2\",\"int3\",\"int4\",";
    json << "\"uint\",\"uint2\",\"uint3\",\"uint4\",";
    json << "\"bool\",\"mat2\",\"mat3\",\"mat4\"";
    json << "],";

    // Add keywords
    json << "\"keywords\":[";
    json << "\"pipeline\",\"pass\",\"vertex\",\"fragment\",\"compute\",";
    json << "\"attributes\",\"use\",\"resources\",";
    json << "\"output\",\"input\",\"if\",\"else\",\"for\",\"while\",\"return\",";
    json << "\"const\",\"eval\",\"struct\",\"enum\"";
    json << "]}";

    return json.str();
}

// Get symbols for autocomplete.
// Returns JSON with symbols, types, and keywords
// flags: "-modules <path>" to add module search path (can be used multiple times)
const char* getSymbols(const char* bwslSource, const char* rcfgSource, const char* flags) {
    // Parse -modules flags
    std::vector<std::string> modulePaths;
    if (flags) {
        const char* p = flags;
        while ((p = strstr(p, "-modules ")) != nullptr) {
            p += 9;  // Skip "-modules "
            while (*p == ' ') p++;  // Skip extra spaces
            const char* end = p;
            while (*end && *end != ' ' && *end != '\0') end++;
            if (end > p) {
                modulePaths.push_back(std::string(p, end - p));
            }
            p = end;
        }
    }

    g_resultBuffer = GetSymbolsJson(bwslSource, rcfgSource ? rcfgSource : "", modulePaths);
    return g_resultBuffer.c_str();
}

} // extern "C"

// Entry point for testing (not used in WASM)
#ifndef BWSL_WASM
int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("Usage: %s <bwsl-source>\n", argv[0]);
        return 1;
    }

    const char* result = compile(argv[1], "", "");
    printf("%s\n", result);
    return 0;
}
#endif
