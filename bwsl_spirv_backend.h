#pragma once
#include "bwsl_ir_gen.h"
#include "bwsl_ir_analysis.h"
#include "bwsl_cfg.h"
#include "bwsl_ast_common.h" // For BWSL_Arena
#include "bwsl_symbol_table.h"
#include "bwsl_ast_soa.h"
#include "bwsl_defs.h"
#include "bwsl_render_config.h"  // For ShaderStage, VertexAttributeType
#include "bwsl_arena.h"
#include "vendor/SPIRV-Headers/include/spirv/unified1/spirv.hpp"
#include <vector>

// Constants from SPIRV spec
constexpr u32 SpvMagicNumber = spv::MagicNumber;
constexpr u32 SpvVersion = 0x00010200; // SPIR-V 1.2

namespace BWSL {

struct SPIRVBuilder {
    // ============= Core ID Management (Hot Data) =============
    alignas(64) u32* spirvIds;           // Maps IR register -> SPIR-V ID
    alignas(64) u16* idTypes;            // CoreType for each SPIR-V ID
    alignas(64) u32* idDecorations;      // Packed decoration flags per ID
    alignas(64) bool* hasPreAllocatedId; // True if register has pre-allocated ID needing definition
    u32 nextId;
    u32 idCapacity;
    
    // ============= Type Deduplication =============
    alignas(64) u32 typeIds[static_cast<u32>(CoreType::COUNT)];  // Direct indexed by CoreType
    alignas(64) u32* compositeTypeIds;  // For struct/array types
    alignas(64) u32* compositeTypeHashes;
    u32 compositeTypeCount;

    // Struct type tracking (maps IR struct type hash -> SPIR-V type ID)
    alignas(64) u32* structTypeIds;      // SPIR-V type IDs for struct types
    alignas(64) u32* structTypeHashes;   // Hash of each registered struct type
    u32 structTypeCount;

    // Struct field type IDs - stores the SPIR-V type ID for each field of each struct
    // Indexed by [structIndex * MAX_FIELDS_PER_STRUCT + fieldIndex]
    static constexpr u32 MAX_FIELDS_PER_STRUCT = 32;
    alignas(64) u32* structFieldTypeIds;  // SPIR-V type IDs for each field (may be array types)

    // Texture type IDs (cached for reuse)
    u32 imageTypeId = 0;          // OpTypeImage for 2D sampled texture
    u32 samplerTypeId = 0;        // OpTypeSampler
    u32 sampledImageTypeId = 0;   // OpTypeSampledImage

    // Built-in input type IDs (cached for reuse)
    u32 globalInvocationIdVarId = 0;
    u32 localInvocationIdVarId = 0;
    u32 workgroupIdVarId = 0;
    u32 numWorkgroupsVarId = 0;
    u32 localInvocationIndexVarId = 0;
    u32 workgroupSizeX = 1;
    u32 workgroupSizeY = 1;
    u32 workgroupSizeZ = 1;

    // ============= Constant Pools (parallel to IR) =============
    alignas(64) u32* floatConstantIds;  // SPIR-V IDs for float constants
    alignas(64) u32* intConstantIds;    // SPIR-V IDs for signed int constants
    alignas(64) u32* uintConstantIds;   // SPIR-V IDs for unsigned int constants
    alignas(64) u32* constantHashes;     // For deduplication
    u32 constantCount;
    u32 boolTrueId = 0;                  // Cached OpConstantTrue ID
    u32 boolFalseId = 0;                 // Cached OpConstantFalse ID
    
    // ============= Binary Sections =============
    struct Section {
        alignas(64) u32* words;
        u32 count;
        u32 capacity;
    };
    
    // Each Section declared in the order they MUST appear in the binary.

    Section capabilities; // All OpCapability instructions must appear first. These declare the feature sets used by the module.

    Section extensions; // All OpExtension instructions, which declare the use of SPIR-V extensions.

