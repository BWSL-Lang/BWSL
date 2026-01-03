#pragma once

#include "bwsl_defs.h"
#include "bwsl_mem_pool.h"
#include "bwsl_types.h"
#include <iostream>
namespace BWSL{

// Forward declaration for CFG (defined in bwsl_cfg.h)
struct CFG;
constexpr u32 NO_BLOCK = 0xFFFFFFFF;

namespace IR {


struct IRProgram {
    // Hot instruction data - separate arrays for better cache usage
    alignas(64) u16* opcodes;          // Operation types
    alignas(64) u16* types;            // Result types  
    alignas(64) u16* flags;            // Instruction flags
    alignas(64) u16* destinations;     // Destination registers
    alignas(64) u16* operands;         // Flattened 4*count array (supports float4 etc.)
    alignas(64) u32* metadata;         // Variable per-instruction data
    u32 instructionCount;
    u32 instructionCapacity;
    
    // Register allocation info
    alignas(64) u16* registerTypes;        // CoreType for each register
    alignas(64) u32* registerLifetimes;    // Packed first:16|last:16 instruction
    alignas(64) u32* registerSpillSlots;   // 0xFFFFFFFF if not spilled
    u32 registerCount;
    
    // Constant pools
    alignas(64) float* floatConstants;
    alignas(64) u32* intConstants;
    alignas(64) u32* uintConstants;     // Unsigned int constants (separate for type safety)
    alignas(64) u64* int64Constants;
    alignas(64) u8* boolConstants;      // Bool constants (0=false, 1=true)
    u32 floatCount;
    u32 intCount;
    u32 uintCount;
    u32 int64Count;
    u32 boolCount;
    
    // Variant branches
    alignas(64) u32* variantInstructionIndices;
    alignas(64) u32* variantAttributeMasks;
    alignas(64) u32* variantTrueBranches;
    alignas(64) u32* variantFalseBranches;
    u32 variantBranchCount;
    
    // Module call sites
    alignas(64) u32* callInstructionIndices;
    alignas(64) u32* callModuleIndices;
    alignas(64) u32* callFunctionIndices;
    alignas(64) u8* callInlineDecisions;  // 0=unknown, 1=inline, 2=call
    u32 callSiteCount;
    
    // Resource binding points
    alignas(64) u32* resourceInstructionIndices;
    alignas(64) u16* resourceBindingSlots;
    alignas(64) u8* resourceTypes;        // texture/buffer/sampler
    alignas(64) u8* resourceStageFlags;   // Which stages need this
    u32 resourceCount;

    alignas(64) u32* structureInfo;  // Packed: type:4 | mergeTarget:28
    alignas(64) u32* continueInfo;   // Continue target for loops (instruction index), 0xFFFFFFFF = none

    // Switch case data (for multi-way branches)
    alignas(64) u32* switchInstructionIndices;  // Which instructions are switches
    alignas(64) u32* switchCaseOffsets;         // [switchCount+1] CSR format for case ranges
    alignas(64) s32* switchCaseValues;          // Flattened case values (signed for negative cases)
    alignas(64) u32* switchCaseTargets;         // Flattened target instructions
    alignas(64) u32* switchDefaultTargets;      // Default target per switch
    u32 switchCount;
    u32 switchCaseCapacity;                     // Capacity for flattened arrays

    // PHI nodes (added by SSA construction)
    alignas(64) u32* phiBlockIndices;           // Which block each PHI belongs to
    alignas(64) u16* phiResultRegs;             // Destination register for each PHI
    alignas(64) u16* phiTypes;                  // CoreType of result
    alignas(64) u32* phiOperandOffsets;         // [phiCount + 1] CSR offsets
    alignas(64) u16* phiOperandValues;          // Flattened: value registers
    alignas(64) u32* phiOperandBlocks;          // Flattened: source blocks
    u32 phiCount;
    u32 phiCapacity;
    u32 phiOperandCapacity;

    // Undefined value registers (for SSA phi operands on paths without definitions)
    alignas(64) u16* undefRegs;                 // Register IDs that are undefined
    alignas(64) u16* undefRegTypes;             // CoreType for each undef register
    u32 undefRegCount;
    u32 undefRegCapacity;

    // Struct type metadata (for custom struct types used in the shader)
    struct StructTypeInfo {
        u32 nameHash;                   // Hash of struct type name
        u32 fieldCount;                 // Number of fields
        u32 fieldOffset;                // Offset into structFieldTypes/structFieldOffsets arrays
        u32 totalSize;                  // Total byte size (std140 layout)
    };
    alignas(64) StructTypeInfo* structTypes;    // Array of struct type definitions
    alignas(64) u16* structFieldTypes;          // Flattened: CoreType for each field
    alignas(64) u32* structFieldNameHashes;     // Flattened: name hash for each field
    alignas(64) u32* structFieldTypeHashes;     // Flattened: custom type hash for each field (0 if not custom)
    alignas(64) u32* structFieldByteOffsets;    // Flattened: byte offset of each field (std140)
    alignas(64) u32* structFieldArraySizes;     // Flattened: array size for each field (0 = not array)
    u32 structTypeCount;
    u32 structTypeCapacity;
    u32 structFieldCapacity;

    // Map register -> struct type hash (for registers holding struct values)
    alignas(64) u32* registerStructTypes;       // structTypeHash or 0 if not a struct

    // Storage buffer pointer tracking
    // For registers holding storage buffer pointers, stores (binding << 16) | flags
    // Flags: bit 0 = is pointer, bits 1-7 = depth into struct (0=buffer, 1=field, 2=array element, etc.)
    alignas(64) u32* registerStorageInfo;       // Storage buffer info or 0 if not a storage ptr
    static constexpr u32 STORAGE_IS_PTR = 0x1;
    static constexpr u32 STORAGE_IS_SHARED = 0x2;
    static constexpr u32 STORAGE_IS_ADDRESS_TAKEN = 0x4;  // Variable has its address taken (&var)
    static constexpr u32 STORAGE_IS_LOCAL_ARRAY = 0x8;    // Function-local array with initializer
    static constexpr u32 STORAGE_BINDING_SHIFT = 16;
    static constexpr u32 STORAGE_DEPTH_SHIFT = 1;
    static constexpr u32 STORAGE_DEPTH_MASK = 0x7F << 1;

