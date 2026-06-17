#pragma once

#include "bwsl_defs.h"
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>

namespace BWSL{
    struct SourceLocation {
        u32 line;
        u32 column;
    };

    // Precomputed line offsets for O(1) source location lookup
    struct LineTable {
        std::vector<u32> lineStarts;  // lineStarts[i] = offset where line i+1 begins

        // Build line table in single pass over source
        void Build(const char* source, u32 length) {
            lineStarts.clear();
            lineStarts.reserve(length / 40);  // Estimate ~40 chars per line
            lineStarts.push_back(0);  // Line 1 starts at offset 0

            for (u32 i = 0; i < length; i++) {
                if (source[i] == '\n') {
                    lineStarts.push_back(i + 1);  // Next line starts after newline
                }
            }
        }

        // O(log n) lookup using binary search
        SourceLocation Get(u32 offset) const {
            if (lineStarts.empty()) return {1, offset + 1};

            // Find the line: upper_bound gives first line starting AFTER offset
            auto it = std::upper_bound(lineStarts.begin(), lineStarts.end(), offset);
            u32 lineIndex = static_cast<u32>(it - lineStarts.begin());

            // lineIndex is 1-based line number (since upper_bound gives us the count of lines before)
            u32 lineStart = lineStarts[lineIndex - 1];
            u32 column = offset - lineStart + 1;

            return {lineIndex, column};
        }
    };

    namespace Utils{

            // FNV-1a hash
            constexpr u32 FNV_OFFSET_BASIS = 2166136261u;
            constexpr u32 FNV_PRIME = 16777619u;

            constexpr u32 HashMix(u32 hash, u32 value) {
                hash ^= value;
                hash *= FNV_PRIME;
                return hash;
            }

            inline u32 HashBytes(const void* buffer, u32 size) {
                const u8* bytes = static_cast<const u8*>(buffer);
                u32 hash = FNV_OFFSET_BASIS;
                for (u32 i = 0; i < size; i++) {
                    hash = HashMix(hash, bytes[i]);
                }
                return hash;
            }

            inline u32 HashWords(const u32* words, u32 count) {
                u32 hash = FNV_OFFSET_BASIS;
                for (u32 i = 0; i < count; i++) {
                    hash = HashMix(hash, words[i]);
                }
                return hash;
            }

            constexpr u32 HashStr(const char* str) {
                u32 hash = FNV_OFFSET_BASIS;
                while (*str) {
                    hash = HashMix(hash, static_cast<u8>(*str++));
                }
                return hash;
            }

            constexpr u32 HashStr(const char* str, u16 length) {
                u32 hash = FNV_OFFSET_BASIS;
                for (u16 i = 0; i < length; i++) {
                    hash = HashMix(hash, static_cast<u8>(str[i]));
                }
                return hash;
            }

            static_assert(HashStr("ArenaString") == HashStr("ArenaString", 11),
                          "HashStr(cstr) and HashStr(ptr,len) must match");

            inline u32 HashFloat(float value) {

                if (value == 0.0f) return 0;  // Handles -0.0f too
                if (std::isnan(value)) return 0x7fc00000;

                union { float f; u32 u; } conv;
                conv.f = value;


                u32 h = conv.u;
                h ^= h >> 16;
                h *= 0x85ebca6b; // https://en.wikipedia.org/wiki/MurmurHash
                h ^= h >> 13;
                h *= 0xc2b2ae35; // https://en.wikipedia.org/wiki/MurmurHash
                h ^= h >> 16;
                return h;
            }

            // Legacy: slow O(n) line/col calculation - prefer LineTable::Get()
            inline SourceLocation GetSourceLocation(const char* sourceBase, u32 offset) {
                u32 line = 1;
                u32 column = 1;

                for (u32 i = 0; i < offset; i++) {
                    if (sourceBase[i] == '\n') {
                        line++;
                        column = 1;
                    } else {
                        column++;
                    }
                }

                return {line, column};
            }

     } // End of Namespace Utils