    Section extInstImports; // extended instruction sets -  All OpExtInstImport instructions, used to import libraries of functions like GLSL.std.450.

    Section memoryModel; // The single, mandatory OpMemoryModel instruction.

    Section entryPoints; // OpEntryPoint declarations, which define the shader's entry points.

    Section executionModes; //  All OpExecutionMode or OpExecutionModeId instructions, specifying modes for the declared entry points.

    Section debugNames; //  All debug instructions, such as OpString, OpSourceContinued, OpSource, and OpSourceExtension.

    Section decorations; //  All decoration instructions, such as OpDecorate, OpMemberDecorate, OpDecorationGroup, and OpGroupDecorate.

    Section typesConstants; // This is the main declaration section. All OpType*, OpConstant*, OpSpecConstant*, and global OpVariable instructions must be placed here. This section must precede all function definitions.
    
    Section globals; // see abov - we just seperate out the globals
    
    Section functions; // Finally, all functions are defined. Each function definition starts with OpFunction and ends with OpFunctionEnd. Inside, the instructions are organized into a control-flow graph of basic blocks, each starting with OpLabel.
    
    // ============= Instruction Building (Hot Path) =============
    alignas(64) u32* currentFunction;    // Current function being built
    u32 currentFunctionSize;
    u32 currentFunctionCapacity;
    
    // ============= Block Management =============
    alignas(64) u32* blockLabels;        // Label IDs for basic blocks
    alignas(64) u32* blockIRIndices;    // Maps IR instruction index -> block
    alignas(64) u32* blockMergePoints;  // Structured control flow merge points
    u32 blockCount;

    // ============= Branch Condition Override =============
    // Used to pre-convert branch conditions to bool before OpSelectionMerge
    u32 branchConditionOverride = 0;     // Pre-converted bool SPIR-V ID
    u16 branchConditionOverrideReg = 0;  // Which register was overridden
    
    // ============= Interface Variables =============
    alignas(64) u32* inputIds;           // Vertex attributes, etc.
    alignas(64) u8* inputLocations;      // Location decorations
    alignas(64) u32* outputIds;          // Fragment outputs, etc.
    alignas(64) u8* outputLocations;
    u32 inputCount;
    u32 outputCount;
    
    // ============= Resource Bindings =============
    alignas(64) u32* uniformBufferIds;
    alignas(64) u32* textureIds;
    alignas(64) u32* samplerIds;
    alignas(64) u32* storageBufferIds;
    alignas(64) u8* bindingSets;         // Descriptor set indices
    alignas(64) u8* bindingIndices;      // Binding within set
    u32 resourceCount;
    
    // ============= IR Mapping Tables =============
    IR::IRProgram* ir;
    BWSL_Arena* arena;
    ShaderStage stage;
    u32 entryPointId;
    u32 glslStd450Id;  // Extended instruction set
    
    // ============= IR Analysis (populated during Initialize) =============
    IRAnalysis analysis;
    const SymbolTableData* symbols;
    CFG* cfg;  // Control flow graph for proper block ordering and PHI emission

    // ============= Debug Options =============
    bool emitDebugNames = false;  // Emit OpName/OpMemberName for debugging

    // ============= Layout Options =============
    bool useStd430Padding = false;  // Use 16-byte stride for vec3 (required for GLSL compatibility)
    
    // ============= Vertex Pulling Configuration =============
    enum class VertexInputMode : u8 {
        // Traditional interleaved vertex attributes via Input variables
        // Uses Location decorations - compatible with vertex descriptors
        Interleaved,
        
        // One storage buffer per attribute (matches ModelData::attributeStreams)
        // Each attribute gets its own Binding decoration
        SeparateBuffers,
        
        // Single unified buffer with per-attribute offset table
        // Used by current Metal shader approach
        UnifiedWithOffsets,
    };
    