    // Shared (workgroup) variable declarations
    alignas(64) u32* sharedNameHashes;
    alignas(64) u16* sharedTypes;
    alignas(64) u32* sharedArraySizes;
    alignas(64) u16* sharedRegisters;
    u32 sharedVarCount;
    u32 sharedVarCapacity;

    // Local (function-scope) array declarations
    alignas(64) u32* localArrayNameHashes;
    alignas(64) u16* localArrayTypes;
    alignas(64) u32* localArrayStructTypes;  // Struct type hash for CUSTOM element types
    alignas(64) u32* localArraySizes;
    alignas(64) u16* localArrayRegisters;
    u32 localArrayCount;
    u32 localArrayCapacity;

    // Buffer element types
    // Index by binding slot (0-31)
    alignas(64) u32 bufferElementStructTypes[32];  // Struct type hash, or 0 if primitive/unknown
    alignas(64) u8 bufferElementCoreTypes[32];     // CoreType enum value for primitive element types

    enum StructureType : u32 {
    STRUCT_NONE          = 0,
    STRUCT_IF_HEADER     = 1 << 28,  // OpSelectionMerge needed
    STRUCT_LOOP_HEADER   = 2 << 28,  // OpLoopMerge needed
    STRUCT_SWITCH_HEADER = 3 << 28,
    STRUCT_TYPE_MASK     = 0xF0000000,
    STRUCT_TARGET_MASK   = 0x0FFFFFFF,
    };

    static u32 PackStructure(u32 type, u32 mergeTarget) {
        return type | (mergeTarget & STRUCT_TARGET_MASK);
    }

    // Access helpers (4 operands per instruction for float4 support)
    u16 GetOperand(u32 inst, u32 op) const { 
        return operands[inst * 4 + op]; 
    }
    
    void SetOperand(u32 inst, u32 op, u16 value) {
        operands[inst * 4 + op] = value;
    }
    
    u16 GetRegisterFirstUse(u32 reg) const {
        return registerLifetimes[reg] >> 16;
    }
    
    u16 GetRegisterLastUse(u32 reg) const {
        return registerLifetimes[reg] & 0xFFFF;
    }

    // Switch case helpers
    u32 GetSwitchCaseCount(u32 switchId) const {
        return switchCaseOffsets[switchId + 1] - switchCaseOffsets[switchId];
    }

    s32 GetSwitchCaseValue(u32 switchId, u32 caseIdx) const {
        return switchCaseValues[switchCaseOffsets[switchId] + caseIdx];
    }

    u32 GetSwitchCaseTarget(u32 switchId, u32 caseIdx) const {
        return switchCaseTargets[switchCaseOffsets[switchId] + caseIdx];
    }

    u32 GetSwitchDefaultTarget(u32 switchId) const {
        return switchDefaultTargets[switchId];
    }

    // PHI helpers
    u32 GetPhiOperandCount(u32 phiIdx) const {
        return phiOperandOffsets[phiIdx + 1] - phiOperandOffsets[phiIdx];
    }

    u16 GetPhiOperandValue(u32 phiIdx, u32 opIdx) const {
        return phiOperandValues[phiOperandOffsets[phiIdx] + opIdx];
    }

    u32 GetPhiOperandBlock(u32 phiIdx, u32 opIdx) const {
        return phiOperandBlocks[phiOperandOffsets[phiIdx] + opIdx];
    }

    void SetPhiOperandValue(u32 phiIdx, u32 opIdx, u16 value) {
        phiOperandValues[phiOperandOffsets[phiIdx] + opIdx] = value;
    }
};

enum OpCode : u16 {
    // ========== Control Flow ==========
    OP_NOP           = 0x00,
    OP_JUMP          = 0x01,
    OP_BRANCH        = 0x02,  // Conditional branch
    OP_CALL          = 0x03,
    OP_RET           = 0x04,
    OP_SELECT        = 0x05,  // Ternary select (for branchless)
    OP_PHI           = 0x06,  // SSA phi node
    OP_SWITCH        = 0x07,
    
    // ========== Memory Operations ==========
    OP_LOAD_CONST    = 0x10,
    OP_LOAD_REG      = 0x11,
    OP_STORE_REG     = 0x12,
    OP_LOAD_ATTR     = 0x13,  // Load vertex attribute
    OP_STORE_OUTPUT  = 0x14,  // Store to shader output
    OP_LOAD_OUTPUT   = 0x7F,  // Load from shader output
    OP_LOAD_UNIFORM  = 0x15,  // Load from uniform buffer
    OP_LOAD_BUFFER   = 0x16,  // Load from storage buffer
    OP_STORE_BUFFER  = 0x17,  // Store to storage buffer
    OP_LOAD_LOCAL    = 0x18,  // Thread-local memory
    OP_STORE_LOCAL   = 0x19,
    OP_LOAD_SHARED   = 0x1A,  // Workgroup shared memory
    OP_STORE_SHARED  = 0x1B,
    OP_LOAD_INPUT    = 0x1C,  // Load fragment input (interpolated varying)
    
    // ========== Arithmetic (Float) ==========
    OP_FADD          = 0x20,
    OP_FSUB          = 0x21,
    OP_FMUL          = 0x22,
    OP_FDIV          = 0x23,
    OP_FMOD          = 0x24,
    OP_FNEG          = 0x25,
    OP_FABS          = 0x26,
    OP_FMIN          = 0x27,
    OP_FMAX          = 0x28,
    OP_FCLAMP        = 0x29,
    OP_FLOOR         = 0x2A,
    OP_CEIL          = 0x2B,
    OP_ROUND         = 0x2C,
    OP_TRUNC         = 0x2D,
    OP_FRACT         = 0x2E,
    OP_FMA           = 0x2F,  // Fused multiply-add
    
    // ========== Arithmetic (Integer) ==========
    OP_IADD          = 0x30,
    OP_ISUB          = 0x31,
    OP_IMUL          = 0x32,
    OP_IDIV          = 0x33,
    OP_IMOD          = 0x34,
    OP_INEG          = 0x35,
    OP_IABS          = 0x36,
    OP_IMIN          = 0x37,
    OP_IMAX          = 0x38,
    OP_ICLAMP        = 0x39,
    OP_UMIN          = 0x3A,
    OP_UMAX          = 0x3B,
    OP_UCLAMP        = 0x3C,
    
