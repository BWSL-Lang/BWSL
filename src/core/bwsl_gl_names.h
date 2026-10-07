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
#include <vector>

namespace BWSL {

enum class GLNameKind : u8 {
    UNIFORM_BLOCK,     // uniform ub_<resource> { T bwsl_u_<resource>; } ...
    UNIFORM_INSTANCE,  // ... bwsl_ub_<resource>;
    UNIFORM_MEMBER,    // Members also need a prefix for GLSL reserved words.
    COMBINED_TEXTURE,  // texture used one-to-one with its sampler
    DEFAULT_SAMPLER,   // sampler synthesized for a texture sampled without one (Metal/HLSL)
    SAMPLER,
    ATTRIBUTE,
    VARYING,
    STRUCT,
    COUNT,
};

// Indexed by GLNameKind. The block name is what the host passes to
// glGetUniformBlockIndex; the instance name only scopes the member inside the
// shader. Sampler and struct names are emitted as written.
static constexpr struct {
    const char* prefix;
    const char* owner;  // Diagnostic text, followed by the source name
} GL_NAME_KINDS[] = {
    {"ub_",      "the uniform block of resource"},
    {"bwsl_ub_", "the uniform block instance of resource"},
    {"bwsl_u_",  "the uniform member of resource"},
    {"t_",       "texture"},
    {"bwsl_s_",  "the default sampler of texture"},
    {"",         "sampler"},
    {"a_",       "attribute"},
    {"v_",       "varying"},
    {"",         "struct"},
};
static_assert(sizeof(GL_NAME_KINDS) / sizeof(GL_NAME_KINDS[0]) ==
              static_cast<size_t>(GLNameKind::COUNT));

struct GLReservedName {
    ArenaString name;    // Identifier as emitted
    ArenaString source;  // Name of the declaration that produces it
    GLNameKind kind;
    u32 position = 0;    // Packed AST line/column of the declaration, 0 if unknown
};

namespace GLNames {

// The name a declaration called `source` is emitted under. Interned, so it
// compares equal (by nameHash) to a source identifier with the same spelling.
// Text is looked up by hash rather than through a source buffer: after
// pass-block instantiation a name can point into a module's source.
inline ArenaString MakeName(GLNameKind kind, ArenaString source) {
    const char* prefix = GL_NAME_KINDS[static_cast<u32>(kind)].prefix;
    if (prefix[0] == '\0') return source;
    return ArenaString::MakeHashOnly(prefix + source.ToString());
}

inline const GLReservedName* FindName(const std::vector<GLReservedName>& names,
                                      ArenaString name) {
    for (const GLReservedName& entry : names) {
        if (entry.name == name) return &entry;
    }
    return nullptr;
}

// Gathers the names the pipeline's own resources, attributes and structs
// produce.
inline void CollectPipelineNames(const AST& ast, const PipelineData& pipeline,
                                 const SymbolTableData& symbols,
                                 std::vector<GLReservedName>* out) {
    auto add = [&](GLNameKind kind, ArenaString source, NodeRef decl) {
        u32 position = ast.GetNamePosition(decl);
        if (position == 0) position = ast.FindPosition(decl);
        out->push_back({MakeName(kind, source), source, kind, position});
    };

    for (u32 i = 0; i < pipeline.resources.count; i++) {
        NodeRef ref = pipeline.resources[i];
        if (ref.Type() != ASTNodeType::RESOURCE_DECL) continue;
        const ResourceDeclData& decl = ast.GetResourceDecl(ref);
        const Symbol* sym = SymbolTable::LookupResource(
            const_cast<SymbolTableData*>(&symbols), decl.name);
        if (!sym || sym->index >= symbols.resources.count) continue;
        switch (symbols.resources[sym->index].type) {
            case ResourceBinding::UniformBuffer:
                add(GLNameKind::UNIFORM_BLOCK, decl.name, ref);
                add(GLNameKind::UNIFORM_INSTANCE, decl.name, ref);
                break;
            case ResourceBinding::Texture:
                add(GLNameKind::COMBINED_TEXTURE, decl.name, ref);
                add(GLNameKind::DEFAULT_SAMPLER, decl.name, ref);
                break;
            case ResourceBinding::Sampler:
                add(GLNameKind::SAMPLER, decl.name, ref);
                break;
            default:
                break;
        }
    }

    for (u32 i = 0; i < pipeline.attributes.count; i++) {
        NodeRef ref = pipeline.attributes[i];
        if (ref.Type() != ASTNodeType::ATTRIBUTE_DECL) continue;
        add(GLNameKind::ATTRIBUTE, ast.GetAttributeDecl(ref).name, ref);
    }

    for (u32 i = 0; i < pipeline.structs.count; i++) {
        NodeRef ref = pipeline.structs[i];
        if (ref.Type() != ASTNodeType::STRUCT_DECL) continue;
        add(GLNameKind::STRUCT, ast.GetStructDecl(ref).name, ref);
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

// "texture 'atlas'", "the uniform block of resource 'render'"
inline std::string DescribeOwner(const GLReservedName& entry) {
    return std::string(GL_NAME_KINDS[static_cast<u32>(entry.kind)].owner) + " '" +
           entry.source.ToString() + "'";
}

// "GL name 't_atlas' of sampler 't_atlas' collides with texture 'atlas';
// rename one of them"
inline std::string DescribeCollision(const GLReservedName& later,
                                     const GLReservedName& earlier) {
    return "GL name '" + later.name.ToString() + "' of " + DescribeOwner(later) +
           " collides with " + DescribeOwner(earlier) + "; rename one of them";
}

// Collects the pipeline-level names into *names. On a collision, returns
// false with the message and the packed position of the later declaration.
inline bool CheckPipelineNames(const AST& ast, const PipelineData& pipeline,
                               const SymbolTableData& symbols,
                               std::vector<GLReservedName>* names,
                               std::string* error, u32* position) {
    names->clear();
    CollectPipelineNames(ast, pipeline, symbols, names);
    u32 earlier = 0;
    s32 clash = FindNameCollision(*names, &earlier);
    if (clash < 0) return true;
    const GLReservedName& entry = (*names)[static_cast<u32>(clash)];
    *error = DescribeCollision(entry, (*names)[earlier]);
    *position = entry.position;
    return false;
}

} // namespace GLNames
} // namespace BWSL