    struct VertexPullingConfig {
        VertexInputMode mode = VertexInputMode::SeparateBuffers;
        u8 attributeMask = 0;          // Which attributes are active
        u32 baseBufferBinding = 0;     // Starting binding index for attribute buffers
        u32 descriptorSet = 0;         // Descriptor set for vertex buffers
        u32 vertexIdBinding = 0;       // Binding for vertex ID (when using pulling)
    };
    
    VertexPullingConfig vertexPullingConfig;
    
    // Storage buffer IDs for vertex pulling (one per attribute)
    u32 attributeBufferIds[8] = {0};
    u32 attributeBufferPtrTypeIds[8] = {0};
    
    // Built-in input variables (vertex shader)
    u32 vertexIdVarId = 0;     // BuiltIn VertexIndex (gl_VertexID)
    u32 instanceIdVarId = 0;   // BuiltIn InstanceIndex (gl_InstanceID)
    
    // ============= Methods =============
    
    void Initialize(BWSL_Arena* arena, IR::IRProgram* ir, ShaderStage stage,
                    const SymbolTableData* symbols = nullptr, CFG* cfg = nullptr);
    void SetComputeWorkgroupSize(u32 x, u32 y, u32 z) {
        workgroupSizeX = x;
        workgroupSizeY = y;
        workgroupSizeZ = z;
    }
    
    // ID allocation
    u32 AllocateId() { return nextId++; }
    u32 GetSpirvId(u16 ir_register);
    u32 GetSpirvIdForBitwise(u16 ir_register, bool useUint);

    // Type management (cached)
    u32 GetTypeId(CoreType type);
    u32 GetVectorTypeId(CoreType base, u32 components);
    u32 GetPointerTypeId(u32 type_id, spv::StorageClass storage);
    u32 GetResultType(u16 dest_reg, u16 op1_reg); // Helper for arithmetic result types
    u32 GetFunctionTypeId(u32 return_type, u32* param_types, u32 param_count);

    // Struct type management
    u32 GetStructTypeId(u32 structTypeHash);  // Get or create SPIR-V struct type from IR struct info

    // Texture type management
    u32 GetImageTypeId();           // Get OpTypeImage ID for 2D sampled texture
    u32 GetSamplerTypeId();         // Get OpTypeSampler ID
    u32 GetSampledImageTypeId();    // Get OpTypeSampledImage ID

    // Constant management
    u32 GetFloatConstantId(float value);
    u32 GetIntConstantId(u32 value, bool isUnsigned = false);
    u32 GetBoolConstantId(bool value);  // Returns OpConstantTrue or OpConstantFalse
    u32 GetCompositeConstantId(u32 type_id, u32* constituents, u32 count);
    
    // Section builders
    void EmitToSection(Section* section, spv::Op op, u32* operands, u32 operand_count);
    void EmitCapability(spv::Capability cap);
    void EmitExtension(const char* extName);
    void EmitDecoration(u32 id, spv::Decoration decoration, u32* params, u32 param_count);
    void EmitMemberDecoration(u32 structTypeId, u32 memberIndex, spv::Decoration decoration, u32 value);
    void EmitName(u32 id, const char* name);
    void EmitMemberName(u32 structTypeId, u32 memberIndex, const char* name);

    // Direct binary emission (hot path)
    template<typename... Args>
    void Emit(spv::Op op, Args... args) {
        constexpr u32 word_count = sizeof...(args) + 1;
        if (currentFunctionSize + word_count > currentFunctionCapacity) {
            GrowCurrentFunction();
        }
        currentFunction[currentFunctionSize++] = (word_count << 16) | op;
        ((currentFunction[currentFunctionSize++] = args), ...);
    }
    
    // IR translation
    void TranslateInstruction(u32 ir_idx);
    spv::Op IRToSpvOp(IR::OpCode op);
    
    // Control flow
    u32 GetOrCreateBlockLabel(u32 ir_idx);
    void EmitBranch(u32 ir_idx);
    void EmitStructuredControlFlow(u32 ir_idx);
    