    // ========== Bitwise ==========
    OP_AND           = 0x40,
    OP_OR            = 0x41,
    OP_XOR           = 0x42,
    OP_NOT           = 0x43,
    OP_SHL           = 0x44,
    OP_SHR           = 0x45,  // Logical shift right
    OP_ASR           = 0x46,  // Arithmetic shift right
    OP_POPCNT        = 0x47,
    OP_CLZ           = 0x48,  // Count leading zeros
    OP_CTZ           = 0x49,  // Count trailing zeros
    OP_REVERSE_BITS  = 0x4A,
    
    // ========== Comparison ==========
    OP_FEQ           = 0x50,  // Float equal
    OP_FNE           = 0x51,  // Float not equal
    OP_FLT           = 0x52,  // Float less than
    OP_FLE           = 0x53,  // Float less or equal
    OP_FGT           = 0x54,  // Float greater than
    OP_FGE           = 0x55,  // Float greater or equal
    OP_IEQ           = 0x56,  // Integer equal
    OP_INE           = 0x57,  // Integer not equal
    OP_ILT           = 0x58,  // Integer less than (signed)
    OP_ILE           = 0x59,
    OP_IGT           = 0x5A,
    OP_IGE           = 0x5B,
    OP_ULT           = 0x5C,  // Unsigned less than
    OP_ULE           = 0x5D,
    OP_UGT           = 0x5E,
    OP_UGE           = 0x5F,
    
    // ========== Type Conversion ==========
    OP_F2I           = 0x60,  // Float to int
    OP_I2F           = 0x61,  // Int to float
    OP_F2U           = 0x62,  // Float to uint
    OP_U2F           = 0x63,  // Uint to float
    OP_I2U           = 0x64,  // Int to uint
    OP_U2I           = 0x65,  // Uint to int
    OP_F2F16         = 0x66,  // Float to half
    OP_F162F         = 0x67,  // Half to float
    OP_BITCAST       = 0x68,  // Reinterpret bits
    OP_SIGN          = 0x69, 
    
    // ========== Vector Operations ==========
    OP_VEC_EXTRACT   = 0x70,  // Extract component
    OP_VEC_INSERT    = 0x71,  // Insert component
    OP_VEC_SHUFFLE   = 0x72,  // Swizzle/shuffle
    OP_VEC_CONSTRUCT = 0x73,  // Build vector from scalars

    // ========== Struct Operations ==========
    OP_STRUCT_CONSTRUCT = 0x75,  // Build struct from field values: dest = struct(f0, f1, f2...)
    OP_STRUCT_EXTRACT   = 0x76,  // Extract field: dest = struct.field (operand0=struct, operand1=fieldIndex)
    OP_STRUCT_INSERT    = 0x77,  // Insert field: dest = struct with field=value (operand0=struct, operand1=fieldIndex, operand2=value)
    OP_STRUCT_LOAD      = 0x78,  // Load entire struct from pointer
    OP_STRUCT_STORE     = 0x79,  // Store entire struct to pointer
    OP_STRUCT_GEP       = 0x7A,  // Get element pointer (for access chains): dest = &struct.field

    // ========== Storage Buffer Access (pointer semantics) ==========
    // These ops maintain pointer semantics for proper SPIR-V access chain generation
    OP_STORAGE_PTR      = 0x7B,  // Get storage buffer base pointer: dest = &buffer (operand0=binding)
    OP_STORAGE_FIELD    = 0x7C,  // Access struct field in storage buffer: dest = ptr.field (operand0=ptr, operand1=fieldIndex)
    OP_STORAGE_INDEX    = 0x7D,  // Index into array in storage buffer: dest = ptr[index] (operand0=ptr, operand1=index)
    OP_STORAGE_LOAD     = 0x7E,  // Load value from storage buffer pointer: dest = *ptr (operand0=ptr)

    // ========== Math Functions ==========
    OP_SQRT          = 0x80,
    OP_RSQRT         = 0x81,
    OP_POW           = 0x82,
    OP_EXP           = 0x83,
    OP_EXP2          = 0x84,
    OP_LOG           = 0x85,
    OP_LOG2          = 0x86,
    OP_SIN           = 0x87,
    OP_COS           = 0x88,
    OP_TAN           = 0x89,
    OP_ASIN          = 0x8A,
    OP_ACOS          = 0x8B,
    OP_ATAN          = 0x8C,
    OP_ATAN2         = 0x8D,
    OP_SINH          = 0x8E,
    OP_COSH          = 0x8F,

    // ========== Geometric ==========
    OP_DOT           = 0x90,
    OP_CROSS         = 0x91,
    OP_LENGTH        = 0x92,
    OP_NORMALIZE     = 0x93,
    OP_DISTANCE      = 0x94,
    OP_REFLECT       = 0x95,
    OP_REFRACT       = 0x96,
    OP_FACEFORWARD   = 0x97,
    
    // ========== Matrix ==========
    OP_MAT_MUL       = 0xA0,  // Matrix * Matrix
    OP_MAT_TRANSPOSE = 0xA1,
    OP_MAT_INVERSE   = 0xA2,
    OP_MAT_DET       = 0xA3,  // Determinant
    OP_MAT_CONSTRUCT = 0xA4,  // Build mat2/3/4 from scalars
    OP_MAT_IDENTITY  = 0xA5,  // Generate identity matrix
    OP_MAT_ZERO      = 0xA6,  // Generate zero matrix
    OP_MAT_VEC_MUL   = 0xA7,  // Matrix * Vector
    OP_VEC_MAT_MUL   = 0xA8,  // Vector * Matrix
    OP_MAT_SCALE     = 0xA9,  // Matrix * Scalar
    
    // ========== Texture Operations ==========
    OP_TEX_SAMPLE      = 0xB0,
    OP_TEX_SAMPLE_LOD  = 0xB1,
    OP_TEX_SAMPLE_BIAS = 0xB2,
    OP_TEX_SAMPLE_GRAD = 0xB3,
    OP_TEX_SAMPLE_CMP  = 0xB4,
    OP_TEX_GATHER      = 0xB5,
    OP_TEX_FETCH       = 0xB6,  // texelFetch
    OP_TEX_SIZE        = 0xB7,
    OP_IMG_LOAD        = 0xB8,  // Image load
    OP_IMG_STORE       = 0xB9,  // Image store
    OP_LOAD_TEX_HANDLE = 0xBA,  // bindless textures
    
    // ========== Derivatives (Fragment only) ==========
    OP_DDX             = 0xC0,
    OP_DDY             = 0xC1,
    OP_DDX_FINE        = 0xC2,
    OP_DDY_FINE        = 0xC3,
    OP_DDX_COARSE      = 0xC4,
    OP_DDY_COARSE      = 0xC5,
    OP_FWIDTH          = 0xC6,
    
