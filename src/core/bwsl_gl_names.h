#pragma once
// Names the text backends give interface objects, and the check that keeps
// them unique.
//
// GL ES 3.00 and GL 4.1 have no layout(binding), so hosts look uniform blocks
// and samplers up by name, and ES 3.00 links varyings by name. These names are
// therefore part of the output's API: the SPIR-V backend emits them as OpName
// (SPIRV-Cross carries them into GLSL, Metal and HLSL), the direct GLES
// emitter writes them itself, and bindings.json reports them as glName.
//
// Each kind of generated name has its own prefix, and no prefix starts
// another, so two generated names of different kinds can't be equal. GLSL
// puts block names, struct type names and global variables in one namespace,
// though, and struct and sampler names are emitted as written: a struct named
// `t_atlas` would clash with texture `atlas`. CollectPipelineNames gathers the
// pipeline-level names and FindNameCollision reports the first clash;
// varyings are checked against the same list when lowering first writes
// them.
#include "core/bwsl_ast_soa.h"
#include "core/bwsl_symbol_table.h"
#include <string>
#include <string_view>
#include <vector>

namespace BWSL {

struct GLReservedName {
    std::string name;   // Identifier as emitted
    std::string owner;  // What produces it, for diagnostics
    u32 position = 0;   // Packed AST line/column of the declaration, 0 if unknown
};

namespace GLNames {

//   uniform ub_<resource> { T <resource>; } bwsl_ub_<resource>;
// The block name is what the host passes to glGetUniformBlockIndex; the
// instance name only scopes the member inside the shader.
inline std::string UniformBlockName(std::string_view resource) {
    return "ub_" + std::string(resource);
}
inline std::string UniformInstanceName(std::string_view resource) {
    return "bwsl_ub_" + std::string(resource);
}
// A texture used one-to-one with its sampler stays a combined image.
inline std::string CombinedTextureName(std::string_view texture) {
    return "t_" + std::string(texture);
}
// Separate sampler synthesized for a texture sampled without one (Metal/HLSL).
inline std::string DefaultSamplerName(std::string_view texture) {
    return "bwsl_s_" + std::string(texture);
}
inline std::string AttributeName(std::string_view attribute) {
    return "a_" + std::string(attribute);
}
inline std::string VaryingName(std::string_view varying) {
    return "v_" + std::string(varying);
}

inline const GLReservedName* FindName(const std::vector<GLReservedName>& names,
                                      std::string_view name) {
    for (const GLReservedName& entry : names) {
        if (entry.name == name) return &entry;
    }
    return nullptr;
}

// Gathers the names the pipeline's own resources, attributes and structs
// produce. Sampler and struct names are emitted as written.
inline void CollectPipelineNames(const AST& ast, const PipelineData& pipeline,
                                 const SymbolTableData& symbols,
                                 const char* sourceBase,
                                 std::vector<GLReservedName>* out) {
    auto add = [&](std::string name, std::string owner, NodeRef decl) {
        u32 position = ast.GetNamePosition(decl);
        if (position == 0) position = ast.FindPosition(decl);
        out->push_back({std::move(name), std::move(owner), position});
    };
    auto quoted = [](const char* kind, const std::string& name) {
        return std::string(kind) + " '" + name + "'";
    };

    for (u32 i = 0; i < pipeline.resources.count; i++) {
        NodeRef ref = pipeline.resources[i];
        if (ref.Type() != ASTNodeType::RESOURCE_DECL) continue;
        const ResourceDeclData& decl = ast.GetResourceDecl(ref);
        const Symbol* sym = SymbolTable::LookupResource(
            const_cast<SymbolTableData*>(&symbols), decl.name);
        if (!sym || sym->index >= symbols.resources.count) continue;
        std::string name = decl.name.ToString(sourceBase);
        switch (symbols.resources[sym->index].type) {
            case ResourceBinding::UniformBuffer:
                add(UniformBlockName(name), "the uniform block of " + quoted("resource", name), ref);
                add(UniformInstanceName(name), "the uniform block instance of " + quoted("resource", name), ref);
                break;
            case ResourceBinding::Texture:
                add(CombinedTextureName(name), quoted("texture", name), ref);
                add(DefaultSamplerName(name), "the default sampler of " + quoted("texture", name), ref);
                break;
            case ResourceBinding::Sampler:
                add(name, quoted("sampler", name), ref);
                break;
            default:
                break;
        }
    }

    for (u32 i = 0; i < pipeline.attributes.count; i++) {
        NodeRef ref = pipeline.attributes[i];
        if (ref.Type() != ASTNodeType::ATTRIBUTE_DECL) continue;
        std::string name = ast.GetAttributeDecl(ref).name.ToString(sourceBase);
        add(AttributeName(name), quoted("attribute", name), ref);
    }

    for (u32 i = 0; i < pipeline.structs.count; i++) {
        NodeRef ref = pipeline.structs[i];
        if (ref.Type() != ASTNodeType::STRUCT_DECL) continue;
        std::string name = ast.GetStructDecl(ref).name.ToString(sourceBase);
        add(name, quoted("struct", name), ref);
    }
}

// Returns the index of the first entry whose name an earlier entry already
// produces, or -1. *earlier receives that earlier entry's index.
inline s32 FindNameCollision(const std::vector<GLReservedName>& names, u32* earlier) {
    for (u32 i = 1; i < names.size(); i++) {
        for (u32 j = 0; j < i; j++) {
            if (names[i].name == names[j].name) {
                *earlier = j;
                return static_cast<s32>(i);
            }
        }
    }
    return -1;
}

// "GL name 't_atlas' of sampler 't_atlas' collides with texture 'atlas';
// rename one of them"
inline std::string DescribeCollision(const std::string& name, const std::string& owner,
                                     const GLReservedName& earlier) {
    return "GL name '" + name + "' of " + owner + " collides with " + earlier.owner +
           "; rename one of them";
}

// Collects the pipeline-level names into *names. On a collision, returns
// false with the message and the packed position of the later declaration.
inline bool CheckPipelineNames(const AST& ast, const PipelineData& pipeline,
                               const SymbolTableData& symbols, const char* sourceBase,
                               std::vector<GLReservedName>* names,
                               std::string* error, u32* position) {
    names->clear();
    CollectPipelineNames(ast, pipeline, symbols, sourceBase, names);
    u32 earlier = 0;
    s32 clash = FindNameCollision(*names, &earlier);
    if (clash < 0) return true;
    const GLReservedName& entry = (*names)[static_cast<u32>(clash)];
    *error = DescribeCollision(entry.name, entry.owner, (*names)[earlier]);
    *position = entry.position;
    return false;
}

} // namespace GLNames
} // namespace BWSL