    // PHI nodes
    void EmitPhiNodes(u32 blockIndex);
    void SimplifyTrivialPhis();  // Pre-simplify PHIs with all-same operands
    
    // Function emission
    void EmitFunction();
    void EmitFunctionBody();
    
    // Preamble (capabilities, imports, memory model)
    void EmitPreamble();
    void EmitEntryPoint();
    
    // Interface setup
    void DeclareInputOutput();
    void DeclareResources();
    u32 CreateInterfaceVariable(CoreType type, spv::StorageClass storage, 
                                u32 location, spv::BuiltIn builtin);
    
    // Debug options
    void SetEmitDebugNames(bool emit) { emitDebugNames = emit; }

    // Layout options
    void SetUseStd430Padding(bool use) { useStd430Padding = use; }

    // Vertex pulling setup
    void SetVertexPullingConfig(const VertexPullingConfig& config) { vertexPullingConfig = config; }
    void DeclareVertexPullingBuffers();
    void DeclareVertexIdBuiltin();
    void DeclareInstanceIdBuiltin();
    u32 CreateStorageBufferForAttribute(u32 attrIdx, CoreType elementType, u32 binding);
    
    // Final assembly
    std::vector<u32> Finalize();
    
private:
    void GrowSection(Section* section);
    void GrowCurrentFunction();
    
    // Fast parallel array helpers
    inline void SetSpirvId(u16 ir_reg, u32 spv_id) {
        if (ir_reg >= idCapacity) GrowIdArrays();
        spirvIds[ir_reg] = spv_id;
    }
    
    void GrowIdArrays();
    