    // ========== Interpolation ==========
    OP_LERP            = 0xD0,
    OP_SMOOTHSTEP      = 0xD1,
    OP_STEP            = 0xD2,
    OP_SATURATE        = 0xD3,

    // these are not ideal here but we filled the math part
    OP_DEGREES         = 0xD4,
    OP_RADIANS         = 0xD5,

    // ========== Enum Operations ==========
    // For sum types (enums with associated data)
    OP_ENUM_CONSTRUCT  = 0xD6,  // Build enum: dest = EnumType::Variant(args...)
                                // metadata = (variantIndex << 16) | enumTypeHash lower bits
                                // operands = variant field values (up to 4 scalars)
    OP_ENUM_TAG        = 0xD7,  // Extract tag: dest = enum.tag (operand0=enum reg)
    OP_ENUM_FIELD      = 0xD8,  // Extract field: dest = enum.field[N] (operand0=enum, operand1=fieldIndex)

    // ========== Local Pointer Operations ==========
    OP_LOCAL_VAR_PTR   = 0xD9,  // Get pointer to local variable: dest = ^var (operand0=var_reg)
    OP_LOCAL_LOAD      = 0xDA,  // Load from local pointer: dest = ptr^ (operand0=ptr_reg)
    OP_LOCAL_STORE     = 0xDB,  // Store to local pointer: ptr^ = value (operand0=ptr_reg, operand1=value_reg)

    // ========== Atomics ==========
    OP_ATOMIC_ADD      = 0xE0,
    OP_ATOMIC_SUB      = 0xE1,
    OP_ATOMIC_MIN      = 0xE2,
    OP_ATOMIC_MAX      = 0xE3,
    OP_ATOMIC_AND      = 0xE4,
    OP_ATOMIC_OR       = 0xE5,
    OP_ATOMIC_XOR      = 0xE6,
    OP_ATOMIC_XCHG     = 0xE7,  // Exchange
    OP_ATOMIC_CMP_XCHG = 0xE8,  // Compare exchange
    
    // ========== Synchronization ==========
    OP_BARRIER         = 0xF0,  // Workgroup barrier
    OP_MEM_FENCE       = 0xF1,  // Memory fence
    OP_DISCARD         = 0xEF,  // Fragment discard (OpKill)

    // ========== Wave/SIMD/Subgroup Operations ==========
    OP_WAVE_MIN        = 0XF2,
    OP_WAVE_MAX        = 0XF3,
    OP_WAVE_ALL        = 0xF8,
    OP_WAVE_ANY        = 0xF9,
    OP_WAVE_BALLOT     = 0xFA,
    OP_WAVE_READ_FIRST = 0xFB,
    OP_WAVE_READ_LANE  = 0xFC,
    OP_WAVE_SUM        = 0xFD,
    OP_WAVE_MUL        = 0xFE,

    OP_ALLOC_ARRAY     = 0x1C,  // Allocate array storage
    OP_ARRAY_ACCESS    = 0x1D,  // Calculate element address
    OP_ARRAY_STORE     = 0x1E,  // Store to array element
    OP_ARRAY_LOAD      = 0x1F,  // Load from array element
    OP_ARRAY_CONSTRUCT = 0x74,  // Build array from elements
    
    OP_INVALID         = 0xFF
};

// Helper to categorize opcodes
inline bool IsMemoryOp(OpCode op) {
    return (op >= 0x10 && op <= 0x1B) || (op >= 0xB8 && op <= 0xB9) || op == OP_LOAD_OUTPUT;
}

inline bool IsFloatOp(OpCode op) {
    return (op >= 0x20 && op <= 0x2F) || (op >= 0x50 && op <= 0x55);
}

inline bool IsIntOp(OpCode op) {
    return (op >= 0x30 && op <= 0x39) || (op >= 0x56 && op <= 0x5F);
}

inline bool IsBranchOp(OpCode op) {
    return op >= OP_JUMP && op <= OP_SWITCH;
}

// True block terminators (end basic blocks in CFG)
inline bool IsTerminator(OpCode op) {
    return op == OP_JUMP || op == OP_BRANCH || op == OP_RET || op == OP_SWITCH || op == OP_DISCARD;
}

inline bool IsTextureOp(OpCode op) {
    return op >= 0xB0 && op <= 0xB9;
}

inline bool IsFragmentOnlyOp(OpCode op) {
    return (op >= 0xC0 && op <= 0xC6) || op == OP_TEX_SAMPLE_BIAS;
}

inline bool IsComputeOnlyOp(OpCode op) {
    return op >= 0xF0 && op <= 0xFE;
}

inline bool HasSideEffects(OpCode op) {
    return IsMemoryOp(op) || IsBranchOp(op) ||
           (op >= OP_ATOMIC_ADD && op <= OP_ATOMIC_CMP_XCHG) ||
           op == OP_BARRIER || op == OP_MEM_FENCE || op == OP_DISCARD;
}


     
// Returns true for instructions that produce side effects and must be kept
// Also includes control flow which shouldn't be eliminated normally (only converted)
inline bool IsOutputOpcode(u16 opcode) {
    return opcode == OP_STORE_OUTPUT ||
           opcode == OP_STORE_BUFFER ||
           opcode == OP_STORE_SHARED ||
           opcode == OP_IMG_STORE ||
           opcode == OP_RET ||               // Return must be kept
           opcode == OP_BRANCH ||            // Control flow - convert, don't eliminate
           opcode == OP_JUMP ||              // Control flow
           opcode == OP_SWITCH ||            // Control flow
           opcode == OP_DISCARD ||           // Fragment discard
           (opcode >= OP_ATOMIC_ADD && opcode <= OP_ATOMIC_CMP_XCHG) ||
           opcode == OP_BARRIER ||
           opcode == OP_MEM_FENCE;
}

// ============= IR Builder (works with SoA) =============
struct IRBuilder {
    IRProgram* program;
    IRMemoryPool* pool;
    
    // Working set
    u32 currentInstruction;
    u16 nextRegister;
    
    // Symbol mapping
    u32* astNodeToInstruction;
    u16* symbolToRegister;
    
    // Constant deduplication
    u32* floatConstantHashes;
    u32* intConstantHashes;
    u32* uintConstantHashes;
    u32 floatConstantCount;
    u32 intConstantCount;
    u32 uintConstantCount;
    
