#pragma once

#include "bwsl_defs.h"
#include "bwsl_ir_gen.h"
#include "bwsl_utils.h"  // For Utils::HashStr
#include <bit>

namespace BWSL {

// IR Analysis results - populated by scanning the IR to determine
// what capabilities, inputs, outputs, and resources are actually used.
struct IRAnalysis {
    // Attribute usage (vertex inputs)
    u32 usedAttributeMask;        // Bit per attribute index (0-15)
    u32 usedOutputMask;           // Bit per output slot
    u32 usedInputMask;            // Bit per input slot (fragment varyings)
    u32 usedBuiltinInputMask;     // Bit per built-in input (vertex_id, instance_id)

    // Built-in input flags (for usedBuiltinInputMask)
    enum BuiltinInputFlags : u32 {
        BUILTIN_VERTEX_ID   = 1 << 0,   // input.vertex_id used
        BUILTIN_INSTANCE_ID = 1 << 1,   // input.instance_id used
        BUILTIN_FRAG_COORD  = 1 << 2,   // input.position (fragment) used
        GLOBAL_ID           = 1 << 3,
        LOCAL_ID            = 1 << 4,
        WORKGROUP_ID        = 1 << 10,
        NUM_WORKGROUPS      = 1 << 11,
        LOCAL_INDEX         = 1 << 12

    };

    // Built-in input helpers
    bool UsesVertexId()      const { return (usedBuiltinInputMask & BUILTIN_VERTEX_ID) != 0; }
    bool UsesInstanceId()    const { return (usedBuiltinInputMask & BUILTIN_INSTANCE_ID) != 0; }
    bool UsesFragCoord()     const { return (usedBuiltinInputMask & BUILTIN_FRAG_COORD) != 0; }
    bool UsesGlobalId()      const { return (usedBuiltinInputMask & GLOBAL_ID) != 0; }
    bool UsesLocalId()       const { return (usedBuiltinInputMask & LOCAL_ID) != 0; }
    bool UsesWorkgroupId()   const { return (usedBuiltinInputMask & WORKGROUP_ID) != 0; }
    bool UsesNumWorkgroups() const { return (usedBuiltinInputMask & NUM_WORKGROUPS) != 0; }
    bool UsesLocalIndex()    const { return (usedBuiltinInputMask & LOCAL_INDEX) != 0; }

    // Resource usage (indexed by binding)
    u32 usedUniformMask;          // Bit per uniform binding (0-31)
    u32 usedTextureMask;          // Bit per texture binding (sampled textures)
    u32 usedSamplerMask;          // Bit per sampler binding
    u32 usedStorageBufferMask;    // Bit per storage buffer binding
    u32 usedStorageImageMask;     // Bit per storage image binding (read/write textures)
    u32 readStorageBufferMask;    // Bit per storage buffer binding read from
    u32 writeStorageBufferMask;   // Bit per storage buffer binding written to
    u32 readStorageImageMask;     // Bit per storage image binding read from
    u32 writeStorageImageMask;    // Bit per storage image binding written to
    
    // Type info for each binding (CoreType stored per slot)
    u8 uniformTypes[32];          // CoreType for each uniform binding
    u32 uniformTypeHashes[32];    // Struct type hash for CUSTOM uniform types, 0 otherwise
    u8 attributeTypes[16];        // CoreType for each attribute
    u8 outputTypes[32];           // CoreType for each output slot
    u8 inputTypes[16];            // CoreType for each fragment input
    
    // Capability requirements (flags)
    enum CapabilityFlags : u32 {
        CAP_NONE              = 0,
        CAP_DERIVATIVES       = 1 << 0,   // DDX/DDY used
        CAP_FINE_DERIVATIVES  = 1 << 1,   // DDX_FINE/DDY_FINE used
        CAP_COARSE_DERIVATIVES= 1 << 2,   // DDX_COARSE/DDY_COARSE used
        CAP_WAVE_OPS          = 1 << 3,   // Wave intrinsics used
        CAP_ATOMICS           = 1 << 4,   // Atomic ops used
        CAP_IMAGE_STORE       = 1 << 5,   // Image write ops used
        CAP_IMAGE_LOAD        = 1 << 6,   // Image read ops used
        CAP_INT64             = 1 << 7,   // 64-bit integers used
        CAP_FLOAT64           = 1 << 8,   // 64-bit floats used
        CAP_STORAGE_BUFFER    = 1 << 9,   // Storage buffers used
        CAP_SAMPLED_1D        = 1 << 10,  // 1D textures used
        CAP_IMAGE_1D          = 1 << 11,  // 1D images used
        CAP_SAMPLED_CUBE      = 1 << 12,  // Cube textures used
        CAP_SAMPLE_RATE       = 1 << 13,  // Sample-rate shading used
        CAP_CLIP_DISTANCE     = 1 << 14,  // ClipDistance used
        CAP_CULL_DISTANCE     = 1 << 15,  // CullDistance used
    };
    u32 capabilityFlags;
    
