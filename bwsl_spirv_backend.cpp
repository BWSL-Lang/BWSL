#include "bwsl_spirv_backend.h"
#include "bwsl_ir_analysis.h"
#include <cstring>
#include "bwsl_utils.h"
#include "vendor/SPIRV-Headers/include/spirv/unified1/GLSL.std.450.h"

namespace BWSL {

// ============= Static lookup tables =============
// This table maps BWSL IR Opcodes to core SPIR-V opcodes.
// When an opcode maps to `spv::OpExtInst`, it indicates that the instruction
// belongs to an extended instruction set (like GLSL.std.450).
// The specific extended instruction must then be looked up in the parallel table.
static constexpr spv::Op IR_TO_SPV_OP_TABLE[256] = {

    // ========== Control Flow ==========
    [IR::OP_NOP]           = spv::OpNop,
    [IR::OP_JUMP]          = spv::OpBranch,
    [IR::OP_BRANCH]        = spv::OpBranchConditional,
    [IR::OP_CALL]          = spv::OpFunctionCall,
    [IR::OP_RET]           = spv::OpReturn, // Can also be spv::OpReturnValue
    [IR::OP_SELECT]        = spv::OpSelect,
    [IR::OP_PHI]           = spv::OpPhi,
    [IR::OP_SWITCH]        = spv::OpSwitch,

    // ========== Memory Operations (most map to Load/Store with different Storage Classes) ==========
    [IR::OP_LOAD_CONST]    = spv::OpConstant, // This is a declaration, not a load instruction
    [IR::OP_LOAD_REG]      = spv::OpCopyObject, // Or often optimized away in SSA
    [IR::OP_STORE_REG]     = spv::OpStore,
    [IR::OP_LOAD_ATTR]     = spv::OpLoad,
    [IR::OP_STORE_OUTPUT]  = spv::OpStore,
    [IR::OP_LOAD_UNIFORM]  = spv::OpLoad,
    [IR::OP_LOAD_BUFFER]   = spv::OpLoad,
    [IR::OP_STORE_BUFFER]  = spv::OpStore,
    [IR::OP_LOAD_LOCAL]    = spv::OpLoad,
    [IR::OP_STORE_LOCAL]   = spv::OpStore,
    [IR::OP_LOAD_SHARED]   = spv::OpLoad,
    [IR::OP_STORE_SHARED]  = spv::OpStore,
    [IR::OP_LOAD_INPUT]    = spv::OpLoad,  // Fragment input varying
 // ========== Arithmetic (Float) ==========
    [IR::OP_FADD]          = spv::OpFAdd,
    [IR::OP_FSUB]          = spv::OpFSub,
    [IR::OP_FMUL]          = spv::OpFMul,
    [IR::OP_FDIV]          = spv::OpFDiv,
    [IR::OP_FMOD]          = spv::OpFMod,
    [IR::OP_FNEG]          = spv::OpFNegate,
    [IR::OP_FABS]          = spv::OpExtInst, // Use GLSL.std.450 table
    [IR::OP_FMIN]          = spv::OpExtInst, // Use GLSL.std.450 table
    [IR::OP_FMAX]          = spv::OpExtInst, // Use GLSL.std.450 table
    [IR::OP_FCLAMP]        = spv::OpExtInst, // Use GLSL.std.450 table
    [IR::OP_FLOOR]         = spv::OpExtInst, // Use GLSL.std.450 table
    [IR::OP_CEIL]          = spv::OpExtInst, // Use GLSL.std.450 table
    [IR::OP_ROUND]         = spv::OpExtInst, // Use GLSL.std.450 table
    [IR::OP_TRUNC]         = spv::OpExtInst, // GLSLstd450Trunc
    [IR::OP_FRACT]         = spv::OpExtInst, // Use GLSL.std.450 table
    [IR::OP_FMA]           = spv::OpExtInst, // GLSLstd450Fma

    // ========== Arithmetic (Integer) ==========
    [IR::OP_IADD]          = spv::OpIAdd,
    [IR::OP_ISUB]          = spv::OpISub,
    [IR::OP_IMUL]          = spv::OpIMul,
    [IR::OP_IDIV]          = spv::OpSDiv,
    [IR::OP_IMOD]          = spv::OpSMod,
    [IR::OP_INEG]          = spv::OpSNegate,
    [IR::OP_IABS]          = spv::OpExtInst, // Use GLSL.std.450 table
    [IR::OP_IMIN]          = spv::OpExtInst, // Use GLSL.std.450 table
    [IR::OP_IMAX]          = spv::OpExtInst, // Use GLSL.std.450 table
    [IR::OP_ICLAMP]        = spv::OpExtInst, // Use GLSL.std.450 table
    [IR::OP_UMIN]          = spv::OpExtInst, // Use GLSL.std.450 table
    [IR::OP_UMAX]          = spv::OpExtInst, // Use GLSL.std.450 table
    [IR::OP_UCLAMP]        = spv::OpExtInst, // Use GLSL.std.450 table

    // ========== Bitwise ==========
    [IR::OP_CLZ]           = spv::OpExtInst, // Use GLSL.std.450 table
    [IR::OP_CTZ]           = spv::OpExtInst, // Use GLSL.std.450 table
    
    // ========== Type Conversion ==========
    [IR::OP_SIGN]          = spv::OpExtInst, // Use GLSL.std.450 table

    // ========== Math Functions ==========
    [IR::OP_SQRT]          = spv::OpExtInst, // this gets looked up via the bwsl std lib
    [IR::OP_RSQRT]         = spv::OpExtInst,
    [IR::OP_POW]           = spv::OpExtInst,
    [IR::OP_EXP]           = spv::OpExtInst,
    [IR::OP_EXP2]          = spv::OpExtInst,
    [IR::OP_LOG]           = spv::OpExtInst,
    [IR::OP_LOG2]          = spv::OpExtInst,
    [IR::OP_SIN]           = spv::OpExtInst,
    [IR::OP_COS]           = spv::OpExtInst,
    [IR::OP_TAN]           = spv::OpExtInst,
    [IR::OP_ASIN]          = spv::OpExtInst,
    [IR::OP_ACOS]          = spv::OpExtInst,
    [IR::OP_ATAN]          = spv::OpExtInst,
    [IR::OP_ATAN2]         = spv::OpExtInst,
    [IR::OP_SINH]          = spv::OpExtInst,
    [IR::OP_COSH]          = spv::OpExtInst,

    // ========== Geometric ==========
    [IR::OP_DOT]           = spv::OpDot,
    [IR::OP_CROSS]         = spv::OpExtInst,
    [IR::OP_LENGTH]        = spv::OpExtInst,
    [IR::OP_NORMALIZE]     = spv::OpExtInst,
    [IR::OP_DISTANCE]      = spv::OpExtInst,
    [IR::OP_REFLECT]       = spv::OpExtInst,
    [IR::OP_REFRACT]       = spv::OpExtInst,
    [IR::OP_FACEFORWARD]   = spv::OpExtInst,

    // ========== Matrix ==========
    [IR::OP_MAT_MUL]       = spv::OpMatrixTimesMatrix,
    [IR::OP_MAT_TRANSPOSE] = spv::OpTranspose,
    [IR::OP_MAT_INVERSE]   = spv::OpExtInst,
    [IR::OP_MAT_DET]       = spv::OpExtInst,
    [IR::OP_MAT_CONSTRUCT] = spv::OpCompositeConstruct,
    [IR::OP_MAT_VEC_MUL]   = spv::OpMatrixTimesVector,
    [IR::OP_VEC_MAT_MUL]   = spv::OpVectorTimesMatrix,
    [IR::OP_MAT_SCALE]     = spv::OpMatrixTimesScalar,

    // ========== Interpolation ==========
    [IR::OP_LERP]          = spv::OpExtInst,
    [IR::OP_SMOOTHSTEP]    = spv::OpExtInst,
    [IR::OP_STEP]          = spv::OpExtInst,
    [IR::OP_SATURATE]      = spv::OpExtInst, // Maps to FClamp(x, 0, 1)
    [IR::OP_DEGREES]       = spv::OpExtInst,
    [IR::OP_RADIANS]       = spv::OpExtInst,

    // ========== Atomics ==========
    [IR::OP_ATOMIC_ADD]      = spv::OpAtomicIAdd,
    [IR::OP_ATOMIC_SUB]      = spv::OpAtomicISub,
    [IR::OP_ATOMIC_MIN]      = spv::OpAtomicSMin, // Or UMin
    [IR::OP_ATOMIC_MAX]      = spv::OpAtomicSMax, // Or UMax
    [IR::OP_ATOMIC_AND]      = spv::OpAtomicAnd,
    [IR::OP_ATOMIC_OR]       = spv::OpAtomicOr,
    [IR::OP_ATOMIC_XOR]      = spv::OpAtomicXor,
    [IR::OP_ATOMIC_XCHG]     = spv::OpAtomicExchange,
    [IR::OP_ATOMIC_CMP_XCHG] = spv::OpAtomicCompareExchange,
    
    // ========== Synchronization ==========
    [IR::OP_BARRIER]         = spv::OpControlBarrier,
    [IR::OP_MEM_FENCE]       = spv::OpMemoryBarrier,

    // ========== Derivatives (Fragment only) ==========
    [IR::OP_DDX]             = spv::OpDPdx,
    [IR::OP_DDY]             = spv::OpDPdy,
    [IR::OP_DDX_FINE]        = spv::OpDPdxFine,
    [IR::OP_DDY_FINE]        = spv::OpDPdyFine,
    [IR::OP_DDX_COARSE]      = spv::OpDPdxCoarse,
    [IR::OP_DDY_COARSE]      = spv::OpDPdyCoarse,
    [IR::OP_FWIDTH]          = spv::OpFwidth,

    // ========== Wave/SIMD/Subgroup Operations (SPIR-V 1.3+) ==========
    // Using raw opcode values since these are not in SPIR-V 1.2 header
    [IR::OP_WAVE_MIN]        = static_cast<spv::Op>(358), // OpGroupNonUniformSMin
    [IR::OP_WAVE_MAX]        = static_cast<spv::Op>(359), // OpGroupNonUniformSMax
    [IR::OP_WAVE_ALL]        = static_cast<spv::Op>(334), // OpGroupNonUniformAll
    [IR::OP_WAVE_ANY]        = static_cast<spv::Op>(335), // OpGroupNonUniformAny
    [IR::OP_WAVE_BALLOT]     = static_cast<spv::Op>(333), // OpGroupNonUniformBallot
    [IR::OP_WAVE_READ_FIRST] = static_cast<spv::Op>(339), // OpGroupNonUniformBroadcastFirst
    [IR::OP_WAVE_READ_LANE]  = static_cast<spv::Op>(337), // OpGroupNonUniformBroadcast
    [IR::OP_WAVE_SUM]        = static_cast<spv::Op>(349), // OpGroupNonUniformIAdd
    [IR::OP_WAVE_MUL]        = static_cast<spv::Op>(353), // OpGroupNonUniformIMul
};

// A sentinel value to indicate that an IR OpCode does not map to a
// GLSL.std.450 extended instruction.
constexpr u32 NO_EXT_INST = 0xFFFFFFFF;

// This table maps BWSL IR Opcodes to their corresponding GLSL.std.450 enum value.
// It should only be accessed if the IR_TO_SPV_OP_TABLE entry for the same
// opcode is `spv::OpExtInst`.
static constexpr u32 IR_TO_GLSL_STD_450_TABLE[256] = {
    // Default all to no-op
    NO_EXT_INST,

    // ========== Arithmetic (Float) ==========
    [IR::OP_FABS]          = GLSLstd450FAbs,
    [IR::OP_FMIN]          = GLSLstd450FMin,
    [IR::OP_FMAX]          = GLSLstd450FMax,
    [IR::OP_FCLAMP]        = GLSLstd450FClamp,
    [IR::OP_FLOOR]         = GLSLstd450Floor,
    [IR::OP_CEIL]          = GLSLstd450Ceil,
    [IR::OP_ROUND]         = GLSLstd450RoundEven,
    [IR::OP_FRACT]         = GLSLstd450Fract,

    // ========== Arithmetic (Integer) ==========
    [IR::OP_IABS]          = GLSLstd450SAbs,
    [IR::OP_IMIN]          = GLSLstd450SMin,
    [IR::OP_IMAX]          = GLSLstd450SMax,
    [IR::OP_ICLAMP]        = GLSLstd450SClamp,
    [IR::OP_UMIN]          = GLSLstd450UMin,
    [IR::OP_UMAX]          = GLSLstd450UMax,
    [IR::OP_UCLAMP]        = GLSLstd450UClamp,

    // ========== Bitwise ==========
    [IR::OP_CLZ]           = GLSLstd450FindUMsb, // Note: FindUMsb gives position, needs conversion for CLZ.
    [IR::OP_CTZ]           = GLSLstd450FindILsb,

    // ========== Type Conversion ==========
    [IR::OP_SIGN]          = GLSLstd450FSign,

    // ========== Math Functions ==========
    [IR::OP_SQRT]          = GLSLstd450Sqrt,
    [IR::OP_RSQRT]         = GLSLstd450InverseSqrt,
    [IR::OP_POW]           = GLSLstd450Pow,
    [IR::OP_EXP]           = GLSLstd450Exp,
    [IR::OP_EXP2]          = GLSLstd450Exp2,
    [IR::OP_LOG]           = GLSLstd450Log,
    [IR::OP_LOG2]          = GLSLstd450Log2,
    [IR::OP_SIN]           = GLSLstd450Sin,
    [IR::OP_COS]           = GLSLstd450Cos,
    [IR::OP_TAN]           = GLSLstd450Tan,
    [IR::OP_ASIN]          = GLSLstd450Asin,
    [IR::OP_ACOS]          = GLSLstd450Acos,
    [IR::OP_ATAN]          = GLSLstd450Atan,
    [IR::OP_ATAN2]         = GLSLstd450Atan2,
    [IR::OP_SINH]          = GLSLstd450Sinh,
    [IR::OP_COSH]          = GLSLstd450Cosh,

    // ========== Geometric ==========
    [IR::OP_CROSS]         = GLSLstd450Cross,
    [IR::OP_LENGTH]        = GLSLstd450Length,
    [IR::OP_NORMALIZE]     = GLSLstd450Normalize,
    [IR::OP_DISTANCE]      = GLSLstd450Distance,
    [IR::OP_REFLECT]       = GLSLstd450Reflect,
    [IR::OP_REFRACT]       = GLSLstd450Refract,
    [IR::OP_FACEFORWARD]   = GLSLstd450FaceForward,
    
    // ========== Matrix ==========
    [IR::OP_MAT_INVERSE]   = GLSLstd450MatrixInverse,
    [IR::OP_MAT_DET]       = GLSLstd450Determinant,

    // ========== Interpolation ==========
    [IR::OP_LERP]          = GLSLstd450FMix,
    [IR::OP_SMOOTHSTEP]    = GLSLstd450SmoothStep,
    [IR::OP_STEP]          = GLSLstd450Step,
    [IR::OP_SATURATE]      = GLSLstd450FClamp, // Special case: emit with (x, 0, 1)
    [IR::OP_DEGREES]       = GLSLstd450Degrees,
    [IR::OP_RADIANS]       = GLSLstd450Radians,
};

// ============= Initialization =============
void SPIRVBuilder::Initialize(BWSL_Arena* arena, IR::IRProgram* ir, ShaderStage stage,
                               const SymbolTableData* symbols, CFG* cfg) {
    this->arena = arena;
    this->ir = ir;
    this->stage = stage;
    this->symbols = symbols;
    this->cfg = cfg;
    workgroupSizeX = 1;
    workgroupSizeY = 1;
    workgroupSizeZ = 1;
    
    // Analyze IR to determine capabilities, resources, and I/O requirements
    AnalyzeIR(&analysis, ir);

    // Initialize ID management
    nextId = 1;
    idCapacity = ir->registerCount + 256;
    spirvIds = (u32*)arena->Allocate(idCapacity * sizeof(u32), 64);
    idTypes = (u16*)arena->Allocate(idCapacity * sizeof(u16), 64);
    idDecorations = (u32*)arena->Allocate(idCapacity * sizeof(u32), 64);
    hasPreAllocatedId = (bool*)arena->Allocate(idCapacity * sizeof(bool), 64);
    memset(spirvIds, 0, idCapacity * sizeof(u32));
    memset(idTypes, 0, idCapacity * sizeof(u16));
    memset(idDecorations, 0, idCapacity * sizeof(u32));
    memset(hasPreAllocatedId, 0, idCapacity * sizeof(bool));
    
    // Initialize type arrays
    memset(typeIds, 0, sizeof(typeIds));
    compositeTypeCount = 0;
    compositeTypeIds = (u32*)arena->Allocate(256 * sizeof(u32), 64);
    compositeTypeHashes = (u32*)arena->Allocate(256 * sizeof(u32), 64);

    // Initialize struct type tracking
    structTypeCount = 0;
    structTypeIds = (u32*)arena->Allocate(64 * sizeof(u32), 64);
    structTypeHashes = (u32*)arena->Allocate(64 * sizeof(u32), 64);
    memset(structTypeIds, 0, 64 * sizeof(u32));
    memset(structTypeHashes, 0, 64 * sizeof(u32));

    // Initialize struct field type IDs (64 structs * 32 fields each)
    structFieldTypeIds = (u32*)arena->Allocate(64 * MAX_FIELDS_PER_STRUCT * sizeof(u32), 64);
    memset(structFieldTypeIds, 0, 64 * MAX_FIELDS_PER_STRUCT * sizeof(u32));
    
    // Initialize constant pools
    constantCount = 0;
    floatConstantIds = (u32*)arena->Allocate(ir->floatCount * sizeof(u32), 64);
    intConstantIds = (u32*)arena->Allocate(ir->intCount * sizeof(u32), 64);
    uintConstantIds = (u32*)arena->Allocate(ir->intCount * sizeof(u32), 64);
    constantHashes = (u32*)arena->Allocate((ir->floatCount + ir->intCount) * sizeof(u32), 64);
    memset(floatConstantIds, 0, ir->floatCount * sizeof(u32));
    memset(intConstantIds, 0, ir->intCount * sizeof(u32));
    memset(uintConstantIds, 0, ir->intCount * sizeof(u32));
    
    // Initialize sections
    auto initSection = [arena](Section* s, u32 initial_capacity) {
        s->words = (u32*)arena->Allocate(initial_capacity * sizeof(u32), 64);
        s->count = 0;
        s->capacity = initial_capacity;
    };
    
    initSection(&capabilities, 32);
    initSection(&extensions, 32);
    initSection(&extInstImports, 32);
    initSection(&memoryModel, 8);
    initSection(&entryPoints, 64);
    initSection(&executionModes, 32);
    initSection(&debugNames, 256);
    initSection(&decorations, 256);
    initSection(&typesConstants, 512);
    initSection(&globals, 128);
    initSection(&functions, 2048);
    
    // Initialize current function buffer
    currentFunctionCapacity = 512;
    currentFunction = (u32*)arena->Allocate(currentFunctionCapacity * sizeof(u32), 64);
    currentFunctionSize = 0;
    
    // Initialize block management
    blockCount = 0;
    blockLabels = (u32*)arena->Allocate(256 * sizeof(u32), 64);
    blockIRIndices = (u32*)arena->Allocate(256 * sizeof(u32), 64);
    blockMergePoints = (u32*)arena->Allocate(256 * sizeof(u32), 64);
    memset(blockLabels, 0, 256 * sizeof(u32));
    
    // Initialize interface variables
    inputCount = outputCount = 0;
    inputIds = (u32*)arena->Allocate(32 * sizeof(u32), 64);
    inputLocations = (u8*)arena->Allocate(32 * sizeof(u8), 64);
    outputIds = (u32*)arena->Allocate(32 * sizeof(u32), 64);
    outputLocations = (u8*)arena->Allocate(32 * sizeof(u8), 64);
    
    // Initialize resource bindings
    resourceCount = 0;
    uniformBufferIds = (u32*)arena->Allocate(32 * sizeof(u32), 64);
    textureIds = (u32*)arena->Allocate(32 * sizeof(u32), 64);
    samplerIds = (u32*)arena->Allocate(32 * sizeof(u32), 64);
    storageBufferIds = (u32*)arena->Allocate(32 * sizeof(u32), 64);
    bindingSets = (u8*)arena->Allocate(128 * sizeof(u8), 64);
    bindingIndices = (u8*)arena->Allocate(128 * sizeof(u8), 64);
    
    // Allocate entry point and import extended instruction set
    entryPointId = AllocateId();
    glslStd450Id = AllocateId();
    
    // Emit the preamble immediately
    EmitPreamble();
}

// ============= Preamble Emission =============
void SPIRVBuilder::EmitPreamble() {
    // 1. Capabilities - emit based on IR analysis results
    
    // Base shader capability - required for all shaders
    EmitCapability(spv::CapabilityShader);
    
    // Derivative capabilities (based on IR analysis)
    if (analysis.HasAny(IRAnalysis::CAP_DERIVATIVES | 
                        IRAnalysis::CAP_FINE_DERIVATIVES | 
                        IRAnalysis::CAP_COARSE_DERIVATIVES)) {
        EmitCapability(spv::CapabilityDerivativeControl);
    }
    
    // Wave/subgroup operations (SPIR-V 1.3+)
    if (analysis.Has(IRAnalysis::CAP_WAVE_OPS)) {
        // GroupNonUniform = 61, GroupNonUniformArithmetic = 63, GroupNonUniformBallot = 64
        EmitCapability(static_cast<spv::Capability>(61));  // GroupNonUniform
        EmitCapability(static_cast<spv::Capability>(63));  // GroupNonUniformArithmetic
        EmitCapability(static_cast<spv::Capability>(64));  // GroupNonUniformBallot
    }
    
    // Atomic operations
    if (analysis.Has(IRAnalysis::CAP_ATOMICS)) {
        // AtomicStorage for buffer atomics
        if (analysis.Has(IRAnalysis::CAP_STORAGE_BUFFER)) {
            EmitCapability(spv::CapabilityAtomicStorage);
        }
    }
    
    // Image read/write capabilities
    if (analysis.Has(IRAnalysis::CAP_IMAGE_STORE)) {
        EmitCapability(spv::CapabilityStorageImageWriteWithoutFormat);
    }
    if (analysis.Has(IRAnalysis::CAP_IMAGE_LOAD)) {
        EmitCapability(spv::CapabilityStorageImageReadWithoutFormat);
    }
    
    // 64-bit types
    if (analysis.Has(IRAnalysis::CAP_INT64)) {
        EmitCapability(spv::CapabilityInt64);
    }
    if (analysis.Has(IRAnalysis::CAP_FLOAT64)) {
        EmitCapability(spv::CapabilityFloat64);
    }
    
    // Storage buffer capability (for buffer load/store)
    // Note: For SPIR-V 1.2+, storage buffers use StorageBuffer storage class
    // which requires SPV_KHR_storage_buffer_storage_class extension
    // We don't need StorageBuffer16BitAccess unless we actually use 16-bit types
    
    // Texture sampling capabilities
    if (analysis.Has(IRAnalysis::CAP_SAMPLED_1D)) {
        EmitCapability(spv::CapabilitySampled1D);
    }
    if (analysis.Has(IRAnalysis::CAP_IMAGE_1D)) {
        EmitCapability(spv::CapabilityImage1D);
    }
    if (analysis.Has(IRAnalysis::CAP_SAMPLED_CUBE)) {
        EmitCapability(spv::CapabilitySampledCubeArray);
    }
    
    // Clip/cull distance
    if (analysis.Has(IRAnalysis::CAP_CLIP_DISTANCE)) {
        EmitCapability(spv::CapabilityClipDistance);
    }
    if (analysis.Has(IRAnalysis::CAP_CULL_DISTANCE)) {
        EmitCapability(spv::CapabilityCullDistance);
    }
    
    // Storage buffer extension for vertex pulling modes or any storage buffer usage
    // SPIR-V 1.2 requires this extension for StorageBuffer storage class
    if (vertexPullingConfig.mode == VertexInputMode::SeparateBuffers ||
        vertexPullingConfig.mode == VertexInputMode::UnifiedWithOffsets ||
        analysis.Has(IRAnalysis::CAP_STORAGE_BUFFER)) {
        EmitExtension("SPV_KHR_storage_buffer_storage_class");
    }
    
    // 2. Import GLSL.std.450 extended instruction set
    // OpExtInstImport format: word_count | op, result_id, "GLSL.std.450\0"
    const char* extName = "GLSL.std.450";
    u32 nameLen = strlen(extName) + 1;  // Include null terminator
    u32 nameWords = (nameLen + 3) / 4;  // Round up to word boundary
    
    u32 wordCount = 2 + nameWords;  // op + result + name words
    
    // Grow section if needed
    if (extInstImports.count + wordCount > extInstImports.capacity) {
        GrowSection(&extInstImports);
    }
    
    extInstImports.words[extInstImports.count++] = (wordCount << 16) | spv::OpExtInstImport;
    extInstImports.words[extInstImports.count++] = glslStd450Id;
    
    // Copy name as words (with padding)
    u32* namePtr = &extInstImports.words[extInstImports.count];
    memset(namePtr, 0, nameWords * 4);  // Zero-fill for padding
    memcpy(namePtr, extName, nameLen);
    extInstImports.count += nameWords;
    
    // 3. Memory Model
    // OpMemoryModel Logical GLSL450
    u32 memModelOps[] = {spv::AddressingModelLogical, spv::MemoryModelGLSL450};
    EmitToSection(&memoryModel, spv::OpMemoryModel, memModelOps, 2);
}

void SPIRVBuilder::EmitEntryPoint() {
    // Determine execution model based on shader stage
    spv::ExecutionModel execModel;
    const char* entryName = "main";
    
    switch (stage) {
        case ShaderStage::Vertex:
            execModel = spv::ExecutionModelVertex;
            break;
        case ShaderStage::Fragment:
            execModel = spv::ExecutionModelFragment;
            break;
        case ShaderStage::Compute:
            execModel = spv::ExecutionModelGLCompute;
            break;
        default:
            execModel = spv::ExecutionModelVertex;
            break;
    }
    
    // OpEntryPoint format: exec_model, entry_point_id, "name", interface_vars...
    u32 nameLen = strlen(entryName) + 1;
    u32 nameWords = (nameLen + 3) / 4;
    
    // Count interface variables (inputs + outputs)
    u32 interfaceCount = inputCount + outputCount;
    u32 wordCount = 3 + nameWords + interfaceCount;  // op + exec_model + entry_id + name + interfaces
    
    if (entryPoints.count + wordCount > entryPoints.capacity) {
        GrowSection(&entryPoints);
    }
    
    entryPoints.words[entryPoints.count++] = (wordCount << 16) | spv::OpEntryPoint;
    entryPoints.words[entryPoints.count++] = execModel;
    entryPoints.words[entryPoints.count++] = entryPointId;
    
    // Copy name
    u32* namePtr = &entryPoints.words[entryPoints.count];
    memset(namePtr, 0, nameWords * 4);
    memcpy(namePtr, entryName, nameLen);
    entryPoints.count += nameWords;
    
    // Add interface variable IDs
    for (u32 i = 0; i < inputCount; i++) {
        entryPoints.words[entryPoints.count++] = inputIds[i];
    }
    for (u32 i = 0; i < outputCount; i++) {
        entryPoints.words[entryPoints.count++] = outputIds[i];
    }
    
    // Emit execution modes
    switch (stage) {
        case ShaderStage::Fragment:
            // OriginUpperLeft - standard for Vulkan
            {
                u32 execModeOps[] = {entryPointId, spv::ExecutionModeOriginUpperLeft};
                EmitToSection(&executionModes, spv::OpExecutionMode, execModeOps, 2);
            }
            break;
        case ShaderStage::Compute:
            // LocalSize - workgroup size (default 1,1,1 - should be configurable)
            {
                u32 execModeOps[] = {
                    entryPointId,
                    spv::ExecutionModeLocalSize,
                    workgroupSizeX,
                    workgroupSizeY,
                    workgroupSizeZ
                };
                EmitToSection(&executionModes, spv::OpExecutionMode, execModeOps, 5);
            }
            break;
        default:
            break;
    }
}

// ============= Type Management =============
u32 SPIRVBuilder::GetVectorTypeId(CoreType base, u32 components) {
    // Check if we can use predefined types
    if (base == CoreType::FLOAT) {
        switch(components) {
            case 2: return GetTypeId(CoreType::FLOAT2);
            case 3: return GetTypeId(CoreType::FLOAT3);
            case 4: return GetTypeId(CoreType::FLOAT4);
        }
    }
    // TODO: Handle other base types and dynamic vector creation
    return 0;
}

u32 SPIRVBuilder::GetPointerTypeId(u32 type_id, spv::StorageClass storage) {
    // Hash the pointer type for deduplication
    u32 hash = type_id ^ (static_cast<u32>(storage) << 16);
    
    // Check composite types for existing pointer
    for (u32 i = 0; i < compositeTypeCount; i++) {
        if (compositeTypeHashes[i] == hash) {
            return compositeTypeIds[i];
        }
    }
    
    // Create new pointer type
    u32 ptr_id = AllocateId();
    u32 ops[] = {ptr_id, storage, type_id};
    EmitToSection(&typesConstants, spv::OpTypePointer, ops, 3);
    
    // Cache it
    compositeTypeIds[compositeTypeCount] = ptr_id;
    compositeTypeHashes[compositeTypeCount] = hash;
    compositeTypeCount++;
    
    return ptr_id;
}

u32 SPIRVBuilder::GetFunctionTypeId(u32 return_type, u32* param_types, u32 param_count) {
    // TODO: Implement function type creation and deduplication
    u32 func_type_id = AllocateId();

    // Build operands: type_id, return_type, param0, param1, ...
    u32* ops = (u32*)arena->Allocate((2 + param_count) * sizeof(u32));
    ops[0] = func_type_id;
    ops[1] = return_type;
    memcpy(&ops[2], param_types, param_count * sizeof(u32));

    EmitToSection(&typesConstants, spv::OpTypeFunction, ops, 2 + param_count);
    return func_type_id;
}

u32 SPIRVBuilder::GetStructTypeId(u32 structTypeHash) {
    // Check if we already have this struct type
    for (u32 i = 0; i < structTypeCount; i++) {
        if (structTypeHashes[i] == structTypeHash && structTypeIds[i] != 0) {
            return structTypeIds[i];
        }
    }

    // Find the struct type info in IR
    if (!ir || !ir->structTypes) {
        return 0;
    }

    const IR::IRProgram::StructTypeInfo* structInfo = nullptr;
    u32 structIdx = 0;
    for (u32 i = 0; i < ir->structTypeCount; i++) {
        if (ir->structTypes[i].nameHash == structTypeHash) {
            structInfo = &ir->structTypes[i];
            structIdx = i;
            break;
        }
    }

    if (!structInfo || structInfo->fieldCount == 0) {
        return 0;
    }

    // Allocate the struct type ID
    u32 struct_type_id = AllocateId();

    // Build OpTypeStruct: result_id, member_type_0, member_type_1, ...
    u32 fieldCount = structInfo->fieldCount;
    u32* ops = (u32*)arena->Allocate((1 + fieldCount) * sizeof(u32));
    ops[0] = struct_type_id;

    // Get SPIR-V type IDs for each field, handling arrays
    for (u32 i = 0; i < fieldCount; i++) {
        CoreType fieldType = static_cast<CoreType>(ir->structFieldTypes[structInfo->fieldOffset + i]);
        u32 fieldTypeId = 0;
        if ((fieldType == CoreType::CUSTOM || fieldType == CoreType::ENUM) &&
            ir->structFieldTypeHashes) {
            u32 fieldTypeHash = ir->structFieldTypeHashes[structInfo->fieldOffset + i];
            if (fieldTypeHash != 0) {
                fieldTypeId = GetStructTypeId(fieldTypeHash);
            }
        } else {
            fieldTypeId = GetTypeId(fieldType);
        }
        if (fieldTypeId == 0) {
            // Fallback to float to keep the type graph valid
            fieldTypeId = GetTypeId(CoreType::FLOAT);
        }

        // Check if this field is an array
        u32 arraySize = 0;
        if (ir->structFieldArraySizes) {
            arraySize = ir->structFieldArraySizes[structInfo->fieldOffset + i];
        }

        if (arraySize > 0) {
            // Create an array type for this field
            u32 arrayTypeId = AllocateId();
            u32 lengthConstId = GetIntConstantId(arraySize, true);  // unsigned constant
            u32 arrayOps[] = {arrayTypeId, fieldTypeId, lengthConstId};
            EmitToSection(&typesConstants, spv::OpTypeArray, arrayOps, 3);

            // Emit ArrayStride decoration (std140: vec3 stride is 16 bytes)
            u32 elementSize = 0;
            switch (fieldType) {
                case CoreType::FLOAT: elementSize = 4; break;
                case CoreType::FLOAT2: elementSize = 8; break;
                case CoreType::FLOAT3: elementSize = 16; break;  // std140 rounds up to 16
                case CoreType::FLOAT4: elementSize = 16; break;
                case CoreType::INT: elementSize = 4; break;
                case CoreType::INT2: elementSize = 8; break;
                case CoreType::INT3: elementSize = 16; break;
                case CoreType::INT4: elementSize = 16; break;
                case CoreType::UINT: elementSize = 4; break;
                case CoreType::MAT4: elementSize = 64; break;
                default: elementSize = 16; break;
            }
            u32 stride_ops[] = {arrayTypeId, spv::DecorationArrayStride, elementSize};
            EmitToSection(&decorations, spv::OpDecorate, stride_ops, 3);

            ops[1 + i] = arrayTypeId;
        } else {
            ops[1 + i] = fieldTypeId;
        }
    }

    EmitToSection(&typesConstants, spv::OpTypeStruct, ops, 1 + fieldCount);

    // Emit struct name for debugging (use ReverseLookup to convert hash back to string)
    if (emitDebugNames) {
        std::string structName = ReverseLookup::GetString(structTypeHash);
        if (!structName.empty()) {
            EmitName(struct_type_id, structName.c_str());
        }
    }

    // Emit member offset decorations and member names for std140 layout
    for (u32 i = 0; i < fieldCount; i++) {
        u32 byteOffset = ir->structFieldByteOffsets[structInfo->fieldOffset + i];
        EmitMemberDecoration(struct_type_id, i, spv::DecorationOffset, byteOffset);

        // Emit member name for debugging
        if (emitDebugNames && ir->structFieldNameHashes) {
            u32 fieldNameHash = ir->structFieldNameHashes[structInfo->fieldOffset + i];
            std::string fieldName = ReverseLookup::GetString(fieldNameHash);
            if (!fieldName.empty()) {
                EmitMemberName(struct_type_id, i, fieldName.c_str());
            }
        }
    }

    // Cache the struct type and its field type IDs
    if (structTypeCount < 64) {
        u32 structIdx = structTypeCount;
        structTypeHashes[structIdx] = structTypeHash;
        structTypeIds[structIdx] = struct_type_id;

        // Store field type IDs for later lookup
        for (u32 i = 0; i < fieldCount && i < MAX_FIELDS_PER_STRUCT; i++) {
            structFieldTypeIds[structIdx * MAX_FIELDS_PER_STRUCT + i] = ops[1 + i];
        }

        structTypeCount++;
    }

    return struct_type_id;
}

// ============= Texture Type Management =============
u32 SPIRVBuilder::GetImageTypeId() {
    if (imageTypeId != 0) return imageTypeId;

    // OpTypeImage: sampled_type, dim, depth, arrayed, ms, sampled, format
    // For a typical 2D sampled texture: float, Dim2D, 0, 0, 0, 1, Unknown
    imageTypeId = AllocateId();
    u32 float_type = GetTypeId(CoreType::FLOAT);
    u32 ops[] = {
        imageTypeId,
        float_type,
        static_cast<u32>(spv::Dim2D),
        0,  // depth (not depth texture)
        0,  // arrayed (not array)
        0,  // ms (not multisampled)
        1,  // sampled (used with sampler)
        static_cast<u32>(spv::ImageFormatUnknown)
    };
    EmitToSection(&typesConstants, spv::OpTypeImage, ops, 8);

    return imageTypeId;
}

u32 SPIRVBuilder::GetSamplerTypeId() {
    if (samplerTypeId != 0) return samplerTypeId;

    samplerTypeId = AllocateId();
    u32 ops[] = {samplerTypeId};
    EmitToSection(&typesConstants, spv::OpTypeSampler, ops, 1);

    return samplerTypeId;
}

u32 SPIRVBuilder::GetSampledImageTypeId() {
    if (sampledImageTypeId != 0) return sampledImageTypeId;

    // OpTypeSampledImage requires the image type
    u32 img_type = GetImageTypeId();
    sampledImageTypeId = AllocateId();
    u32 ops[] = {sampledImageTypeId, img_type};
    EmitToSection(&typesConstants, spv::OpTypeSampledImage, ops, 2);

    return sampledImageTypeId;
}

// ============= Constant Management =============
u32 SPIRVBuilder::GetFloatConstantId(float value) {
    // Check for existing constant
    u32 hash = Utils::HashFloat(value);
    for (u32 i = 0; i < ir->floatCount; i++) {
        if (ir->floatConstants[i] == value && floatConstantIds[i] != 0) {
            return floatConstantIds[i];
        }
    }
    
    // Create new constant
    u32 const_id = AllocateId();
    u32 type_id = GetTypeId(CoreType::FLOAT);
    
    // Emit OpConstant
    u32 ops[] = {type_id, const_id, *(u32*)&value};
    EmitToSection(&typesConstants, spv::OpConstant, ops, 3);
    
    // Cache it
    for (u32 i = 0; i < ir->floatCount; i++) {
        if (ir->floatConstants[i] == value) {
            floatConstantIds[i] = const_id;
            break;
        }
    }
    
    return const_id;
}

u32 SPIRVBuilder::GetIntConstantId(u32 value, bool isUnsigned) {
    // Similar to float constants, but handle both signed and unsigned
    // Use separate caches for signed vs unsigned constants
    u32* cacheArray = isUnsigned ? uintConstantIds : intConstantIds;

    // First check cache
    for (u32 i = 0; i < ir->intCount; i++) {
        if (ir->intConstants[i] == value && cacheArray[i] != 0) {
            return cacheArray[i];
        }
    }

    u32 const_id = AllocateId();
    u32 type_id = GetTypeId(isUnsigned ? CoreType::UINT : CoreType::INT);

    u32 ops[] = {type_id, const_id, value};
    EmitToSection(&typesConstants, spv::OpConstant, ops, 3);

    // Cache the result
    for (u32 i = 0; i < ir->intCount; i++) {
        if (ir->intConstants[i] == value) {
            cacheArray[i] = const_id;
            break;
        }
    }

    return const_id;
}

u32 SPIRVBuilder::GetBoolConstantId(bool value) {
    // Cache bool constants (only two possible values)
    if (value) {
        if (boolTrueId != 0) return boolTrueId;
        u32 const_id = AllocateId();
        u32 type_id = GetTypeId(CoreType::BOOL);
        u32 ops[] = {type_id, const_id};
        EmitToSection(&typesConstants, spv::OpConstantTrue, ops, 2);
        boolTrueId = const_id;
        return const_id;
    } else {
        if (boolFalseId != 0) return boolFalseId;
        u32 const_id = AllocateId();
        u32 type_id = GetTypeId(CoreType::BOOL);
        u32 ops[] = {type_id, const_id};
        EmitToSection(&typesConstants, spv::OpConstantFalse, ops, 2);
        boolFalseId = const_id;
        return const_id;
    }
}

u32 SPIRVBuilder::GetCompositeConstantId(u32 type_id, u32* constituents, u32 count) {
    // TODO: Implement composite constant creation
    u32 const_id = AllocateId();
    
    u32* ops = (u32*)arena->Allocate((2 + count) * sizeof(u32));
    ops[0] = type_id;
    ops[1] = const_id;
    memcpy(&ops[2], constituents, count * sizeof(u32));
    
    EmitToSection(&typesConstants, spv::OpConstantComposite, ops, 2 + count);
    return const_id;
}

// ============= Section Builders =============
void SPIRVBuilder::EmitToSection(Section* section, spv::Op op, u32* operands, u32 operand_count) {
    u32 word_count = 1 + operand_count;
    
    if (section->count + word_count > section->capacity) {
        GrowSection(section);
    }
    
    section->words[section->count++] = (word_count << 16) | op;
    memcpy(&section->words[section->count], operands, operand_count * sizeof(u32));
    section->count += operand_count;
}

void SPIRVBuilder::EmitCapability(spv::Capability cap) {
    u32 ops[] = {cap};
    EmitToSection(&capabilities, spv::OpCapability, ops, 1);
}

void SPIRVBuilder::EmitExtension(const char* extName) {
    // OpExtension format: word_count | op, "extension_name\0"
    u32 nameLen = strlen(extName) + 1;  // Include null terminator
    u32 nameWords = (nameLen + 3) / 4;  // Round up to word boundary
    u32 wordCount = 1 + nameWords;  // op + name words
    
    // Grow section if needed
    if (extensions.count + wordCount > extensions.capacity) {
        GrowSection(&extensions);
    }
    
    extensions.words[extensions.count++] = (wordCount << 16) | spv::OpExtension;
    
    // Copy name with padding
    char* namePtr = reinterpret_cast<char*>(&extensions.words[extensions.count]);
    memset(namePtr, 0, nameWords * 4);  // Zero-fill for padding
    memcpy(namePtr, extName, nameLen - 1);  // Copy without null (will be zero from memset)
    extensions.count += nameWords;
}

void SPIRVBuilder::EmitDecoration(u32 id, spv::Decoration decoration, u32* params, u32 param_count) {
    u32* ops = (u32*)arena->Allocate((2 + param_count) * sizeof(u32));
    ops[0] = id;
    ops[1] = decoration;
    memcpy(&ops[2], params, param_count * sizeof(u32));
    
    EmitToSection(&decorations, spv::OpDecorate, ops, 2 + param_count);
}

void SPIRVBuilder::EmitMemberDecoration(u32 structTypeId, u32 memberIndex, spv::Decoration decoration, u32 value) {
    // OpMemberDecorate format: struct_type, member, decoration, [operands]
    u32 word_count = 5;  // header + struct + member + decoration + value

    if (decorations.count + word_count > decorations.capacity) {
        GrowSection(&decorations);
    }

    decorations.words[decorations.count++] = (word_count << 16) | spv::OpMemberDecorate;
    decorations.words[decorations.count++] = structTypeId;
    decorations.words[decorations.count++] = memberIndex;
    decorations.words[decorations.count++] = static_cast<u32>(decoration);
    decorations.words[decorations.count++] = value;
}

void SPIRVBuilder::EmitName(u32 id, const char* name) {
    if (!name || !name[0]) return;

    u32 nameLen = strlen(name);
    u32 nameWords = (nameLen + 4) / 4;  // Round up including null terminator
    u32 wordCount = 2 + nameWords;  // OpName + target + string words

    if (debugNames.count + wordCount > debugNames.capacity) {
        GrowSection(&debugNames);
    }

    u32* words = debugNames.words + debugNames.count;
    words[0] = (wordCount << 16) | spv::OpName;
    words[1] = id;
    memset(&words[2], 0, nameWords * 4);  // Zero-fill for padding/null terminator
    memcpy(&words[2], name, nameLen);
    debugNames.count += wordCount;
}

void SPIRVBuilder::EmitMemberName(u32 structTypeId, u32 memberIndex, const char* name) {
    if (!name || !name[0]) return;

    u32 nameLen = strlen(name);
    u32 nameWords = (nameLen + 4) / 4;  // Round up including null terminator
    u32 wordCount = 3 + nameWords;  // OpMemberName + struct + member + string words

    if (debugNames.count + wordCount > debugNames.capacity) {
        GrowSection(&debugNames);
    }

    u32* words = debugNames.words + debugNames.count;
    words[0] = (wordCount << 16) | spv::OpMemberName;
    words[1] = structTypeId;
    words[2] = memberIndex;
    memset(&words[3], 0, nameWords * 4);  // Zero-fill for padding/null terminator
    memcpy(&words[3], name, nameLen);
    debugNames.count += wordCount;
}

// ============= IR Translation =============

// Forward declarations for helpers used in TranslateInstruction
static CoreType GetFallbackAttributeType(u32 attrIdx);
static CoreType GetFallbackOutputType(u32 slot);

// Helper to get result type for arithmetic operations
u32 SPIRVBuilder::GetResultType(u16 dest_reg, u16 op1_reg) {
    u32 typeId = 0;

    // Try destination register type first
    if (ir->registerTypes && dest_reg < ir->registerCount) {
        CoreType regType = static_cast<CoreType>(ir->registerTypes[dest_reg]);
        if (regType != CoreType::VOID && regType != CoreType::INVALID) {
            if (regType == CoreType::CUSTOM || regType == CoreType::ENUM) {
                if (ir->registerStructTypes) {
                    u32 structHash = ir->registerStructTypes[dest_reg];
                    if (structHash != 0) {
                        typeId = GetStructTypeId(structHash);
                        if (typeId != 0) return typeId;
                    }
                }
            } else {
                typeId = GetTypeId(regType);
                if (typeId != 0) return typeId;
            }
        }
    }

    // Fallback: use first operand's type
    if (op1_reg & 0x8000) {
        // Float constant
        return GetTypeId(CoreType::FLOAT);
    } else if (op1_reg & 0x4000) {
        // Int constant
        return GetTypeId(CoreType::INT);
    } else if (op1_reg & 0x2000) {
        // Uint constant
        return GetTypeId(CoreType::UINT);
    } else if (ir->registerTypes && op1_reg < ir->registerCount) {
        CoreType op1Type = static_cast<CoreType>(ir->registerTypes[op1_reg]);
        if (op1Type == CoreType::CUSTOM || op1Type == CoreType::ENUM) {
            if (ir->registerStructTypes) {
                u32 structHash = ir->registerStructTypes[op1_reg];
                if (structHash != 0) {
                    typeId = GetStructTypeId(structHash);
                    if (typeId != 0) return typeId;
                }
            }
        } else {
            typeId = GetTypeId(op1Type);
            if (typeId != 0) return typeId;
        }
    }

    // Ultimate fallback: float is the most common type in shaders
    return GetTypeId(CoreType::FLOAT);
}

CoreType SPIRVBuilder::GetOperandType(u16 reg) {
    if ((reg & 0xC000) == 0xC000) return CoreType::BOOL;  // Bool constant
    if (reg & 0x8000) return CoreType::FLOAT;             // Float constant
    if (reg & 0x4000) return CoreType::INT;               // Int constant
    if (reg & 0x2000) return CoreType::UINT;              // Uint constant

    if (ir->registerTypes && reg < ir->registerCount) {
        return static_cast<CoreType>(ir->registerTypes[reg]);
    }
    return CoreType::FLOAT;
}

CoreType GetScalarComponentType(CoreType vecType) {
    switch (vecType) {
        case CoreType::FLOAT:
        case CoreType::FLOAT2:
        case CoreType::FLOAT3:
        case CoreType::FLOAT4:
            return CoreType::FLOAT;
        case CoreType::INT:
        case CoreType::INT2:
        case CoreType::INT3:
        case CoreType::INT4:
            return CoreType::INT;
        case CoreType::UINT:
        case CoreType::UINT2:
        case CoreType::UINT3:
        case CoreType::UINT4:
            return CoreType::UINT;
        case CoreType::BOOL:
        case CoreType::BOOL2:
        case CoreType::BOOL3:
        case CoreType::BOOL4:
            return CoreType::BOOL;
        default:{
            // print to std error
            fprintf(stderr, "Error: GetScalarComponentType failed for type %u\n", vecType);
            return CoreType::FLOAT;  // Fallback
            
        }
    }
}

spv::Op SPIRVBuilder::IRToSpvOp(IR::OpCode op) {
    if (static_cast<u32>(op) < 256) {
        return IR_TO_SPV_OP_TABLE[static_cast<u32>(op)];
    }
    return spv::OpNop;
}

void SPIRVBuilder::TranslateInstruction(u32 ir_idx) {
    IR::OpCode op = static_cast<IR::OpCode>(ir->opcodes[ir_idx]);
    spv::Op spv_op = IRToSpvOp(op);


    u32 dest = GetSpirvId(ir->destinations[ir_idx]);

    // Get type ID - special handling for CUSTOM and ENUM types which need struct lookup
    CoreType instType = static_cast<CoreType>(ir->types[ir_idx]);
    u32 type_id = 0;
    if (instType == CoreType::CUSTOM || instType == CoreType::ENUM) {
        // Look up struct type from register's struct type hash
        u16 dest_reg = ir->destinations[ir_idx];
        if (dest_reg < 512 && ir->registerStructTypes) {
            u32 structHash = ir->registerStructTypes[dest_reg];
            if (structHash != 0) {
                type_id = GetStructTypeId(structHash);
            }
        }
    } else {
        type_id = GetTypeId(instType);
    }

    switch(op) {
        // ========== No-ops and pass-through ==========
        case IR::OP_NOP: {
            // No operation - but if it has a destination register, emit OpUndef
            // This handles placeholders for unimplemented features
            u16 dest_reg = ir->destinations[ir_idx];
            if (dest_reg != 0 && dest_reg != 0xFFFF) {
                CoreType resultType = static_cast<CoreType>(ir->types[ir_idx]);
                if (resultType == CoreType::INVALID || resultType == CoreType::VOID) {
                    resultType = CoreType::FLOAT;  // Default fallback
                }
                u32 result_type_id = GetTypeId(resultType);
                if (result_type_id != 0) {
                    Emit(spv::OpUndef, result_type_id, dest);
                }
            }
            break;
        }

        case IR::OP_CALL: {
            // Function call - for now emit OpUndef as placeholder since inlining isn't implemented
            // TODO: Implement function inlining during IR lowering
            // The destination register needs a valid SPIR-V ID that's actually defined
            u16 dest_reg = ir->destinations[ir_idx];
            CoreType resultType = static_cast<CoreType>(ir->types[ir_idx]);
            if (resultType == CoreType::INVALID || resultType == CoreType::VOID) {
                resultType = CoreType::FLOAT3;  // Default for position decompression functions
            }
            u32 result_type_id = GetTypeId(resultType);
            // OpUndef: result_type result_id
            Emit(spv::OpUndef, result_type_id, dest);
            break;
        }

        case IR::OP_LOAD_REG: {
            // Copy register to register
            u16 src_reg = ir->GetOperand(ir_idx, 0);
            u32 src_id = GetSpirvId(src_reg);
            u16 dest_reg = ir->destinations[ir_idx];

            // Check if source is a constant (float 0x8000, int 0x4000, bool 0xC000)
            bool srcIsConstant = (src_reg & 0xC000) != 0;
            bool needsPreallocDef = (dest_reg < idCapacity && hasPreAllocatedId[dest_reg]);

            if ((srcIsConstant || needsPreallocDef) && dest_reg < idCapacity) {
                // For constants, we must emit OpCopyObject to create a properly defined ID.
                // This is critical for phi nodes: the phi may be emitted before this block,
                // so the destination register needs an actual instruction defining it.
                CoreType destType = CoreType::FLOAT;
                if (dest_reg < ir->registerCount && ir->registerTypes) {
                    destType = static_cast<CoreType>(ir->registerTypes[dest_reg]);
                }
                // Infer type from constant encoding if register type not set
                if (destType == CoreType::VOID || destType == CoreType::INVALID) {
                    if ((src_reg & 0xC000) == 0xC000) {
                        destType = CoreType::BOOL;
                    } else if (src_reg & 0x8000) {
                        destType = CoreType::FLOAT;
                    } else {
                        destType = CoreType::INT;
                    }
                }
                u32 type_id = 0;
                if ((destType == CoreType::CUSTOM || destType == CoreType::ENUM) &&
                    ir->registerStructTypes && dest_reg < ir->registerCount) {
                    u32 structHash = ir->registerStructTypes[dest_reg];
                    if (structHash != 0) {
                        type_id = GetStructTypeId(structHash);
                    }
                } else {
                    type_id = GetTypeId(destType);
                }
                if (type_id == 0) {
                    type_id = GetTypeId(CoreType::FLOAT);
                }
                Emit(spv::OpCopyObject, type_id, dest, src_id);
                if (needsPreallocDef) {
                    hasPreAllocatedId[dest_reg] = false;
                }
            } else if (dest_reg < idCapacity) {
                // For register-to-register copy, just alias the SPIR-V ID
                spirvIds[dest_reg] = src_id;
            }
            break;
        }
        
        case IR::OP_LOAD_CONST: {
            // Load constant - the constant value is in metadata or operands
            u16 dest_reg = ir->destinations[ir_idx];
            // The constant index/value is typically in operand[0]
            u16 const_ref = ir->GetOperand(ir_idx, 0);
            u32 const_id = GetSpirvId(const_ref);
            if (dest_reg < idCapacity && hasPreAllocatedId[dest_reg]) {
                // Define the pre-allocated ID so forward-referenced PHIs stay valid.
                u32 type_id = GetResultType(dest_reg, const_ref);
                Emit(spv::OpCopyObject, type_id, dest, const_id);
                hasPreAllocatedId[dest_reg] = false;
            } else if (dest_reg < idCapacity) {
                // Map dest register to constant ID
                spirvIds[dest_reg] = const_id;
            }
            break;
        }
        
        case IR::OP_STORE_REG: {
            // Store to a register (variable assignment)
            // In SSA form, this is just aliasing: dest = src
            //
            // Special handling for PHI operands: if this register was pre-allocated
            // an ID because a PHI references it from a not-yet-processed block,
            // we need to define that ID with OpCopyObject. After defining it,
            // we clear the flag so subsequent STORE_REG to the same register
            // just aliases without emitting another definition.
            u16 dest_reg = ir->destinations[ir_idx];
            u16 src_reg = ir->GetOperand(ir_idx, 0);
            u32 src_id = GetSpirvId(src_reg);

            if (dest_reg < idCapacity && hasPreAllocatedId[dest_reg]) {
                // This register has a pre-allocated ID that needs to be defined
                u32 existing_id = spirvIds[dest_reg];
                if (existing_id != src_id) {
                    // Define the pre-allocated ID by copying from the source
                    u32 type_id = GetResultType(dest_reg, src_reg);
                    Emit(spv::OpCopyObject, type_id, existing_id, src_id);
                }
                // Clear the flag - ID is now defined, subsequent STORE_REG will just alias
                hasPreAllocatedId[dest_reg] = false;
                // Keep the pre-allocated ID in spirvIds so PHIs can reference it
            } else if (dest_reg < idCapacity) {
                // No pre-allocated ID (or already defined), just alias
                spirvIds[dest_reg] = src_id;
            }
            break;
        }
        
        // ========== Arithmetic ==========
        case IR::OP_FADD:
        case IR::OP_FSUB:
        case IR::OP_FDIV: {
            u16 dest_reg = ir->destinations[ir_idx];
            u16 op1_reg = ir->GetOperand(ir_idx, 0);
            u16 op2_reg = ir->GetOperand(ir_idx, 1);
            u32 op1 = GetSpirvId(op1_reg);
            u32 op2 = GetSpirvId(op2_reg);

            // Check for scalar-vector mixing which requires splatting
            // Constants (0x8000=float, 0x4000=int, 0x2000=uint, 0xC000=bool) are scalars
            bool op1_is_scalar = (op1_reg & 0xE000) != 0;
            bool op2_is_scalar = (op2_reg & 0xE000) != 0;

            CoreType destType = CoreType::FLOAT;
            if (ir->registerTypes && dest_reg < ir->registerCount) {
                destType = static_cast<CoreType>(ir->registerTypes[dest_reg]);
            }

            // Check register types for non-constant operands
            if (ir->registerTypes) {
                if (!op1_is_scalar && op1_reg < ir->registerCount) {
                    CoreType op1_type = static_cast<CoreType>(ir->registerTypes[op1_reg]);
                    op1_is_scalar = (op1_type == CoreType::FLOAT || op1_type == CoreType::INT || op1_type == CoreType::UINT);
                }
                if (!op2_is_scalar && op2_reg < ir->registerCount) {
                    CoreType op2_type = static_cast<CoreType>(ir->registerTypes[op2_reg]);
                    op2_is_scalar = (op2_type == CoreType::FLOAT || op2_type == CoreType::INT || op2_type == CoreType::UINT);
                }
            }

            u32 result_type = GetResultType(dest_reg, op1_is_scalar ? op2_reg : op1_reg);

            // If one operand is scalar and result is vector, we need to splat
            bool resultIsVector = (destType == CoreType::FLOAT2 || destType == CoreType::FLOAT3 || destType == CoreType::FLOAT4 ||
                                   destType == CoreType::INT2 || destType == CoreType::INT3 || destType == CoreType::INT4);

            if (resultIsVector) {
                u32 numComponents = (destType == CoreType::FLOAT2 || destType == CoreType::INT2) ? 2 :
                                    (destType == CoreType::FLOAT3 || destType == CoreType::INT3) ? 3 : 4;

                if (op1_is_scalar && !op2_is_scalar) {
                    // Splat op1 to a vector
                    u32 splatted = AllocateId();
                    if (currentFunctionSize + 3 + numComponents > currentFunctionCapacity) {
                        GrowCurrentFunction();
                    }
                    currentFunction[currentFunctionSize++] = ((3 + numComponents) << 16) | spv::OpCompositeConstruct;
                    currentFunction[currentFunctionSize++] = result_type;
                    currentFunction[currentFunctionSize++] = splatted;
                    for (u32 i = 0; i < numComponents; i++) {
                        currentFunction[currentFunctionSize++] = op1;
                    }
                    op1 = splatted;
                } else if (op2_is_scalar && !op1_is_scalar) {
                    // Splat op2 to a vector
                    u32 splatted = AllocateId();
                    if (currentFunctionSize + 3 + numComponents > currentFunctionCapacity) {
                        GrowCurrentFunction();
                    }
                    currentFunction[currentFunctionSize++] = ((3 + numComponents) << 16) | spv::OpCompositeConstruct;
                    currentFunction[currentFunctionSize++] = result_type;
                    currentFunction[currentFunctionSize++] = splatted;
                    for (u32 i = 0; i < numComponents; i++) {
                        currentFunction[currentFunctionSize++] = op2;
                    }
                    op2 = splatted;
                }
            }

            Emit(spv_op, result_type, dest, op1, op2);
            break;
        }
        
        case IR::OP_FMUL: {
            u16 dest_reg = ir->destinations[ir_idx];
            u16 op1_reg = ir->GetOperand(ir_idx, 0);
            u16 op2_reg = ir->GetOperand(ir_idx, 1);
            u32 op1 = GetSpirvId(op1_reg);
            u32 op2 = GetSpirvId(op2_reg);
            u32 result_type = GetResultType(dest_reg, op1_reg);

            // Check for vector-scalar multiplication
            // Constants (0x8000=float, 0x4000=int, 0x2000=uint, 0xC000=bool) are always scalars
            bool op1_is_scalar = (op1_reg & 0xE000) != 0;
            bool op2_is_scalar = (op2_reg & 0xE000) != 0;

            // Check register types independently for each non-constant operand
            if (ir->registerTypes) {
                if (!op1_is_scalar && op1_reg < ir->registerCount) {
                    CoreType op1_type = static_cast<CoreType>(ir->registerTypes[op1_reg]);
                    op1_is_scalar = (op1_type == CoreType::FLOAT || op1_type == CoreType::INT || op1_type == CoreType::UINT);
                }
                if (!op2_is_scalar && op2_reg < ir->registerCount) {
                    CoreType op2_type = static_cast<CoreType>(ir->registerTypes[op2_reg]);
                    op2_is_scalar = (op2_type == CoreType::FLOAT || op2_type == CoreType::INT || op2_type == CoreType::UINT);
                }
            }

            if (op2_is_scalar && !op1_is_scalar) {
                // Vector * Scalar -> OpVectorTimesScalar
                Emit(spv::OpVectorTimesScalar, result_type, dest, op1, op2);
            } else if (op1_is_scalar && !op2_is_scalar) {
                // Scalar * Vector -> swap and use OpVectorTimesScalar
                Emit(spv::OpVectorTimesScalar, result_type, dest, op2, op1);
            } else {
                // Both vectors or both scalars -> OpFMul
                Emit(spv::OpFMul, result_type, dest, op1, op2);
            }
            break;
        }
        
        case IR::OP_IADD:
        case IR::OP_ISUB:
        case IR::OP_IMUL:
        case IR::OP_IDIV:
        case IR::OP_IMOD: {
            u16 dest_reg = ir->destinations[ir_idx];
            u32 op1 = GetSpirvId(ir->GetOperand(ir_idx, 0));
            u32 op2 = GetSpirvId(ir->GetOperand(ir_idx, 1));
            u32 result_type = GetTypeId(CoreType::INT);
            if (ir->registerTypes && dest_reg < ir->registerCount) {
                CoreType regType = static_cast<CoreType>(ir->registerTypes[dest_reg]);
                if (regType != CoreType::VOID && regType != CoreType::INVALID) {
                    result_type = GetTypeId(regType);
                }
            }
            Emit(spv_op, result_type, dest, op1, op2);
            break;
        }

        // ========== Matrix Operations ==========
        case IR::OP_MAT_MUL: {
            // Matrix * Matrix
            u16 dest_reg = ir->destinations[ir_idx];
            u32 op1 = GetSpirvId(ir->GetOperand(ir_idx, 0));
            u32 op2 = GetSpirvId(ir->GetOperand(ir_idx, 1));
            u32 result_type = GetTypeId(CoreType::MAT4);  // Default to mat4
            if (ir->registerTypes && dest_reg < ir->registerCount) {
                CoreType regType = static_cast<CoreType>(ir->registerTypes[dest_reg]);
                if (regType == CoreType::MAT2 || regType == CoreType::MAT3 || regType == CoreType::MAT4) {
                    result_type = GetTypeId(regType);
                }
            }
            Emit(spv::OpMatrixTimesMatrix, result_type, dest, op1, op2);
            break;
        }

        case IR::OP_MAT_VEC_MUL: {
            // Matrix * Vector -> Vector
            // SPIR-V requires: vector components == matrix columns
            u16 dest_reg = ir->destinations[ir_idx];
            u16 op1_reg = ir->GetOperand(ir_idx, 0);  // Matrix
            u16 op2_reg = ir->GetOperand(ir_idx, 1);  // Vector
            u32 op1 = GetSpirvId(op1_reg);
            u32 op2 = GetSpirvId(op2_reg);

            // Get destination type to determine expected dimensions
            // For square matrices, result vec size == matrix columns == matrix rows
            CoreType destType = CoreType::FLOAT4;
            if (ir->registerTypes && dest_reg < ir->registerCount) {
                destType = static_cast<CoreType>(ir->registerTypes[dest_reg]);
            }

            u32 expectedSize = 4;
            if (destType == CoreType::FLOAT2) expectedSize = 2;
            else if (destType == CoreType::FLOAT3) expectedSize = 3;

            // Get input vector type - check both constant flags and register types
            CoreType vecType = CoreType::FLOAT4;  // Default assumption
            bool vecTypeKnown = false;
            if (!(op2_reg & 0xE000)) {
                // Not a constant (0x8000=float, 0x4000=int, 0x2000=uint), check register type
                if (ir->registerTypes && op2_reg < ir->registerCount) {
                    CoreType regType = static_cast<CoreType>(ir->registerTypes[op2_reg]);
                    if (regType == CoreType::FLOAT2 || regType == CoreType::FLOAT3 || regType == CoreType::FLOAT4) {
                        vecType = regType;
                        vecTypeKnown = true;
                    }
                }
            }

            u32 vecSize = 4;
            if (vecType == CoreType::FLOAT2) vecSize = 2;
            else if (vecType == CoreType::FLOAT3) vecSize = 3;

            // If result is smaller than vec4, we may need to truncate
            // This handles cases where the IR type info is missing or incorrect
            if (expectedSize < 4 || vecSize > expectedSize) {
                CoreType targetVecType = destType;  // Match destination type
                u32 target_vec_type_id = GetTypeId(targetVecType);

                // Truncate: extract first N components using VectorShuffle
                u32 truncated = AllocateId();
                u32 wordCount = 5 + expectedSize;  // OpVectorShuffle base + indices
                currentFunction[currentFunctionSize++] = (wordCount << 16) | spv::OpVectorShuffle;
                currentFunction[currentFunctionSize++] = target_vec_type_id;
                currentFunction[currentFunctionSize++] = truncated;
                currentFunction[currentFunctionSize++] = op2;
                currentFunction[currentFunctionSize++] = op2;
                for (u32 i = 0; i < expectedSize; i++) {
                    currentFunction[currentFunctionSize++] = i;
                }
                op2 = truncated;
            }

            // Result type matches the destination register type
            u32 result_type = GetTypeId(destType);
            Emit(spv::OpMatrixTimesVector, result_type, dest, op1, op2);
            break;
        }

        case IR::OP_VEC_MAT_MUL: {
            // Vector * Matrix -> Vector
            u16 dest_reg = ir->destinations[ir_idx];
            u32 op1 = GetSpirvId(ir->GetOperand(ir_idx, 0));  // Vector
            u32 op2 = GetSpirvId(ir->GetOperand(ir_idx, 1));  // Matrix
            // Result type is the vector type
            u32 result_type = GetTypeId(CoreType::FLOAT4);  // Default to vec4
            if (ir->registerTypes && dest_reg < ir->registerCount) {
                CoreType regType = static_cast<CoreType>(ir->registerTypes[dest_reg]);
                if (regType == CoreType::FLOAT2 || regType == CoreType::FLOAT3 || regType == CoreType::FLOAT4) {
                    result_type = GetTypeId(regType);
                }
            }
            Emit(spv::OpVectorTimesMatrix, result_type, dest, op1, op2);
            break;
        }

        case IR::OP_MAT_SCALE: {
            // Matrix * Scalar -> Matrix
            u16 dest_reg = ir->destinations[ir_idx];
            u32 op1 = GetSpirvId(ir->GetOperand(ir_idx, 0));  // Matrix
            u32 op2 = GetSpirvId(ir->GetOperand(ir_idx, 1));  // Scalar
            u32 result_type = GetTypeId(CoreType::MAT4);  // Default to mat4
            if (ir->registerTypes && dest_reg < ir->registerCount) {
                CoreType regType = static_cast<CoreType>(ir->registerTypes[dest_reg]);
                if (regType == CoreType::MAT2 || regType == CoreType::MAT3 || regType == CoreType::MAT4) {
                    result_type = GetTypeId(regType);
                }
            }
            Emit(spv::OpMatrixTimesScalar, result_type, dest, op1, op2);
            break;
        }

        case IR::OP_MAT_TRANSPOSE: {
            u16 dest_reg = ir->destinations[ir_idx];
            u32 op1 = GetSpirvId(ir->GetOperand(ir_idx, 0));
            u32 result_type = GetTypeId(CoreType::MAT4);
            if (ir->registerTypes && dest_reg < ir->registerCount) {
                CoreType regType = static_cast<CoreType>(ir->registerTypes[dest_reg]);
                if (regType == CoreType::MAT2 || regType == CoreType::MAT3 || regType == CoreType::MAT4) {
                    result_type = GetTypeId(regType);
                }
            }
            Emit(spv::OpTranspose, result_type, dest, op1);
            break;
        }

        // ========== Bitwise/Logical Operations ==========
        // Note: For bitwise ops, int constants need to match the type of the other operand
        // For booleans, use OpLogicalAnd/OpLogicalOr instead of bitwise ops
        case IR::OP_AND: {
            u16 dest_reg = ir->destinations[ir_idx];
            u16 op1_reg = ir->GetOperand(ir_idx, 0);
            u16 op2_reg = ir->GetOperand(ir_idx, 1);
            u32 result_type = GetResultType(dest_reg, op1_reg);

            CoreType op1_type = GetOperandType(op1_reg);

            // Booleans require OpLogicalAnd, integers use OpBitwiseAnd
            if (op1_type == CoreType::BOOL) {
                u32 op1 = GetSpirvId(op1_reg);
                u32 op2 = GetSpirvId(op2_reg);
                Emit(spv::OpLogicalAnd, result_type, dest, op1, op2);
            } else {
                bool useUint = (op1_type == CoreType::UINT);
                u32 op1 = GetSpirvIdForBitwise(op1_reg, useUint);
                u32 op2 = GetSpirvIdForBitwise(op2_reg, useUint);
                Emit(spv::OpBitwiseAnd, result_type, dest, op1, op2);
            }
            break;
        }

        case IR::OP_OR: {
            u16 dest_reg = ir->destinations[ir_idx];
            u16 op1_reg = ir->GetOperand(ir_idx, 0);
            u16 op2_reg = ir->GetOperand(ir_idx, 1);
            u32 result_type = GetResultType(dest_reg, op1_reg);
            CoreType op1_type = GetOperandType(op1_reg);

            // Booleans require OpLogicalOr, integers use OpBitwiseOr
            if (op1_type == CoreType::BOOL) {
                u32 op1 = GetSpirvId(op1_reg);
                u32 op2 = GetSpirvId(op2_reg);
                Emit(spv::OpLogicalOr, result_type, dest, op1, op2);
            } else {
                bool useUint = (op1_type == CoreType::UINT);
                u32 op1 = GetSpirvIdForBitwise(op1_reg, useUint);
                u32 op2 = GetSpirvIdForBitwise(op2_reg, useUint);
                Emit(spv::OpBitwiseOr, result_type, dest, op1, op2);
            }
            break;
        }

        case IR::OP_XOR: {
            u16 dest_reg = ir->destinations[ir_idx];
            u16 op1_reg = ir->GetOperand(ir_idx, 0);
            u16 op2_reg = ir->GetOperand(ir_idx, 1);
            u32 result_type = GetResultType(dest_reg, op1_reg);
            CoreType op1_type = static_cast<CoreType>(ir->registerTypes[op1_reg & 0x3FFF]);
            bool useUint = (op1_type == CoreType::UINT);
            u32 op1 = GetSpirvIdForBitwise(op1_reg, useUint);
            u32 op2 = GetSpirvIdForBitwise(op2_reg, useUint);
            Emit(spv::OpBitwiseXor, result_type, dest, op1, op2);
            break;
        }

        case IR::OP_NOT: {
            u16 dest_reg = ir->destinations[ir_idx];
            u16 op_reg = ir->GetOperand(ir_idx, 0);
            u32 operand = GetSpirvId(op_reg);
            u32 result_type = GetResultType(dest_reg, op_reg);
            CoreType op_type = GetOperandType(op_reg);

            // Booleans require OpLogicalNot, integers use OpNot (bitwise)
            if (op_type == CoreType::BOOL || op_type == CoreType::BOOL2 ||
                op_type == CoreType::BOOL3 || op_type == CoreType::BOOL4) {
                Emit(spv::OpLogicalNot, result_type, dest, operand);
            } else {
                Emit(spv::OpNot, result_type, dest, operand);
            }
            break;
        }

        // ========== Unary Negation ==========
        case IR::OP_FNEG: {
            // Float negation: -x
            u16 dest_reg = ir->destinations[ir_idx];
            u32 operand = GetSpirvId(ir->GetOperand(ir_idx, 0));
            u32 result_type = GetResultType(dest_reg, ir->GetOperand(ir_idx, 0));
            Emit(spv::OpFNegate, result_type, dest, operand);
            break;
        }

        case IR::OP_INEG: {
            // Integer negation: -x
            u16 dest_reg = ir->destinations[ir_idx];
            u32 operand = GetSpirvId(ir->GetOperand(ir_idx, 0));
            u32 result_type = GetResultType(dest_reg, ir->GetOperand(ir_idx, 0));
            Emit(spv::OpSNegate, result_type, dest, operand);
            break;
        }

        // ========== Derivatives (Fragment only) ==========
        case IR::OP_DDX:
        case IR::OP_DDY:
        case IR::OP_DDX_FINE:
        case IR::OP_DDY_FINE:
        case IR::OP_DDX_COARSE:
        case IR::OP_DDY_COARSE:
        case IR::OP_FWIDTH: {
            u16 dest_reg = ir->destinations[ir_idx];
            u16 op_reg = ir->GetOperand(ir_idx, 0);
            u32 operand = GetSpirvId(op_reg);
            u32 result_type = GetResultType(dest_reg, op_reg);
            spv::Op spv_op = IR_TO_SPV_OP_TABLE[static_cast<u32>(op)];
            Emit(spv_op, result_type, dest, operand);
            break;
        }

        // ========== Shift Operations ==========
        case IR::OP_SHL: {
            u16 dest_reg = ir->destinations[ir_idx];
            u16 base_reg = ir->GetOperand(ir_idx, 0);
            u16 shift_reg = ir->GetOperand(ir_idx, 1);
            u32 result_type = GetResultType(dest_reg, base_reg);
            CoreType base_type = static_cast<CoreType>(ir->registerTypes[base_reg & 0x3FFF]);
            bool useUint = (base_type == CoreType::UINT);
            u32 base = GetSpirvIdForBitwise(base_reg, useUint);
            u32 shift = GetSpirvIdForBitwise(shift_reg, useUint);
            Emit(spv::OpShiftLeftLogical, result_type, dest, base, shift);
            break;
        }

        case IR::OP_SHR: {
            // Logical shift right (unsigned)
            u16 dest_reg = ir->destinations[ir_idx];
            u16 base_reg = ir->GetOperand(ir_idx, 0);
            u16 shift_reg = ir->GetOperand(ir_idx, 1);
            u32 result_type = GetResultType(dest_reg, base_reg);
            CoreType base_type = static_cast<CoreType>(ir->registerTypes[base_reg & 0x3FFF]);
            bool useUint = (base_type == CoreType::UINT);
            u32 base = GetSpirvIdForBitwise(base_reg, useUint);
            u32 shift = GetSpirvIdForBitwise(shift_reg, useUint);
            Emit(spv::OpShiftRightLogical, result_type, dest, base, shift);
            break;
        }

        case IR::OP_ASR: {
            // Arithmetic shift right (signed)
            u16 dest_reg = ir->destinations[ir_idx];
            u32 base = GetSpirvId(ir->GetOperand(ir_idx, 0));
            u32 shift = GetSpirvId(ir->GetOperand(ir_idx, 1));
            u32 result_type = GetResultType(dest_reg, ir->GetOperand(ir_idx, 0));
            Emit(spv::OpShiftRightArithmetic, result_type, dest, base, shift);
            break;
        }

        case IR::OP_POPCNT: {
            // Bit count (popcount)
            u16 dest_reg = ir->destinations[ir_idx];
            u16 op_reg = ir->GetOperand(ir_idx, 0);
            u32 operand = GetSpirvId(op_reg);
            u32 result_type = GetResultType(dest_reg, op_reg);
            Emit(spv::OpBitCount, result_type, dest, operand);
            break;
        }

        case IR::OP_REVERSE_BITS: {
            // Bit reverse
            u16 dest_reg = ir->destinations[ir_idx];
            u16 op_reg = ir->GetOperand(ir_idx, 0);
            u32 operand = GetSpirvId(op_reg);
            u32 result_type = GetResultType(dest_reg, op_reg);
            Emit(spv::OpBitReverse, result_type, dest, operand);
            break;
        }
        
        // ========== Float Comparison ==========
        case IR::OP_FEQ:
        case IR::OP_FNE:
        case IR::OP_FLT:
        case IR::OP_FLE:
        case IR::OP_FGT:
        case IR::OP_FGE: {
            u16 op1_reg = ir->GetOperand(ir_idx, 0);
            u16 op2_reg = ir->GetOperand(ir_idx, 1);
            u32 op1 = GetSpirvId(op1_reg);
            u32 op2 = GetSpirvId(op2_reg);

            // Determine operand types to check for vector comparison
            // Constants (0x8000=float, 0x4000=int, 0x2000=uint, 0xC000=bool) are scalars
            bool op1_is_scalar = (op1_reg & 0xE000) != 0;
            bool op2_is_scalar = (op2_reg & 0xE000) != 0;
            CoreType op1_type = CoreType::FLOAT;
            CoreType op2_type = CoreType::FLOAT;

            if (ir->registerTypes) {
                if (!op1_is_scalar && op1_reg < ir->registerCount) {
                    op1_type = static_cast<CoreType>(ir->registerTypes[op1_reg]);
                    op1_is_scalar = (op1_type == CoreType::FLOAT || op1_type == CoreType::INT || op1_type == CoreType::UINT);
                }
                if (!op2_is_scalar && op2_reg < ir->registerCount) {
                    op2_type = static_cast<CoreType>(ir->registerTypes[op2_reg]);
                    op2_is_scalar = (op2_type == CoreType::FLOAT || op2_type == CoreType::INT || op2_type == CoreType::UINT);
                }
            }

            // Determine result type and number of components
            u32 result_type;
            u32 numComponents = 1;
            CoreType vectorType = CoreType::FLOAT;

            if (!op1_is_scalar) {
                vectorType = op1_type;
            } else if (!op2_is_scalar) {
                vectorType = op2_type;
            }

            // Determine number of components based on vector type
            if (vectorType == CoreType::FLOAT2 || vectorType == CoreType::INT2 || vectorType == CoreType::UINT2) {
                numComponents = 2;
                result_type = GetTypeId(CoreType::BOOL2);
            } else if (vectorType == CoreType::FLOAT3 || vectorType == CoreType::INT3 || vectorType == CoreType::UINT3) {
                numComponents = 3;
                result_type = GetTypeId(CoreType::BOOL3);
            } else if (vectorType == CoreType::FLOAT4 || vectorType == CoreType::INT4 || vectorType == CoreType::UINT4) {
                numComponents = 4;
                result_type = GetTypeId(CoreType::BOOL4);
            } else {
                // Scalar comparison
                result_type = GetTypeId(CoreType::BOOL);
            }

            // If one operand is scalar and other is vector, splat the scalar
            if (numComponents > 1) {
                u32 float_type = GetTypeId(CoreType::FLOAT);
                u32 vec_type = GetTypeId(vectorType);

                if (op1_is_scalar && !op2_is_scalar) {
                    // Splat op1 to vector
                    u32 splatted = AllocateId();
                    currentFunction[currentFunctionSize++] = ((3 + numComponents) << 16) | spv::OpCompositeConstruct;
                    currentFunction[currentFunctionSize++] = vec_type;
                    currentFunction[currentFunctionSize++] = splatted;
                    for (u32 i = 0; i < numComponents; i++) {
                        currentFunction[currentFunctionSize++] = op1;
                    }
                    op1 = splatted;
                } else if (op2_is_scalar && !op1_is_scalar) {
                    // Splat op2 to vector
                    u32 splatted = AllocateId();
                    currentFunction[currentFunctionSize++] = ((3 + numComponents) << 16) | spv::OpCompositeConstruct;
                    currentFunction[currentFunctionSize++] = vec_type;
                    currentFunction[currentFunctionSize++] = splatted;
                    for (u32 i = 0; i < numComponents; i++) {
                        currentFunction[currentFunctionSize++] = op2;
                    }
                    op2 = splatted;
                }
            }

            // Map IR opcode to SPIR-V opcode
            spv::Op cmp_op;
            switch (op) {
                case IR::OP_FEQ: cmp_op = spv::OpFOrdEqual; break;
                case IR::OP_FNE: cmp_op = spv::OpFOrdNotEqual; break;
                case IR::OP_FLT: cmp_op = spv::OpFOrdLessThan; break;
                case IR::OP_FLE: cmp_op = spv::OpFOrdLessThanEqual; break;
                case IR::OP_FGT: cmp_op = spv::OpFOrdGreaterThan; break;
                case IR::OP_FGE: cmp_op = spv::OpFOrdGreaterThanEqual; break;
                default: cmp_op = spv::OpNop; break;
            }

            Emit(cmp_op, result_type, dest, op1, op2);
            break;
        }
        
        // ========== Integer Comparison (Signed) ==========
        case IR::OP_IEQ:
        case IR::OP_INE:
        case IR::OP_ILT:
        case IR::OP_ILE:
        case IR::OP_IGT:
        case IR::OP_IGE: {
            u16 op1_reg = ir->GetOperand(ir_idx, 0);
            u16 op2_reg = ir->GetOperand(ir_idx, 1);
            u32 op1 = GetSpirvId(op1_reg);
            u32 op2 = GetSpirvId(op2_reg);
            u32 bool_type = GetTypeId(CoreType::BOOL);

            // Check if operands are booleans - use OpLogicalEqual/OpLogicalNotEqual
            CoreType op1_type = CoreType::INT;
            if (ir->registerTypes && (op1_reg & 0xE000) == 0 && op1_reg < ir->registerCount) {
                op1_type = static_cast<CoreType>(ir->registerTypes[op1_reg]);
            } else if ((op1_reg & 0xC000) == 0xC000) {
                // Bool constant flag (0xC000)
                op1_type = CoreType::BOOL;
            }

            spv::Op cmp_op;
            if (op1_type == CoreType::BOOL) {
                // Use logical operations for booleans
                switch (op) {
                    case IR::OP_IEQ: cmp_op = spv::OpLogicalEqual; break;
                    case IR::OP_INE: cmp_op = spv::OpLogicalNotEqual; break;
                    default:
                        // Less than/greater than don't make sense for booleans
                        // Fall back to integer comparison (will fail validation if wrong)
                        cmp_op = spv::OpIEqual;
                        break;
                }
            } else {
                switch (op) {
                    case IR::OP_IEQ: cmp_op = spv::OpIEqual; break;
                    case IR::OP_INE: cmp_op = spv::OpINotEqual; break;
                    case IR::OP_ILT: cmp_op = spv::OpSLessThan; break;
                    case IR::OP_ILE: cmp_op = spv::OpSLessThanEqual; break;
                    case IR::OP_IGT: cmp_op = spv::OpSGreaterThan; break;
                    case IR::OP_IGE: cmp_op = spv::OpSGreaterThanEqual; break;
                    default: cmp_op = spv::OpNop; break;
                }
            }

            Emit(cmp_op, bool_type, dest, op1, op2);
            break;
        }

        // ========== Integer Comparison (Unsigned) ==========
        case IR::OP_ULT:
        case IR::OP_ULE:
        case IR::OP_UGT:
        case IR::OP_UGE: {
            u32 op1 = GetSpirvId(ir->GetOperand(ir_idx, 0));
            u32 op2 = GetSpirvId(ir->GetOperand(ir_idx, 1));
            u32 bool_type = GetTypeId(CoreType::BOOL);

            spv::Op cmp_op;
            switch (op) {
                case IR::OP_ULT: cmp_op = spv::OpULessThan; break;
                case IR::OP_ULE: cmp_op = spv::OpULessThanEqual; break;
                case IR::OP_UGT: cmp_op = spv::OpUGreaterThan; break;
                case IR::OP_UGE: cmp_op = spv::OpUGreaterThanEqual; break;
                default: cmp_op = spv::OpNop; break;
            }

            Emit(cmp_op, bool_type, dest, op1, op2);
            break;
        }

        // ========== Select (Ternary) ==========
        case IR::OP_SELECT: {
            // OpSelect: result = condition ? true_val : false_val
            // BWSL select(a, b, cond) = if cond then b else a
            // IR convention: SELECT(false_val, true_val, condition)
            // SPIR-V OpSelect: OpSelect result_type result condition true_val false_val
            u16 dest_reg = ir->destinations[ir_idx];
            u16 false_val_reg = ir->GetOperand(ir_idx, 0);  // First arg is false value
            u16 true_val_reg = ir->GetOperand(ir_idx, 1);   // Second arg is true value
            u32 condition = GetSpirvId(ir->GetOperand(ir_idx, 2));  // Third arg is condition
            u32 true_val = GetSpirvId(true_val_reg);
            u32 false_val = GetSpirvId(false_val_reg);

            // Result type must match the value types (NOT the condition type)
            // Get type from true_val operand, not from destination (which may be incorrectly typed as bool)
            CoreType valType = CoreType::FLOAT;
            if (ir->registerTypes && true_val_reg < ir->registerCount) {
                valType = static_cast<CoreType>(ir->registerTypes[true_val_reg]);
            } else if ((true_val_reg & 0xC000) == 0xC000) {
                valType = CoreType::BOOL;   // Bool constant (0xC000 prefix)
            } else if (true_val_reg & 0x8000) {
                valType = CoreType::FLOAT;  // Float constant
            } else if (true_val_reg & 0x4000) {
                valType = CoreType::INT;    // Int constant
            } else if (true_val_reg & 0x2000) {
                valType = CoreType::UINT;   // Uint constant
            }
            u32 result_type = GetTypeId(valType);

            // Check if result is a vector - if so, we need to splat scalar bool condition to bool vector
            CoreType destType = valType;

            u32 numComponents = 0;
            switch (destType) {
                case CoreType::FLOAT2: case CoreType::INT2: case CoreType::UINT2:
                    numComponents = 2; break;
                case CoreType::FLOAT3: case CoreType::INT3: case CoreType::UINT3:
                    numComponents = 3; break;
                case CoreType::FLOAT4: case CoreType::INT4: case CoreType::UINT4:
                    numComponents = 4; break;
                default:
                    numComponents = 0; break;  // Scalar result, no splatting needed
            }

            if (numComponents > 0) {
                // Splat scalar bool to bool vector
                u32 bool_vec_type = GetTypeId(numComponents == 2 ? CoreType::BOOL2 :
                                              numComponents == 3 ? CoreType::BOOL3 : CoreType::BOOL4);
                u32 splatted_cond = AllocateId();
                if (currentFunctionSize + 3 + numComponents > currentFunctionCapacity) {
                    GrowCurrentFunction();
                }
                currentFunction[currentFunctionSize++] = ((3 + numComponents) << 16) | spv::OpCompositeConstruct;
                currentFunction[currentFunctionSize++] = bool_vec_type;
                currentFunction[currentFunctionSize++] = splatted_cond;
                for (u32 i = 0; i < numComponents; i++) {
                    currentFunction[currentFunctionSize++] = condition;
                }
                condition = splatted_cond;
            }

            // OpSelect: result_type result condition object1(true) object2(false)
            Emit(spv::OpSelect, result_type, dest, condition, true_val, false_val);
            break;
        }

        // ========== Type Conversion Operations ==========
        case IR::OP_I2F: {
            // Signed int to float: OpConvertSToF
            u16 dest_reg = ir->destinations[ir_idx];
            u32 operand = GetSpirvId(ir->GetOperand(ir_idx, 0));
            u32 result_type = GetTypeId(CoreType::FLOAT);
            Emit(spv::OpConvertSToF, result_type, dest, operand);
            break;
        }

        case IR::OP_U2F: {
            // Unsigned int to float: OpConvertUToF
            u16 dest_reg = ir->destinations[ir_idx];
            u32 operand = GetSpirvId(ir->GetOperand(ir_idx, 0));
            u32 result_type = GetTypeId(CoreType::FLOAT);
            Emit(spv::OpConvertUToF, result_type, dest, operand);
            break;
        }

        case IR::OP_F2I: {
            // Float to signed int: OpConvertFToS
            u16 dest_reg = ir->destinations[ir_idx];
            u32 operand = GetSpirvId(ir->GetOperand(ir_idx, 0));
            u32 result_type = GetTypeId(CoreType::INT);
            Emit(spv::OpConvertFToS, result_type, dest, operand);
            break;
        }

        case IR::OP_F2U: {
            // Float to unsigned int: OpConvertFToU
            u16 dest_reg = ir->destinations[ir_idx];
            u32 operand = GetSpirvId(ir->GetOperand(ir_idx, 0));
            u32 result_type = GetTypeId(CoreType::UINT);
            Emit(spv::OpConvertFToU, result_type, dest, operand);
            break;
        }

        case IR::OP_I2U:
        case IR::OP_U2I: {
            // Int/uint conversion is just bitcast (same bit representation)
            u16 dest_reg = ir->destinations[ir_idx];
            u32 operand = GetSpirvId(ir->GetOperand(ir_idx, 0));
            CoreType resultType = (op == IR::OP_I2U) ? CoreType::UINT : CoreType::INT;
            u32 result_type = GetTypeId(resultType);
            Emit(spv::OpBitcast, result_type, dest, operand);
            break;
        }

        // ========== Extended Instructions (GLSL.std.450) ==========
        // Single-operand functions (result type matches input type)
        case IR::OP_SQRT:
        case IR::OP_RSQRT:
        case IR::OP_EXP:
        case IR::OP_EXP2:
        case IR::OP_LOG:
        case IR::OP_LOG2:
        case IR::OP_SIN:
        case IR::OP_COS:
        case IR::OP_TAN:
        case IR::OP_ASIN:
        case IR::OP_ACOS:
        case IR::OP_ATAN:
        case IR::OP_FLOOR:
        case IR::OP_CEIL:
        case IR::OP_ROUND:
        case IR::OP_TRUNC:
        case IR::OP_FRACT:
        case IR::OP_FABS:
        case IR::OP_IABS:  // Integer abs (SAbs in GLSL.std.450)
        case IR::OP_SIGN:
        case IR::OP_NORMALIZE:
        case IR::OP_CLZ:
        case IR::OP_CTZ:
        case IR::OP_DEGREES:
        case IR::OP_RADIANS: {
            u16 dest_reg = ir->destinations[ir_idx];
            u32 result_type = GetResultType(dest_reg, ir->GetOperand(ir_idx, 0));
            u32 operand = GetSpirvId(ir->GetOperand(ir_idx, 0));
            u32 glsl_op = IR_TO_GLSL_STD_450_TABLE[static_cast<u32>(op)];
            
            // OpExtInst: result_type, result_id, set_id, instruction, operands...
            if (currentFunctionSize + 6 > currentFunctionCapacity) {
                GrowCurrentFunction();
            }
            currentFunction[currentFunctionSize++] = (6 << 16) | spv::OpExtInst;
            currentFunction[currentFunctionSize++] = result_type;
            currentFunction[currentFunctionSize++] = dest;
            currentFunction[currentFunctionSize++] = glslStd450Id;
            currentFunction[currentFunctionSize++] = glsl_op;
            currentFunction[currentFunctionSize++] = operand;
            break;
        }

        // Saturate: clamp(x, 0.0, 1.0) - single operand in IR, emits FClamp with implicit 0,1
        case IR::OP_SATURATE: {
            u16 dest_reg = ir->destinations[ir_idx];
            u16 op_reg = ir->GetOperand(ir_idx, 0);
            u32 result_type = GetResultType(dest_reg, op_reg);
            u32 operand = GetSpirvId(op_reg);
            u32 glsl_op = GLSLstd450FClamp;

            // Get scalar float constants
            u32 zero_scalar = GetFloatConstantId(0.0f);
            u32 one_scalar = GetFloatConstantId(1.0f);

            // For vector types, construct vector constants matching the operand type
            u32 zero_const = zero_scalar;
            u32 one_const = one_scalar;

            // Check if operand is a vector type
            CoreType opType = CoreType::FLOAT;
            if (ir->registerTypes && op_reg < ir->registerCount) {
                opType = static_cast<CoreType>(ir->registerTypes[op_reg]);
            }

            u32 numComponents = 1;
            if (opType == CoreType::FLOAT2) numComponents = 2;
            else if (opType == CoreType::FLOAT3) numComponents = 3;
            else if (opType == CoreType::FLOAT4) numComponents = 4;

            if (numComponents > 1) {
                // Create vector constants by splatting scalar values
                u32 vec_type = GetTypeId(opType);

                // Create vec(0, 0, ...) - splat zero_scalar for all components
                u32 zero_constituents[4] = {zero_scalar, zero_scalar, zero_scalar, zero_scalar};
                zero_const = GetCompositeConstantId(vec_type, zero_constituents, numComponents);

                // Create vec(1, 1, ...) - splat one_scalar for all components
                u32 one_constituents[4] = {one_scalar, one_scalar, one_scalar, one_scalar};
                one_const = GetCompositeConstantId(vec_type, one_constituents, numComponents);
            }

            // OpExtInst: result_type, result_id, set_id, instruction, x, minVal, maxVal
            if (currentFunctionSize + 8 > currentFunctionCapacity) {
                GrowCurrentFunction();
            }
            currentFunction[currentFunctionSize++] = (8 << 16) | spv::OpExtInst;
            currentFunction[currentFunctionSize++] = result_type;
            currentFunction[currentFunctionSize++] = dest;
            currentFunction[currentFunctionSize++] = glslStd450Id;
            currentFunction[currentFunctionSize++] = glsl_op;
            currentFunction[currentFunctionSize++] = operand;
            currentFunction[currentFunctionSize++] = zero_const;
            currentFunction[currentFunctionSize++] = one_const;
            break;
        }

        // Single-operand functions that return scalar float (length, distance with 1 arg)
        case IR::OP_LENGTH: {
            u32 result_type = GetTypeId(CoreType::FLOAT);  // Length always returns scalar
            u32 operand = GetSpirvId(ir->GetOperand(ir_idx, 0));
            u32 glsl_op = IR_TO_GLSL_STD_450_TABLE[static_cast<u32>(op)];
            
            if (currentFunctionSize + 6 > currentFunctionCapacity) {
                GrowCurrentFunction();
            }
            currentFunction[currentFunctionSize++] = (6 << 16) | spv::OpExtInst;
            currentFunction[currentFunctionSize++] = result_type;
            currentFunction[currentFunctionSize++] = dest;
            currentFunction[currentFunctionSize++] = glslStd450Id;
            currentFunction[currentFunctionSize++] = glsl_op;
            currentFunction[currentFunctionSize++] = operand;
            break;
        }
        
        // Dot product - core SPIR-V op, returns scalar float
        case IR::OP_DOT: {
            u32 result_type = GetTypeId(CoreType::FLOAT);  // Dot always returns scalar
            u32 op1 = GetSpirvId(ir->GetOperand(ir_idx, 0));
            u32 op2 = GetSpirvId(ir->GetOperand(ir_idx, 1));
            
            // OpDot: result_type, result_id, vector1, vector2
            Emit(spv::OpDot, result_type, dest, op1, op2);
            break;
        }
        
        // Two-operand extended functions (result type matches input type)
        case IR::OP_POW:
        case IR::OP_ATAN2:
        case IR::OP_FMIN:
        case IR::OP_FMAX:
        case IR::OP_IMIN:   // Integer min (SMin in GLSL.std.450)
        case IR::OP_IMAX:   // Integer max (SMax in GLSL.std.450)
        case IR::OP_UMIN:   // Unsigned min (UMin in GLSL.std.450)
        case IR::OP_UMAX:   // Unsigned max (UMax in GLSL.std.450)
        case IR::OP_STEP:
        case IR::OP_REFLECT:
        case IR::OP_CROSS: {
            u16 dest_reg = ir->destinations[ir_idx];
            u16 op1_reg = ir->GetOperand(ir_idx, 0);
            u16 op2_reg = ir->GetOperand(ir_idx, 1);
            u32 result_type = GetResultType(dest_reg, op1_reg);
            u32 op1 = GetSpirvId(op1_reg);
            u32 op2 = GetSpirvId(op2_reg);
            u32 glsl_op = IR_TO_GLSL_STD_450_TABLE[static_cast<u32>(op)];

            // For GLSL.std.450 functions like Pow, both operands must match result type
            // Handle scalar-vector mismatches by splatting scalar to vector
            // Constants (0x8000=float, 0x4000=int, 0x2000=uint, 0xC000=bool) are scalars
            bool op1_is_scalar = (op1_reg & 0xE000) != 0;
            bool op2_is_scalar = (op2_reg & 0xE000) != 0;
            CoreType op1_type = CoreType::FLOAT;
            CoreType op2_type = CoreType::FLOAT;

            if (ir->registerTypes) {
                if (!op1_is_scalar && op1_reg < ir->registerCount) {
                    op1_type = static_cast<CoreType>(ir->registerTypes[op1_reg]);
                    op1_is_scalar = (op1_type == CoreType::FLOAT || op1_type == CoreType::INT || op1_type == CoreType::UINT);
                }
                if (!op2_is_scalar && op2_reg < ir->registerCount) {
                    op2_type = static_cast<CoreType>(ir->registerTypes[op2_reg]);
                    op2_is_scalar = (op2_type == CoreType::FLOAT || op2_type == CoreType::INT || op2_type == CoreType::UINT);
                }
            }

            // Determine vector type and component count
            // Skip VOID and INVALID types - only use valid vector types
            CoreType vectorType = CoreType::FLOAT;
            u32 numComponents = 1;
            if (!op1_is_scalar && op1_type != CoreType::VOID && op1_type != CoreType::INVALID) {
                vectorType = op1_type;
            } else if (!op2_is_scalar && op2_type != CoreType::VOID && op2_type != CoreType::INVALID) {
                vectorType = op2_type;
            }

            if (vectorType == CoreType::FLOAT2 || vectorType == CoreType::INT2 || vectorType == CoreType::UINT2) {
                numComponents = 2;
            } else if (vectorType == CoreType::FLOAT3 || vectorType == CoreType::INT3 || vectorType == CoreType::UINT3) {
                numComponents = 3;
            } else if (vectorType == CoreType::FLOAT4 || vectorType == CoreType::INT4 || vectorType == CoreType::UINT4) {
                numComponents = 4;
            }

            // Splat scalar operands to match vector type
            if (numComponents > 1) {
                u32 vec_type = GetTypeId(vectorType);
                // Update result_type to use the correct vector type
                result_type = vec_type;
                if (op1_is_scalar && !op2_is_scalar) {
                    u32 splatted = AllocateId();
                    currentFunction[currentFunctionSize++] = ((3 + numComponents) << 16) | spv::OpCompositeConstruct;
                    currentFunction[currentFunctionSize++] = vec_type;
                    currentFunction[currentFunctionSize++] = splatted;
                    for (u32 i = 0; i < numComponents; i++) {
                        currentFunction[currentFunctionSize++] = op1;
                    }
                    op1 = splatted;
                } else if (op2_is_scalar && !op1_is_scalar) {
                    u32 splatted = AllocateId();
                    currentFunction[currentFunctionSize++] = ((3 + numComponents) << 16) | spv::OpCompositeConstruct;
                    currentFunction[currentFunctionSize++] = vec_type;
                    currentFunction[currentFunctionSize++] = splatted;
                    for (u32 i = 0; i < numComponents; i++) {
                        currentFunction[currentFunctionSize++] = op2;
                    }
                    op2 = splatted;
                }
            }

            if (currentFunctionSize + 7 > currentFunctionCapacity) {
                GrowCurrentFunction();
            }
            currentFunction[currentFunctionSize++] = (7 << 16) | spv::OpExtInst;
            currentFunction[currentFunctionSize++] = result_type;
            currentFunction[currentFunctionSize++] = dest;
            currentFunction[currentFunctionSize++] = glslStd450Id;
            currentFunction[currentFunctionSize++] = glsl_op;
            currentFunction[currentFunctionSize++] = op1;
            currentFunction[currentFunctionSize++] = op2;
            break;
        }
        
        // Distance returns scalar float
        case IR::OP_DISTANCE: {
            u32 result_type = GetTypeId(CoreType::FLOAT);
            u32 op1 = GetSpirvId(ir->GetOperand(ir_idx, 0));
            u32 op2 = GetSpirvId(ir->GetOperand(ir_idx, 1));
            u32 glsl_op = IR_TO_GLSL_STD_450_TABLE[static_cast<u32>(op)];
            
            if (currentFunctionSize + 7 > currentFunctionCapacity) {
                GrowCurrentFunction();
            }
            currentFunction[currentFunctionSize++] = (7 << 16) | spv::OpExtInst;
            currentFunction[currentFunctionSize++] = result_type;
            currentFunction[currentFunctionSize++] = dest;
            currentFunction[currentFunctionSize++] = glslStd450Id;
            currentFunction[currentFunctionSize++] = glsl_op;
            currentFunction[currentFunctionSize++] = op1;
            currentFunction[currentFunctionSize++] = op2;
            break;
        }
        
        // Lerp (FMix) - needs special handling for scalar interpolant with vector operands
        case IR::OP_LERP: {
            u16 dest_reg = ir->destinations[ir_idx];
            u16 op1_reg = ir->GetOperand(ir_idx, 0);
            u16 op2_reg = ir->GetOperand(ir_idx, 1);
            u16 op3_reg = ir->GetOperand(ir_idx, 2);
            u32 result_type = GetResultType(dest_reg, op1_reg);
            u32 op1 = GetSpirvId(op1_reg);
            u32 op2 = GetSpirvId(op2_reg);
            u32 op3 = GetSpirvId(op3_reg);
            u32 glsl_op = GLSLstd450FMix;

            // Check if op3 (interpolant) is scalar while result is vector
            // SPIR-V FMix requires all operands to match result type
            // Constants (0x8000=float, 0x4000=int, 0x2000=uint, 0xC000=bool) are scalars
            bool op3_is_scalar = (op3_reg & 0xE000) != 0;
            CoreType op1_type = CoreType::FLOAT;
            CoreType op3_type = CoreType::FLOAT;

            if (ir->registerTypes) {
                if (op1_reg < ir->registerCount) {
                    op1_type = static_cast<CoreType>(ir->registerTypes[op1_reg]);
                }
                if (!op3_is_scalar && op3_reg < ir->registerCount) {
                    op3_type = static_cast<CoreType>(ir->registerTypes[op3_reg]);
                    op3_is_scalar = (op3_type == CoreType::FLOAT || op3_type == CoreType::INT || op3_type == CoreType::UINT);
                }
            }

            // Determine if we need to splat op3 to match vector type
            u32 numComponents = 1;
            if (op1_type == CoreType::FLOAT2 || op1_type == CoreType::INT2 || op1_type == CoreType::UINT2) {
                numComponents = 2;
            } else if (op1_type == CoreType::FLOAT3 || op1_type == CoreType::INT3 || op1_type == CoreType::UINT3) {
                numComponents = 3;
            } else if (op1_type == CoreType::FLOAT4 || op1_type == CoreType::INT4 || op1_type == CoreType::UINT4) {
                numComponents = 4;
            }

            // Splat scalar interpolant to vector if needed
            if (numComponents > 1 && op3_is_scalar) {
                u32 vec_type = GetTypeId(op1_type);
                result_type = vec_type;

                if (currentFunctionSize + 3 + numComponents > currentFunctionCapacity) {
                    GrowCurrentFunction();
                }
                u32 splatted = AllocateId();
                currentFunction[currentFunctionSize++] = ((3 + numComponents) << 16) | spv::OpCompositeConstruct;
                currentFunction[currentFunctionSize++] = vec_type;
                currentFunction[currentFunctionSize++] = splatted;
                for (u32 i = 0; i < numComponents; i++) {
                    currentFunction[currentFunctionSize++] = op3;
                }
                op3 = splatted;
            }

            if (currentFunctionSize + 8 > currentFunctionCapacity) {
                GrowCurrentFunction();
            }
            currentFunction[currentFunctionSize++] = (8 << 16) | spv::OpExtInst;
            currentFunction[currentFunctionSize++] = result_type;
            currentFunction[currentFunctionSize++] = dest;
            currentFunction[currentFunctionSize++] = glslStd450Id;
            currentFunction[currentFunctionSize++] = glsl_op;
            currentFunction[currentFunctionSize++] = op1;
            currentFunction[currentFunctionSize++] = op2;
            currentFunction[currentFunctionSize++] = op3;
            break;
        }

        // Three-operand extended functions
        case IR::OP_FCLAMP:
        case IR::OP_ICLAMP:   // Integer clamp (SClamp in GLSL.std.450)
        case IR::OP_UCLAMP:   // Unsigned clamp (UClamp in GLSL.std.450)
        case IR::OP_SMOOTHSTEP:
        case IR::OP_FMA:
        case IR::OP_REFRACT: {
            u16 dest_reg = ir->destinations[ir_idx];
            u16 op1_reg = ir->GetOperand(ir_idx, 0);
            u16 op2_reg = ir->GetOperand(ir_idx, 1);
            u16 op3_reg = ir->GetOperand(ir_idx, 2);
            u32 result_type = GetResultType(dest_reg, op1_reg);
            u32 op1 = GetSpirvId(op1_reg);
            u32 op2 = GetSpirvId(op2_reg);
            u32 op3 = GetSpirvId(op3_reg);
            u32 glsl_op = IR_TO_GLSL_STD_450_TABLE[static_cast<u32>(op)];

            // For GLSL.std.450 ops (except Refract), operands must match result type.
            // Splat scalar operands to vector when needed.
            bool op1_is_scalar = (op1_reg & 0xE000) != 0;
            bool op2_is_scalar = (op2_reg & 0xE000) != 0;
            bool op3_is_scalar = (op3_reg & 0xE000) != 0;
            CoreType op1_type = CoreType::FLOAT;
            CoreType op2_type = CoreType::FLOAT;
            CoreType op3_type = CoreType::FLOAT;

            if (ir->registerTypes) {
                if (!op1_is_scalar && op1_reg < ir->registerCount) {
                    op1_type = static_cast<CoreType>(ir->registerTypes[op1_reg]);
                    op1_is_scalar = (op1_type == CoreType::FLOAT || op1_type == CoreType::INT || op1_type == CoreType::UINT);
                }
                if (!op2_is_scalar && op2_reg < ir->registerCount) {
                    op2_type = static_cast<CoreType>(ir->registerTypes[op2_reg]);
                    op2_is_scalar = (op2_type == CoreType::FLOAT || op2_type == CoreType::INT || op2_type == CoreType::UINT);
                }
                if (!op3_is_scalar && op3_reg < ir->registerCount) {
                    op3_type = static_cast<CoreType>(ir->registerTypes[op3_reg]);
                    op3_is_scalar = (op3_type == CoreType::FLOAT || op3_type == CoreType::INT || op3_type == CoreType::UINT);
                }
            }

            CoreType vectorType = CoreType::FLOAT;
            if (!op1_is_scalar && op1_type != CoreType::VOID && op1_type != CoreType::INVALID) {
                vectorType = op1_type;
            } else if (!op2_is_scalar && op2_type != CoreType::VOID && op2_type != CoreType::INVALID) {
                vectorType = op2_type;
            } else if (!op3_is_scalar && op3_type != CoreType::VOID && op3_type != CoreType::INVALID) {
                vectorType = op3_type;
            }

            u32 numComponents = 1;
            if (vectorType == CoreType::FLOAT2 || vectorType == CoreType::INT2 || vectorType == CoreType::UINT2) {
                numComponents = 2;
            } else if (vectorType == CoreType::FLOAT3 || vectorType == CoreType::INT3 || vectorType == CoreType::UINT3) {
                numComponents = 3;
            } else if (vectorType == CoreType::FLOAT4 || vectorType == CoreType::INT4 || vectorType == CoreType::UINT4) {
                numComponents = 4;
            }

            if (numComponents > 1 && op != IR::OP_REFRACT) {
                u32 vec_type = GetTypeId(vectorType);
                result_type = vec_type;
                if (op1_is_scalar) {
                    if (currentFunctionSize + 3 + numComponents > currentFunctionCapacity) {
                        GrowCurrentFunction();
                    }
                    u32 splatted = AllocateId();
                    currentFunction[currentFunctionSize++] = ((3 + numComponents) << 16) | spv::OpCompositeConstruct;
                    currentFunction[currentFunctionSize++] = vec_type;
                    currentFunction[currentFunctionSize++] = splatted;
                    for (u32 i = 0; i < numComponents; i++) {
                        currentFunction[currentFunctionSize++] = op1;
                    }
                    op1 = splatted;
                }
                if (op2_is_scalar) {
                    if (currentFunctionSize + 3 + numComponents > currentFunctionCapacity) {
                        GrowCurrentFunction();
                    }
                    u32 splatted = AllocateId();
                    currentFunction[currentFunctionSize++] = ((3 + numComponents) << 16) | spv::OpCompositeConstruct;
                    currentFunction[currentFunctionSize++] = vec_type;
                    currentFunction[currentFunctionSize++] = splatted;
                    for (u32 i = 0; i < numComponents; i++) {
                        currentFunction[currentFunctionSize++] = op2;
                    }
                    op2 = splatted;
                }
                if (op3_is_scalar) {
                    if (currentFunctionSize + 3 + numComponents > currentFunctionCapacity) {
                        GrowCurrentFunction();
                    }
                    u32 splatted = AllocateId();
                    currentFunction[currentFunctionSize++] = ((3 + numComponents) << 16) | spv::OpCompositeConstruct;
                    currentFunction[currentFunctionSize++] = vec_type;
                    currentFunction[currentFunctionSize++] = splatted;
                    for (u32 i = 0; i < numComponents; i++) {
                        currentFunction[currentFunctionSize++] = op3;
                    }
                    op3 = splatted;
                }
            }

            if (currentFunctionSize + 8 > currentFunctionCapacity) {
                GrowCurrentFunction();
            }
            currentFunction[currentFunctionSize++] = (8 << 16) | spv::OpExtInst;
            currentFunction[currentFunctionSize++] = result_type;
            currentFunction[currentFunctionSize++] = dest;
            currentFunction[currentFunctionSize++] = glslStd450Id;
            currentFunction[currentFunctionSize++] = glsl_op;
            currentFunction[currentFunctionSize++] = op1;
            currentFunction[currentFunctionSize++] = op2;
            currentFunction[currentFunctionSize++] = op3;
            break;
        }
        
        case IR::OP_LOAD_ATTR: {
            u32 attr_idx = ir->GetOperand(ir_idx, 0);
            
            // Get the attribute type
            CoreType attrType = static_cast<CoreType>(analysis.attributeTypes[attr_idx]);
            if (attrType == CoreType::VOID || attrType == CoreType::INVALID ||
                static_cast<u8>(attrType) == 0) {
                attrType = GetFallbackAttributeType(attr_idx);
            }
            u32 load_type_id = GetTypeId(attrType);
            
            if (vertexPullingConfig.mode == VertexInputMode::SeparateBuffers) {
                // Vertex pulling: load from storage buffer indexed by gl_VertexIndex
                // 1. Load vertex index
                u32 uint_type = GetTypeId(CoreType::UINT);
                u32 vertex_idx_id = AllocateId();
                Emit(spv::OpLoad, uint_type, vertex_idx_id, vertexIdVarId);
                
                // 2. AccessChain into buffer[0][vertex_idx]
                // Buffer struct is: struct { T[] data; }
                // Access path: buffer -> member 0 -> array[vertex_idx]
                u32 buffer_var_id = attributeBufferIds[attr_idx];
                if (buffer_var_id == 0) break;  // Buffer not declared
                
                u32 zero_const = GetIntConstantId(0);
                u32 element_ptr_type = GetPointerTypeId(load_type_id, spv::StorageClassStorageBuffer);
                u32 element_ptr_id = AllocateId();
                
                // OpAccessChain: result_type, result, base, indices...
                // Indices: 0 (first struct member = the array), vertex_idx (array element)
                if (currentFunctionSize + 6 > currentFunctionCapacity) {
                    GrowCurrentFunction();
                }
                currentFunction[currentFunctionSize++] = (6 << 16) | spv::OpAccessChain;
                currentFunction[currentFunctionSize++] = element_ptr_type;
                currentFunction[currentFunctionSize++] = element_ptr_id;
                currentFunction[currentFunctionSize++] = buffer_var_id;
                currentFunction[currentFunctionSize++] = zero_const;  // struct member 0
                currentFunction[currentFunctionSize++] = vertex_idx_id;  // array index
                
                // 3. Load the value
                Emit(spv::OpLoad, load_type_id, dest, element_ptr_id);
            }
            else if (vertexPullingConfig.mode == VertexInputMode::UnifiedWithOffsets) {
                // TODO: Unified buffer mode - load from single buffer with offset table
                // For now, fall through to interleaved handling
                // This would need: load offset from offset table, compute address, load value
            }
            else {
                // Interleaved mode: load from input variable
                u32 ptr_id = 0;
                for (u32 i = 0; i < inputCount; i++) {
                    if (inputLocations[i] == attr_idx) {
                        ptr_id = inputIds[i];
                        break;
                    }
                }
                if (ptr_id != 0 && load_type_id != 0) {
                    Emit(spv::OpLoad, load_type_id, dest, ptr_id);
                }
            }
            break;
        }
        
        case IR::OP_LOAD_INPUT: {
            u32 input_slot = ir->GetOperand(ir_idx, 0);

            // Handle built-in inputs (vertex_id, instance_id)
            if (input_slot == BuiltinInputSlot::VERTEX_ID) {
                u32 uint_type = GetTypeId(CoreType::UINT);
                if (vertexIdVarId != 0) {
                    Emit(spv::OpLoad, uint_type, dest, vertexIdVarId);
                }
                break;
            }
            if (input_slot == BuiltinInputSlot::INSTANCE_ID) {
                u32 uint_type = GetTypeId(CoreType::UINT);
                if (instanceIdVarId != 0) {
                    Emit(spv::OpLoad, uint_type, dest, instanceIdVarId);
                }
                break;
            }
            if (input_slot == BuiltinInputSlot::GLOBAL_INVOCATION_ID) {
                u32 uint3_type = GetTypeId(CoreType::UINT3);
                if (globalInvocationIdVarId != 0) {
                    Emit(spv::OpLoad, uint3_type, dest, globalInvocationIdVarId);
                }
                break;
            }
            if (input_slot == BuiltinInputSlot::LOCAL_INVOCATION_ID) {
                u32 uint3_type = GetTypeId(CoreType::UINT3);
                if (localInvocationIdVarId != 0) {
                    Emit(spv::OpLoad, uint3_type, dest, localInvocationIdVarId);
                }
                break;
            }
            if (input_slot == BuiltinInputSlot::WORKGROUP_ID) {
                u32 uint3_type = GetTypeId(CoreType::UINT3);
                if (workgroupIdVarId != 0) {
                    Emit(spv::OpLoad, uint3_type, dest, workgroupIdVarId);
                }
                break;
            }
            if (input_slot == BuiltinInputSlot::NUM_WORKGROUPS) {
                u32 uint3_type = GetTypeId(CoreType::UINT3);
                if (numWorkgroupsVarId != 0) {
                    Emit(spv::OpLoad, uint3_type, dest, numWorkgroupsVarId);
                }
                break;
            }
            if (input_slot == BuiltinInputSlot::LOCAL_INVOCATION_INDEX) {
                u32 uint_type = GetTypeId(CoreType::UINT);
                if (localInvocationIndexVarId != 0) {
                    Emit(spv::OpLoad, uint_type, dest, localInvocationIndexVarId);
                }
                break;
            }

            // Fragment shader loading interpolated varying from vertex output
            // Get the input type - for varyings, default to float3
            CoreType inputType = CoreType::FLOAT3;
            if (ir->registerTypes && ir->destinations[ir_idx] < ir->registerCount) {
                CoreType regType = static_cast<CoreType>(ir->registerTypes[ir->destinations[ir_idx]]);
                if (regType != CoreType::VOID && regType != CoreType::INVALID) {
                    inputType = regType;
                }
            }
            u32 load_type_id = GetTypeId(inputType);

            // Find the input variable by slot/location
            u32 ptr_id = 0;
            for (u32 i = 0; i < inputCount; i++) {
                if (inputLocations[i] == input_slot) {
                    ptr_id = inputIds[i];
                    break;
                }
            }

            if (ptr_id != 0 && load_type_id != 0) {
                Emit(spv::OpLoad, load_type_id, dest, ptr_id);
            }
            break;
        }

        case IR::OP_LOAD_UNIFORM: {
            // Load from uniform buffer
            u32 binding = ir->GetOperand(ir_idx, 0);
            u16 dest_reg = ir->destinations[ir_idx];

            // Get the uniform type from register type info
            CoreType uniformType = CoreType::FLOAT4;  // Default
            if (ir->registerTypes && dest_reg < ir->registerCount) {
                CoreType regType = static_cast<CoreType>(ir->registerTypes[dest_reg]);
                if (regType != CoreType::VOID && regType != CoreType::INVALID) {
                    uniformType = regType;
                }
            }
            u32 load_type_id = GetTypeId(uniformType);

            // Get the uniform buffer variable
            u32 buffer_var_id = uniformBufferIds[binding];
            if (buffer_var_id != 0 && load_type_id != 0) {
                // Access chain to get pointer to the first (and only) member of the struct
                u32 zero_const = GetIntConstantId(0);
                u32 member_ptr_type = GetPointerTypeId(load_type_id, spv::StorageClassUniform);
                u32 member_ptr_id = AllocateId();

                // OpAccessChain: result_type, result, base, index
                if (currentFunctionSize + 5 > currentFunctionCapacity) {
                    GrowCurrentFunction();
                }
                currentFunction[currentFunctionSize++] = (5 << 16) | spv::OpAccessChain;
                currentFunction[currentFunctionSize++] = member_ptr_type;
                currentFunction[currentFunctionSize++] = member_ptr_id;
                currentFunction[currentFunctionSize++] = buffer_var_id;
                currentFunction[currentFunctionSize++] = zero_const;  // member index 0

                // Load the value
                Emit(spv::OpLoad, load_type_id, dest, member_ptr_id);
            }
            break;
        }

        case IR::OP_STORE_OUTPUT: {
            // Note: IR lowering puts the value register in destinations, not operands
            // EmitInstruction(OP_STORE_OUTPUT, valueReg, slot) -> destinations = valueReg
            u32 value = GetSpirvId(ir->destinations[ir_idx]);

            // Slot is now stored in operand[0] (set during IR lowering)
            // This enables dynamic vertex-to-fragment varying resolution
            u32 slot = ir->GetOperand(ir_idx, 0);

            // Find the output variable by looking through our declared outputs
            u32 ptr_id = 0;
            for (u32 i = 0; i < outputCount; i++) {
                if (outputLocations[i] == slot ||
                    (outputLocations[i] == 0xFF && slot == OutputSlot::POSITION)) {
                    ptr_id = outputIds[i];
                    break;
                }
            }
            if (ptr_id != 0) {
                Emit(spv::OpStore, ptr_id, value);
            }
            break;
        }
        
        // ========== Vector Operations ==========
        case IR::OP_VEC_CONSTRUCT: {
            // Build a vector from components
            // IR now has 4 operand slots natively for float4 support
            // operands use 0xFFFF as sentinel for "unused"
            u16 dest_reg = ir->destinations[ir_idx];
            CoreType resultType = CoreType::FLOAT4;
            if (ir->registerTypes && dest_reg < ir->registerCount) {
                resultType = static_cast<CoreType>(ir->registerTypes[dest_reg]);
            }
            
            u32 result_type_id = GetTypeId(resultType);
            
            // Get actual argument count from metadata
            u32 argCount = ir->metadata[ir_idx];
            if (argCount == 0 || argCount > 4) argCount = 4; // Fallback
            
            // Collect input operands (0xFFFF means unused)
            // Store both SPIR-V IDs and original register numbers for type checking
            u32 inputIds[4] = {0, 0, 0, 0};
            u16 inputRegs[4] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
            u32 inputCount = 0;

            // All 4 operands from operands array
            for (u32 c = 0; c < argCount; c++) {
                u16 op_reg = ir->GetOperand(ir_idx, c);
                if (op_reg == 0xFFFF) continue; // Sentinel for unused
                inputRegs[inputCount] = op_reg;
                inputIds[inputCount++] = GetSpirvId(op_reg);
            }

            // OpCompositeConstruct can take mixed vectors and scalars
            // Handle scalar broadcast: float3(x) -> OpCompositeConstruct %v3float %x %x %x
            if (inputCount == 0) {
                // No operands provided: emit a zero vector to ensure a defined ID
                u32 requiredComponents = 1;
                switch (resultType) {
                    case CoreType::FLOAT2: case CoreType::INT2: case CoreType::UINT2:
                        requiredComponents = 2; break;
                    case CoreType::FLOAT3: case CoreType::INT3: case CoreType::UINT3:
                        requiredComponents = 3; break;
                    case CoreType::FLOAT4: case CoreType::INT4: case CoreType::UINT4:
                        requiredComponents = 4; break;
                    default: break;
                }

                CoreType desiredScalarType = GetScalarComponentType(resultType);
                u32 zeroId = 0;
                if (desiredScalarType == CoreType::FLOAT) {
                    zeroId = GetFloatConstantId(0.0f);
                } else if (desiredScalarType == CoreType::INT) {
                    zeroId = GetIntConstantId(0, false);
                } else if (desiredScalarType == CoreType::UINT) {
                    zeroId = GetIntConstantId(0, true);
                } else if (desiredScalarType == CoreType::BOOL) {
                    zeroId = GetBoolConstantId(false);
                }

                u32 wordCount = 3 + requiredComponents;
                if (currentFunctionSize + wordCount > currentFunctionCapacity) {
                    GrowCurrentFunction();
                }
                currentFunction[currentFunctionSize++] = (wordCount << 16) | spv::OpCompositeConstruct;
                currentFunction[currentFunctionSize++] = result_type_id;
                currentFunction[currentFunctionSize++] = dest;
                for (u32 c = 0; c < requiredComponents; c++) {
                    currentFunction[currentFunctionSize++] = zeroId;
                }
                break;
            }

            if (inputCount > 0) {
                // Determine required component count for target vector type
                u32 requiredComponents = 1;
                switch (resultType) {
                    case CoreType::FLOAT2: case CoreType::INT2: case CoreType::UINT2:
                        requiredComponents = 2; break;
                    case CoreType::FLOAT3: case CoreType::INT3: case CoreType::UINT3:
                        requiredComponents = 3; break;
                    case CoreType::FLOAT4: case CoreType::INT4: case CoreType::UINT4:
                        requiredComponents = 4; break;
                    default: break;
                }

                CoreType desiredScalarType = GetScalarComponentType(resultType);
                auto convertScalar = [&](u32 value_id, CoreType srcType) -> u32 {
                    if (srcType == desiredScalarType) return value_id;
                    u32 dest_type_id = GetTypeId(desiredScalarType);
                    u32 converted = AllocateId();

                    if (srcType == CoreType::BOOL) {
                        u32 zero_id = 0;
                        u32 one_id = 0;
                        if (desiredScalarType == CoreType::FLOAT) {
                            zero_id = GetFloatConstantId(0.0f);
                            one_id = GetFloatConstantId(1.0f);
                        } else if (desiredScalarType == CoreType::INT) {
                            zero_id = GetIntConstantId(0, false);
                            one_id = GetIntConstantId(1, false);
                        } else if (desiredScalarType == CoreType::UINT) {
                            zero_id = GetIntConstantId(0, true);
                            one_id = GetIntConstantId(1, true);
                        }
                        Emit(spv::OpSelect, dest_type_id, converted, value_id, one_id, zero_id);
                        return converted;
                    }

                    if (srcType == CoreType::INT && desiredScalarType == CoreType::FLOAT) {
                        Emit(spv::OpConvertSToF, dest_type_id, converted, value_id);
                        return converted;
                    }
                    if (srcType == CoreType::UINT && desiredScalarType == CoreType::FLOAT) {
                        Emit(spv::OpConvertUToF, dest_type_id, converted, value_id);
                        return converted;
                    }
                    if (srcType == CoreType::FLOAT && desiredScalarType == CoreType::INT) {
                        Emit(spv::OpConvertFToS, dest_type_id, converted, value_id);
                        return converted;
                    }
                    if (srcType == CoreType::FLOAT && desiredScalarType == CoreType::UINT) {
                        Emit(spv::OpConvertFToU, dest_type_id, converted, value_id);
                        return converted;
                    }
                    if (srcType == CoreType::INT && desiredScalarType == CoreType::UINT) {
                        Emit(spv::OpBitcast, dest_type_id, converted, value_id);
                        return converted;
                    }
                    if (srcType == CoreType::UINT && desiredScalarType == CoreType::INT) {
                        Emit(spv::OpBitcast, dest_type_id, converted, value_id);
                        return converted;
                    }

                    Emit(spv::OpBitcast, dest_type_id, converted, value_id);
                    return converted;
                };

                // If result is scalar (requiredComponents == 1), just copy the value
                // OpCompositeConstruct requires at least 2 constituents
                if (requiredComponents == 1 && inputCount == 1) {
                    // Just assign the scalar directly via OpCopyObject
                    Emit(spv::OpCopyObject, result_type_id, dest, inputIds[0]);
                    break;
                }

                // Check input types and decompose vectors into scalars
                // This handles float3(uv, 2.0) where uv is float2
                // We only extract enough components to satisfy requiredComponents
                u32 scalarIds[8];  // Enough for 2 * vec4 = 8 components max
                u32 scalarCount = 0;

                for (u32 c = 0; c < inputCount && scalarCount < requiredComponents; c++) {
                    u16 op_reg = inputRegs[c];
                    if (op_reg == 0xFFFF) continue;

                    // Check the type of this operand
                    CoreType opType = CoreType::FLOAT;
                    if ((op_reg & 0xC000) == 0xC000) {
                        opType = CoreType::BOOL;
                    } else if (op_reg & 0x8000) {
                        opType = CoreType::FLOAT;
                    } else if (op_reg & 0x4000) {
                        opType = CoreType::INT;
                    } else if (op_reg & 0x2000) {
                        opType = CoreType::UINT;
                    } else if (ir->registerTypes && op_reg < ir->registerCount) {
                        opType = static_cast<CoreType>(ir->registerTypes[op_reg]);
                    }

                    // Determine how many components this operand contributes
                    u32 opComponents = 1;
                    switch (opType) {
                        case CoreType::FLOAT2: case CoreType::INT2: case CoreType::UINT2:
                            opComponents = 2; break;
                        case CoreType::FLOAT3: case CoreType::INT3: case CoreType::UINT3:
                            opComponents = 3; break;
                        case CoreType::FLOAT4: case CoreType::INT4: case CoreType::UINT4:
                            opComponents = 4; break;
                        default: break;
                    }

                    CoreType opScalarType = GetScalarComponentType(opType);
                    if (opComponents == 1) {
                        // Scalar - use directly
                        scalarIds[scalarCount++] = convertScalar(inputIds[c], opScalarType);
                    } else {
                        // Vector - extract each component (but only as many as we still need)
                        u32 componentsToExtract = (opComponents < (requiredComponents - scalarCount))
                                                   ? opComponents
                                                   : (requiredComponents - scalarCount);
                        u32 op_scalar_type_id = GetTypeId(opScalarType);
                        for (u32 comp = 0; comp < componentsToExtract; comp++) {
                            u32 extracted = AllocateId();
                            // OpCompositeExtract: result_type result_id composite index...
                            if (currentFunctionSize + 5 > currentFunctionCapacity) {
                                GrowCurrentFunction();
                            }
                            currentFunction[currentFunctionSize++] = (5 << 16) | spv::OpCompositeExtract;
                            currentFunction[currentFunctionSize++] = op_scalar_type_id;
                            currentFunction[currentFunctionSize++] = extracted;
                            currentFunction[currentFunctionSize++] = inputIds[c];
                            currentFunction[currentFunctionSize++] = comp;
                            scalarIds[scalarCount++] = convertScalar(extracted, opScalarType);
                        }
                    }
                }

                // If we have fewer scalars than required components, broadcast/splat
                // This handles float3(0.0) -> OpCompositeConstruct %v3float %0 %0 %0
                u32 actualCount = scalarCount;
                if (scalarCount == 1 && requiredComponents > 1) {
                    // Scalar broadcast - replicate the single input
                    u32 scalarId = scalarIds[0];
                    for (u32 c = 1; c < requiredComponents; c++) {
                        scalarIds[c] = scalarId;
                    }
                    actualCount = requiredComponents;
                }

                if (actualCount < requiredComponents) {
                    u32 zeroId = 0;
                    if (desiredScalarType == CoreType::FLOAT) {
                        zeroId = GetFloatConstantId(0.0f);
                    } else if (desiredScalarType == CoreType::INT) {
                        zeroId = GetIntConstantId(0, false);
                    } else if (desiredScalarType == CoreType::UINT) {
                        zeroId = GetIntConstantId(0, true);
                    } else if (desiredScalarType == CoreType::BOOL) {
                        zeroId = GetBoolConstantId(false);
                    }

                    while (actualCount < requiredComponents) {
                        scalarIds[actualCount++] = zeroId;
                    }
                }

                u32 wordCount = 3 + actualCount;
                if (currentFunctionSize + wordCount > currentFunctionCapacity) {
                    GrowCurrentFunction();
                }
                currentFunction[currentFunctionSize++] = (wordCount << 16) | spv::OpCompositeConstruct;
                currentFunction[currentFunctionSize++] = result_type_id;
                currentFunction[currentFunctionSize++] = dest;
                for (u32 c = 0; c < actualCount; c++) {
                    currentFunction[currentFunctionSize++] = scalarIds[c];
                }
            }
            break;
        }

        case IR::OP_MAT_CONSTRUCT: {
            // Build a matrix from column vectors (generated by IR lowering)
            // IR lowering now generates OP_VEC_CONSTRUCT for each column, then OP_MAT_CONSTRUCT
            // The operands are column vector registers, metadata contains column count
            u16 dest_reg = ir->destinations[ir_idx];
            CoreType resultType = CoreType::MAT4;
            if (ir->registerTypes && dest_reg < ir->registerCount) {
                resultType = static_cast<CoreType>(ir->registerTypes[dest_reg]);
            }

            u32 result_type_id = GetTypeId(resultType);

            // Determine number of columns from metadata
            u32 numColumns = ir->metadata[ir_idx];
            if (numColumns == 0) {
                numColumns = (resultType == CoreType::MAT2) ? 2 : (resultType == CoreType::MAT3) ? 3 : 4;
            }

            // Collect column vector IDs
            u32 columnIds[4];
            for (u32 col = 0; col < numColumns; col++) {
                u16 op_reg = ir->GetOperand(ir_idx, col);
                columnIds[col] = GetSpirvId(op_reg);
            }

            // Build matrix from column vectors
            u32 matWordCount = 3 + numColumns;
            if (currentFunctionSize + matWordCount > currentFunctionCapacity) {
                GrowCurrentFunction();
            }
            currentFunction[currentFunctionSize++] = (matWordCount << 16) | spv::OpCompositeConstruct;
            currentFunction[currentFunctionSize++] = result_type_id;
            currentFunction[currentFunctionSize++] = dest;
            for (u32 col = 0; col < numColumns; col++) {
                currentFunction[currentFunctionSize++] = columnIds[col];
            }
            break;
        }

        case IR::OP_VEC_EXTRACT: {
            // Extract a single component from a vector
            // operand 0: source vector register
            // operand 1: component index (0-3 for x/y/z/w)
            u16 src_reg = ir->GetOperand(ir_idx, 0);
            u32 src_id = GetSpirvId(src_reg);
            u32 component_idx = ir->GetOperand(ir_idx, 1);

            // Determine the scalar component type from the source vector type
            CoreType scalarType = CoreType::FLOAT;  // Default fallback
            if (ir->registerTypes && src_reg < ir->registerCount) {
                CoreType srcType = static_cast<CoreType>(ir->registerTypes[src_reg]);
                scalarType = GetScalarComponentType(srcType);
            }
            u32 result_type = GetTypeId(scalarType);

            // OpCompositeExtract: result_type result_id composite index...
            if (currentFunctionSize + 5 > currentFunctionCapacity) {
                GrowCurrentFunction();
            }
            currentFunction[currentFunctionSize++] = (5 << 16) | spv::OpCompositeExtract;
            currentFunction[currentFunctionSize++] = result_type;
            currentFunction[currentFunctionSize++] = dest;
            currentFunction[currentFunctionSize++] = src_id;
            currentFunction[currentFunctionSize++] = component_idx;
            break;
        }

        case IR::OP_VEC_INSERT: {
            // Insert a scalar into a vector at a specific component index
            // operand 0: source vector register
            // operand 1: component index (0-3 for x/y/z/w)
            // operand 2: value to insert (scalar)
            u16 src_vec_reg = ir->GetOperand(ir_idx, 0);
            u32 src_vec_id = GetSpirvId(src_vec_reg);
            u32 component_idx = ir->GetOperand(ir_idx, 1);
            u16 value_reg = ir->GetOperand(ir_idx, 2);
            u32 value_id = GetSpirvId(value_reg);

            // Get result type from the source vector
            u32 result_type = GetResultType(ir->destinations[ir_idx], src_vec_reg);

            // OpCompositeInsert: result_type result_id object composite index...
            // object = the value being inserted
            // composite = the vector/struct being modified
            if (currentFunctionSize + 6 > currentFunctionCapacity) {
                GrowCurrentFunction();
            }
            currentFunction[currentFunctionSize++] = (6 << 16) | spv::OpCompositeInsert;
            currentFunction[currentFunctionSize++] = result_type;
            currentFunction[currentFunctionSize++] = dest;
            currentFunction[currentFunctionSize++] = value_id;     // object (value being inserted)
            currentFunction[currentFunctionSize++] = src_vec_id;   // composite (vector being modified)
            currentFunction[currentFunctionSize++] = component_idx;
            break;
        }

        case IR::OP_VEC_SHUFFLE: {
            // Vector shuffle/swizzle for multi-component assignment
            // operand 0: first source vector (original)
            // operand 1: second source vector (value to insert)
            // metadata: packed shuffle indices (4 bits each, up to 4 components)
            u16 src0_reg = ir->GetOperand(ir_idx, 0);
            u16 src1_reg = ir->GetOperand(ir_idx, 1);
            u32 src0_id = GetSpirvId(src0_reg);
            u32 src1_id = GetSpirvId(src1_reg);
            u32 shuffleMask = ir->metadata[ir_idx];

            // Get result type from destination register
            u16 dest_reg = ir->destinations[ir_idx];
            u32 result_type = GetResultType(dest_reg, src0_reg);

            // Determine number of components from result type
            CoreType destType = CoreType::FLOAT4;
            if (ir->registerTypes && dest_reg < ir->registerCount) {
                destType = static_cast<CoreType>(ir->registerTypes[dest_reg]);
            }
            u32 numComponents = 4;
            if (destType == CoreType::FLOAT3 || destType == CoreType::INT3 || destType == CoreType::UINT3) {
                numComponents = 3;
            } else if (destType == CoreType::FLOAT2 || destType == CoreType::INT2 || destType == CoreType::UINT2) {
                numComponents = 2;
            }

            // OpVectorShuffle: result_type result_id vec0 vec1 indices...
            u32 wordCount = 5 + numComponents;
            if (currentFunctionSize + wordCount > currentFunctionCapacity) {
                GrowCurrentFunction();
            }
            currentFunction[currentFunctionSize++] = (wordCount << 16) | spv::OpVectorShuffle;
            currentFunction[currentFunctionSize++] = result_type;
            currentFunction[currentFunctionSize++] = dest;
            currentFunction[currentFunctionSize++] = src0_id;
            currentFunction[currentFunctionSize++] = src1_id;

            // Unpack and emit component indices
            for (u32 i = 0; i < numComponents; i++) {
                u32 idx = (shuffleMask >> (i * 4)) & 0xF;
                currentFunction[currentFunctionSize++] = idx;
            }
            break;
        }

        // ========== Struct Operations ==========
        case IR::OP_STRUCT_EXTRACT: {
            // Extract a field from a struct: dest = struct.field
            // operand 0: source struct register
            // operand 1: field index
            // metadata: full 32-bit struct type hash
            u16 src_reg = ir->GetOperand(ir_idx, 0);
            u32 src_id = GetSpirvId(src_reg);
            u32 field_idx = ir->GetOperand(ir_idx, 1);

            // Get the struct type hash from metadata or register
            u32 metadata = ir->metadata[ir_idx];
            u32 struct_type_hash = metadata;  // Full 32-bit hash
            if (struct_type_hash == 0 && src_reg < 512 && ir->registerStructTypes) {
                struct_type_hash = ir->registerStructTypes[src_reg];
            }

            // Determine result type from struct field types
            u32 result_type = GetTypeId(CoreType::FLOAT);  // Default
            if (struct_type_hash != 0 && ir->structTypes) {
                for (u32 i = 0; i < ir->structTypeCount; i++) {
                    if (ir->structTypes[i].nameHash == struct_type_hash) {
                        u32 fieldOffset = ir->structTypes[i].fieldOffset;
                        if (field_idx < ir->structTypes[i].fieldCount) {
                            CoreType fieldType = static_cast<CoreType>(ir->structFieldTypes[fieldOffset + field_idx]);
                            if ((fieldType == CoreType::CUSTOM || fieldType == CoreType::ENUM) &&
                                ir->structFieldTypeHashes) {
                                u32 fieldTypeHash = ir->structFieldTypeHashes[fieldOffset + field_idx];
                                if (fieldTypeHash != 0) {
                                    result_type = GetStructTypeId(fieldTypeHash);
                                }
                            } else {
                                result_type = GetTypeId(fieldType);
                            }
                        }
                        break;
                    }
                }
            }
            if (result_type == 0) {
                result_type = GetTypeId(CoreType::FLOAT);
            }

            // OpCompositeExtract: result_type result_id composite index...
            if (currentFunctionSize + 5 > currentFunctionCapacity) {
                GrowCurrentFunction();
            }
            currentFunction[currentFunctionSize++] = (5 << 16) | spv::OpCompositeExtract;
            currentFunction[currentFunctionSize++] = result_type;
            currentFunction[currentFunctionSize++] = dest;
            currentFunction[currentFunctionSize++] = src_id;
            currentFunction[currentFunctionSize++] = field_idx;
            break;
        }

        case IR::OP_STRUCT_INSERT: {
            // Insert a field into a struct: dest = struct with field=value
            // operand 0: source struct register
            // operand 1: field index
            // operand 2: value to insert
            // metadata: full 32-bit struct type hash
            u16 struct_reg = ir->GetOperand(ir_idx, 0);
            u32 struct_id = GetSpirvId(struct_reg);
            u32 field_idx = ir->GetOperand(ir_idx, 1);
            u16 value_reg = ir->GetOperand(ir_idx, 2);
            u32 value_id = GetSpirvId(value_reg);

            // Get struct type for result type
            u32 metadata = ir->metadata[ir_idx];
            u32 struct_type_hash = metadata;  // Full 32-bit hash
            if (struct_type_hash == 0 && struct_reg < 512 && ir->registerStructTypes) {
                struct_type_hash = ir->registerStructTypes[struct_reg];
            }

            u32 result_type = GetStructTypeId(struct_type_hash);
            if (result_type == 0) {
                // Fallback: just copy the struct (shouldn't happen)
                if (currentFunctionSize + 4 > currentFunctionCapacity) {
                    GrowCurrentFunction();
                }
                currentFunction[currentFunctionSize++] = (4 << 16) | spv::OpCopyObject;
                currentFunction[currentFunctionSize++] = GetTypeId(CoreType::FLOAT4);
                currentFunction[currentFunctionSize++] = dest;
                currentFunction[currentFunctionSize++] = struct_id;
                break;
            }

            // OpCompositeInsert: result_type result_id object composite indices...
            if (currentFunctionSize + 6 > currentFunctionCapacity) {
                GrowCurrentFunction();
            }
            currentFunction[currentFunctionSize++] = (6 << 16) | spv::OpCompositeInsert;
            currentFunction[currentFunctionSize++] = result_type;
            currentFunction[currentFunctionSize++] = dest;
            currentFunction[currentFunctionSize++] = value_id;      // Object to insert
            currentFunction[currentFunctionSize++] = struct_id;     // Composite
            currentFunction[currentFunctionSize++] = field_idx;     // Index
            break;
        }

        case IR::OP_STRUCT_CONSTRUCT: {
            // Build struct from field values: dest = struct(f0, f1, f2...)
            // Uses all 4 operand slots for field values
            // metadata: struct type hash
            u32 struct_type_hash = ir->metadata[ir_idx];
            u32 result_type = GetStructTypeId(struct_type_hash);

            if (result_type == 0) {
                // Unknown struct type - emit placeholder
                break;
            }

            // Count field values from operands (non-0xFFFF values)
            u32 fieldIds[4] = {0, 0, 0, 0};
            u32 fieldCount = 0;
            for (u32 i = 0; i < 4; i++) {
                u16 op_reg = ir->GetOperand(ir_idx, i);
                if (op_reg == 0xFFFF) continue;
                fieldIds[fieldCount++] = GetSpirvId(op_reg);
            }

            // OpCompositeConstruct: result_type result_id constituents...
            u32 wordCount = 3 + fieldCount;
            if (currentFunctionSize + wordCount > currentFunctionCapacity) {
                GrowCurrentFunction();
            }
            currentFunction[currentFunctionSize++] = (wordCount << 16) | spv::OpCompositeConstruct;
            currentFunction[currentFunctionSize++] = result_type;
            currentFunction[currentFunctionSize++] = dest;
            for (u32 i = 0; i < fieldCount; i++) {
                currentFunction[currentFunctionSize++] = fieldIds[i];
            }
            break;
        }

        case IR::OP_ENUM_CONSTRUCT: {
            // Build enum variant: dest = EnumType::Variant(args...)
            // metadata: (variantIndex << 16) | (argCount << 8) | (enumHash low bits)
            // operands: variant field values
            u32 metadata = ir->metadata[ir_idx];
            u32 variantIndex = (metadata >> 16) & 0xFFFF;
            u32 argCount = (metadata >> 8) & 0xFF;

            // Get enum struct type from register struct type info
            u16 dest_reg = ir->destinations[ir_idx];
            u32 enumStructHash = 0;
            if (dest_reg < 512 && ir->registerStructTypes) {
                enumStructHash = ir->registerStructTypes[dest_reg];
            }

            u32 result_type = GetStructTypeId(enumStructHash);
            if (result_type == 0) {
                break;
            }

            // Build constituents: [tag, field0, field1, ...]
            // Find struct field count to pad with zeros
            u32 totalFieldCount = 1;  // At least the tag
            for (u32 i = 0; i < structTypeCount; i++) {
                if (structTypeHashes[i] == enumStructHash) {
                    // Get field count from IR struct info
                    for (u32 s = 0; s < ir->structTypeCount; s++) {
                        if (ir->structTypes[s].nameHash == enumStructHash) {
                            totalFieldCount = ir->structTypes[s].fieldCount;
                            break;
                        }
                    }
                    break;
                }
            }

            // Prepare constituents
            u32 constituents[16] = {0};
            u32 constituentCount = 0;

            // First constituent: tag (variant index)
            constituents[constituentCount++] = GetIntConstantId(variantIndex, false);

            // Remaining constituents: field values from operands, padded with zeros
            // Vector types need to be decomposed into individual scalars
            for (u32 i = 0; i < 4 && constituentCount < totalFieldCount; i++) {
                u16 op_reg = ir->GetOperand(ir_idx, i);
                if (op_reg != 0x3FFF && op_reg != 0xFFFF) {
                    // Check if this operand is a vector type that needs decomposition
                    CoreType opType = CoreType::FLOAT;
                    if (ir->registerTypes && op_reg < ir->registerCount) {
                        opType = static_cast<CoreType>(ir->registerTypes[op_reg]);
                    }

                    u32 componentCount = 1;
                    switch (opType) {
                        case CoreType::FLOAT2:
                        case CoreType::INT2:
                        case CoreType::UINT2:
                            componentCount = 2;
                            break;
                        case CoreType::FLOAT3:
                        case CoreType::INT3:
                        case CoreType::UINT3:
                            componentCount = 3;
                            break;
                        case CoreType::FLOAT4:
                        case CoreType::INT4:
                        case CoreType::UINT4:
                            componentCount = 4;
                            break;
                        default:
                            componentCount = 1;
                            break;
                    }

                    if (componentCount == 1) {
                        // Scalar - use directly
                        constituents[constituentCount++] = GetSpirvId(op_reg);
                    } else {
                        // Vector - extract each component
                        u32 vec_id = GetSpirvId(op_reg);
                        u32 scalar_type = GetTypeId(CoreType::FLOAT);  // Enum fields are always floats in our struct
                        for (u32 c = 0; c < componentCount && constituentCount < totalFieldCount; c++) {
                            u32 extract_id = AllocateId();
                            // OpCompositeExtract: result_type result_id composite index
                            Emit(spv::OpCompositeExtract, scalar_type, extract_id, vec_id, c);
                            constituents[constituentCount++] = extract_id;
                        }
                    }
                } else if (i < argCount) {
                    // Unexpected sentinel - use zero
                    constituents[constituentCount++] = GetFloatConstantId(0.0f);
                } else {
                    // Padding with zeros
                    constituents[constituentCount++] = GetFloatConstantId(0.0f);
                }
            }

            // Pad remaining fields with zeros
            while (constituentCount < totalFieldCount) {
                constituents[constituentCount++] = GetFloatConstantId(0.0f);
            }

            // OpCompositeConstruct: result_type result_id constituents...
            u32 wordCount = 3 + constituentCount;
            if (currentFunctionSize + wordCount > currentFunctionCapacity) {
                GrowCurrentFunction();
            }
            currentFunction[currentFunctionSize++] = (wordCount << 16) | spv::OpCompositeConstruct;
            currentFunction[currentFunctionSize++] = result_type;
            currentFunction[currentFunctionSize++] = dest;
            for (u32 i = 0; i < constituentCount; i++) {
                currentFunction[currentFunctionSize++] = constituents[i];
            }
            break;
        }

        case IR::OP_ENUM_TAG: {
            // Extract tag from enum: dest = enum.tag (field 0)
            u16 enum_reg = ir->GetOperand(ir_idx, 0);
            u32 enum_id = GetSpirvId(enum_reg);

            u32 result_type = GetTypeId(CoreType::INT);

            // OpCompositeExtract: result_type result_id composite index...
            if (currentFunctionSize + 5 > currentFunctionCapacity) {
                GrowCurrentFunction();
            }
            currentFunction[currentFunctionSize++] = (5 << 16) | spv::OpCompositeExtract;
            currentFunction[currentFunctionSize++] = result_type;
            currentFunction[currentFunctionSize++] = dest;
            currentFunction[currentFunctionSize++] = enum_id;
            currentFunction[currentFunctionSize++] = 0;  // Index 0 = tag field
            break;
        }

        case IR::OP_ENUM_FIELD: {
            // Extract field from enum: dest = enum.field[N]
            // operand 0: enum register
            // operand 1: field index (0-based into variant data, so actual index is 1+N)
            u16 enum_reg = ir->GetOperand(ir_idx, 0);
            u16 field_idx_reg = ir->GetOperand(ir_idx, 1);
            u32 enum_id = GetSpirvId(enum_reg);

            // For now, assume field index is a constant
            u32 field_idx = 0;
            if (field_idx_reg & 0x4000) {
                // Int constant
                u32 idx = field_idx_reg & 0x3FFF;
                field_idx = ir->intConstants[idx];
            }

            u32 result_type = GetTypeId(CoreType::FLOAT);  // Assume float for now

            // OpCompositeExtract: result_type result_id composite index...
            // Field index is offset by 1 because field 0 is the tag
            if (currentFunctionSize + 5 > currentFunctionCapacity) {
                GrowCurrentFunction();
            }
            currentFunction[currentFunctionSize++] = (5 << 16) | spv::OpCompositeExtract;
            currentFunction[currentFunctionSize++] = result_type;
            currentFunction[currentFunctionSize++] = dest;
            currentFunction[currentFunctionSize++] = enum_id;
            currentFunction[currentFunctionSize++] = 1 + field_idx;  // Offset by 1 for tag
            break;
        }

        case IR::OP_ARRAY_LOAD: {
            // Load element from array: dest = array[index]
            // operand 0: base array register
            // operand 1: index register
            u16 base_reg = ir->GetOperand(ir_idx, 0);
            u16 index_reg = ir->GetOperand(ir_idx, 1);
            u32 base_id = GetSpirvId(base_reg);
            u32 index_id = GetSpirvId(index_reg);

            // Get the element type from the destination register
            CoreType elemType = CoreType::FLOAT;
            u16 dest_reg = ir->destinations[ir_idx];
            if (ir->registerTypes && dest_reg < ir->registerCount) {
                CoreType regType = static_cast<CoreType>(ir->registerTypes[dest_reg]);
                if (regType != CoreType::VOID && regType != CoreType::INVALID) {
                    elemType = regType;
                }
            }
            u32 elem_type_id = GetTypeId(elemType);

            // Non-storage arrays are lowered as placeholder values; ignore index to keep SPIR-V valid.
            Emit(spv::OpUndef, elem_type_id, dest);
            break;
        }

        case IR::OP_ARRAY_STORE: {
            // Store element to array: array[index] = value
            // dest: base array register
            // operand 0: index register
            // operand 1: value register
            u16 base_reg = ir->destinations[ir_idx];
            u16 index_reg = ir->GetOperand(ir_idx, 0);
            u16 value_reg = ir->GetOperand(ir_idx, 1);

            bool isStoragePtr = ir->registerStorageInfo &&
                base_reg < ir->registerCount &&
                (ir->registerStorageInfo[base_reg] & IR::IRProgram::STORAGE_IS_PTR);

            if (isStoragePtr) {
                spv::StorageClass storageClass = spv::StorageClassStorageBuffer;
                if (ir->registerStorageInfo[base_reg] & IR::IRProgram::STORAGE_IS_SHARED) {
                    storageClass = spv::StorageClassWorkgroup;
                }

                CoreType elemType = CoreType::FLOAT;
                if (ir->registerTypes && value_reg < ir->registerCount) {
                    CoreType regType = static_cast<CoreType>(ir->registerTypes[value_reg]);
                    if (regType != CoreType::VOID && regType != CoreType::INVALID) {
                        elemType = regType;
                    }
                } else if (ir->registerTypes && base_reg < ir->registerCount) {
                    CoreType regType = static_cast<CoreType>(ir->registerTypes[base_reg]);
                    if (regType != CoreType::VOID && regType != CoreType::INVALID) {
                        elemType = regType;
                    }
                }

                u32 elem_ptr_type = GetPointerTypeId(GetTypeId(elemType), storageClass);
                u32 base_id = GetSpirvId(base_reg);
                u32 index_id = GetSpirvId(index_reg);
                u32 value_id = GetSpirvId(value_reg);

                u32 ptr_id = AllocateId();
                Emit(spv::OpAccessChain, elem_ptr_type, ptr_id, base_id, index_id);
                Emit(spv::OpStore, ptr_id, value_id);
            } else {
                // For local arrays (not storage), define the base register as a simple assignment.
                // This keeps SSA values defined even though full array semantics are not implemented.
                CoreType elemType = CoreType::FLOAT;
                if (ir->registerTypes && value_reg < ir->registerCount) {
                    CoreType regType = static_cast<CoreType>(ir->registerTypes[value_reg]);
                    if (regType != CoreType::VOID && regType != CoreType::INVALID) {
                        elemType = regType;
                    }
                } else if (ir->registerTypes && base_reg < ir->registerCount) {
                    CoreType regType = static_cast<CoreType>(ir->registerTypes[base_reg]);
                    if (regType != CoreType::VOID && regType != CoreType::INVALID) {
                        elemType = regType;
                    }
                }

                u32 type_id = GetTypeId(elemType);
                if (type_id == 0) {
                    type_id = GetTypeId(CoreType::FLOAT);
                }
                u32 value_id = GetSpirvId(value_reg);
                u32 base_id = GetSpirvId(base_reg);
                Emit(spv::OpCopyObject, type_id, base_id, value_id);

                if (base_reg < idCapacity && hasPreAllocatedId[base_reg]) {
                    hasPreAllocatedId[base_reg] = false;
                }
            }
            break;
        }

        // ========== Storage Buffer Access Chain Operations ==========
        // These ops maintain pointer semantics for proper SPIR-V OpAccessChain generation

        case IR::OP_STORAGE_PTR: {
            // Get storage buffer base pointer: dest = &buffer
            // operand0 = binding index
            u16 binding = ir->GetOperand(ir_idx, 0);

            // Get the storage buffer variable ID
            u32 ssbo_var_id = storageBufferIds[binding];
            if (ssbo_var_id == 0) {
                // Storage buffer not declared - emit undef
                u32 type_id = GetTypeId(CoreType::UINT);
                Emit(spv::OpUndef, type_id, dest);
            } else {
                // The storage buffer variable IS the pointer - just record the mapping
                // We don't emit any instruction, just map the dest register to the variable
                spirvIds[ir->destinations[ir_idx]] = ssbo_var_id;
            }
            break;
        }

        case IR::OP_STORAGE_FIELD: {
            // Access struct field in storage buffer: dest = ptr.field
            // operand0 = base pointer register
            // operand1 = field index (literal)
            // metadata = struct type hash
            u16 base_reg = ir->GetOperand(ir_idx, 0);
            u16 field_idx = ir->GetOperand(ir_idx, 1);
            u32 base_id = GetSpirvId(base_reg);
            u32 structTypeHash = ir->metadata[ir_idx];

            // Get the element type from destination register
            CoreType elemType = CoreType::FLOAT;
            u16 dest_reg = ir->destinations[ir_idx];
            if (ir->registerTypes && dest_reg < ir->registerCount) {
                CoreType regType = static_cast<CoreType>(ir->registerTypes[dest_reg]);
                if (regType != CoreType::VOID && regType != CoreType::INVALID) {
                    elemType = regType;
                }
            }

            // For storage buffer struct field access, we need a pointer to the field type
            // The field type was already created when declaring the struct type
            // We must use the SAME type ID to match SPIR-V's strict type checking

            // Look up the cached field type ID from when the struct was created
            u32 fieldTypeId = 0;
            if (structTypeHash != 0) {
                for (u32 i = 0; i < structTypeCount; i++) {
                    if (structTypeHashes[i] == structTypeHash) {
                        if (field_idx < MAX_FIELDS_PER_STRUCT) {
                            fieldTypeId = structFieldTypeIds[i * MAX_FIELDS_PER_STRUCT + field_idx];
                        }
                        break;
                    }
                }
            }

            // Fallback to element type if field type wasn't found
            if (fieldTypeId == 0) {
                fieldTypeId = GetTypeId(elemType);
            }

            spv::StorageClass storageClass = spv::StorageClassStorageBuffer;
            if (ir->registerStorageInfo && base_reg < ir->registerCount) {
                if (ir->registerStorageInfo[base_reg] & IR::IRProgram::STORAGE_IS_SHARED) {
                    storageClass = spv::StorageClassWorkgroup;
                }
            }
            u32 field_ptr_type = GetPointerTypeId(fieldTypeId, storageClass);

            // Emit OpAccessChain: result = base[field_idx]
            // For storage buffer structs, we access the field directly (no wrapper)
            u32 field_id = GetIntConstantId(field_idx, false);

            // OpAccessChain result_type result base indices...
            if (currentFunctionSize + 5 > currentFunctionCapacity) {
                GrowCurrentFunction();
            }
            currentFunction[currentFunctionSize++] = (5 << 16) | spv::OpAccessChain;
            currentFunction[currentFunctionSize++] = field_ptr_type;
            currentFunction[currentFunctionSize++] = dest;
            currentFunction[currentFunctionSize++] = base_id;
            currentFunction[currentFunctionSize++] = field_id; // Field within struct
            break;
        }

        case IR::OP_STORAGE_INDEX: {
            // Index into array in storage buffer: dest = ptr[index]
            // operand0 = base pointer register (points to array)
            // operand1 = index register
            u16 base_reg = ir->GetOperand(ir_idx, 0);
            u16 index_reg = ir->GetOperand(ir_idx, 1);
            u32 base_id = GetSpirvId(base_reg);
            u32 index_id = GetSpirvId(index_reg);

            // Get the element type from destination register
            CoreType elemType = CoreType::FLOAT;
            u16 dest_reg = ir->destinations[ir_idx];
            if (ir->registerTypes && dest_reg < ir->registerCount) {
                CoreType regType = static_cast<CoreType>(ir->registerTypes[dest_reg]);
                if (regType != CoreType::VOID && regType != CoreType::INVALID) {
                    elemType = regType;
                }
            }

            spv::StorageClass storageClass = spv::StorageClassStorageBuffer;
            if (ir->registerStorageInfo && base_reg < ir->registerCount) {
                if (ir->registerStorageInfo[base_reg] & IR::IRProgram::STORAGE_IS_SHARED) {
                    storageClass = spv::StorageClassWorkgroup;
                }
            }

            // Get pointer type for the element
            u32 elem_ptr_type = GetPointerTypeId(GetTypeId(elemType), storageClass);

            // Emit OpAccessChain with the dynamic index
            Emit(spv::OpAccessChain, elem_ptr_type, dest, base_id, index_id);
            break;
        }

        case IR::OP_STORAGE_LOAD: {
            // Load value from storage buffer pointer: dest = *ptr
            // operand0 = pointer register
            u16 ptr_reg = ir->GetOperand(ir_idx, 0);
            u32 ptr_id = GetSpirvId(ptr_reg);

            // Get the element type from destination register
            CoreType elemType = CoreType::FLOAT;
            u16 dest_reg = ir->destinations[ir_idx];
            if (ir->registerTypes && dest_reg < ir->registerCount) {
                CoreType regType = static_cast<CoreType>(ir->registerTypes[dest_reg]);
                if (regType != CoreType::VOID && regType != CoreType::INVALID) {
                    elemType = regType;
                }
            }
            u32 elem_type_id = GetTypeId(elemType);

            // Emit OpLoad
            Emit(spv::OpLoad, elem_type_id, dest, ptr_id);
            break;
        }

        case IR::OP_BRANCH: {
            // metadata encoding: (falseTarget << 16) | trueTarget
            u16 cond_reg = ir->GetOperand(ir_idx, 0);
            u32 metadata = ir->metadata[ir_idx];
            u32 true_target = metadata & 0xFFFF;
            u32 false_target = metadata >> 16;

            // Check if EmitBranch already converted this condition to bool
            u32 condition;
            if (branchConditionOverride != 0 && branchConditionOverrideReg == cond_reg) {
                // Use the pre-converted bool
                condition = branchConditionOverride;
            } else {
                condition = GetSpirvId(cond_reg);

                // SPIR-V requires boolean condition for OpBranchConditional
                // If condition is not bool, convert it to bool via != 0 comparison
                CoreType condType = CoreType::BOOL;
                if (cond_reg & 0xC000) {
                    // Constant-encoded condition
                    if ((cond_reg & 0xC000) == 0xC000) {
                        condType = CoreType::BOOL;
                    } else if (cond_reg & 0x8000) {
                        condType = CoreType::FLOAT;
                    } else if (cond_reg & 0x4000) {
                        condType = CoreType::INT;
                    } else if (cond_reg & 0x2000) {
                        condType = CoreType::UINT;
                    }
                } else if (cond_reg < ir->registerCount && ir->registerTypes) {
                    condType = static_cast<CoreType>(ir->registerTypes[cond_reg]);
                }

                if (condType != CoreType::BOOL) {
                    // Convert non-bool to bool: condition != 0
                    u32 bool_type = GetTypeId(CoreType::BOOL);
                    u32 bool_result = AllocateId();

                    // Determine comparison op and zero constant based on type
                    if (mask(condType) & TypeMasks::FLOAT_TYPES) {
                        u32 zero = GetFloatConstantId(0.0f);
                        Emit(spv::OpFOrdNotEqual, bool_type, bool_result, condition, zero);
                    } else if (mask(condType) & TypeMasks::UINT_TYPES) {
                        u32 zero = GetIntConstantId(0, true);  // unsigned zero
                        Emit(spv::OpINotEqual, bool_type, bool_result, condition, zero);
                    } else if (mask(condType) & TypeMasks::INT_TYPES) {
                        u32 zero = GetIntConstantId(0, false); // signed zero
                        Emit(spv::OpINotEqual, bool_type, bool_result, condition, zero);
                    } else {
                        // Default: treat as int comparison
                        u32 zero = GetIntConstantId(0, false);
                        Emit(spv::OpINotEqual, bool_type, bool_result, condition, zero);
                    }
                    condition = bool_result;
                }
            }

            u32 true_label = GetOrCreateBlockLabel(true_target);
            u32 false_label = GetOrCreateBlockLabel(false_target);
            Emit(spv::OpBranchConditional, condition, true_label, false_label);
            break;
        }
        
        case IR::OP_JUMP: {
            u32 target = ir->metadata[ir_idx]; // Target instruction index in metadata
            u32 label = GetOrCreateBlockLabel(target);
            Emit(spv::OpBranch, label);
            break;
        }
        
        case IR::OP_SWITCH: {
            // Get switch data from IR - metadata stores the switchId
            u32 switchId = ir->metadata[ir_idx];
            u32 selector = GetSpirvId(ir->GetOperand(ir_idx, 0));
            u32 default_target = ir->GetSwitchDefaultTarget(switchId);
            u32 default_label = GetOrCreateBlockLabel(default_target);

            // Get case count
            u32 caseCount = ir->GetSwitchCaseCount(switchId);

            // Build OpSwitch: selector, default_label, then pairs of (literal, label)
            // Total words: 3 + 2*caseCount
            if (currentFunctionSize + 3 + 2*caseCount > currentFunctionCapacity) {
                GrowCurrentFunction();
            }

            currentFunction[currentFunctionSize++] = ((3 + 2*caseCount) << 16) | spv::OpSwitch;
            currentFunction[currentFunctionSize++] = selector;
            currentFunction[currentFunctionSize++] = default_label;

            for (u32 i = 0; i < caseCount; i++) {
                u32 caseValue = ir->GetSwitchCaseValue(switchId, i);
                u32 caseTarget = ir->GetSwitchCaseTarget(switchId, i);
                u32 caseLabel = GetOrCreateBlockLabel(caseTarget);

                currentFunction[currentFunctionSize++] = caseValue;
                currentFunction[currentFunctionSize++] = caseLabel;
            }
            break;
        }
        
        case IR::OP_RET: {
            if (ir->destinations[ir_idx] != 0) {
                u32 value = GetSpirvId(ir->destinations[ir_idx]);
                Emit(spv::OpReturnValue, value);
            } else {
                Emit(spv::OpReturn);
            }
            break;
        }

        case IR::OP_DISCARD: {
            // Fragment discard - terminates fragment shader execution
            // OpKill must appear in a fragment shader only
            Emit(spv::OpKill);
            break;
        }

        case IR::OP_BARRIER: {
            u32 scope = GetIntConstantId(static_cast<u32>(spv::ScopeWorkgroup), true);
            u32 semantics = GetIntConstantId(static_cast<u32>(
                spv::MemorySemanticsAcquireReleaseMask |
                spv::MemorySemanticsWorkgroupMemoryMask), true);
            Emit(spv::OpControlBarrier, scope, scope, semantics);
            break;
        }

        case IR::OP_MEM_FENCE: {
            u32 scope = GetIntConstantId(static_cast<u32>(spv::ScopeWorkgroup), true);
            u32 semantics = GetIntConstantId(static_cast<u32>(
                spv::MemorySemanticsAcquireReleaseMask |
                spv::MemorySemanticsUniformMemoryMask |
                spv::MemorySemanticsWorkgroupMemoryMask), true);
            Emit(spv::OpMemoryBarrier, scope, semantics);
            break;
        }

        case IR::OP_TEX_SAMPLE:
        case IR::OP_TEX_SAMPLE_LOD:
        case IR::OP_TEX_SAMPLE_BIAS:
        case IR::OP_TEX_SAMPLE_GRAD: {
            // Texture sampling: dest = sample(texture, coord)
            // IR format: s0 = texture (with 0x2000 marker), s1 = coord
            // The resources are declared as combined image samplers (OpTypeSampledImage)
            u16 tex_reg = ir->GetOperand(ir_idx, 0);
            u16 coord_reg = ir->GetOperand(ir_idx, 1);
            u16 tex_slot = tex_reg & 0x0FFF;  // Extract binding from 0x2000 | binding
            u32 coord_id = GetSpirvId(coord_reg);

            // Get combined image sampler variable ID
            u32 tex_var_id = textureIds[tex_slot];

            if (tex_var_id == 0) {
                // Texture not found - emit placeholder (OpUndef)
                u32 result_type = GetTypeId(CoreType::FLOAT4);
                Emit(spv::OpUndef, result_type, dest);
                break;
            }

            // Get result type (float4 for most texture samples)
            u32 result_type = GetTypeId(CoreType::FLOAT4);

            // Get sampled image type for the load
            u32 sampled_img_type = GetSampledImageTypeId();

            // Load the combined sampled image
            u32 sampled_img_id = AllocateId();
            Emit(spv::OpLoad, sampled_img_type, sampled_img_id, tex_var_id);

            // Sample the texture
            switch (op) {
                case IR::OP_TEX_SAMPLE:
                    Emit(spv::OpImageSampleImplicitLod, result_type, dest, sampled_img_id, coord_id);
                    break;
                case IR::OP_TEX_SAMPLE_LOD: {
                    // Explicit LOD - operand 2 is LOD level
                    u32 lod_id = GetSpirvId(ir->GetOperand(ir_idx, 2));
                    // OpImageSampleExplicitLod with Lod operand
                    if (currentFunctionSize + 7 > currentFunctionCapacity) GrowCurrentFunction();
                    currentFunction[currentFunctionSize++] = (7 << 16) | spv::OpImageSampleExplicitLod;
                    currentFunction[currentFunctionSize++] = result_type;
                    currentFunction[currentFunctionSize++] = dest;
                    currentFunction[currentFunctionSize++] = sampled_img_id;
                    currentFunction[currentFunctionSize++] = coord_id;
                    currentFunction[currentFunctionSize++] = 0x2;  // Lod image operand
                    currentFunction[currentFunctionSize++] = lod_id;
                    break;
                }
                case IR::OP_TEX_SAMPLE_BIAS: {
                    // Bias - operand 2 is bias value
                    u32 bias_id = GetSpirvId(ir->GetOperand(ir_idx, 2));
                    if (currentFunctionSize + 7 > currentFunctionCapacity) GrowCurrentFunction();
                    currentFunction[currentFunctionSize++] = (7 << 16) | spv::OpImageSampleImplicitLod;
                    currentFunction[currentFunctionSize++] = result_type;
                    currentFunction[currentFunctionSize++] = dest;
                    currentFunction[currentFunctionSize++] = sampled_img_id;
                    currentFunction[currentFunctionSize++] = coord_id;
                    currentFunction[currentFunctionSize++] = 0x1;  // Bias image operand
                    currentFunction[currentFunctionSize++] = bias_id;
                    break;
                }
                case IR::OP_TEX_SAMPLE_GRAD: {
                    // Explicit gradients - operand 2 is ddx, operand 3 is ddy
                    u32 ddx_id = GetSpirvId(ir->GetOperand(ir_idx, 2));
                    u32 ddy_id = GetSpirvId(ir->GetOperand(ir_idx, 3));
                    if (currentFunctionSize + 9 > currentFunctionCapacity) GrowCurrentFunction();
                    currentFunction[currentFunctionSize++] = (9 << 16) | spv::OpImageSampleExplicitLod;
                    currentFunction[currentFunctionSize++] = result_type;
                    currentFunction[currentFunctionSize++] = dest;
                    currentFunction[currentFunctionSize++] = sampled_img_id;
                    currentFunction[currentFunctionSize++] = coord_id;
                    currentFunction[currentFunctionSize++] = 0x4;  // Grad image operand
                    currentFunction[currentFunctionSize++] = ddx_id;
                    currentFunction[currentFunctionSize++] = ddy_id;
                    break;
                }
                default:
                    break;
            }
            break;
        }

        // TODO: Add more opcode translations (OP_DISCARD for OpKill, etc.)
    }
}

// ============= Control Flow =============
u32 SPIRVBuilder::GetOrCreateBlockLabel(u32 ir_idx) {
    // Check if we already have a label for this IR index
    for (u32 i = 0; i < blockCount; i++) {
        if (blockIRIndices[i] == ir_idx) {
            return blockLabels[i];
        }
    }
    
    // Create new label
    u32 label_id = AllocateId();
    blockLabels[blockCount] = label_id;
    blockIRIndices[blockCount] = ir_idx;
    blockCount++;
    
    return label_id;
}

void SPIRVBuilder::EmitBranch(u32 ir_idx) {
    // For OP_BRANCH, we may need to convert the condition to bool BEFORE
    // emitting OpSelectionMerge, since SPIR-V requires OpSelectionMerge
    // to be immediately followed by OpBranchConditional
    IR::OpCode op = static_cast<IR::OpCode>(ir->opcodes[ir_idx]);

    if (op == IR::OP_BRANCH) {
        // Pre-convert condition to bool if needed (before merge instruction)
        u16 cond_reg = ir->GetOperand(ir_idx, 0);
        CoreType condType = CoreType::BOOL;
        if (cond_reg & 0xC000) {
            // Constant-encoded condition
            if ((cond_reg & 0xC000) == 0xC000) {
                condType = CoreType::BOOL;
            } else if (cond_reg & 0x8000) {
                condType = CoreType::FLOAT;
            } else if (cond_reg & 0x4000) {
                condType = CoreType::INT;
            } else if (cond_reg & 0x2000) {
                condType = CoreType::UINT;
            }
        } else if (cond_reg < ir->registerCount && ir->registerTypes) {
            condType = static_cast<CoreType>(ir->registerTypes[cond_reg]);
        }

        if (condType != CoreType::BOOL) {
            // Convert non-bool to bool: condition != 0
            u32 condition = GetSpirvId(cond_reg);
            u32 bool_type = GetTypeId(CoreType::BOOL);
            u32 bool_result = AllocateId();

            if (mask(condType) & TypeMasks::FLOAT_TYPES) {
                u32 zero = GetFloatConstantId(0.0f);
                Emit(spv::OpFOrdNotEqual, bool_type, bool_result, condition, zero);
            } else if (mask(condType) & TypeMasks::UINT_TYPES) {
                u32 zero = GetIntConstantId(0, true);
                Emit(spv::OpINotEqual, bool_type, bool_result, condition, zero);
            } else {
                u32 zero = GetIntConstantId(0, false);
                Emit(spv::OpINotEqual, bool_type, bool_result, condition, zero);
            }

            // Store the converted bool ID for use in TranslateInstruction
            // We use a simple approach: override the register mapping temporarily
            branchConditionOverride = bool_result;
            branchConditionOverrideReg = cond_reg;
        } else {
            branchConditionOverride = 0;
        }
    }

    // Check if this instruction has structured control flow info
    u32 structInfo = ir->structureInfo[ir_idx];

    if (structInfo != 0) {
        // This is a structured control flow header - emit merge instruction first
        EmitStructuredControlFlow(ir_idx);
    }

    // Now emit the actual branch
    TranslateInstruction(ir_idx);

    // Reset override
    branchConditionOverride = 0;
}

void SPIRVBuilder::EmitStructuredControlFlow(u32 ir_idx) {
    // Read structure info from IR
    u32 structInfo = ir->structureInfo[ir_idx];
    if (structInfo == 0) return;
    
    u32 structType = structInfo & IR::IRProgram::STRUCT_TYPE_MASK;
    u32 mergeInst = structInfo & IR::IRProgram::STRUCT_TARGET_MASK;
    
    // Get the merge block label
    u32 mergeLabel = GetOrCreateBlockLabel(mergeInst);
    
    switch (structType) {
        case IR::IRProgram::STRUCT_IF_HEADER: {
            // OpSelectionMerge: emit before OpBranchConditional
            // Format: OpSelectionMerge merge_block selection_control
            // Selection control 0 = None
            Emit(spv::OpSelectionMerge, mergeLabel, static_cast<u32>(spv::SelectionControlMaskNone));
            break;
        }
        
        case IR::IRProgram::STRUCT_LOOP_HEADER: {
            // OpLoopMerge: emit before OpBranch or OpBranchConditional in loop header
            // Format: OpLoopMerge merge_block continue_block loop_control
            
            // Get continue target from IR
            u32 continueInst = ir->continueInfo[ir_idx];
            u32 continueLabel = (continueInst != 0xFFFFFFFF) ? 
                                GetOrCreateBlockLabel(continueInst) : mergeLabel;
            
            Emit(spv::OpLoopMerge, mergeLabel, continueLabel, static_cast<u32>(spv::LoopControlMaskNone));
            break;
        }
        
        case IR::IRProgram::STRUCT_SWITCH_HEADER: {
            // OpSelectionMerge: emit before OpSwitch
            Emit(spv::OpSelectionMerge, mergeLabel, static_cast<u32>(spv::SelectionControlMaskNone));
            break;
        }
        
        default:
            break;
    }
}

void SPIRVBuilder::EmitPhiNodes(u32 blockIndex) {
    // Emit all PHI nodes for a given block
    // PHI nodes must be emitted at the start of a block, right after OpLabel
    
    // IR PHI storage:
    // - phiBlockIndices[i]: which block the PHI belongs to
    // - phiResultRegs[i]: result register
    // - phiTypes[i]: result type
    // - phiOperandOffsets[i]: start index into phiOperandValues/phiOperandBlocks
    // - phiOperandValues[]: values from each predecessor
    // - phiOperandBlocks[]: which predecessor each value comes from
    
    if (!ir->phiBlockIndices || !ir->phiOperandOffsets || !ir->phiOperandValues || ir->phiCount == 0) return;

    for (u32 phi_idx = 0; phi_idx < ir->phiCount; phi_idx++) {
        if (ir->phiBlockIndices[phi_idx] != blockIndex) continue;

        // Get PHI operand count using the IR program's helper
        u32 operandCount = ir->GetPhiOperandCount(phi_idx);
        if (operandCount == 0) continue;
        
        u16 phiResultReg = ir->phiResultRegs[phi_idx];
        
        // Check if all PHI operands have the same value (trivial PHI)
        // This can happen after variant specialization eliminates one branch
        bool isTrivial = true;
        u16 firstValueReg = ir->GetPhiOperandValue(phi_idx, 0);
        for (u32 op = 1; op < operandCount; op++) {
            if (ir->GetPhiOperandValue(phi_idx, op) != firstValueReg) {
                isTrivial = false;
                break;
            }
        }
        
        if (isTrivial) {
            // All operands are the same - just alias the result to the source value
            // Don't emit an OpPhi, just map the result register to the source's ID
            u32 source_id = GetSpirvId(firstValueReg);
            if (phiResultReg < idCapacity) {
                spirvIds[phiResultReg] = source_id;
            }
            continue;
        }
        
        // Get PHI result type and register
        // Use pre-allocated ID if available (from EmitFunctionBody Phase 1),
        // otherwise allocate a fresh one
        u32 result_id;
        if (phiResultReg < idCapacity && spirvIds[phiResultReg] != 0) {
            // Use the pre-allocated ID
            result_id = spirvIds[phiResultReg];
        } else {
            // Fallback: allocate a fresh ID and map it
            result_id = AllocateId();
            if (phiResultReg < idCapacity) {
                spirvIds[phiResultReg] = result_id;
            }
        }

        // Get the PHI result type
        CoreType phiType = static_cast<CoreType>(ir->phiTypes[phi_idx]);
        u32 type_id = 0;
        if (phiType == CoreType::CUSTOM || phiType == CoreType::ENUM) {
            // For struct/custom types, get the type from one of the PHI operands
            u16 firstValueReg = ir->GetPhiOperandValue(phi_idx, 0);
            if (firstValueReg < ir->registerCount && ir->registerStructTypes) {
                u32 structHash = ir->registerStructTypes[firstValueReg];
                type_id = GetStructTypeId(structHash);
            }
        } else if (phiType == CoreType::INVALID || phiType == CoreType::VOID) {
            // Try to infer from the first operand register's type
            u16 firstValueReg = ir->GetPhiOperandValue(phi_idx, 0);
            if (firstValueReg < ir->registerCount && ir->registerTypes) {
                CoreType opType = static_cast<CoreType>(ir->registerTypes[firstValueReg]);
                if (opType == CoreType::CUSTOM || opType == CoreType::ENUM) {
                    if (ir->registerStructTypes) {
                        u32 structHash = ir->registerStructTypes[firstValueReg];
                        type_id = GetStructTypeId(structHash);
                    }
                } else {
                    type_id = GetTypeId(opType);
                }
            }
        } else {
            type_id = GetTypeId(phiType);
        }
        if (type_id == 0) {
            // Final fallback to a safe scalar type
            type_id = GetTypeId(CoreType::FLOAT);
        }
        
        // Build OpPhi instruction
        // Format: OpPhi result_type result_id (variable_id, parent_block_id)+
        u32 totalWords = 3 + operandCount * 2; // opcode + type + result + pairs
        
        if (currentFunctionSize + totalWords > currentFunctionCapacity) {
            GrowCurrentFunction();
        }
        
        currentFunction[currentFunctionSize++] = (totalWords << 16) | spv::OpPhi;
        currentFunction[currentFunctionSize++] = type_id;
        currentFunction[currentFunctionSize++] = result_id;
        
        // Emit operand pairs (value, block label)
        for (u32 op = 0; op < operandCount; op++) {
            u16 valueReg = ir->GetPhiOperandValue(phi_idx, op);
            u32 sourceBlockIdx = ir->GetPhiOperandBlock(phi_idx, op);
            
            // Get the SPIR-V ID for the value
            u32 value_id = GetSpirvId(valueReg);
            
            // Get the block label - the source block's first instruction determines its label
            u32 sourceBlockFirstInst = cfg->firstInst[sourceBlockIdx];
            u32 block_label = GetOrCreateBlockLabel(sourceBlockFirstInst);
            
            currentFunction[currentFunctionSize++] = value_id;
            currentFunction[currentFunctionSize++] = block_label;
        }
    }
}

// ============= Function Emission =============

void SPIRVBuilder::SimplifyTrivialPhis() {
    // Pre-process PHIs before emission: if all operands of a PHI have the same value,
    // simply map the PHI result register to that value's SPIR-V ID.
    // This handles cases where variant specialization eliminates one branch,
    // making a PHI trivial.
    
    if (!ir) return;
    if (!ir->phiBlockIndices || !ir->phiResultRegs || !ir->phiOperandOffsets || !ir->phiOperandValues) return;
    if (ir->phiCount == 0) return;
    
    for (u32 phi_idx = 0; phi_idx < ir->phiCount; phi_idx++) {
        u32 operandCount = ir->GetPhiOperandCount(phi_idx);
        if (operandCount == 0) continue;
        
        // Check if all operands have the same value register
        u16 firstValueReg = ir->GetPhiOperandValue(phi_idx, 0);
        bool allSame = true;
        for (u32 op = 1; op < operandCount; op++) {
            if (ir->GetPhiOperandValue(phi_idx, op) != firstValueReg) {
                allSame = false;
                break;
            }
        }
        
        if (allSame) {
            // Trivial PHI - map result register to source value's ID
            u16 phiResultReg = ir->phiResultRegs[phi_idx];
            u32 source_id = GetSpirvId(firstValueReg);
            if (phiResultReg < idCapacity) {
                spirvIds[phiResultReg] = source_id;
            }
        }
    }
}

void SPIRVBuilder::EmitFunction() {
    // Emit the main shader function
    // 1. Declare interface variables
    DeclareInputOutput();
    DeclareResources();
    DeclareSharedVariables();
    
    // 2. Emit entry point (now that we know all interface variables)
    EmitEntryPoint();
    
    // 3. Pre-simplify trivial PHIs before function body
    // This handles cases where variant specialization makes a PHI trivial
    // Must be after DeclareInputOutput sets up spirvIds array
    SimplifyTrivialPhis();
    
    // 4. Emit function type (void function taking no parameters)
    u32 void_type = GetTypeId(CoreType::VOID);
    u32 func_type_id = GetFunctionTypeId(void_type, nullptr, 0);
    
    // 5. OpFunction
    // Format: result_type, result_id, function_control, function_type
    Emit(spv::OpFunction, void_type, entryPointId, 
         static_cast<u32>(spv::FunctionControlMaskNone), func_type_id);
    
    // 6. Emit function body
    EmitFunctionBody();
    
    // 7. OpFunctionEnd
    Emit(spv::OpFunctionEnd);
    
    // 8. Copy current function to functions section
    if (functions.count + currentFunctionSize > functions.capacity) {
        while (functions.count + currentFunctionSize > functions.capacity) {
            GrowSection(&functions);
        }
    }
    memcpy(&functions.words[functions.count], currentFunction, currentFunctionSize * sizeof(u32));
    functions.count += currentFunctionSize;
}

void SPIRVBuilder::EmitFunctionBody() {
    // Emit all basic blocks in CFG order
    // This ensures proper PHI node emission and structured control flow

    // IMPORTANT: Pre-allocate SPIR-V IDs in the correct order to handle PHI dependencies.
    //
    // Problem: When PHI A references PHI B's result (e.g., loop header PHI references
    // if-merge PHI), and PHI A is emitted before PHI B, we need PHI B's result ID
    // to already be allocated. Otherwise, we'd allocate a temporary ID for the operand
    // that never gets defined.
    //
    // Solution: Pre-allocate PHI RESULT IDs first, then pre-allocate operand IDs.
    // This ensures that when we reference a PHI result as an operand, we get the
    // correct ID that will be defined when that PHI is emitted.

    // Phase 1: Pre-allocate IDs for all PHI RESULTS
    if (ir->phiCount > 0 && ir->phiResultRegs) {
        for (u32 phi_idx = 0; phi_idx < ir->phiCount; phi_idx++) {
            u16 phiResultReg = ir->phiResultRegs[phi_idx];
            if (phiResultReg < idCapacity && spirvIds[phiResultReg] == 0) {
                spirvIds[phiResultReg] = AllocateId();
            }
        }
    }

    // Phase 2: Pre-allocate IDs for PHI OPERANDS (that aren't PHI results)
    // This handles cases where a PHI references a value from a block that comes later
    // in CFG order (e.g., back edges in loops).
    //
    // We also need to check if the register is an "undef" register (from SSA when a variable
    // isn't defined on some path to a PHI) and emit OpUndef for it.
    if (ir->phiCount > 0 && ir->phiOperandValues) {
        for (u32 phi_idx = 0; phi_idx < ir->phiCount; phi_idx++) {
            u32 operandCount = ir->GetPhiOperandCount(phi_idx);
            for (u32 op = 0; op < operandCount; op++) {
                u16 valueReg = ir->GetPhiOperandValue(phi_idx, op);
                // Skip constants (they have special encoding and are handled differently)
                // 0x8000=float, 0x4000=int, 0x2000=uint, 0xC000=bool
                if (valueReg & 0xE000) continue;
                // Pre-allocate an ID for this register if it doesn't have one
                // Use GetSpirvId which handles undef registers properly
                if (valueReg < idCapacity && spirvIds[valueReg] == 0) {
                    // Check if this is an undef register first
                    bool isUndef = false;
                    if (ir->undefRegs && ir->undefRegCount > 0) {
                        for (u32 i = 0; i < ir->undefRegCount; i++) {
                            if (ir->undefRegs[i] == valueReg) {
                                isUndef = true;
                                break;
                            }
                        }
                    }
                    // Call GetSpirvId - it will check for undef registers and emit OpUndef if needed
                    (void)GetSpirvId(valueReg);
                    // Mark as pre-allocated if NOT an undef (undef already has definition)
                    // This flag tells STORE_REG to emit OpCopyObject to define this ID
                    if (!isUndef && valueReg < idCapacity) {
                        hasPreAllocatedId[valueReg] = true;
                    }
                }
            }
        }
    }

    if (!cfg || cfg->blockCount == 0) {
        // Fallback for trivial shaders without CFG
        u32 entry_label = GetOrCreateBlockLabel(0);
        Emit(spv::OpLabel, entry_label);
        
        for (u32 i = 0; i < ir->instructionCount; i++) {
            TranslateInstruction(i);
        }
        
        if (ir->instructionCount == 0 || 
            !IR::IsTerminator(static_cast<IR::OpCode>(ir->opcodes[ir->instructionCount - 1]))) {
            Emit(spv::OpReturn);
        }
        return;
    }
    
    // Iterate blocks in CFG order
    for (u32 blockIdx = 0; blockIdx < cfg->blockCount; blockIdx++) {
        u32 firstInst = cfg->firstInst[blockIdx];
        u32 lastInst = cfg->lastInst[blockIdx];
        
        // Emit block label
        u32 labelId = GetOrCreateBlockLabel(firstInst);
        Emit(spv::OpLabel, labelId);
        
        // Emit PHI nodes first (required by SPIR-V spec)
        EmitPhiNodes(blockIdx);
        
        // Emit all instructions in this block EXCEPT the terminator
        // We need special handling for terminators to emit merge instructions first
        IR::OpCode lastOp = static_cast<IR::OpCode>(ir->opcodes[lastInst]);
        bool lastIsTerminator = IR::IsTerminator(lastOp);
        
        u32 endInst = lastIsTerminator ? lastInst : lastInst + 1;
        for (u32 i = firstInst; i < endInst; i++) {
            TranslateInstruction(i);
        }
        
        // Handle terminator with structured control flow
        if (lastIsTerminator) {
            // Use EmitBranch which handles bool conversion BEFORE merge instruction
            EmitBranch(lastInst);
        } else {
            // Block doesn't end with an explicit terminator
            // Fall-through blocks need explicit branches in SPIR-V
            // Use CFG successor information to find the target
            u32 succCount = cfg->TotalSuccessorCount(blockIdx);
            if (succCount > 0) {
                u32 succBlockIdx = cfg->GetAnySuccessor(blockIdx, 0);
                if (succBlockIdx != NO_BLOCK && succBlockIdx < cfg->blockCount) {
                    u32 succFirstInst = cfg->firstInst[succBlockIdx];
                    u32 succLabel = GetOrCreateBlockLabel(succFirstInst);
                    Emit(spv::OpBranch, succLabel);
                } else {
                    Emit(spv::OpReturn);
                }
            } else if (blockIdx + 1 < cfg->blockCount) {
                // Fallback: branch to next block in order
                u32 nextBlockFirstInst = cfg->firstInst[blockIdx + 1];
                u32 nextLabel = GetOrCreateBlockLabel(nextBlockFirstInst);
                Emit(spv::OpBranch, nextLabel);
            } else {
                // Last block with no terminator - emit return
                Emit(spv::OpReturn);
            }
        }
    }
}

// ============= Interface Setup =============

// Helper to create an interface variable with decoration
u32 SPIRVBuilder::CreateInterfaceVariable(CoreType type, spv::StorageClass storage, 
                                          u32 location, spv::BuiltIn builtin) {
    u32 type_id = GetTypeId(type);
    u32 ptr_type_id = GetPointerTypeId(type_id, storage);
    u32 var_id = AllocateId();
    
    // OpVariable
    u32 ops[] = {ptr_type_id, var_id, static_cast<u32>(storage)};
    EmitToSection(&globals, spv::OpVariable, ops, 3);
    
    // Apply decoration
    if (builtin != spv::BuiltInMax) {
        // BuiltIn decoration
        u32 builtin_val[] = {static_cast<u32>(builtin)};
        EmitDecoration(var_id, spv::DecorationBuiltIn, builtin_val, 1);
    } else {
        // Location decoration
        u32 loc[] = {location};
        EmitDecoration(var_id, spv::DecorationLocation, loc, 1);
    }
    
    return var_id;
}

// Helper: Get fallback attribute type by index (standard vertex attribute layout)
static CoreType GetFallbackAttributeType(u32 attrIdx) {
    // Standard attribute layout fallbacks when type info not available:
    // 0: position (float3)
    // 1: normal (float3)
    // 2: texcoord (float2)
    // 3: tangent (float4)
    // 4: color (float4)
    // 5: boneWeights (float4)
    // 6: boneIndices (uint4)
    // 7+: custom (float4 default)
    switch (attrIdx) {
        case 0: return CoreType::FLOAT3;  // position
        case 1: return CoreType::FLOAT3;  // normal
        case 2: return CoreType::FLOAT2;  // texcoord
        case 3: return CoreType::FLOAT4;  // tangent
        case 4: return CoreType::FLOAT4;  // color
        case 5: return CoreType::FLOAT4;  // boneWeights
        case 6: return CoreType::UINT4;   // boneIndices
        default: return CoreType::FLOAT4; // custom
    }
}

// Helper: Get fallback output type by slot
static CoreType GetFallbackOutputType(u32 slot) {
    switch (slot) {
        case OutputSlot::POSITION: return CoreType::FLOAT4;
        case OutputSlot::COLOR:    return CoreType::FLOAT4;
        case OutputSlot::DEPTH:    return CoreType::FLOAT;
        default:                   return CoreType::FLOAT4; // varyings
    }
}

void SPIRVBuilder::DeclareInputOutput() {
    // ============= Vertex Shader =============
    if (stage == ShaderStage::Vertex) {
        // Declare built-in inputs (vertex_id, instance_id) if used by shader code
        // These are separate from vertex pulling which also uses vertex_id internally
        if (analysis.UsesVertexId() && vertexIdVarId == 0) {
            DeclareVertexIdBuiltin();
        }
        if (analysis.UsesInstanceId() && instanceIdVarId == 0) {
            DeclareInstanceIdBuiltin();
        }

        // Check vertex pulling mode
        if (vertexPullingConfig.mode == VertexInputMode::SeparateBuffers ||
            vertexPullingConfig.mode == VertexInputMode::UnifiedWithOffsets) {
            // Use vertex pulling - attributes come from storage buffers
            // DeclareVertexIdBuiltin may have already been called above
            if (vertexIdVarId == 0) {
                DeclareVertexIdBuiltin();
            }
            DeclareVertexPullingBuffers();
        } else {
            // Traditional interleaved mode - attributes via Input variables with Location
            // --- Inputs: Only declare attributes that are actually used ---
            for (u32 attrIdx = 0; attrIdx < 16; attrIdx++) {
                if (!(analysis.usedAttributeMask & (1 << attrIdx))) continue;
                
                // Get type from analysis (captured from IR), fall back to symbol table, then defaults
                CoreType type = static_cast<CoreType>(analysis.attributeTypes[attrIdx]);
                if (type == CoreType::VOID || type == CoreType::INVALID) {
                    // Try symbol table
                    if (symbols) {
                        for (u32 i = 0; i < symbols->attributes.count; i++) {
                            if (symbols->attributes[i].attributeIndex == attrIdx) {
                                type = symbols->attributes[i].typeInfo.coreType;
                                break;
                            }
                        }
                    }
                    // Final fallback
                    if (type == CoreType::VOID || type == CoreType::INVALID) {
                        type = GetFallbackAttributeType(attrIdx);
                    }
                }
                
                u32 var_id = CreateInterfaceVariable(type, spv::StorageClassInput, 
                                                      attrIdx, spv::BuiltInMax);
                inputIds[inputCount] = var_id;
                inputLocations[inputCount] = attrIdx;
                inputCount++;
            }
        }
        
        // --- Outputs: Position builtin (if used) ---
        if (analysis.usedOutputMask & (1 << OutputSlot::POSITION)) {
            u32 var_id = CreateInterfaceVariable(CoreType::FLOAT4, spv::StorageClassOutput,
                                                  0, spv::BuiltInPosition);
            outputIds[outputCount] = var_id;
            outputLocations[outputCount] = 0xFF; // Mark as builtin
            outputCount++;
        }
        
        // --- Outputs: User varyings (if used) ---
        u32 locationCounter = 0;
        for (u32 slot = OutputSlot::VARYING0; slot <= OutputSlot::VARYING3; slot++) {
            if (!(analysis.usedOutputMask & (1 << slot))) continue;

            // Get type from analysis or use fallback
            CoreType type = static_cast<CoreType>(analysis.outputTypes[slot]);
            if (type == CoreType::VOID || type == CoreType::INVALID) {
                type = GetFallbackOutputType(slot);
            }

            u32 var_id = CreateInterfaceVariable(type, spv::StorageClassOutput,
                                                  locationCounter, spv::BuiltInMax);

            // Emit consistent varying names for WebGL compatibility
            // (GLSL ES 300 matches varyings by name, not location)
            char varyingName[16];
            snprintf(varyingName, sizeof(varyingName), "varying%u", locationCounter);
            EmitName(var_id, varyingName);

            locationCounter++;
            outputIds[outputCount] = var_id;
            // Store the slot ID for OP_STORE_OUTPUT lookup
            outputLocations[outputCount] = slot;
            outputCount++;
        }
    }
    
    // ============= Fragment Shader =============
    else if (stage == ShaderStage::Fragment) {
        // --- Inputs: Varyings from vertex shader ---
        // Use usedInputMask which tracks OP_LOAD_INPUT usage (input.normal, etc.)
        // Slots are VARYING0+index, so location = slot - VARYING0 to match vertex output
        // IMPORTANT: Location must match vertex shader even if some varyings are unused
        for (u32 slot = OutputSlot::VARYING0; slot <= OutputSlot::VARYING0 + 15; slot++) {
            if (!(analysis.usedInputMask & (1 << slot))) continue;

            // Get type from analysis or use fallback
            CoreType type = static_cast<CoreType>(analysis.inputTypes[slot]);
            if (type == CoreType::VOID || type == CoreType::INVALID) {
                type = CoreType::FLOAT3;  // Default for varyings like normal
            }

            // Location = slot - VARYING0 to match vertex shader output locations
            // e.g., slot=VARYING0+1 → location=1, matching vertex "varying1"
            u32 location = slot - OutputSlot::VARYING0;
            u32 var_id = CreateInterfaceVariable(type, spv::StorageClassInput,
                                                  location, spv::BuiltInMax);

            // Emit consistent varying names for WebGL compatibility
            // (GLSL ES 300 matches varyings by name, not location)
            char varyingName[16];
            snprintf(varyingName, sizeof(varyingName), "varying%u", location);
            EmitName(var_id, varyingName);

            inputIds[inputCount] = var_id;
            inputLocations[inputCount] = slot;  // Store actual slot for OP_LOAD_INPUT lookup
            inputCount++;
        }
        
        // --- Output: Fragment color (if used) ---
        if (analysis.usedOutputMask & (1 << OutputSlot::COLOR)) {
            CoreType type = static_cast<CoreType>(analysis.outputTypes[OutputSlot::COLOR]);
            if (type == CoreType::VOID || type == CoreType::INVALID) {
                type = CoreType::FLOAT4;
            }
            u32 var_id = CreateInterfaceVariable(type, spv::StorageClassOutput,
                                                  0, spv::BuiltInMax);
            outputIds[outputCount] = var_id;
            // Store the slot ID (COLOR=1) for OP_STORE_OUTPUT lookup, not SPIR-V location
            outputLocations[outputCount] = OutputSlot::COLOR;
            outputCount++;
        }
        
        // --- Output: Fragment depth (if used) ---
        if (analysis.usedOutputMask & (1 << OutputSlot::DEPTH)) {
            u32 var_id = CreateInterfaceVariable(CoreType::FLOAT, spv::StorageClassOutput,
                                                  0, spv::BuiltInFragDepth);
            outputIds[outputCount] = var_id;
            // Store the slot ID (DEPTH=16) for OP_STORE_OUTPUT lookup
            outputLocations[outputCount] = OutputSlot::DEPTH;
            outputLocations[outputCount] = 0xFF; // Mark as builtin
            outputCount++;
        }
    }
    
    // ============= Compute Shader =============
    else if (stage == ShaderStage::Compute) {
        // Compute shaders need builtin inputs for dispatch coordinates
        // Declare all compute built-ins and store their variable IDs for later use

        // GlobalInvocationId (uint3) - most commonly used
        if (analysis.UsesGlobalId()) {
            globalInvocationIdVarId = CreateInterfaceVariable(CoreType::UINT3, spv::StorageClassInput,
                                                               0, spv::BuiltInGlobalInvocationId);
            inputIds[inputCount] = globalInvocationIdVarId;
            inputLocations[inputCount] = 0xFF; // Mark as builtin
            inputCount++;
        }

        // LocalInvocationId (uint3)
        if (analysis.UsesLocalId()) {
            localInvocationIdVarId = CreateInterfaceVariable(CoreType::UINT3, spv::StorageClassInput,
                                                              0, spv::BuiltInLocalInvocationId);
            inputIds[inputCount] = localInvocationIdVarId;
            inputLocations[inputCount] = 0xFF;
            inputCount++;
        }

        // WorkgroupId (uint3)
        if (analysis.UsesWorkgroupId()) {
            workgroupIdVarId = CreateInterfaceVariable(CoreType::UINT3, spv::StorageClassInput,
                                                        0, spv::BuiltInWorkgroupId);
            inputIds[inputCount] = workgroupIdVarId;
            inputLocations[inputCount] = 0xFF;
            inputCount++;
        }

        // NumWorkgroups (uint3)
        if (analysis.UsesNumWorkgroups()) {
            numWorkgroupsVarId = CreateInterfaceVariable(CoreType::UINT3, spv::StorageClassInput,
                                                          0, spv::BuiltInNumWorkgroups);
            inputIds[inputCount] = numWorkgroupsVarId;
            inputLocations[inputCount] = 0xFF;
            inputCount++;
        }

        // LocalInvocationIndex (uint) - single scalar, not uint3
        if (analysis.UsesLocalIndex()) {
            localInvocationIndexVarId = CreateInterfaceVariable(CoreType::UINT, spv::StorageClassInput,
                                                                 0, spv::BuiltInLocalInvocationIndex);
            inputIds[inputCount] = localInvocationIndexVarId;
            inputLocations[inputCount] = 0xFF;
            inputCount++;
        }
    }
}

void SPIRVBuilder::DeclareSharedVariables() {
    if (!ir || ir->sharedVarCount == 0) return;

    for (u32 i = 0; i < ir->sharedVarCount; i++) {
        CoreType elemType = static_cast<CoreType>(ir->sharedTypes[i]);
        u32 elemTypeId = GetTypeId(elemType);
        if (elemTypeId == 0) {
            continue;
        }

        u32 varTypeId = elemTypeId;
        u32 arraySize = ir->sharedArraySizes[i];
        if (arraySize > 0) {
            u32 arrayTypeId = AllocateId();
            u32 lengthConstId = GetIntConstantId(arraySize, true);
            u32 arrayOps[] = {arrayTypeId, elemTypeId, lengthConstId};
            EmitToSection(&typesConstants, spv::OpTypeArray, arrayOps, 3);
            varTypeId = arrayTypeId;
        }

        u32 ptrTypeId = GetPointerTypeId(varTypeId, spv::StorageClassWorkgroup);
        u32 varId = AllocateId();
        u32 varOps[] = {ptrTypeId, varId, static_cast<u32>(spv::StorageClassWorkgroup)};
        EmitToSection(&globals, spv::OpVariable, varOps, 3);

        if (emitDebugNames) {
            std::string name = ReverseLookup::GetString(ir->sharedNameHashes[i]);
            if (!name.empty()) {
                EmitName(varId, name.c_str());
            }
        }

        u16 reg = ir->sharedRegisters[i];
        if (reg < idCapacity) {
            spirvIds[reg] = varId;
        }
    }
}

// ============= Vertex Pulling Implementation =============

void SPIRVBuilder::DeclareVertexIdBuiltin() {
    // Declare gl_VertexIndex (SPIR-V BuiltIn VertexIndex) for indexing into attribute buffers
    u32 uint_type = GetTypeId(CoreType::UINT);
    u32 ptr_type = GetPointerTypeId(uint_type, spv::StorageClassInput);

    vertexIdVarId = AllocateId();

    // OpVariable for vertex ID input
    u32 ops[] = {ptr_type, vertexIdVarId, static_cast<u32>(spv::StorageClassInput)};
    EmitToSection(&globals, spv::OpVariable, ops, 3);

    // Decorate as BuiltIn VertexIndex
    u32 builtin_val[] = {static_cast<u32>(spv::BuiltInVertexIndex)};
    EmitDecoration(vertexIdVarId, spv::DecorationBuiltIn, builtin_val, 1);

    // Add to interface list for entry point
    inputIds[inputCount] = vertexIdVarId;
    inputLocations[inputCount] = 0xFF;  // Mark as builtin
    inputCount++;
}

void SPIRVBuilder::DeclareInstanceIdBuiltin() {
    // Declare gl_InstanceIndex (SPIR-V BuiltIn InstanceIndex) for instanced rendering
    u32 uint_type = GetTypeId(CoreType::UINT);
    u32 ptr_type = GetPointerTypeId(uint_type, spv::StorageClassInput);

    instanceIdVarId = AllocateId();

    // OpVariable for instance ID input
    u32 ops[] = {ptr_type, instanceIdVarId, static_cast<u32>(spv::StorageClassInput)};
    EmitToSection(&globals, spv::OpVariable, ops, 3);

    // Decorate as BuiltIn InstanceIndex
    u32 builtin_val[] = {static_cast<u32>(spv::BuiltInInstanceIndex)};
    EmitDecoration(instanceIdVarId, spv::DecorationBuiltIn, builtin_val, 1);

    // Add to interface list for entry point
    inputIds[inputCount] = instanceIdVarId;
    inputLocations[inputCount] = 0xFF;  // Mark as builtin
    inputCount++;
}

void SPIRVBuilder::DeclareVertexPullingBuffers() {
    if (vertexPullingConfig.mode == VertexInputMode::SeparateBuffers) {
        // Declare one storage buffer per attribute
        u32 binding = vertexPullingConfig.baseBufferBinding;
        
        for (u32 attrIdx = 0; attrIdx < 8; attrIdx++) {
            if (!(analysis.usedAttributeMask & (1 << attrIdx))) continue;
            
            // Get type from analysis or fallback
            CoreType type = static_cast<CoreType>(analysis.attributeTypes[attrIdx]);
            if (type == CoreType::VOID || type == CoreType::INVALID) {
                type = GetFallbackAttributeType(attrIdx);
            }
            
            u32 buffer_id = CreateStorageBufferForAttribute(attrIdx, type, binding);
            attributeBufferIds[attrIdx] = buffer_id;
            binding++;
        }
    } else if (vertexPullingConfig.mode == VertexInputMode::UnifiedWithOffsets) {
        // Declare unified vertex buffer (as byte array)
        // and offset table buffer
        
        // Unified buffer at base binding
        u32 uint_type = GetTypeId(CoreType::UINT);
        u32 runtime_array_type = AllocateId();
        {
            u32 ops[] = {runtime_array_type, uint_type};
            EmitToSection(&typesConstants, spv::OpTypeRuntimeArray, ops, 2);
        }
        
        // Decorate array stride
        u32 stride[] = {4};  // u32 stride
        EmitDecoration(runtime_array_type, spv::DecorationArrayStride, stride, 1);
        
        // Struct wrapper for buffer block
        u32 struct_type = AllocateId();
        {
            u32 ops[] = {struct_type, runtime_array_type};
            EmitToSection(&typesConstants, spv::OpTypeStruct, ops, 2);
        }
        EmitDecoration(struct_type, spv::DecorationBlock, nullptr, 0);
        
        // Member 0 offset decoration (OpMemberDecorate)
        EmitMemberDecoration(struct_type, 0, spv::DecorationOffset, 0);
        
        // Pointer type
        u32 ptr_type = GetPointerTypeId(struct_type, spv::StorageClassStorageBuffer);
        
        // Variable
        u32 buffer_id = AllocateId();
        {
            u32 ops[] = {ptr_type, buffer_id, static_cast<u32>(spv::StorageClassStorageBuffer)};
            EmitToSection(&globals, spv::OpVariable, ops, 3);
        }
        
        // Bindings
        u32 set[] = {vertexPullingConfig.descriptorSet};
        EmitDecoration(buffer_id, spv::DecorationDescriptorSet, set, 1);
        u32 binding[] = {vertexPullingConfig.baseBufferBinding};
        EmitDecoration(buffer_id, spv::DecorationBinding, binding, 1);
        
        // Store in first slot (unified buffer)
        attributeBufferIds[0] = buffer_id;
        
        // Offset table at base binding + 1
        // Similar structure but for offset table
        u32 offset_struct_type = AllocateId();
        {
            u32 ops[] = {offset_struct_type, runtime_array_type};
            EmitToSection(&typesConstants, spv::OpTypeStruct, ops, 2);
        }
        EmitDecoration(offset_struct_type, spv::DecorationBlock, nullptr, 0);
        EmitMemberDecoration(offset_struct_type, 0, spv::DecorationOffset, 0);
        
        u32 offset_ptr_type = GetPointerTypeId(offset_struct_type, spv::StorageClassStorageBuffer);
        
        u32 offset_buffer_id = AllocateId();
        {
            u32 ops[] = {offset_ptr_type, offset_buffer_id, static_cast<u32>(spv::StorageClassStorageBuffer)};
            EmitToSection(&globals, spv::OpVariable, ops, 3);
        }
        
        EmitDecoration(offset_buffer_id, spv::DecorationDescriptorSet, set, 1);
        u32 offset_binding[] = {vertexPullingConfig.baseBufferBinding + 1};
        EmitDecoration(offset_buffer_id, spv::DecorationBinding, offset_binding, 1);
        
        // Store offset table ID
        attributeBufferIds[1] = offset_buffer_id;
    }
}

u32 SPIRVBuilder::CreateStorageBufferForAttribute(u32 attrIdx, CoreType elementType, u32 binding) {
    // Create a storage buffer containing a runtime array of the attribute type
    // This maps to ModelData::attributeStreams[attrIdx]
    
    u32 element_type_id = GetTypeId(elementType);
    
    // Runtime array of the element type
    u32 runtime_array_type = AllocateId();
    {
        u32 ops[] = {runtime_array_type, element_type_id};
        EmitToSection(&typesConstants, spv::OpTypeRuntimeArray, ops, 2);
    }
    
    // Decorate array stride based on element type
    // Note: GLSL std430 requires vec3/ivec3/uvec3 to have 16-byte stride (same as vec4)
    // For Metal/HLSL we can use packed 12-byte stride
    u32 stride = 0;
    u32 vec3Stride = useStd430Padding ? 16 : 12;
    switch (elementType) {
        case CoreType::FLOAT:  stride = 4; break;
        case CoreType::FLOAT2: stride = 8; break;
        case CoreType::FLOAT3: stride = vec3Stride; break;
        case CoreType::FLOAT4: stride = 16; break;
        case CoreType::INT:    stride = 4; break;
        case CoreType::INT2:   stride = 8; break;
        case CoreType::INT3:   stride = vec3Stride; break;
        case CoreType::INT4:   stride = 16; break;
        case CoreType::UINT:   stride = 4; break;
        case CoreType::UINT2:  stride = 8; break;
        case CoreType::UINT3:  stride = vec3Stride; break;
        case CoreType::UINT4:  stride = 16; break;
        default:               stride = 16; break;
    }
    
    u32 stride_val[] = {stride};
    EmitDecoration(runtime_array_type, spv::DecorationArrayStride, stride_val, 1);
    
    // Struct wrapper (required for storage buffers)
    u32 struct_type_id = AllocateId();
    {
        u32 ops[] = {struct_type_id, runtime_array_type};
        EmitToSection(&typesConstants, spv::OpTypeStruct, ops, 2);
    }
    
    // Decorate struct as Block (required for storage buffers)
    EmitDecoration(struct_type_id, spv::DecorationBlock, nullptr, 0);
    
    // Member 0 offset decoration (OpMemberDecorate requires: target, member, decoration, [operands])
    EmitMemberDecoration(struct_type_id, 0, spv::DecorationOffset, 0);
    
    // Pointer to struct
    u32 ptr_type_id = GetPointerTypeId(struct_type_id, spv::StorageClassStorageBuffer);
    attributeBufferPtrTypeIds[attrIdx] = ptr_type_id;
    
    // Variable declaration
    u32 var_id = AllocateId();
    {
        u32 ops[] = {ptr_type_id, var_id, static_cast<u32>(spv::StorageClassStorageBuffer)};
        EmitToSection(&globals, spv::OpVariable, ops, 3);
    }
    
    // Descriptor set decoration
    u32 set_val[] = {vertexPullingConfig.descriptorSet};
    EmitDecoration(var_id, spv::DecorationDescriptorSet, set_val, 1);
    
    // Binding decoration
    u32 binding_val[] = {binding};
    EmitDecoration(var_id, spv::DecorationBinding, binding_val, 1);
    
    // Name decoration for debugging
    // Emit debug names for attribute buffers if enabled
    if (emitDebugNames && attrIdx < 8) {
        static const char* attrNames[] = {
            "position_buffer", "normal_buffer", "texcoord_buffer", "color_buffer",
            "tangent_buffer", "bitangent_buffer", "boneIndices_buffer", "boneWeights_buffer"
        };
        EmitName(var_id, attrNames[attrIdx]);
    }

    return var_id;
}

// Helper: Get std140 size for a type
static u32 GetStd140Size(CoreType type) {
    switch (type) {
        case CoreType::FLOAT:  return 4;
        case CoreType::INT:    return 4;
        case CoreType::UINT:   return 4;
        case CoreType::BOOL:   return 4;
        case CoreType::FLOAT2: return 8;
        case CoreType::FLOAT3: return 16;  // Aligned to vec4
        case CoreType::FLOAT4: return 16;
        case CoreType::INT2:   return 8;
        case CoreType::INT3:   return 16;
        case CoreType::INT4:   return 16;
        case CoreType::UINT2:  return 8;
        case CoreType::UINT3:  return 16;
        case CoreType::UINT4:  return 16;
        case CoreType::MAT2:   return 32;   // 2 * vec4
        case CoreType::MAT3:   return 48;   // 3 * vec4
        case CoreType::MAT4:   return 64;   // 4 * vec4
        default:               return 16;
    }
}

// Helper: Check if type is a matrix
static bool IsMatrixType(CoreType type) {
    return type == CoreType::MAT2 || type == CoreType::MAT3 || type == CoreType::MAT4;
}

void SPIRVBuilder::DeclareResources() {
    // Declare resources based on IR analysis results
    // Each uniform binding in BWSL is a single typed value (resources.modelMatrix, etc.)
    // We create a struct wrapper for each to satisfy SPIR-V UBO requirements
    
    // ============= Uniform Buffers =============
    for (u32 binding = 0; binding < 32; binding++) {
        if (!(analysis.usedUniformMask & (1 << binding))) continue;
        
        // Get the actual type from analysis (derived from IR register types)
        CoreType uniformType = static_cast<CoreType>(analysis.uniformTypes[binding]);
        if (uniformType == CoreType::VOID || uniformType == CoreType::INVALID) {
            // Fallback to float4 if type unknown
            uniformType = CoreType::FLOAT4;
        }
        
        u32 member_type_id = GetTypeId(uniformType);
        
        // Create struct type with single member (SPIR-V requires struct for UBOs)
        u32 struct_type_id = AllocateId();
        {
            u32 ops[] = {struct_type_id, member_type_id};
            EmitToSection(&typesConstants, spv::OpTypeStruct, ops, 2);
        }
        
        // Decorate struct as Block (required for uniform buffers)
        EmitDecoration(struct_type_id, spv::DecorationBlock, nullptr, 0);
        
        // Member offset decoration (member 0 at offset 0)
        u32 offset_ops[] = {struct_type_id, 0, spv::DecorationOffset, 0};
        EmitToSection(&decorations, spv::OpMemberDecorate, offset_ops, 4);
        
        // Matrix-specific decorations
        if (IsMatrixType(uniformType)) {
            u32 col_major[] = {struct_type_id, 0, spv::DecorationColMajor};
            EmitToSection(&decorations, spv::OpMemberDecorate, col_major, 3);
            
            // MatrixStride = 16 (vec4 per column)
            u32 stride[] = {struct_type_id, 0, spv::DecorationMatrixStride, 16};
            EmitToSection(&decorations, spv::OpMemberDecorate, stride, 4);
        }
        
        // Create pointer type (Uniform storage class)
        u32 ptr_type_id = AllocateId();
        {
            u32 ops[] = {ptr_type_id, spv::StorageClassUniform, struct_type_id};
            EmitToSection(&typesConstants, spv::OpTypePointer, ops, 3);
        }
        
        // Create the variable
        u32 var_id = AllocateId();
        {
            u32 ops[] = {ptr_type_id, var_id, spv::StorageClassUniform};
            EmitToSection(&globals, spv::OpVariable, ops, 3);
        }
        
        // Decorate with DescriptorSet and Binding
        u32 set_val[] = {0};
        u32 bind_val[] = {binding};
        EmitDecoration(var_id, spv::DecorationDescriptorSet, set_val, 1);
        EmitDecoration(var_id, spv::DecorationBinding, bind_val, 1);
        
        uniformBufferIds[binding] = var_id;
        bindingSets[resourceCount] = 0;
        bindingIndices[resourceCount] = binding;
        resourceCount++;
    }
    
    // ============= Textures (combined image samplers) =============
    // First create shared image and sampled image types if any textures used
    u32 image_type_id = 0;
    u32 sampled_image_type_id = 0;
    u32 ptr_sampled_image_type = 0;

    if (analysis.usedTextureMask != 0) {
        // Reuse cached image type to avoid duplicate OpTypeImage declarations
        image_type_id = GetImageTypeId();
        sampled_image_type_id = GetSampledImageTypeId();

        ptr_sampled_image_type = AllocateId();
        {
            u32 ops[] = {ptr_sampled_image_type, spv::StorageClassUniformConstant, sampled_image_type_id};
            EmitToSection(&typesConstants, spv::OpTypePointer, ops, 3);
        }
    }
    
    // Create texture variables for each used binding
    for (u32 binding = 0; binding < 32; binding++) {
        if (!(analysis.usedTextureMask & (1 << binding))) continue;
        
        u32 tex_var_id = AllocateId();
        {
            u32 ops[] = {ptr_sampled_image_type, tex_var_id, spv::StorageClassUniformConstant};
            EmitToSection(&globals, spv::OpVariable, ops, 3);
        }
        
        // Decorate - textures typically in set 0 after uniforms
        u32 set_val[] = {0};
        u32 bind_val[] = {binding + analysis.UniformCount()};  // Offset by uniform count
        EmitDecoration(tex_var_id, spv::DecorationDescriptorSet, set_val, 1);
        EmitDecoration(tex_var_id, spv::DecorationBinding, bind_val, 1);
        
        textureIds[binding] = tex_var_id;
        bindingSets[resourceCount] = 0;
        bindingIndices[resourceCount] = binding + analysis.UniformCount();
        resourceCount++;
    }
    
    // ============= Storage Buffers =============
    if (analysis.usedStorageBufferMask != 0) {
        // Create default runtime array type for storage buffers without struct types
        u32 default_float4_type = GetTypeId(CoreType::FLOAT4);
        u32 default_runtime_array_type = AllocateId();
        {
            u32 ops[] = {default_runtime_array_type, default_float4_type};
            EmitToSection(&typesConstants, spv::OpTypeRuntimeArray, ops, 2);
        }

        // Array stride decoration for default type
        u32 stride_ops[] = {default_runtime_array_type, spv::DecorationArrayStride, 16};
        EmitToSection(&decorations, spv::OpDecorate, stride_ops, 3);

        // Create default struct containing runtime array
        u32 default_ssbo_struct_type = AllocateId();
        {
            u32 ops[] = {default_ssbo_struct_type, default_runtime_array_type};
            EmitToSection(&typesConstants, spv::OpTypeStruct, ops, 2);
        }

        // Block decoration for default type
        EmitDecoration(default_ssbo_struct_type, spv::DecorationBlock, nullptr, 0);

        // Member offset for default type
        u32 member_offset[] = {default_ssbo_struct_type, 0, spv::DecorationOffset, 0};
        EmitToSection(&decorations, spv::OpMemberDecorate, member_offset, 4);

        // Default pointer type
        u32 default_ptr_ssbo_type = AllocateId();
        {
            u32 ops[] = {default_ptr_ssbo_type, spv::StorageClassStorageBuffer, default_ssbo_struct_type};
            EmitToSection(&typesConstants, spv::OpTypePointer, ops, 3);
        }

        // Create variables for each used binding
        for (u32 binding = 0; binding < 32; binding++) {
            if (!(analysis.usedStorageBufferMask & (1 << binding))) continue;

            // Check if this storage buffer has a struct type from the symbol table
            u32 structTypeHash = 0;
            if (symbols) {
                for (u32 r = 0; r < symbols->resources.count; r++) {
                    const ResourceData& resData = symbols->resources[r];
                    if (resData.bindingIndex == binding &&
                        resData.type == ResourceBinding::Buffer &&
                        resData.structTypeHash != 0) {
                        structTypeHash = resData.structTypeHash;
                        break;
                    }
                }
            }

            u32 ptr_ssbo_type;
            u32 ssbo_struct_type;

            if (structTypeHash != 0) {
                // Use the actual struct type for this storage buffer
                ssbo_struct_type = GetStructTypeId(structTypeHash);
                if (ssbo_struct_type != 0) {
                    // Block decoration for struct type
                    EmitDecoration(ssbo_struct_type, spv::DecorationBlock, nullptr, 0);

                    // Pointer type for the struct
                    ptr_ssbo_type = AllocateId();
                    u32 ops[] = {ptr_ssbo_type, spv::StorageClassStorageBuffer, ssbo_struct_type};
                    EmitToSection(&typesConstants, spv::OpTypePointer, ops, 3);
                } else {
                    // Fallback to default
                    ptr_ssbo_type = default_ptr_ssbo_type;
                }
            } else {
                ptr_ssbo_type = default_ptr_ssbo_type;
            }

            u32 ssbo_var_id = AllocateId();
            {
                u32 ops[] = {ptr_ssbo_type, ssbo_var_id, spv::StorageClassStorageBuffer};
                EmitToSection(&globals, spv::OpVariable, ops, 3);
            }

            // Decorate - storage buffers in set 1
            u32 set_val[] = {1};
            u32 bind_val[] = {binding};
            EmitDecoration(ssbo_var_id, spv::DecorationDescriptorSet, set_val, 1);
            EmitDecoration(ssbo_var_id, spv::DecorationBinding, bind_val, 1);

            // Emit name for debugging - look up resource name from symbol table
            if (emitDebugNames && symbols) {
                // Find the symbol that corresponds to this resource binding
                for (u32 s = 0; s < symbols->symbols.count; s++) {
                    const Symbol& sym = symbols->symbols[s];
                    if (sym.kind == SymbolKind::RESOURCE && sym.index < symbols->resources.count) {
                        const ResourceData& resData = symbols->resources[sym.index];
                        if (resData.bindingIndex == binding && resData.type == ResourceBinding::Buffer) {
                            std::string bufferName = ReverseLookup::GetString(sym.name.nameHash);
                            if (!bufferName.empty()) {
                                EmitName(ssbo_var_id, bufferName.c_str());
                            }
                            break;
                        }
                    }
                }
            }

            storageBufferIds[binding] = ssbo_var_id;
            bindingSets[resourceCount] = 1;
            bindingIndices[resourceCount] = binding;
            resourceCount++;
        }
    }
}

// ============= Final Assembly =============
std::vector<u32> SPIRVBuilder::Finalize() {
    std::vector<u32> spirv;
    
    // Reserve space for efficiency
    u32 total_size = 5 + // header
                     capabilities.count +
                     extensions.count +
                     extInstImports.count +
                     memoryModel.count +
                     entryPoints.count +
                     executionModes.count +
                     debugNames.count +
                     decorations.count +
                     typesConstants.count +
                     globals.count +
                     functions.count;
    spirv.reserve(total_size);
    
    // Header
    spirv.push_back(SpvMagicNumber);
    spirv.push_back(SpvVersion);
    spirv.push_back(0); // Generator ID
    spirv.push_back(nextId); // Bound
    spirv.push_back(0); // Schema
    
    // Sections in required order
    auto appendSection = [&spirv](const Section& s) {
        spirv.insert(spirv.end(), s.words, s.words + s.count);
    };
    
    appendSection(capabilities);
    appendSection(extensions);
    appendSection(extInstImports);
    appendSection(memoryModel);
    appendSection(entryPoints);
    appendSection(executionModes);
    appendSection(debugNames);
    appendSection(decorations);
    appendSection(typesConstants);
    appendSection(globals);
    appendSection(functions);
    
    return spirv;
}

// ============= Memory Management =============
void SPIRVBuilder::GrowSection(Section* section) {
    u32 new_capacity = section->capacity * 2;
    u32* new_words = (u32*)arena->Allocate(new_capacity * sizeof(u32), 64);
    memcpy(new_words, section->words, section->count * sizeof(u32));
    section->words = new_words;
    section->capacity = new_capacity;
}

void SPIRVBuilder::GrowCurrentFunction() {
    u32 new_capacity = currentFunctionCapacity * 2;
    u32* new_func = (u32*)arena->Allocate(new_capacity * sizeof(u32), 64);
    memcpy(new_func, currentFunction, currentFunctionSize * sizeof(u32));
    currentFunction = new_func;
    currentFunctionCapacity = new_capacity;
}

void SPIRVBuilder::GrowIdArrays() {
    u32 new_capacity = idCapacity * 2;

    u32* new_spirv_ids = (u32*)arena->Allocate(new_capacity * sizeof(u32), 64);
    u16* new_id_types = (u16*)arena->Allocate(new_capacity * sizeof(u16), 64);
    u32* new_id_decorations = (u32*)arena->Allocate(new_capacity * sizeof(u32), 64);
    bool* new_has_pre_allocated = (bool*)arena->Allocate(new_capacity * sizeof(bool), 64);

    memcpy(new_spirv_ids, spirvIds, idCapacity * sizeof(u32));
    memcpy(new_id_types, idTypes, idCapacity * sizeof(u16));
    memcpy(new_id_decorations, idDecorations, idCapacity * sizeof(u32));
    memcpy(new_has_pre_allocated, hasPreAllocatedId, idCapacity * sizeof(bool));

    memset(&new_spirv_ids[idCapacity], 0, (new_capacity - idCapacity) * sizeof(u32));
    memset(&new_id_types[idCapacity], 0, (new_capacity - idCapacity) * sizeof(u16));
    memset(&new_id_decorations[idCapacity], 0, (new_capacity - idCapacity) * sizeof(u32));
    memset(&new_has_pre_allocated[idCapacity], 0, (new_capacity - idCapacity) * sizeof(bool));

    spirvIds = new_spirv_ids;
    idTypes = new_id_types;
    idDecorations = new_id_decorations;
    hasPreAllocatedId = new_has_pre_allocated;
    idCapacity = new_capacity;
}

u32 SPIRVBuilder::HashCompositeType(u32* components, u32 count) {
    u32 hash = 2166136261u;
    for (u32 i = 0; i < count; i++) {
        hash ^= components[i];
        hash *= 16777619u;
    }
    return hash;
}

} // namespace BWSL