    // Note: Unused operand slots default to 0x3FFF (invalid register, skipped by SSA)
    void EmitInstruction(u16 opcode, u16 dest, u16 s0, u16 s1 = 0x3FFF, u16 s2 = 0x3FFF, u16 s3 = 0x3FFF) {
        if (currentInstruction >= program->instructionCapacity) {
            GrowInstructionArrays();
        }
        
        u32 idx = currentInstruction++;
        program->opcodes[idx] = opcode;
        program->destinations[idx] = dest;
        program->operands[idx * 4] = s0;
        program->operands[idx * 4 + 1] = s1;
        program->operands[idx * 4 + 2] = s2;
        program->operands[idx * 4 + 3] = s3;
        program->types[idx] = 0;  // Set based on context
        program->flags[idx] = 0;
        
        program->instructionCount = currentInstruction;
    }
    
    u16 EmitConstant(float value) {
        // Check for existing constant
        u32 hash = Utils::HashFloat(value);
        for (u32 i = 0; i < program->floatCount; i++) {
            if (floatConstantHashes[i] == hash &&
                program->floatConstants[i] == value) {
                return 0x8000 | i;  // High bit indicates constant
            }
        }

        // Add new constant
        u32 slot = program->floatCount++;
        program->floatConstants[slot] = value;
        floatConstantHashes[slot] = hash;
        return 0x8000 | slot;
    }

    u16 EmitConstantBool(bool value) {
        // Check for existing constant
        u8 boolVal = value ? 1 : 0;
        for (u32 i = 0; i < program->boolCount; i++) {
            if (program->boolConstants[i] == boolVal) {
                return 0xC000 | i;  // 0xC000 prefix for bool constants
            }
        }

        // Add new constant
        u32 slot = program->boolCount++;
        program->boolConstants[slot] = boolVal;
        return 0xC000 | slot;
    }

    void GrowInstructionArrays() {
        u32 newCapacity = program->instructionCapacity * 2;
        
        // Reallocate all instruction arrays
        program->opcodes = (u16*)pool->Reallocate(
            program->opcodes, 
            newCapacity * sizeof(u16), 
            64);
        program->types = (u16*)pool->Reallocate(
            program->types, 
            newCapacity * sizeof(u16), 
            64);
        program->flags = (u16*)pool->Reallocate(
            program->flags, 
            newCapacity * sizeof(u16), 
            64);
        program->destinations = (u16*)pool->Reallocate(
            program->destinations, 
            newCapacity * sizeof(u16), 
            64);
        program->operands = (u16*)pool->Reallocate(
            program->operands, 
            newCapacity * 3 * sizeof(u16), 
            64);
        program->metadata = (u32*)pool->Reallocate(
            program->metadata, 
            newCapacity * sizeof(u32), 
            64);
        program->structureInfo = (u32*)pool->Reallocate(
            program->structureInfo, 
            newCapacity * sizeof(u32), 
            64);
        program->continueInfo = (u32*)pool->Reallocate(
            program->continueInfo, 
            newCapacity * sizeof(u32), 
            64);
            
        program->instructionCapacity = newCapacity;
    }
};

// ============= Optimization passes (SoA-optimized) =============
struct OptimizationPass {