    // Helper methods
    bool Has(CapabilityFlags cap) const { return (capabilityFlags & cap) != 0; }
    bool HasAny(u32 caps) const { return (capabilityFlags & caps) != 0; }
    bool HasAll(u32 caps) const { return (capabilityFlags & caps) == caps; }
    void Set(CapabilityFlags cap) { capabilityFlags |= cap; }
    
    // Resource counts (derived from popcount of masks)
    u32 UniformCount() const { return static_cast<u32>(std::popcount(usedUniformMask)); }
    u32 TextureCount() const { return static_cast<u32>(std::popcount(usedTextureMask)); }
    u32 SamplerCount() const { return static_cast<u32>(std::popcount(usedSamplerMask)); }
    u32 StorageBufferCount() const { return static_cast<u32>(std::popcount(usedStorageBufferMask)); }
    u32 StorageImageCount() const { return static_cast<u32>(std::popcount(usedStorageImageMask)); }
    u32 AttributeCount() const { return static_cast<u32>(std::popcount(usedAttributeMask)); }
    u32 OutputCount() const { return static_cast<u32>(std::popcount(usedOutputMask)); }
    u32 InputCount() const { return static_cast<u32>(std::popcount(usedInputMask)); }
    
    // Iteration helpers - get binding index from mask
    // Returns next set bit index starting from 'start', or 32 if none
    static u32 NextSetBit(u32 mask, u32 start = 0) {
        if (start >= 32) return 32;
        u32 shifted = mask >> start;
        if (shifted == 0) return 32;
        return start + static_cast<u32>(std::countr_zero(shifted));
    }
};

// Output slot definitions (for usedOutputMask)
namespace OutputSlot {
    constexpr u32 POSITION = 0;       // output.position -> BuiltIn Position
    constexpr u32 COLOR    = 1;       // output.color -> Location 0 (fragment)
    constexpr u32 VARYING0 = 2;       // First varying output
    constexpr u32 VARYING1 = 3;
    constexpr u32 VARYING2 = 4;
    constexpr u32 VARYING3 = 5;
    // Fragment depth
    constexpr u32 DEPTH    = 16;      // output.depth -> BuiltIn FragDepth
}

// Built-in input slot definitions (for vertex shader built-ins)
// High values (0x80+) distinguish from fragment varyings (0-15)
namespace BuiltinInputSlot {
    constexpr u32 VERTEX_ID              = 0x80;   // input.vertex_id -> BuiltIn VertexIndex
    constexpr u32 INSTANCE_ID            = 0x81;   // input.instance_id -> BuiltIn InstanceIndex
    constexpr u32 FRAG_COORD             = 0x82;   // input.position (fragment) -> BuiltIn FragCoord
    constexpr u32 GLOBAL_INVOCATION_ID   = 0x90;
    constexpr u32 LOCAL_INVOCATION_ID    = 0x91;
    constexpr u32 WORKGROUP_ID           = 0x92;
    constexpr u32 NUM_WORKGROUPS         = 0x93;
    constexpr u32 LOCAL_INVOCATION_INDEX = 0x94;
}

// Well-known output name hashes
namespace OutputHash {
    constexpr u32 POSITION = 0x8B7E3A1D;  // Hash of "position"
    constexpr u32 COLOR    = 0x5C3D2A18;  // Hash of "color"
    constexpr u32 DEPTH    = 0x4E8C1B2D;  // Hash of "depth"
}

// Well-known built-in input name hashes
namespace BuiltinHash {
    constexpr u32 VERTEX_ID      = Utils::HashStr("vertex_id");
    constexpr u32 INSTANCE_ID    = Utils::HashStr("instance_id");
    constexpr u32 POSITION       = Utils::HashStr("position");  // input.position (fragment) -> FragCoord
    constexpr u32 GLOBAL_ID      = Utils::HashStr("global_id");
    constexpr u32 LOCAL_ID       = Utils::HashStr("local_id");
    constexpr u32 WORKGROUP_ID   = Utils::HashStr("workgroup_id");
    constexpr u32 NUM_WORKGROUPS = Utils::HashStr("num_workgroups");
    constexpr u32 LOCAL_INDEX    = Utils::HashStr("local_index");
}

// Analyze IR to populate analysis results
void AnalyzeIR(IRAnalysis* analysis, const IR::IRProgram* ir);

// Map output name hash to slot index
u32 OutputHashToSlot(u32 nameHash);

} // namespace BWSL