    namespace ReverseLookup {
    // Static table for known compile-time hash literals. These reserve the
    // legacy FNV values so dynamic source identifiers that collide with them
    // are assigned a distinct intern id instead of stealing the literal id.
    static constexpr struct { u32 hash; const char* str; } BUILTIN_STRINGS[] = {
        {Utils::HashStr("10_10_10"), "10_10_10"},
        {Utils::HashStr("::"), "::"},
        {Utils::HashStr("<global>"), "<global>"},
        {Utils::HashStr("BWSLMath"), "BWSLMath"},
        {Utils::HashStr("BwslFrexpResult"), "BwslFrexpResult"},
        {Utils::HashStr("BwslModfResult"), "BwslModfResult"},
        {Utils::HashStr("PBR"), "PBR"},
        {Utils::HashStr("PostFx"), "PostFx"},
        {Utils::HashStr("T"), "T"},
        {Utils::HashStr("U"), "U"},
        {Utils::HashStr("V"), "V"},
        {Utils::HashStr("_"), "_"},
        {Utils::HashStr("a"), "a"},
        {Utils::HashStr("abs"), "abs"},
        {Utils::HashStr("ambientColor"), "ambientColor"},
        {Utils::HashStr("attributes"), "attributes"},
        {Utils::HashStr("b"), "b"},
        {Utils::HashStr("bitangent"), "bitangent"},
        {Utils::HashStr("boneIndices"), "boneIndices"},
        {Utils::HashStr("boneMatrices"), "boneMatrices"},
        {Utils::HashStr("boneWeights"), "boneWeights"},
        {Utils::HashStr("bool"), "bool"},
        {Utils::HashStr("bool2"), "bool2"},
        {Utils::HashStr("bool3"), "bool3"},
        {Utils::HashStr("bool4"), "bool4"},
        {Utils::HashStr("buffer"), "buffer"},
        {Utils::HashStr("cameraPosition"), "cameraPosition"},
        {Utils::HashStr("cbuffer"), "cbuffer"},
        {Utils::HashStr("ceil"), "ceil"},
        {Utils::HashStr("clamp"), "clamp"},
        {Utils::HashStr("color"), "color"},
        {Utils::HashStr("compressed"), "compressed"},
        {Utils::HashStr("constraint"), "constraint"},
        {Utils::HashStr("ddx"), "ddx"},
        {Utils::HashStr("ddy"), "ddy"},
        {Utils::HashStr("depth"), "depth"},
        {Utils::HashStr("dmat2"), "dmat2"},
        {Utils::HashStr("dmat3"), "dmat3"},
        {Utils::HashStr("dmat4"), "dmat4"},
        {Utils::HashStr("dot"), "dot"},
        {Utils::HashStr("double"), "double"},
        {Utils::HashStr("double2"), "double2"},
        {Utils::HashStr("double3"), "double3"},
        {Utils::HashStr("double4"), "double4"},
        {Utils::HashStr("enum"), "enum"},
        {Utils::HashStr("exponent"), "exponent"},
        {Utils::HashStr("flat"), "flat"},
        {Utils::HashStr("float"), "float"},
        {Utils::HashStr("float2"), "float2"},
        {Utils::HashStr("float2x2"), "float2x2"},
        {Utils::HashStr("float3"), "float3"},
        {Utils::HashStr("float3x3"), "float3x3"},
        {Utils::HashStr("float4"), "float4"},
        {Utils::HashStr("float4x4"), "float4x4"},
        {Utils::HashStr("floor"), "floor"},
        {Utils::HashStr("fraction"), "fraction"},
        {Utils::HashStr("g"), "g"},
        {Utils::HashStr("global_id"), "global_id"},
        {Utils::HashStr("has_"), "has_"},
        {Utils::HashStr("import"), "import"},
        {Utils::HashStr("input"), "input"},
        {Utils::HashStr("instance"), "instance"},
        {Utils::HashStr("instance_id"), "instance_id"},
        {Utils::HashStr("int"), "int"},
        {Utils::HashStr("int2"), "int2"},
        {Utils::HashStr("int3"), "int3"},
        {Utils::HashStr("int4"), "int4"},
        {Utils::HashStr("int64"), "int64"},
        {Utils::HashStr("int64x2"), "int64x2"},
        {Utils::HashStr("int64x3"), "int64x3"},
        {Utils::HashStr("int64x4"), "int64x4"},
        {Utils::HashStr("inverseProjMatrix"), "inverseProjMatrix"},
        {Utils::HashStr("inverseViewMatrix"), "inverseViewMatrix"},
        {Utils::HashStr("ivec2"), "ivec2"},
        {Utils::HashStr("ivec3"), "ivec3"},
        {Utils::HashStr("ivec4"), "ivec4"},
        {Utils::HashStr("length"), "length"},
        {Utils::HashStr("lerp"), "lerp"},
        {Utils::HashStr("lightColor"), "lightColor"},
        {Utils::HashStr("lightDirection"), "lightDirection"},
        {Utils::HashStr("lightPosition"), "lightPosition"},
        {Utils::HashStr("lightViewProjMatrix"), "lightViewProjMatrix"},
        {Utils::HashStr("local_id"), "local_id"},
        {Utils::HashStr("local_index"), "local_index"},
        {Utils::HashStr("mantissa"), "mantissa"},
        {Utils::HashStr("mat2"), "mat2"},
        {Utils::HashStr("mat3"), "mat3"},
        {Utils::HashStr("mat4"), "mat4"},
        {Utils::HashStr("max"), "max"},
        {Utils::HashStr("min"), "min"},
        {Utils::HashStr("modelMatrix"), "modelMatrix"},
        {Utils::HashStr("modelViewMatrix"), "modelViewMatrix"},
        {Utils::HashStr("modelViewProjectionMatrix"), "modelViewProjectionMatrix"},
        {Utils::HashStr("mvpMatrix"), "mvpMatrix"},
        {Utils::HashStr("noperspective"), "noperspective"},
        {Utils::HashStr("normal"), "normal"},
        {Utils::HashStr("normalMatrix"), "normalMatrix"},
        {Utils::HashStr("normalize"), "normalize"},
        {Utils::HashStr("num_workgroups"), "num_workgroups"},
        {Utils::HashStr("octahedral16"), "octahedral16"},
        {Utils::HashStr("output"), "output"},
        {Utils::HashStr("pipeline"), "pipeline"},
        {Utils::HashStr("position"), "position"},
        {Utils::HashStr("pow"), "pow"},
        {Utils::HashStr("previousModelMatrix"), "previousModelMatrix"},
        {Utils::HashStr("previousViewProjMatrix"), "previousViewProjMatrix"},
        {Utils::HashStr("projMatrix"), "projMatrix"},
        {Utils::HashStr("projectionMatrix"), "projectionMatrix"},
        {Utils::HashStr("r"), "r"},
        {Utils::HashStr("resources"), "resources"},
        {Utils::HashStr("rg"), "rg"},
        {Utils::HashStr("rgb"), "rgb"},
        {Utils::HashStr("sample"), "sample"},
        {Utils::HashStr("sampler"), "sampler"},
        {Utils::HashStr("saturate"), "saturate"},
        {Utils::HashStr("self"), "self"},
        {Utils::HashStr("sqrt"), "sqrt"},
        {Utils::HashStr("tag"), "tag"},
        {Utils::HashStr("tangent"), "tangent"},
        {Utils::HashStr("texcoord"), "texcoord"},
        {Utils::HashStr("texture2D"), "texture2D"},
        {Utils::HashStr("texture3D"), "texture3D"},
        {Utils::HashStr("textureCube"), "textureCube"},
        {Utils::HashStr("uint"), "uint"},
        {Utils::HashStr("uint2"), "uint2"},
        {Utils::HashStr("uint3"), "uint3"},
        {Utils::HashStr("uint4"), "uint4"},
        {Utils::HashStr("uint64"), "uint64"},
        {Utils::HashStr("uint64x2"), "uint64x2"},
        {Utils::HashStr("uint64x3"), "uint64x3"},
        {Utils::HashStr("uint64x4"), "uint64x4"},
        {Utils::HashStr("uint8888"), "uint8888"},
        {Utils::HashStr("unorm16_16"), "unorm16_16"},
        {Utils::HashStr("unorm8888"), "unorm8888"},
        {Utils::HashStr("vec2"), "vec2"},
        {Utils::HashStr("vec3"), "vec3"},
        {Utils::HashStr("vec4"), "vec4"},
        {Utils::HashStr("vertex_id"), "vertex_id"},
        {Utils::HashStr("viewMatrix"), "viewMatrix"},
        {Utils::HashStr("viewProjMatrix"), "viewProjMatrix"},
        {Utils::HashStr("viewProjectionMatrix"), "viewProjectionMatrix"},
        {Utils::HashStr("void"), "void"},
        {Utils::HashStr("w"), "w"},
        {Utils::HashStr("whole"), "whole"},
        {Utils::HashStr("workgroup_id"), "workgroup_id"},
        {Utils::HashStr("worldPosition"), "worldPosition"},
        {Utils::HashStr("x"), "x"},
        {Utils::HashStr("xy"), "xy"},
        {Utils::HashStr("xyz"), "xyz"},
        {Utils::HashStr("y"), "y"},
        {Utils::HashStr("z"), "z"}
    };

