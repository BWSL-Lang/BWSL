// SPIRV-Cross Wrapper
// Compiles SPIRV-Cross separately to avoid macro conflicts with BWSL's defs.h
// (defs.h defines u32, f32, f64 as macros which conflict with SPIRV-Cross)

#include <string>
#include <string_view>
#include <vector>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

// spv::HasResultAndType, for walking raw SPIR-V.
#define SPV_ENABLE_UTILITY_CODE

// Include SPIRV-Cross headers (before any BWSL headers)
#include "SPIRV-Cross/spirv_msl.hpp"
#include "SPIRV-Cross/spirv_hlsl.hpp"

// Unity build for SPIRV-Cross
#include "SPIRV-Cross/spirv_cross.cpp"
#include "SPIRV-Cross/spirv_parser.cpp"
#include "SPIRV-Cross/spirv_cross_parsed_ir.cpp"
#include "SPIRV-Cross/spirv_cfg.cpp"
#include "SPIRV-Cross/spirv_glsl.cpp"
#include "SPIRV-Cross/spirv_msl.cpp"
#include "SPIRV-Cross/spirv_hlsl.cpp"

namespace spirv_cross_wrapper {

static std::string MetalResourceBindingError(spirv_cross::CompilerMSL& compiler) {
    const auto resources = compiler.get_shader_resources();
    for (const auto& buffer : resources.uniform_buffers)
        if (compiler.get_decoration(buffer.id, spv::DecorationBinding) >= 31)
            return "error: Metal supports buffer bindings 0 through 30; reduce buffer resources or vertex-pulling attributes";
    for (const auto& buffer : resources.storage_buffers)
        if (compiler.get_decoration(buffer.id, spv::DecorationBinding) >= 31)
            return "error: Metal supports buffer bindings 0 through 30; reduce buffer resources or vertex-pulling attributes";
    for (const auto& sampler : resources.separate_samplers)
        if (compiler.get_decoration(sampler.id, spv::DecorationBinding) >= 16)
            return "error: Metal supports sampler bindings 0 through 15";
    if (resources.separate_samplers.size() + resources.sampled_images.size() > 16)
        return "error: Metal supports at most 16 active samplers";
    return {};
}

static uint32_t RequiredMSLVersion(const spirv_cross::CompilerMSL& compiler) {
    for (auto capability : compiler.get_declared_capabilities()) {
        switch (capability) {
            case spv::CapabilityGroupNonUniformVote:
            case spv::CapabilityGroupNonUniformArithmetic:
            case spv::CapabilityGroupNonUniformBallot:
            case spv::CapabilityGroupNonUniformClustered:
            case spv::CapabilityGroupNonUniformQuad:
                return spirv_cross::CompilerMSL::Options::make_msl_version(2, 1);
            default: break;
        }
    }
    return spirv_cross::CompilerMSL::Options::make_msl_version(2, 0);
}

// Helper to add MSL resource bindings that preserve SPIR-V binding indices
// Note: Metal only allows sampler indices 0-15, so we remap sampler bindings
// to stay within this range while keeping texture bindings as-is.
static void preserveBindingIndices(spirv_cross::CompilerMSL& compiler, spv::ExecutionModel stage) {
    auto resources = compiler.get_shader_resources();
    bool samplerSlots[16] = {};
    for (const auto& res : resources.separate_samplers) {
        uint32_t binding = compiler.get_decoration(res.id, spv::DecorationBinding);
        if (binding >= 16) SPIRV_CROSS_THROW("Metal supports sampler bindings 0 through 15");
        samplerSlots[binding] = true;
    }
    // A texture array's samplers take `count` consecutive slots.
    auto allocateSamplerSlots = [&](uint32_t count) -> uint32_t {
        for (uint32_t first = 0; first + count <= 16; ++first) {
            uint32_t free = 0;
            while (free < count && !samplerSlots[first + free]) ++free;
            if (free < count) continue;
            for (uint32_t slot = first; slot < first + count; ++slot) samplerSlots[slot] = true;
            return first;
        }
        SPIRV_CROSS_THROW("Metal supports at most 16 active samplers");
    };

    // Helper lambda to add binding for a resource (non-sampler)
    auto addBinding = [&](const spirv_cross::Resource& res) {
        uint32_t set = compiler.get_decoration(res.id, spv::DecorationDescriptorSet);
        uint32_t binding = compiler.get_decoration(res.id, spv::DecorationBinding);

        spirv_cross::MSLResourceBinding mslBinding;
        mslBinding.stage = stage;
        mslBinding.desc_set = set;
        mslBinding.binding = binding;
        mslBinding.msl_buffer = binding;
        mslBinding.msl_texture = binding;
        mslBinding.msl_sampler = binding;  // Not used for non-sampler resources
        compiler.add_msl_resource_binding(mslBinding);
    };

    // Helper lambda to add binding for combined image sampler
    // Texture binding stays as-is, sampler binding is remapped to 0-15 range
    auto addSampledImageBinding = [&](const spirv_cross::Resource& res) {
        uint32_t set = compiler.get_decoration(res.id, spv::DecorationDescriptorSet);
        uint32_t binding = compiler.get_decoration(res.id, spv::DecorationBinding);

        spirv_cross::MSLResourceBinding mslBinding;
        mslBinding.stage = stage;
        mslBinding.desc_set = set;
        mslBinding.binding = binding;
        mslBinding.msl_buffer = binding;
        mslBinding.msl_texture = binding;
        // Remap sampler to sequential slots within 0-15 range
        const auto& type = compiler.get_type(res.type_id);
        mslBinding.msl_sampler = allocateSamplerSlots(type.array.empty() ? 1 : type.array[0]);
        compiler.add_msl_resource_binding(mslBinding);
    };

    // Uniform buffers
    for (const auto& res : resources.uniform_buffers) {
        if (compiler.get_decoration(res.id, spv::DecorationBinding) >= 31)
            SPIRV_CROSS_THROW("Metal supports buffer bindings 0 through 30; reduce buffer resources or vertex-pulling attributes");
        addBinding(res);
    }

    // Storage buffers
    for (const auto& res : resources.storage_buffers) {
        if (compiler.get_decoration(res.id, spv::DecorationBinding) >= 31)
            SPIRV_CROSS_THROW("Metal supports buffer bindings 0 through 30; reduce buffer resources or vertex-pulling attributes");
        addBinding(res);
    }

    // Separate images (textures)
    for (const auto& res : resources.separate_images) {
        addBinding(res);
    }

    // Separate samplers - these also need remapped bindings
    for (const auto& res : resources.separate_samplers) {
        uint32_t set = compiler.get_decoration(res.id, spv::DecorationDescriptorSet);
        uint32_t binding = compiler.get_decoration(res.id, spv::DecorationBinding);

        spirv_cross::MSLResourceBinding mslBinding;
        mslBinding.stage = stage;
        mslBinding.desc_set = set;
        mslBinding.binding = binding;
        mslBinding.msl_buffer = binding;
        mslBinding.msl_texture = binding;
        mslBinding.msl_sampler = binding;
        compiler.add_msl_resource_binding(mslBinding);
    }

    // Combined image samplers - texture binding preserved, sampler remapped
    for (const auto& res : resources.sampled_images) {
        addSampledImageBinding(res);
    }

    // Storage images
    for (const auto& res : resources.storage_images) {
        addBinding(res);
    }
}

static bool LiteralStringEquals(const std::vector<uint32_t>& words,
                                size_t start,
                                size_t end,
                                const char* expected) {
    size_t charIndex = 0;
    for (size_t i = start; i < end; i++) {
        uint32_t word = words[i];
        for (uint32_t byte = 0; byte < 4; byte++) {
            char c = static_cast<char>((word >> (byte * 8)) & 0xffu);
            char want = expected[charIndex];
            if (c != want) {
                return false;
            }
            if (c == '\0') {
                return true;
            }
            charIndex++;
        }
    }
    return expected[charIndex] == '\0';
}

static std::string CheckGLSLESSPIRVCompatRaw(const std::vector<uint32_t>& spirv,
                                             int glslVersion,
                                             bool es) {
    if (!es || spirv.size() < 5) return {};

    std::unordered_set<uint32_t> glslStd450Ids;
    for (size_t offset = 5; offset < spirv.size();) {
        uint32_t first = spirv[offset];
        uint16_t op = static_cast<uint16_t>(first & 0xffffu);
        uint16_t wordCount = static_cast<uint16_t>(first >> 16);
        if (wordCount == 0 || offset + wordCount > spirv.size()) {
            return "error: malformed SPIR-V instruction stream";
        }

        if (op == spv::OpExtInstImport && wordCount >= 3 &&
            LiteralStringEquals(spirv, offset + 2, offset + wordCount,
                                "GLSL.std.450")) {
            glslStd450Ids.insert(spirv[offset + 1]);
        } else if (op == spv::OpExtInst && wordCount >= 5) {
            uint32_t setId = spirv[offset + 3];
            uint32_t extOp = spirv[offset + 4];
            if (glslStd450Ids.count(setId) != 0) {
                if (extOp == GLSLstd450Ldexp) {
                    return "error: GLSL ES does not support GLSL.std.450 Ldexp; use the direct GLES ldexp fallback";
                }
                if (extOp == GLSLstd450ModfStruct) {
                    return "error: GLSL ES cannot cross-compile GLSL.std.450 ModfStruct; use the direct GLES modf fallback";
                }
            }
        } else if (op == spv::OpDPdxFine || op == spv::OpDPdyFine ||
                   op == spv::OpDPdxCoarse || op == spv::OpDPdyCoarse ||
                   op == spv::OpFwidthFine || op == spv::OpFwidthCoarse) {
            return "error: GLSL ES does not support fine/coarse derivative opcodes; use the direct GLES derivative fallback";
        } else if (glslVersion < 310 && op == spv::OpImageQueryLevels) {
            return "error: GLSL ES 300 does not support textureQueryLevels";
        } else if (glslVersion < 310 && op == spv::OpImageGather) {
            // SPIRV-Cross asserts for every gather on ESSL 300 in WASM builds,
            // including the form without optional image operands. Return to
            // the direct GLES fallback before invoking compiler.compile().
            return "error: GLSL ES 300 does not support textureGather; use the direct GLES gather fallback";
        }

        offset += wordCount;
    }
    return {};
}

// Whether the module carries line information (OpLine, from bwslc
// -debug-info): the generated source then gets #line directives pointing
// at the .bwsl lines.
static bool HasLineInfo(const std::vector<uint32_t>& spirv) {
    for (size_t offset = 5; offset < spirv.size();) {
        uint32_t count = spirv[offset] >> 16;
        if ((spirv[offset] & 0xFFFF) == spv::OpLine) return true;
        if (count == 0) break;
        offset += count;
    }
    return false;
}

static void EnableLineDirectives(spirv_cross::CompilerGLSL& compiler, const std::vector<uint32_t>& spirv) {
    auto options = compiler.get_common_options();
    options.emit_line_directives = HasLineInfo(spirv);
    compiler.set_common_options(options);
}

// Keeps the last of each run of #line directives with no code between
// them, the only one that applies: SPIRV-Cross writes one per OpLine and
// per DebugLine, and several source lines often fold into one statement.
static std::string CollapseLineDirectives(std::string source) {
    if (source.find("#line ") == std::string::npos) return source;
    std::string result;
    result.reserve(source.size());
    std::string pending;  // A #line not followed by code yet
    size_t start = 0;
    while (start < source.size()) {
        size_t end = source.find('\n', start);
        if (end == std::string::npos) end = source.size();
        std::string_view line(source.data() + start, end - start);
        if (line.rfind("#line ", 0) == 0) {
            pending.assign(line);
        } else {
            if (!pending.empty()) {
                result.append(pending).push_back('\n');
                pending.clear();
            }
            result.append(line);
            if (end < source.size()) result.push_back('\n');
        }
        start = end + 1;
    }
    return result;
}

// SPIRV-Cross writes GLSL line directives as `#line 12 "file"`, which needs
// GL_GOOGLE_cpp_style_line_directive, an extension GL drivers don't have.
// Standard GLSL numbers the source instead: `#line 12 0`. Each file gets a
// number in the order it first appears, listed in a comment after #version.
static void UseStandardGLSLLineDirectives(std::string& source) {
    if (source.find("#line ") == std::string::npos) return;
    std::vector<std::string> files;
    std::string result;
    result.reserve(source.size());
    size_t start = 0;
    while (start < source.size()) {
        size_t end = source.find('\n', start);
        if (end == std::string::npos) end = source.size();
        std::string_view line(source.data() + start, end - start);
        if (line == "#extension GL_GOOGLE_cpp_style_line_directive : require") {
            start = end + 1;
            continue;
        }
        size_t quote = line.find('"');
        if (line.rfind("#line ", 0) == 0 && quote != std::string_view::npos && line.back() == '"') {
            std::string file(line.substr(quote + 1, line.size() - quote - 2));
            size_t index = 0;
            while (index < files.size() && files[index] != file) index++;
            if (index == files.size()) files.push_back(file);
            result.append(line.substr(0, quote));
            result.append(std::to_string(index));
        } else {
            result.append(line);
        }
        if (end < source.size()) result.push_back('\n');
        start = end + 1;
    }
    std::string legend = "// #line source string numbers:\n";
    for (size_t i = 0; i < files.size(); i++) legend += "//   " + std::to_string(i) + ": " + files[i] + "\n";
    size_t afterVersion = result.rfind("#version", 0) == 0 ? result.find('\n') : std::string::npos;
    result.insert(afterVersion == std::string::npos ? 0 : afterVersion + 1, legend);
    source = std::move(result);
}

// HLSL cbuffer packing differs from std140. A scalar or vector packs into what
// is left of the previous 16-byte row, where std140 aligns a vector to its
// size (float a; float3 b puts b at 4, not 16). The last element of an array,
// the last column of a matrix and the end of a struct are not padded to 16
// bytes, so the next member packs into them (float w[3]; float c puts c at
// 36, not 48). SPIRV-Cross can place a cbuffer's top-level members with
// packoffset, but a struct inside one is laid out by HLSL packing.
//
// RewriteUniformStructsForHLSL adds padding members to the structs a uniform
// reaches, so that HLSL packing puts each member at its std140 offset and
// ends each struct on a 16-byte boundary. The module only feeds SPIRV-Cross's
// HLSL output: padding may sit in the unused tail of an array's last row,
// which std140 counts as part of the array. The data stays where it was.
namespace hlsl_cbuffer {

struct Module {
    const std::vector<uint32_t>& words;
    std::unordered_map<uint32_t, size_t> defs;          // type or constant id -> offset
    std::unordered_map<uint32_t, uint32_t> resultTypes; // value id -> type id
    std::unordered_map<uint64_t, uint32_t> memberOffsets;
    std::unordered_set<uint64_t> rowMajorMembers;
    std::unordered_set<uint32_t> blocks;
};

struct Plan {
    std::vector<int32_t> members;   // each new member: an old index, or -1 for padding
    std::vector<uint32_t> padOffsets;
    std::vector<uint32_t> newIndex; // old member index -> new index
    uint32_t padType = 0;
};

static uint64_t MemberKey(uint32_t type, uint32_t member) {
    return (uint64_t(type) << 32) | member;
}

static uint16_t OpAt(const Module& m, size_t offset) { return uint16_t(m.words[offset] & 0xffffu); }

static uint16_t TypeOp(const Module& m, uint32_t type) {
    auto def = m.defs.find(type);
    return def == m.defs.end() ? 0 : OpAt(m, def->second);
}

static uint32_t Operand(const Module& m, uint32_t type, uint32_t index) {
    return m.words[m.defs.at(type) + index];
}

static uint32_t MemberCount(const Module& m, uint32_t type) {
    return (m.words[m.defs.at(type)] >> 16) - 2;
}

static bool ConstantValue(const Module& m, uint32_t id, uint32_t* value) {
    auto def = m.defs.find(id);
    if (def == m.defs.end() || OpAt(m, def->second) != spv::OpConstant) return false;
    *value = m.words[def->second + 3];
    return true;
}

// The type an index steps into, or 0 when it can't be followed.
static uint32_t StepInto(const Module& m, uint32_t type, uint32_t index) {
    switch (TypeOp(m, type)) {
        case spv::OpTypeStruct: return index < MemberCount(m, type) ? Operand(m, type, 2 + index) : 0;
        case spv::OpTypeArray:
        case spv::OpTypeRuntimeArray:
        case spv::OpTypeVector:
        case spv::OpTypeMatrix: return Operand(m, type, 2);
        default: return 0;
    }
}

struct Packing {
    uint32_t size = 0;
    uint32_t alignment = 4;
    bool isStruct = false;
};

// HLSL cbuffer packing of a type: the size it takes and the alignment it
// starts at. A struct's size is after the rewrite, which ends it on a 16-byte
// boundary.
static bool PackType(const Module& m, uint32_t type, bool rowMajor, Packing* out) {
    switch (TypeOp(m, type)) {
        case spv::OpTypeFloat:
        case spv::OpTypeInt:
            if (Operand(m, type, 2) != 32) return false;
            *out = {4, 4, false};
            return true;
        case spv::OpTypeVector: {
            Packing component;
            if (!PackType(m, Operand(m, type, 2), false, &component)) return false;
            *out = {4 * Operand(m, type, 3), 4, false};
            return true;
        }
        case spv::OpTypeMatrix: {
            // Column-major: each column takes a row, the last one only its
            // components.
            uint32_t column = Operand(m, type, 2);
            if (rowMajor || TypeOp(m, column) != spv::OpTypeVector) return false;
            *out = {Operand(m, type, 3) * 16 - (4 - Operand(m, column, 3)) * 4, 16, false};
            return true;
        }
        case spv::OpTypeArray: {
            // Each element starts a row; the last one takes only its size.
            uint32_t length = 0;
            Packing element;
            if (!ConstantValue(m, Operand(m, type, 3), &length) || length == 0 ||
                !PackType(m, Operand(m, type, 2), rowMajor, &element))
                return false;
            *out = {(length - 1) * ((element.size + 15) & ~15u) + element.size, 16, false};
            return true;
        }
        case spv::OpTypeStruct: {
            uint32_t count = MemberCount(m, type);
            uint32_t end = 0;
            if (count > 0) {
                auto offset = m.memberOffsets.find(MemberKey(type, count - 1));
                Packing last;
                if (offset == m.memberOffsets.end() ||
                    !PackType(m, Operand(m, type, 2 + count - 1),
                              m.rowMajorMembers.count(MemberKey(type, count - 1)) != 0, &last))
                    return false;
                end = offset->second + last.size;
            }
            *out = {(end + 15) & ~15u, 16, true};
            return true;
        }
        default: return false;
    }
}

// Where HLSL packing puts a member, given where the previous one ended: at
// its alignment, or on the next row if it would straddle one. SPIRV-Cross
// also starts the member after a struct on a new row.
static uint32_t HLSLOffset(const Packing& p, uint32_t cursor, uint32_t previousAlignment) {
    uint32_t alignment = p.alignment;
    if (p.size > 0 && cursor / 16 != (cursor + p.size - 1) / 16) alignment = 16;
    if (previousAlignment > alignment) alignment = previousAlignment;
    return (cursor + alignment - 1) & ~(alignment - 1);
}

// Plans the padding that makes HLSL packing match the struct's offsets.
// Returns false when the struct needs none or can't be matched.
static bool PlanStruct(const Module& m, uint32_t type, uint32_t padType, Plan* plan) {
    uint32_t count = MemberCount(m, type);
    plan->newIndex.assign(count, 0);
    plan->padType = padType;
    uint32_t cursor = 0, previousAlignment = 1;
    auto padTo = [&](uint32_t offset) {
        for (; cursor < offset; cursor += 4) {
            plan->members.push_back(-1);
            plan->padOffsets.push_back(cursor);
        }
    };
    for (uint32_t i = 0; i < count; i++) {
        auto offset = m.memberOffsets.find(MemberKey(type, i));
        Packing p;
        if (offset == m.memberOffsets.end() ||
            !PackType(m, Operand(m, type, 2 + i), m.rowMajorMembers.count(MemberKey(type, i)) != 0, &p))
            return false;
        uint32_t expected = HLSLOffset(p, cursor, previousAlignment);
        if (expected < offset->second) {
            cursor = (cursor + previousAlignment - 1) & ~(previousAlignment - 1);
            padTo(offset->second);
            expected = HLSLOffset(p, cursor, 1);
        }
        if (expected != offset->second) return false;
        plan->newIndex[i] = uint32_t(plan->members.size());
        plan->members.push_back(int32_t(i));
        cursor = offset->second + p.size;
        previousAlignment = p.isStruct ? 16 : 1;
    }
    padTo((cursor + 15) & ~15u);
    return !plan->padOffsets.empty() && padType != 0;
}

static void AppendName(std::vector<uint32_t>& out, uint32_t type, uint32_t member, const std::string& name) {
    out.push_back(uint32_t(3 + name.size() / 4 + 1) << 16 | spv::OpMemberName);
    out.push_back(type);
    out.push_back(member);
    for (size_t i = 0; i <= name.size(); i += 4) {
        uint32_t word = 0;
        for (size_t b = 0; b < 4 && i + b < name.size(); b++)
            word |= uint32_t(uint8_t(name[i + b])) << (8 * b);
        out.push_back(word);
    }
}

static std::vector<uint32_t> RewriteUniformStructsForHLSL(const std::vector<uint32_t>& words) {
    if (words.size() < 5) return words;
    Module m{words, {}, {}, {}, {}, {}};
    std::unordered_map<uint32_t, uint32_t> padTypes; // struct id -> a 32-bit scalar declared before it
    std::vector<uint32_t> uniformPointers;
    uint32_t floatType = 0, intType = 0;
    for (size_t offset = 5; offset < words.size();) {
        uint16_t op = uint16_t(words[offset] & 0xffffu);
        uint16_t count = uint16_t(words[offset] >> 16);
        if (count == 0 || offset + count > words.size()) return words;
        bool hasResult = false, hasType = false;
        spv::HasResultAndType(spv::Op(op), &hasResult, &hasType);
        if (hasResult && !hasType) m.defs[words[offset + 1]] = offset;
        if (hasResult && hasType) {
            m.resultTypes[words[offset + 2]] = words[offset + 1];
            if (op == spv::OpConstant) m.defs[words[offset + 2]] = offset;
        }
        if (op == spv::OpTypeFloat && words[offset + 2] == 32 && !floatType) floatType = words[offset + 1];
        if (op == spv::OpTypeInt && words[offset + 2] == 32 && !intType) intType = words[offset + 1];
        if (op == spv::OpTypeStruct) padTypes[words[offset + 1]] = floatType ? floatType : intType;
        if (op == spv::OpDecorate && words[offset + 2] == spv::DecorationBlock) m.blocks.insert(words[offset + 1]);
        if (op == spv::OpMemberDecorate && count >= 5 && words[offset + 3] == spv::DecorationOffset)
            m.memberOffsets[MemberKey(words[offset + 1], words[offset + 2])] = words[offset + 4];
        if (op == spv::OpMemberDecorate && words[offset + 3] == spv::DecorationRowMajor)
            m.rowMajorMembers.insert(MemberKey(words[offset + 1], words[offset + 2]));
        if (op == spv::OpVariable && (words[offset + 3] == spv::StorageClassUniform ||
                                      words[offset + 3] == spv::StorageClassPushConstant))
            uniformPointers.push_back(words[offset + 1]);
        offset += count;
    }

    // The structs a uniform reaches, below its block, whose top-level members
    // packoffset places.
    std::vector<uint32_t> pending;
    for (uint32_t pointer : uniformPointers)
        if (TypeOp(m, pointer) == spv::OpTypePointer && m.blocks.count(Operand(m, pointer, 3)))
            pending.push_back(Operand(m, pointer, 3));
    std::unordered_set<uint32_t> reached;
    while (!pending.empty()) {
        uint32_t type = pending.back();
        pending.pop_back();
        uint16_t op = TypeOp(m, type);
        if (op == spv::OpTypeArray || op == spv::OpTypeRuntimeArray) {
            pending.push_back(Operand(m, type, 2));
        } else if (op == spv::OpTypeStruct && reached.insert(type).second) {
            for (uint32_t i = 0; i < MemberCount(m, type); i++) pending.push_back(Operand(m, type, 2 + i));
        }
    }
    std::unordered_map<uint32_t, Plan> plans;
    for (uint32_t type : reached) {
        Plan plan;
        if (!m.blocks.count(type) && PlanStruct(m, type, padTypes[type], &plan)) plans[type] = std::move(plan);
    }
    if (plans.empty()) return words;

    // Renumbered access chain indices are new constants, declared ahead of
    // the functions.
    uint32_t nextId = words[3];
    std::unordered_map<uint64_t, uint32_t> indexConstants;
    for (const auto& [id, offset] : m.defs)
        if (OpAt(m, offset) == spv::OpConstant && TypeOp(m, words[offset + 1]) == spv::OpTypeInt)
            indexConstants.emplace(MemberKey(words[offset + 1], words[offset + 3]), id);
    std::vector<uint32_t> newConstants;
    auto indexConstant = [&](uint32_t intTypeId, uint32_t value) {
        auto [it, added] = indexConstants.emplace(MemberKey(intTypeId, value), nextId);
        if (added) {
            newConstants.insert(newConstants.end(), {(4u << 16) | spv::OpConstant, intTypeId, nextId, value});
            nextId++;
        }
        return it->second;
    };

    std::vector<uint32_t> out(words.begin(), words.begin() + 5);
    out.reserve(words.size() + 256);
    size_t functionsStart = 0;
    std::unordered_set<uint32_t> decorated, named;
    for (size_t offset = 5; offset < words.size();) {
        uint16_t op = uint16_t(words[offset] & 0xffffu);
        uint16_t count = uint16_t(words[offset] >> 16);
        const uint32_t* w = &words[offset];
        offset += count;

        if (op == spv::OpFunction && functionsStart == 0) functionsStart = out.size();

        auto plan = plans.end();
        if (op == spv::OpMemberName || op == spv::OpMemberDecorate || op == spv::OpTypeStruct ||
            op == spv::OpCompositeConstruct || op == spv::OpConstantComposite ||
            op == spv::OpSpecConstantComposite)
            plan = plans.find(w[1]);

        if (plan != plans.end() && (op == spv::OpMemberName || op == spv::OpMemberDecorate)) {
            const Plan& p = plan->second;
            if (op == spv::OpMemberName && named.insert(w[1]).second) {
                for (size_t i = 0, k = 0; i < p.members.size(); i++)
                    if (p.members[i] < 0) AppendName(out, w[1], uint32_t(i), "bwsl_pad" + std::to_string(k++));
            }
            if (op == spv::OpMemberDecorate && decorated.insert(w[1]).second) {
                for (size_t i = 0, k = 0; i < p.members.size(); i++)
                    if (p.members[i] < 0)
                        out.insert(out.end(), {(5u << 16) | spv::OpMemberDecorate, w[1], uint32_t(i),
                                               uint32_t(spv::DecorationOffset), p.padOffsets[k++]});
            }
            out.insert(out.end(), w, w + count);
            out[out.size() - count + 2] = p.newIndex[w[2]];
            continue;
        }

        if (plan != plans.end() && op == spv::OpTypeStruct) {
            const Plan& p = plan->second;
            out.push_back(uint32_t(2 + p.members.size()) << 16 | spv::OpTypeStruct);
            out.push_back(w[1]);
            for (int32_t member : p.members) out.push_back(member < 0 ? p.padType : w[2 + member]);
            continue;
        }

        if (plan != plans.end()) {
            // A constructed struct value gets undefined padding.
            const Plan& p = plan->second;
            uint32_t pad = nextId++;
            out.insert(out.end(), {(3u << 16) | (op == spv::OpCompositeConstruct ? spv::OpUndef
                                                                                 : spv::OpConstantNull),
                                   p.padType, pad});
            out.push_back(uint32_t(3 + p.members.size()) << 16 | op);
            out.push_back(w[1]);
            out.push_back(w[2]);
            for (int32_t member : p.members) out.push_back(member < 0 ? pad : w[3 + member]);
            continue;
        }

        bool chain = op == spv::OpAccessChain || op == spv::OpInBoundsAccessChain ||
                     op == spv::OpPtrAccessChain || op == spv::OpInBoundsPtrAccessChain;
        if (chain || op == spv::OpCompositeExtract || op == spv::OpCompositeInsert) {
            // Walk the indices from the base type, renumbering each step into
            // a padded struct.
            uint32_t type = 0, first = 0;
            if (chain) {
                auto pointer = m.resultTypes.find(w[3]);
                if (pointer != m.resultTypes.end() && TypeOp(m, pointer->second) == spv::OpTypePointer)
                    type = Operand(m, pointer->second, 3);
                first = op == spv::OpPtrAccessChain || op == spv::OpInBoundsPtrAccessChain ? 5 : 4;
            } else if (op == spv::OpCompositeExtract) {
                auto composite = m.resultTypes.find(w[3]);
                if (composite != m.resultTypes.end()) type = composite->second;
                first = 4;
            } else {
                type = w[1];
                first = 5;
            }
            size_t start = out.size();
            out.insert(out.end(), w, w + count);
            for (uint32_t i = first; i < count && type; i++) {
                uint32_t member = w[i];
                if (chain && TypeOp(m, type) == spv::OpTypeStruct && !ConstantValue(m, w[i], &member)) break;
                auto padded = plans.find(type);
                if (padded != plans.end() && member < padded->second.newIndex.size()) {
                    uint32_t renumbered = padded->second.newIndex[member];
                    out[start + i] = chain ? indexConstant(m.resultTypes.at(w[i]), renumbered) : renumbered;
                }
                type = StepInto(m, type, member);
            }
            continue;
        }

        out.insert(out.end(), w, w + count);
    }
    if (functionsStart == 0) return words;
    out.insert(out.begin() + functionsStart, newConstants.begin(), newConstants.end());
    out[3] = nextId;
    return out;
}

} // namespace hlsl_cbuffer

std::string CompileToMSL(const std::vector<uint32_t>& spirv) {
#ifdef SPIRV_CROSS_EXCEPTIONS_TO_ASSERTIONS
    // No exceptions - just compile (will abort on error)
    spirv_cross::CompilerMSL compiler(spirv);
    spirv_cross::CompilerMSL::Options mslOpts;
    mslOpts.platform = spirv_cross::CompilerMSL::Options::macOS;
    mslOpts.msl_version = RequiredMSLVersion(compiler);
    compiler.set_msl_options(mslOpts);
    EnableLineDirectives(compiler, spirv);
    if (auto error = MetalResourceBindingError(compiler); !error.empty()) return error;

    // Preserve SPIR-V binding indices in Metal output
    auto entry = compiler.get_entry_points_and_stages()[0];
    preserveBindingIndices(compiler, entry.execution_model);

    return CollapseLineDirectives(compiler.compile());
#else
    try {
        spirv_cross::CompilerMSL compiler(spirv);
        spirv_cross::CompilerMSL::Options mslOpts;
        mslOpts.platform = spirv_cross::CompilerMSL::Options::macOS;
        mslOpts.msl_version = RequiredMSLVersion(compiler);
        compiler.set_msl_options(mslOpts);
        EnableLineDirectives(compiler, spirv);
        if (auto error = MetalResourceBindingError(compiler); !error.empty()) return error;

        // Preserve SPIR-V binding indices in Metal output
        auto entry = compiler.get_entry_points_and_stages()[0];
        preserveBindingIndices(compiler, entry.execution_model);

        return CollapseLineDirectives(compiler.compile());
    } catch (const spirv_cross::CompilerError& e) {
        return std::string("error: ") + e.what();
    } catch (const std::exception& e) {
        return std::string("error: ") + e.what();
    }
#endif
}

std::string CompileToHLSL(const std::vector<uint32_t>& spirv, int shaderModel) {
#ifdef SPIRV_CROSS_EXCEPTIONS_TO_ASSERTIONS
    auto words = hlsl_cbuffer::RewriteUniformStructsForHLSL(spirv);
    spirv_cross::CompilerHLSL compiler(words);
    spirv_cross::CompilerHLSL::Options hlslOpts;
    auto numWorkgroups = compiler.remap_num_workgroups_builtin();
    if (numWorkgroups) {
        compiler.set_name(numWorkgroups, "bwsl_num_workgroups");
        compiler.set_decoration(numWorkgroups, spv::DecorationDescriptorSet, 3);
        compiler.set_decoration(numWorkgroups, spv::DecorationBinding, 0);
    }
    hlslOpts.shader_model = numWorkgroups && shaderModel < 51 ? 51 : shaderModel;
    compiler.set_hlsl_options(hlslOpts);
    EnableLineDirectives(compiler, words);
    return CollapseLineDirectives(compiler.compile());
#else
    try {
        auto words = hlsl_cbuffer::RewriteUniformStructsForHLSL(spirv);
        spirv_cross::CompilerHLSL compiler(words);
        spirv_cross::CompilerHLSL::Options hlslOpts;
        auto numWorkgroups = compiler.remap_num_workgroups_builtin();
        if (numWorkgroups) {
            compiler.set_name(numWorkgroups, "bwsl_num_workgroups");
            compiler.set_decoration(numWorkgroups, spv::DecorationDescriptorSet, 3);
            compiler.set_decoration(numWorkgroups, spv::DecorationBinding, 0);
        }
        hlslOpts.shader_model = numWorkgroups && shaderModel < 51 ? 51 : shaderModel;
        compiler.set_hlsl_options(hlslOpts);
        EnableLineDirectives(compiler, words);
        return CollapseLineDirectives(compiler.compile());
    } catch (const spirv_cross::CompilerError& e) {
        return std::string("error: ") + e.what();
    } catch (const std::exception& e) {
        return std::string("error: ") + e.what();
    }
#endif
}

// Returns non-empty error string if the SPIR-V module uses features that
// require GLSL ES 3.10+ when the target is below 310.
static std::string CheckGLSLESCompat(spirv_cross::CompilerGLSL& compiler,
                                     int glslVersion, bool es) {
    if (!es || glslVersion >= 310) return {};
    auto resources = compiler.get_shader_resources();
    if (!resources.storage_buffers.empty()) {
        return "error: GLSL ES " + std::to_string(glslVersion) +
               " does not support storage buffers (requires ES 310+)";
    }
    if (!resources.storage_images.empty()) {
        return "error: GLSL ES " + std::to_string(glslVersion) +
               " does not support storage images (requires ES 310+)";
    }
    for (const auto& ep : compiler.get_entry_points_and_stages()) {
        if (ep.execution_model == spv::ExecutionModelGLCompute) {
            return "error: GLSL ES " + std::to_string(glslVersion) +
                   " does not support compute shaders (requires ES 310+)";
        }
    }
    return {};
}

// Inspects emitted GLSL ES source for builtins that require ES 310+.
static std::string CheckGLSLESEmittedCompat(const std::string& source,
                                            int glslVersion, bool es) {
    if (!es || glslVersion >= 310) return {};
    static const char* es310_only[] = {
        "findMSB(", "findLSB(", "bitCount(", "bitfieldReverse(",
        "bitfieldExtract(", "bitfieldInsert(",
        "uaddCarry(", "usubBorrow(", "umulExtended(", "imulExtended(",
        "ldexp(",
    };
    for (const char* kw : es310_only) {
        if (source.find(kw) != std::string::npos) {
            return std::string("error: GLSL ES ") + std::to_string(glslVersion) +
                   " does not support builtin '" + kw + ")' (requires ES 310+)";
        }
    }
    return {};
}

static bool IsGLSLIdentChar(char c) {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           c == '_';
}

static bool ReplaceFunctionCalls(std::string& source, const char* from, const char* to) {
    bool replaced = false;
    size_t pos = 0;
    const size_t fromLen = std::char_traits<char>::length(from);
    const size_t toLen = std::char_traits<char>::length(to);
    while ((pos = source.find(from, pos)) != std::string::npos) {
        if (pos > 0 && IsGLSLIdentChar(source[pos - 1])) {
            pos += fromLen;
            continue;
        }
        source.replace(pos, fromLen, to);
        pos += toLen;
        replaced = true;
    }
    return replaced;
}

static size_t GLSLESPolyfillInsertionPoint(const std::string& source) {
    size_t pos = 0;
    if (source.rfind("#version", 0) == 0) {
        size_t end = source.find('\n', pos);
        pos = (end == std::string::npos) ? source.size() : end + 1;
    }

    while (pos < source.size()) {
        size_t end = source.find('\n', pos);
        size_t lineEnd = (end == std::string::npos) ? source.size() : end;
        std::string_view line(source.data() + pos, lineEnd - pos);
        if (line.empty() ||
            line.rfind("#", 0) == 0 ||
            line.rfind("precision ", 0) == 0) {
            pos = (end == std::string::npos) ? source.size() : end + 1;
            continue;
        }
        break;
    }
    return pos;
}

static void PatchGLSLES300Packing4x8Builtins(std::string& source,
                                             int glslVersion,
                                             bool es) {
    if (!es || glslVersion >= 310) return;

    bool needsPolyfill = false;
    needsPolyfill |= ReplaceFunctionCalls(source, "unpackUnorm4x8(", "bwsl_unpackUnorm4x8(");
    needsPolyfill |= ReplaceFunctionCalls(source, "unpackSnorm4x8(", "bwsl_unpackSnorm4x8(");
    needsPolyfill |= ReplaceFunctionCalls(source, "packUnorm4x8(", "bwsl_packUnorm4x8(");
    needsPolyfill |= ReplaceFunctionCalls(source, "packSnorm4x8(", "bwsl_packSnorm4x8(");
    if (!needsPolyfill) return;

    static const char* polyfill =
        "uint bwsl_packUnorm4x8(vec4 v) {\n"
        "    uvec4 u = uvec4(round(clamp(v, vec4(0.0), vec4(1.0)) * 255.0));\n"
        "    return (u.x & 255u) | ((u.y & 255u) << 8) | ((u.z & 255u) << 16) | ((u.w & 255u) << 24);\n"
        "}\n\n"
        "vec4 bwsl_unpackUnorm4x8(uint p) {\n"
        "    uvec4 u = uvec4(p & 255u, (p >> 8) & 255u, (p >> 16) & 255u, (p >> 24) & 255u);\n"
        "    return vec4(u) / 255.0;\n"
        "}\n\n"
        "uint bwsl_packSnorm4x8(vec4 v) {\n"
        "    ivec4 i = ivec4(round(clamp(v, vec4(-1.0), vec4(1.0)) * 127.0));\n"
        "    uvec4 u = uvec4((i + ivec4(256)) & ivec4(255));\n"
        "    return (u.x & 255u) | ((u.y & 255u) << 8) | ((u.z & 255u) << 16) | ((u.w & 255u) << 24);\n"
        "}\n\n"
        "int bwsl_unpackSnorm8(uint v) {\n"
        "    int i = int(v & 255u);\n"
        "    return (i > 127) ? (i - 256) : i;\n"
        "}\n\n"
        "vec4 bwsl_unpackSnorm4x8(uint p) {\n"
        "    ivec4 i = ivec4(bwsl_unpackSnorm8(p), bwsl_unpackSnorm8(p >> 8), bwsl_unpackSnorm8(p >> 16), bwsl_unpackSnorm8(p >> 24));\n"
        "    return max(vec4(i) / 127.0, vec4(-1.0));\n"
        "}\n\n";
    source.insert(GLSLESPolyfillInsertionPoint(source), polyfill);
}

static void combineGLSLSamplers(spirv_cross::CompilerGLSL& compiler) {
    uint32_t dummy = compiler.build_dummy_sampler_for_combined_images();
    compiler.build_combined_image_samplers();
    for (const auto& pair : compiler.get_combined_image_samplers()) {
        std::string name = "bwsl_tex_" + std::to_string(compiler.get_decoration(pair.image_id, spv::DecorationDescriptorSet)) +
            "_" + std::to_string(compiler.get_decoration(pair.image_id, spv::DecorationBinding)) + "_sampler_";
        if (pair.sampler_id == dummy) name += "dummy";
        else name += std::to_string(compiler.get_decoration(pair.sampler_id, spv::DecorationDescriptorSet)) +
            "_" + std::to_string(compiler.get_decoration(pair.sampler_id, spv::DecorationBinding));
        compiler.set_name(pair.combined_id, name);
    }
}

// SPIRV-Cross turns an array variable that is only ever assigned a constant
// into a lookup table: a module-scope `const` array, indexed directly. Drivers
// handle dynamic indexing into `const` arrays badly (NVIDIA copies the whole
// array into local memory on every invocation, see issue #151), while a plain
// initialized array is cheap. This keeps such variables as ordinary arrays.
class GLSLCompiler : public spirv_cross::CompilerGLSL {
public:
    using spirv_cross::CompilerGLSL::CompilerGLSL;

protected:
    // Runs after SPIRV-Cross's analysis (which finds the lookup tables) and
    // before anything is emitted, on every compile pass.
    void emit_header() override {
        ir.for_each_typed_id<spirv_cross::SPIRVariable>([&](uint32_t, spirv_cross::SPIRVariable& var) {
            if (!variable_is_lut(var)) return;
            get<spirv_cross::SPIRConstant>(var.static_expression).is_used_as_lut = false;
            var.static_expression = 0;
            var.statically_assigned = false;
            var.remapped_variable = false;
        });
        spirv_cross::CompilerGLSL::emit_header();
    }
};

std::string CompileToGLSL(const std::vector<uint32_t>& spirv, int glslVersion, bool es) {
#ifdef SPIRV_CROSS_EXCEPTIONS_TO_ASSERTIONS
    if (auto err = CheckGLSLESSPIRVCompatRaw(spirv, glslVersion, es); !err.empty()) {
        return err;
    }
    GLSLCompiler compiler(spirv);
    if (auto err = CheckGLSLESCompat(compiler, glslVersion, es); !err.empty()) {
        return err;
    }
    spirv_cross::CompilerGLSL::Options glslOpts;
    glslOpts.version = glslVersion;
    glslOpts.es = es;
    glslOpts.vulkan_semantics = false;
    glslOpts.separate_shader_objects = true;
    // ES fragment shaders otherwise default to mediump float while vertex
    // shaders default to highp; drivers that resolve varying precision from
    // the stage default (Intel iGPUs) then reject or truncate the interface.
    glslOpts.fragment.default_float_precision = spirv_cross::CompilerGLSL::Options::Highp;
    glslOpts.fragment.default_int_precision = spirv_cross::CompilerGLSL::Options::Highp;
    glslOpts.emit_line_directives = HasLineInfo(spirv);
    compiler.set_common_options(glslOpts);
    combineGLSLSamplers(compiler);
        std::string result = compiler.compile();
    PatchGLSLES300Packing4x8Builtins(result, glslVersion, es);
    result = CollapseLineDirectives(std::move(result));
    UseStandardGLSLLineDirectives(result);
    if (auto err = CheckGLSLESEmittedCompat(result, glslVersion, es); !err.empty()) {
        return err;
    }
    return result;
#else
    try {
        if (auto err = CheckGLSLESSPIRVCompatRaw(spirv, glslVersion, es); !err.empty()) {
            return err;
        }
        GLSLCompiler compiler(spirv);
        if (auto err = CheckGLSLESCompat(compiler, glslVersion, es); !err.empty()) {
            return err;
        }
        spirv_cross::CompilerGLSL::Options glslOpts;
        glslOpts.version = glslVersion;
        glslOpts.es = es;
        glslOpts.vulkan_semantics = false;
        glslOpts.separate_shader_objects = true;
        // ES fragment shaders otherwise default to mediump float while vertex
        // shaders default to highp; drivers that resolve varying precision from
        // the stage default (Intel iGPUs) then reject or truncate the interface.
        glslOpts.fragment.default_float_precision = spirv_cross::CompilerGLSL::Options::Highp;
        glslOpts.fragment.default_int_precision = spirv_cross::CompilerGLSL::Options::Highp;
        glslOpts.emit_line_directives = HasLineInfo(spirv);
        compiler.set_common_options(glslOpts);
        combineGLSLSamplers(compiler);
        std::string result = compiler.compile();
        PatchGLSLES300Packing4x8Builtins(result, glslVersion, es);
        result = CollapseLineDirectives(std::move(result));
        UseStandardGLSLLineDirectives(result);
        if (auto err = CheckGLSLESEmittedCompat(result, glslVersion, es); !err.empty()) {
            return err;
        }
        return result;
    } catch (const spirv_cross::CompilerError& e) {
        return std::string("error: ") + e.what();
    } catch (const std::exception& e) {
        return std::string("error: ") + e.what();
    }
#endif
}

} // namespace spirv_cross_wrapper