    // Type hashing for deduplication
    u32 HashCompositeType(u32* components, u32 count);
};

// ============= Inline Implementations (Hot Path) =============

inline u32 SPIRVBuilder::GetSpirvId(u16 ir_register) {
    // Handle special encoding for constants
    // Check bool first since 0xC000 & 0x8000 == 0x8000
    if ((ir_register & 0xC000) == 0xC000) {
        // Bool constant (0xC000 prefix)
        u32 idx = ir_register & 0x3FFF;
        return GetBoolConstantId(ir->boolConstants[idx] != 0);
    }
    if (ir_register & 0x8000) {
        // Float constant
        u32 idx = ir_register & 0x7FFF;
        return GetFloatConstantId(ir->floatConstants[idx]);
    }
    if (ir_register & 0x4000) {
        // Int constant (default to signed)
        u32 idx = ir_register & 0x3FFF;
        return GetIntConstantId(ir->intConstants[idx], false);
    }
    if (ir_register & 0x2000) {
        // Uint constant (always unsigned)
        u32 idx = ir_register & 0x1FFF;
        return GetIntConstantId(ir->uintConstants[idx], true);
    }

    // Regular register
    if (ir_register >= idCapacity) return 0;

    u32 id = spirvIds[ir_register];
    if (id == 0) {
        // Check if this is an undef register (from SSA for uninitialized variables)
        bool isUndef = false;
        u16 undefType = 0;
        for (u32 i = 0; i < ir->undefRegCount; i++) {
            if (ir->undefRegs[i] == ir_register) {
                isUndef = true;
                undefType = ir->undefRegTypes[i];
                break;
            }
        }

        if (isUndef) {
            // Emit OpUndef for this register
            id = AllocateId();
            spirvIds[ir_register] = id;
            idTypes[id] = undefType;
            // Emit OpUndef in the types/constants section
            u32 type_id = GetTypeId(static_cast<CoreType>(undefType));
            u32 ops[] = {type_id, id};
            EmitToSection(&typesConstants, spv::OpUndef, ops, 2);
        } else {
            // Allocate on first use
            id = AllocateId();
            spirvIds[ir_register] = id;
            if (ir_register < ir->registerCount && ir->registerTypes) {
                idTypes[id] = ir->registerTypes[ir_register];
            }
        }
    }
    return id;
}

// Get SPIR-V ID for bitwise operations, using uint for int constants when needed
inline u32 SPIRVBuilder::GetSpirvIdForBitwise(u16 ir_register, bool useUint) {
    if (ir_register & 0x8000) {
        // Float constant - shouldn't happen for bitwise ops
        u32 idx = ir_register & 0x7FFF;
        return GetFloatConstantId(ir->floatConstants[idx]);
    }
    if (ir_register & 0x4000) {
        // Int constant - use the appropriate signedness
        u32 idx = ir_register & 0x3FFF;
        return GetIntConstantId(ir->intConstants[idx], useUint);
    }
    if (ir_register & 0x2000) {
        // Uint constant - always unsigned
        u32 idx = ir_register & 0x1FFF;
        return GetIntConstantId(ir->uintConstants[idx], true);
    }

    // Regular register - use normal GetSpirvId
    return GetSpirvId(ir_register);
}

inline u32 SPIRVBuilder::GetTypeId(CoreType type) {
    u32 idx = static_cast<u32>(type);
    if (idx >= static_cast<u32>(CoreType::COUNT)) return 0;

    // Early check for types that don't have SPIR-V equivalents
    // Return 0 for invalid, string, enum, generic types, and resources
    // These should not allocate IDs as they won't emit OpType instructions
    switch (type) {
        case CoreType::INVALID:
        case CoreType::STRING:
        case CoreType::CUSTOM:  // Structs handled separately via GetStructTypeId
        case CoreType::ENUM:
        case CoreType::GENERIC_T:
        case CoreType::GENERIC_U:
        case CoreType::GENERIC_V:
        case CoreType::CONSTRAINT:
        case CoreType::CBUFFER:
        case CoreType::BUFFER:
        case CoreType::TEXTURE2D:
        case CoreType::TEXTURE3D:
        case CoreType::TEXTURECUBE:
        case CoreType::TEXTURE2DARRAY:
        case CoreType::SAMPLER:
        case CoreType::VERTEX_FUNCTION:
        case CoreType::FRAGMENT_FUNCTION:
        case CoreType::COMPUTE_FUNCTION:
        case CoreType::PASS_BLOCK:
            return 0;  // These don't have direct SPIR-V type equivalents
        default:
            break;
    }

    u32 id = typeIds[idx];
    if (id == 0) {
        // Lazy creation: allocate a new <id> and store it.
        id = AllocateId();
        typeIds[idx] = id;

        // Emit the corresponding OpType* instruction to the types_constants section.
        switch (type) {
            // ============ Scalar Types ============
            case CoreType::VOID:   EmitToSection(&typesConstants, spv::OpTypeVoid, &id, 1); break;
            case CoreType::BOOL:   EmitToSection(&typesConstants, spv::OpTypeBool, &id, 1); break;
            case CoreType::FLOAT:  { u32 ops[] = {id, 32}; EmitToSection(&typesConstants, spv::OpTypeFloat, ops, 2); break; }
            case CoreType::INT:    { u32 ops[] = {id, 32, 1}; EmitToSection(&typesConstants, spv::OpTypeInt, ops, 3); break; } // Signed
            case CoreType::UINT:   { u32 ops[] = {id, 32, 0}; EmitToSection(&typesConstants, spv::OpTypeInt, ops, 3); break; } // Unsigned

            // ============ 64-bit Scalar Types ============
            // THIS IS CURRENTLY NOT SUPPORTED. WOULD ENTAIL QUITE A BIT OF WORK
            /* (type casting system (promotion, narrowing), parsing, semantic analysis,ir gen, overload resolution etc. For now, we stick to 32 bit )
            case CoreType::DOUBLE: { u32 ops[] = {id, 64}; EmitToSection(&typesConstants, spv::OpTypeFloat, ops, 2); break; }
            case CoreType::INT64:  { u32 ops[] = {id, 64, 1}; EmitToSection(&typesConstants, spv::OpTypeInt, ops, 3); break; }
            case CoreType::UINT64: { u32 ops[] = {id, 64, 0}; EmitToSection(&typesConstants, spv::OpTypeInt, ops, 3); break; }
            */

            // ============ Bool Vector Types ============
            case CoreType::BOOL2:  { u32 ops[] = {id, GetTypeId(CoreType::BOOL), 2}; EmitToSection(&typesConstants, spv::OpTypeVector, ops, 3); break; }
            case CoreType::BOOL3:  { u32 ops[] = {id, GetTypeId(CoreType::BOOL), 3}; EmitToSection(&typesConstants, spv::OpTypeVector, ops, 3); break; }
            case CoreType::BOOL4:  { u32 ops[] = {id, GetTypeId(CoreType::BOOL), 4}; EmitToSection(&typesConstants, spv::OpTypeVector, ops, 3); break; }

            // ============ Float Vector Types ============
            case CoreType::FLOAT2: { u32 ops[] = {id, GetTypeId(CoreType::FLOAT), 2}; EmitToSection(&typesConstants, spv::OpTypeVector, ops, 3); break; }
            case CoreType::FLOAT3: { u32 ops[] = {id, GetTypeId(CoreType::FLOAT), 3}; EmitToSection(&typesConstants, spv::OpTypeVector, ops, 3); break; }
            case CoreType::FLOAT4: { u32 ops[] = {id, GetTypeId(CoreType::FLOAT), 4}; EmitToSection(&typesConstants, spv::OpTypeVector, ops, 3); break; }

            // ============ Int Vector Types ============
            case CoreType::INT2:   { u32 ops[] = {id, GetTypeId(CoreType::INT), 2}; EmitToSection(&typesConstants, spv::OpTypeVector, ops, 3); break; }
            case CoreType::INT3:   { u32 ops[] = {id, GetTypeId(CoreType::INT), 3}; EmitToSection(&typesConstants, spv::OpTypeVector, ops, 3); break; }
            case CoreType::INT4:   { u32 ops[] = {id, GetTypeId(CoreType::INT), 4}; EmitToSection(&typesConstants, spv::OpTypeVector, ops, 3); break; }

            // ============ Unsigned Int Vector Types ============
            case CoreType::UINT2:  { u32 ops[] = {id, GetTypeId(CoreType::UINT), 2}; EmitToSection(&typesConstants, spv::OpTypeVector, ops, 3); break; }
            case CoreType::UINT3:  { u32 ops[] = {id, GetTypeId(CoreType::UINT), 3}; EmitToSection(&typesConstants, spv::OpTypeVector, ops, 3); break; }
            case CoreType::UINT4:  { u32 ops[] = {id, GetTypeId(CoreType::UINT), 4}; EmitToSection(&typesConstants, spv::OpTypeVector, ops, 3); break; }

            // ============ Matrix Types (Column-Major) ============
            case CoreType::MAT2:   { u32 ops[] = {id, GetTypeId(CoreType::FLOAT2), 2}; EmitToSection(&typesConstants, spv::OpTypeMatrix, ops, 3); break; }
            case CoreType::MAT3:   { u32 ops[] = {id, GetTypeId(CoreType::FLOAT3), 3}; EmitToSection(&typesConstants, spv::OpTypeMatrix, ops, 3); break; }
            case CoreType::MAT4:   { u32 ops[] = {id, GetTypeId(CoreType::FLOAT4), 4}; EmitToSection(&typesConstants, spv::OpTypeMatrix, ops, 3); break; }

            default:
                // All handled types should be in this switch - if we get here something is wrong
                // Don't allocate an ID for unknown types
                typeIds[idx] = 0;  // Clear the allocated ID
                return 0;
        }
    }
    return id;
}

} // namespace BWSL