    // Dynamic string table for runtime-registered strings (e.g., module/struct names)
    inline std::unordered_map<u32, std::string>& GetDynamicTable() {
        static std::unordered_map<u32, std::string> table;
        return table;
    }

    inline std::unordered_map<u32, std::vector<u32>>& GetInternBuckets() {
        static std::unordered_map<u32, std::vector<u32>> buckets;
        return buckets;
    }

    inline bool TextEquals(const char* lhs, const char* rhs, u16 length) {
        return std::strlen(lhs) == length && std::memcmp(lhs, rhs, length) == 0;
    }

    inline const char* FindBuiltinString(u32 id) {
        for (const auto& entry : BUILTIN_STRINGS) {
            if (entry.hash == id) {
                return entry.str;
            }
        }
        return nullptr;
    }

    inline bool IdMatchesText(u32 id, const char* str, u16 length) {
        if (const char* builtin = FindBuiltinString(id)) {
            return TextEquals(builtin, str, length);
        }

        auto& dynTable = GetDynamicTable();
        auto it = dynTable.find(id);
        return it != dynTable.end() &&
               it->second.size() == length &&
               std::memcmp(it->second.data(), str, length) == 0;
    }

    inline bool IdOccupiedByDifferentText(u32 id, const char* str, u16 length) {
        if (const char* builtin = FindBuiltinString(id)) {
            return !TextEquals(builtin, str, length);
        }

        auto& dynTable = GetDynamicTable();
        auto it = dynTable.find(id);
        return it != dynTable.end() &&
               (it->second.size() != length ||
                std::memcmp(it->second.data(), str, length) != 0);
    }