    // Dead code elimination using SoA
void EliminateDeadCode(IRProgram* prog) {
    u32* registerWriters = (u32*)alloca(prog->registerCount * sizeof(u32));
    memset(registerWriters, 0xFF, prog->registerCount * sizeof(u32));
    
    for (u32 i = 0; i < prog->instructionCount; i++) {
        u16 dest = prog->destinations[i];
        if (dest < prog->registerCount) {
            registerWriters[dest] = i;
        }
    }
    
    bool* used = (bool*)alloca(prog->instructionCount);
    memset(used, 0, prog->instructionCount);
    
    u32* workQueue = (u32*)alloca(prog->instructionCount * sizeof(u32));
    u32 queueHead = 0;
    u32 queueTail = 0;
    
    // Seed with output instructions
    for (u32 i = 0; i < prog->instructionCount; i++) {
        if (IsOutputOpcode(prog->opcodes[i])) {
            workQueue[queueTail++] = i;
            used[i] = true;
        }
    }

    // Seed with registers used in PHI nodes
    // If a register is used by a Phi, its definition must be kept alive.
    if (prog->phiCount > 0 && prog->phiOperandValues != nullptr) {
        for (u32 i = 0; i < prog->phiCount; i++) {
            u32 count = prog->GetPhiOperandCount(i);
            for (u32 j = 0; j < count; j++) {
                u16 op = prog->GetPhiOperandValue(i, j);
                // If operand is a register (not a constant)
                if ((op & 0xC000) == 0 && op < prog->registerCount) {
                    u32 producer = registerWriters[op];
                    // Mark the instruction that produced this operand as used
                    if (producer != 0xFFFFFFFF && !used[producer]) {
                        used[producer] = true;
                        workQueue[queueTail++] = producer;
                    }
                }
            }
        }
    }

    // Process queue
    while (queueHead < queueTail) {
        u32 idx = workQueue[queueHead++];

        // Add operand producers to queue (check all 4 operands)
        for (u32 j = 0; j < 4; j++) {
            u16 op = prog->GetOperand(idx, j);
            if ((op & 0xC000) == 0 && op < prog->registerCount) {
                u32 producer = registerWriters[op];
                if (producer != 0xFFFFFFFF && !used[producer]) {
                    used[producer] = true;
                    workQueue[queueTail++] = producer;
                }
            }
        }
    }

    // Preserve merge/continue targets for structured control flow.
    if (prog->structureInfo) {
        for (u32 i = 0; i < prog->instructionCount; i++) {
            u32 info = prog->structureInfo[i];
            if (info == 0) continue;
            u32 mergeInst = info & IRProgram::STRUCT_TARGET_MASK;
            if (mergeInst < prog->instructionCount && !used[mergeInst]) {
                used[mergeInst] = true;
                workQueue[queueTail++] = mergeInst;
            }
            if ((info & IRProgram::STRUCT_TYPE_MASK) == IRProgram::STRUCT_LOOP_HEADER && prog->continueInfo) {
                u32 contInst = prog->continueInfo[i];
                if (contInst != 0xFFFFFFFF && contInst < prog->instructionCount && !used[contInst]) {
                    used[contInst] = true;
                    workQueue[queueTail++] = contInst;
                }
            }
        }
    }

    // Re-run queue for any newly marked instructions.
    while (queueHead < queueTail) {
        u32 idx = workQueue[queueHead++];
        for (u32 j = 0; j < 4; j++) {
            u16 op = prog->GetOperand(idx, j);
            if ((op & 0xC000) == 0 && op < prog->registerCount) {
                u32 producer = registerWriters[op];
                if (producer != 0xFFFFFFFF && !used[producer]) {
                    used[producer] = true;
                    workQueue[queueTail++] = producer;
                }
            }
        }
    }
        
        // Build instruction index remapping table
        u32* indexRemap = (u32*)alloca(prog->instructionCount * sizeof(u32));
        memset(indexRemap, 0xFF, prog->instructionCount * sizeof(u32));  // 0xFFFFFFFF = eliminated

        u32 newIdx = 0;
        for (u32 i = 0; i < prog->instructionCount; i++) {
            if (used[i]) {
                indexRemap[i] = newIdx++;
            }
        }

        // Compact arrays in parallel and update control flow metadata
        u32 writeIdx = 0;
        u32 oldCount = prog->instructionCount;
        for (u32 i = 0; i < oldCount; i++) {
            if (!used[i]) continue;

            if (writeIdx != i) {
                prog->opcodes[writeIdx] = prog->opcodes[i];
                prog->types[writeIdx] = prog->types[i];
                prog->flags[writeIdx] = prog->flags[i];
                prog->destinations[writeIdx] = prog->destinations[i];
                prog->operands[writeIdx * 4] = prog->operands[i * 4];
                prog->operands[writeIdx * 4 + 1] = prog->operands[i * 4 + 1];
                prog->operands[writeIdx * 4 + 2] = prog->operands[i * 4 + 2];
                prog->operands[writeIdx * 4 + 3] = prog->operands[i * 4 + 3];
            }

            u16 op = prog->opcodes[writeIdx];
            u32 meta = prog->metadata[i];
            if (op == OP_JUMP) {
                u32 newTarget = (meta < oldCount) ? indexRemap[meta] : meta;
                prog->metadata[writeIdx] = (newTarget != 0xFFFFFFFF) ? newTarget : writeIdx + 1;
            } else if (op == OP_BRANCH) {
                u32 oldFalse = meta >> 16;
                u32 oldTrue = meta & 0xFFFF;
                u32 newFalse = (oldFalse < oldCount) ? indexRemap[oldFalse] : oldFalse;
                u32 newTrue = (oldTrue < oldCount) ? indexRemap[oldTrue] : oldTrue;
                if (newFalse == 0xFFFFFFFF) newFalse = writeIdx + 1;
                if (newTrue == 0xFFFFFFFF) newTrue = writeIdx + 1;
                prog->metadata[writeIdx] = (newFalse << 16) | (newTrue & 0xFFFF);
            } else {
                prog->metadata[writeIdx] = meta;
            }

            if (prog->structureInfo) {
                u32 info = prog->structureInfo[i];
                if (info != 0) {
                    u32 type = info & IRProgram::STRUCT_TYPE_MASK;
                    u32 mergeInst = info & IRProgram::STRUCT_TARGET_MASK;
                    if (mergeInst < oldCount) {
                        u32 newMerge = indexRemap[mergeInst];
                        if (newMerge == 0xFFFFFFFF) newMerge = writeIdx + 1;
                        info = type | (newMerge & IRProgram::STRUCT_TARGET_MASK);
                    }
                }
                prog->structureInfo[writeIdx] = info;
            }

            if (prog->continueInfo) {
                u32 contInst = prog->continueInfo[i];
                if (contInst != 0xFFFFFFFF && contInst < oldCount) {
                    u32 newCont = indexRemap[contInst];
                    if (newCont == 0xFFFFFFFF) newCont = writeIdx + 1;
                    contInst = newCont;
                }
                prog->continueInfo[writeIdx] = contInst;
            }

            writeIdx++;
        }
        prog->instructionCount = writeIdx;
    }
    
    // Variant specialization using SoA
    void SpecializeForVariant(IRProgram* prog, u32 variantMask) {
        // Process all variant branches
        for (u32 i = 0; i < prog->variantBranchCount; i++) {
            if (prog->variantAttributeMasks[i] & variantMask) {
                u32 instIdx = prog->variantInstructionIndices[i];
                // Convert to unconditional jump
                prog->opcodes[instIdx] = OP_JUMP;
                prog->operands[instIdx * 4] = prog->variantTrueBranches[i];
            }
        }
    }
    
