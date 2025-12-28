#pragma once

#include "bwsl_defs.h"
#include <cmath>
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
            constexpr u32 HashStr(const char* str) {
                u32 hash = 2166136261u;
                while (*str) {
                    hash ^= static_cast<u8>(*str++);
                    hash *= 16777619u;
                }
                return hash;
            }

            constexpr u32 HashStr(const char* str, u16 length) {
                u32 hash = 2166136261u;
                for (u16 i = 0; i < length; i++) {
                    hash ^= static_cast<u8>(str[i]);
                    hash *= 16777619u;
                }
                return hash;
            }

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
    // Static table for common built-in strings
    static constexpr struct { u32 hash; const char* str; } BUILTIN_STRINGS[] = {
        {Utils::HashStr("position"), "position"},
        {Utils::HashStr("normal"), "normal"},
        {Utils::HashStr("texcoord"), "texcoord"},
        {Utils::HashStr("tangent"), "tangent"},
        {Utils::HashStr("bitangent"), "bitangent"},
        {Utils::HashStr("color"), "color"},
        {Utils::HashStr("boneIndices"), "boneIndices"},
        {Utils::HashStr("boneWeights"), "boneWeights"},
        {Utils::HashStr("resources"), "resources"},
        {Utils::HashStr("attributes"), "attributes"},
        {Utils::HashStr("output"), "output"},
        {Utils::HashStr("<global>"), "<global>"},
        {Utils::HashStr("import"), "import"},
        {Utils::HashStr("pipeline"), "pipeline"},
        {Utils::HashStr("compressed"), "compressed"},
        {Utils::HashStr("instance"), "instance"},
        {Utils::HashStr("optional"), "optional"}

    };

    // Dynamic string table for runtime-registered strings (e.g., module/struct names)
    inline std::unordered_map<u32, std::string>& GetDynamicTable() {
        static std::unordered_map<u32, std::string> table;
        return table;
    }

    // Register a string for later reverse lookup by hash
    inline void Register(u32 hash, const char* str) {
        GetDynamicTable()[hash] = str;
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