    inline u32 AllocateCollisionId() {
        static u32 nextId = 0x80000000u;
        auto& dynTable = GetDynamicTable();
        while (nextId == 0 ||
               FindBuiltinString(nextId) != nullptr ||
               dynTable.find(nextId) != dynTable.end()) {
            nextId++;
        }
        return nextId++;
    }

    inline u32 InternWithHash(u32 hash, const char* str, u16 length) {
        auto& buckets = GetInternBuckets();
        auto& bucket = buckets[hash];
        for (u32 id : bucket) {
            if (IdMatchesText(id, str, length)) {
                return id;
            }
        }

        for (const auto& entry : BUILTIN_STRINGS) {
            if (entry.hash == hash && TextEquals(entry.str, str, length)) {
                return hash;
            }
        }

        u32 id = IdOccupiedByDifferentText(hash, str, length)
            ? AllocateCollisionId()
            : hash;
        GetDynamicTable()[id] = std::string(str, length);
        bucket.push_back(id);
        return id;
    }

    inline u32 Intern(const char* str, u16 length) {
        return InternWithHash(Utils::HashStr(str, length), str, length);
    }

    inline u32 Intern(const char* str) {
        const char* text = str ? str : "";
        size_t length = std::strlen(text);
        return Intern(text, static_cast<u16>(length));
    }

    inline u32 Intern(const std::string& str) {
        return Intern(str.data(), static_cast<u16>(str.size()));
    }

    // Register a string for later reverse lookup by hash
    inline void Register(u32 hash, const char* str) {
        const char* text = str ? str : "";
        InternWithHash(hash, text, static_cast<u16>(std::strlen(text)));
    }

    inline void Register(u32 hash, const char* str, u16 length) {
        InternWithHash(hash, str, length);
    }

    inline std::string GetString(u32 hash) {
        // Check builtin table first
        for (const auto& entry : BUILTIN_STRINGS) {
            if (entry.hash == hash) return entry.str;
        }

        // Check dynamic table
        auto& dynTable = GetDynamicTable();
        auto it = dynTable.find(hash);
        if (it != dynTable.end()) {
            return it->second;
        }

        // Fallback for unknown hashes (shouldn't happen often)
        char buf[32];
        snprintf(buf, sizeof(buf), "<hash:0x%08X>", hash);
        return buf;
    }
}


} // End of Namespace BWSL