    // Attribute mask-based dead code elimination
    // Finds OP_LOAD_ATTR for attributes NOT in the mask and marks them as dead.
    // Then propagates this "poison" through dependent instructions.
    // 
    // The goal: When compiling with position-only (mask=0x01), any code that
    // loads the normal attribute (index=1) should be eliminated, along with
    // all instructions that depend on it.
    //
    // Returns: true if any instructions were eliminated
    bool EliminateUnavailableAttributes(IRProgram* prog, u32 attributeMask, bool debug = false) {
        if (prog->instructionCount == 0) return false;
        
        // Track which registers contain "poison" (from unavailable attributes)
        bool* poisonedRegisters = (bool*)alloca(prog->registerCount * sizeof(bool));
        memset(poisonedRegisters, 0, prog->registerCount * sizeof(bool));
        
        // Track which instructions should be eliminated
        bool* eliminate = (bool*)alloca(prog->instructionCount * sizeof(bool));
        memset(eliminate, 0, prog->instructionCount * sizeof(bool));
        
        // First pass: find OP_LOAD_ATTR for unavailable attributes
        for (u32 i = 0; i < prog->instructionCount; i++) {
            if (prog->opcodes[i] == OP_LOAD_ATTR) {
                u32 attrIdx = prog->GetOperand(i, 0);
                // Check if this attribute is NOT in the mask
                if ((attributeMask & (1 << attrIdx)) == 0) {
                    // This attribute is unavailable - mark as poison
                    u16 dest = prog->destinations[i];
                    if (dest < prog->registerCount) {
                        poisonedRegisters[dest] = true;
                    }
                    eliminate[i] = true;
                    if (debug) printf("        [Elim] Inst %u: LOAD_ATTR %u -> R%u poisoned\n", i, attrIdx, dest);
                }
            }
        }
        
        // Build register writers map for dependency tracking
        u32* registerWriters = (u32*)alloca(prog->registerCount * sizeof(u32));
        memset(registerWriters, 0xFF, prog->registerCount * sizeof(u32));
        for (u32 i = 0; i < prog->instructionCount; i++) {
            u16 dest = prog->destinations[i];
            if (dest < prog->registerCount) {
                registerWriters[dest] = i;
            }
        }
        
        // Propagate poison: any instruction that uses a poisoned register
        // also produces a poisoned result (and should be eliminated).
        // 
        // IMPORTANT: We do NOT poison a register just because ONE of its writers
        // uses poisoned input. We only poison a register if:
        // 1. The LOAD_ATTR that defines it is for an unavailable attribute
        // 2. ALL remaining (non-eliminated) writers to that register are poisoned
        //
        // For simplicity, we just mark instructions as eliminated if they use 
        // poisoned values, without poisoning their output registers.
        
        bool changed = true;
        while (changed) {
            changed = false;
            for (u32 i = 0; i < prog->instructionCount; i++) {
                if (eliminate[i]) continue;  // Already marked for elimination
                
                u16 opcode = prog->opcodes[i];
                
                // Skip instructions whose operands are NOT register references
                // LOAD_ATTR: operand[0] is attribute index, not a register
                // LOAD_UNIFORM/LOAD_BUFFER: may have buffer/offset as operands
                if (opcode == OP_LOAD_ATTR) continue;  // Already handled in first pass
                
                // Check if any operand is poisoned
                bool anyPoisoned = false;
                for (u32 j = 0; j < 3; j++) {
                    u16 op = prog->GetOperand(i, j);
                    // Only check register references (not constants or special values)
                    if ((op & 0x8000) == 0 && (op & 0x4000) == 0 && op < prog->registerCount) {
                        if (poisonedRegisters[op]) {
                            anyPoisoned = true;
                            break;
                        }
                    }
                }
                
                if (anyPoisoned) {
                    // This instruction uses a poisoned value
                    // If it's NOT an output instruction, mark for elimination
                    if (!IsOutputOpcode(prog->opcodes[i])) {
                        eliminate[i] = true;
                        changed = true;
                        
                        // When we eliminate an instruction, its destination register
                        // becomes undefined. Mark it as poisoned so that any remaining
                        // instruction that uses it can be handled (either eliminated
                        // or converted, e.g., branches).
                        u16 dest = prog->destinations[i];
                        if (dest < prog->registerCount && !poisonedRegisters[dest]) {
                            poisonedRegisters[dest] = true;
                            if (debug) printf("        [Elim] Inst %u: 0x%02X uses poison -> R%u poisoned\n", i, prog->opcodes[i], dest);
                            changed = true;
                        } else if (debug && dest < prog->registerCount) {
                            printf("        [Elim] Inst %u: 0x%02X uses poison (R%u already poisoned)\n", i, prog->opcodes[i], dest);
                        }
                    } else if (debug) {
                        printf("        [Keep] Inst %u: 0x%02X (output opcode, uses poison)\n", i, prog->opcodes[i]);
                    }
                    // Note: Output instructions with poisoned inputs are NOT eliminated.
                    // They keep their references to poisoned registers. The caller must
                    // ensure that any remaining register references are valid after this pass.
                }
            }
        }
        
        // Handle PHI nodes with poisoned operands
        // If a PHI has a poisoned operand, replace it with a non-poisoned operand value
        // or a default constant. This happens when SSA creates a PHI that merges
        // values from eliminated (unavailable attribute) code paths.
        if (prog->phiCount > 0 && prog->phiOperandOffsets != nullptr) {
            for (u32 phiIdx = 0; phiIdx < prog->phiCount; phiIdx++) {
                u32 operandCount = prog->GetPhiOperandCount(phiIdx);
                if (operandCount == 0) continue;
                
                // Find non-poisoned operand(s) and check for poison
                u16 nonPoisonedValue = 0xFFFF;
                bool hasPoison = false;
                
                for (u32 j = 0; j < operandCount; j++) {
                    u16 value = prog->GetPhiOperandValue(phiIdx, j);
                    
                    // Check if this operand is a poisoned register
                    if ((value & 0xC000) == 0 && value < prog->registerCount && poisonedRegisters[value]) {
                        hasPoison = true;
                    } else {
                        // Track first non-poisoned value for replacement
                        if (nonPoisonedValue == 0xFFFF) {
                            nonPoisonedValue = value;
                        }
                    }
                }
                
                // If PHI has poisoned operands, replace them with non-poisoned value
                if (hasPoison && nonPoisonedValue != 0xFFFF) {
                    for (u32 j = 0; j < operandCount; j++) {
                        u16 value = prog->GetPhiOperandValue(phiIdx, j);
                        if ((value & 0xC000) == 0 && value < prog->registerCount && poisonedRegisters[value]) {
                            prog->SetPhiOperandValue(phiIdx, j, nonPoisonedValue);
                            if (debug) printf("        [PHI %u] Replaced poisoned R%u with R%u\n", 
                                            phiIdx, value, nonPoisonedValue);
                        }
                    }
                }
            }
        }
        
        // Handle branches that depend on poisoned conditions
        for (u32 i = 0; i < prog->instructionCount; i++) {
            if (prog->opcodes[i] == OP_BRANCH) {
                // Conditional branch - operand 0 is condition
                u16 cond = prog->GetOperand(i, 0);
                if ((cond & 0x8000) == 0 && cond < prog->registerCount) {
                    if (poisonedRegisters[cond]) {
                        // The branch condition depends on unavailable attribute
                        // Convert to unconditional jump to the false branch
                        // (conservative: assume condition is false when attribute unavailable)
                        // 
                        // Branch targets are stored in metadata:
                        // metadata = (falseTarget << 16) | (trueTarget & 0xFFFF)
                        u32 meta = prog->metadata[i];
                        u32 falseTarget = meta >> 16;
                        
                        prog->opcodes[i] = OP_JUMP;
                        prog->metadata[i] = falseTarget;  // JUMP uses metadata for target
                        prog->SetOperand(i, 0, 0);
                        prog->SetOperand(i, 1, 0);
                        prog->SetOperand(i, 2, 0);
                        
                        // Mark branch condition as eliminated
                        eliminate[i] = false;  // Keep the JUMP
                    }
                }
            }
        }
        
        // Build instruction index remapping table (old index -> new index)
        u32* indexRemap = (u32*)alloca(prog->instructionCount * sizeof(u32));
        memset(indexRemap, 0xFF, prog->instructionCount * sizeof(u32));  // 0xFFFFFFFF = eliminated
        
        u32 newIdx = 0;
        for (u32 i = 0; i < prog->instructionCount; i++) {
            if (!eliminate[i]) {
                indexRemap[i] = newIdx++;
            }
        }
        
        // Compact the instruction array, updating jump/branch targets in metadata
        u32 writeIdx = 0;
        u32 oldCount = prog->instructionCount;
        bool anyEliminated = false;
        for (u32 i = 0; i < oldCount; i++) {
            if (!eliminate[i]) {
                if (writeIdx != i) {
                    prog->opcodes[writeIdx] = prog->opcodes[i];
                    prog->types[writeIdx] = prog->types[i];
                    prog->flags[writeIdx] = prog->flags[i];
                    prog->destinations[writeIdx] = prog->destinations[i];
                    prog->operands[writeIdx * 4] = prog->operands[i * 4];
                    prog->operands[writeIdx * 4 + 1] = prog->operands[i * 4 + 1];
                    prog->operands[writeIdx * 4 + 2] = prog->operands[i * 4 + 2];
                    prog->operands[writeIdx * 4 + 3] = prog->operands[i * 4 + 3];
                    
                    // Update metadata for control flow instructions
                    u32 meta = prog->metadata[i];
                    u16 op = prog->opcodes[writeIdx];
                    if (op == OP_JUMP) {
                        // JUMP: metadata is target instruction index
                        u32 newTarget = (meta < oldCount) ? indexRemap[meta] : meta;
                        prog->metadata[writeIdx] = (newTarget != 0xFFFFFFFF) ? newTarget : writeIdx + 1;
                    } else if (op == OP_BRANCH) {
                        // BRANCH: metadata = (falseTarget << 16) | trueTarget
                        u32 oldFalse = meta >> 16;
                        u32 oldTrue = meta & 0xFFFF;
                        u32 newFalse = (oldFalse < oldCount) ? indexRemap[oldFalse] : oldFalse;
                        u32 newTrue = (oldTrue < oldCount) ? indexRemap[oldTrue] : oldTrue;
                        // If target was eliminated, point to next instruction as fallback
                        if (newFalse == 0xFFFFFFFF) newFalse = writeIdx + 1;
                        if (newTrue == 0xFFFFFFFF) newTrue = writeIdx + 1;
                        prog->metadata[writeIdx] = (newFalse << 16) | (newTrue & 0xFFFF);
                    } else {
                        prog->metadata[writeIdx] = meta;
                    }
                } else {
                    // Even when not moving, may need to update targets
                    u16 op = prog->opcodes[writeIdx];
                    u32 meta = prog->metadata[writeIdx];
                    if (op == OP_JUMP) {
                        u32 newTarget = (meta < oldCount) ? indexRemap[meta] : meta;
                        prog->metadata[writeIdx] = (newTarget != 0xFFFFFFFF) ? newTarget : writeIdx + 1;
                    } else if (op == OP_BRANCH) {
                        u32 oldFalse = meta >> 16;
                        u32 oldTrue = meta & 0xFFFF;
                        u32 newFalse = (oldFalse < oldCount) ? indexRemap[oldFalse] : oldFalse;
                        u32 newTrue = (oldTrue < oldCount) ? indexRemap[oldTrue] : oldTrue;
                        if (newFalse == 0xFFFFFFFF) newFalse = writeIdx + 1;
                        if (newTrue == 0xFFFFFFFF) newTrue = writeIdx + 1;
                        prog->metadata[writeIdx] = (newFalse << 16) | (newTrue & 0xFFFF);
                    }
                }

                if (prog->structureInfo) {
                    u32 info = prog->structureInfo[i];
                    if (info != 0) {
                        u32 type = info & IRProgram::STRUCT_TYPE_MASK;
                        u32 mergeInst = info & IRProgram::STRUCT_TARGET_MASK;
                        if (mergeInst < oldCount) {
                            u32 newMerge = indexRemap[mergeInst];
                            if (newMerge == 0xFFFFFFFF) newMerge = writeIdx + 1;
                            info = type | (newMerge & IRProgram::STRUCT_TARGET_MASK);
                        }
                    }
                    prog->structureInfo[writeIdx] = info;
                }

                if (prog->continueInfo) {
                    u32 contInst = prog->continueInfo[i];
                    if (contInst != 0xFFFFFFFF && contInst < oldCount) {
                        u32 newCont = indexRemap[contInst];
                        if (newCont == 0xFFFFFFFF) newCont = writeIdx + 1;
                        contInst = newCont;
                    }
                    prog->continueInfo[writeIdx] = contInst;
                }
                writeIdx++;
            } else {
                anyEliminated = true;
            }
        }
        prog->instructionCount = writeIdx;
        
        return anyEliminated;
    }

    
    // Register pressure calculation using SoA
    void CalculateRegisterPressure(IRProgram* prog) {
        // First pass: find first use of each register
        memset(prog->registerLifetimes, 0xFF, prog->registerCount * sizeof(u32));
        
        for (u32 i = 0; i < prog->instructionCount; i++) {
            u16 dest = prog->destinations[i];
            if (dest < prog->registerCount) {
                u32 lifetime = prog->registerLifetimes[dest];
                u16 firstUse = lifetime >> 16;
                u16 lastUse = lifetime & 0xFFFF;
                
                if (firstUse == 0xFFFF) firstUse = i;
                lastUse = i;
                
                prog->registerLifetimes[dest] = (firstUse << 16) | lastUse;
            }
            
            // Check operands
            for (u32 j = 0; j < 3; j++) {
                u16 op = prog->GetOperand(i, j);
                if ((op & 0x8000) == 0 && op < prog->registerCount) {
                    u32 lifetime = prog->registerLifetimes[op];
                    u16 firstUse = lifetime >> 16;
                    u16 lastUse = lifetime & 0xFFFF;
                    
                    if (firstUse == 0xFFFF) firstUse = i;
                    lastUse = i;
                    
                    prog->registerLifetimes[op] = (firstUse << 16) | lastUse;
                }
            }
        }
    }
    
    // CFG-aware dead block elimination
    // Marks instructions in unreachable blocks as dead, then runs standard DCE
    // Implementation in bwsl_ir_gen.cpp (requires full CFG definition)
    void EliminateDeadBlocks(IRProgram* prog, BWSL::CFG* cfg);
};




}
}
