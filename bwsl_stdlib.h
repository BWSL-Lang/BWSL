#pragma once

#include "bwsl_render_config.h"  // For ShaderStage

#include "bwsl_types.h"
#include "bwsl_utils.h"
#include <array>
#include "vendor/SPIRV-Headers/include/spirv/1.2/GLSL.std.450.h"
#include "vendor/SPIRV-Headers/include/spirv/1.2/spirv.hpp"

// Sentinel values for opcodes that are not applicable
constexpr spv::Op SPV_OP_NONE = spv::OpMax;
constexpr u16   SPV_EXT_NONE = 0xFFFF;

struct SpvMapping {
    spv::Op coreOp;
    u16   extOp; // Can hold GLSLstd450 enum or other extension opcodes
};
namespace BWSL {
namespace StdLib {

enum class Intrinsic : u16 {
    // Math
    LERP,
    SMOOTHSTEP,
    SATURATE,
    FRACT,
    STEP,
    CLAMP,
    SIGN,
    ABS,
    MIN,
    MAX,
    FLOOR,
    CEIL,
    ROUND,
    MOD,
    POW,
    SQRT,
    RSQRT,
    EXP,
    EXP2,
    LOG,
    LOG2,
    
    // Trigonometry
    SIN,
    COS,
    TAN,
    ASIN,
    ACOS,
    ATAN,
    ATAN2,
    SINCOS,
    DEGREES,
    RADIANS,
    
    // Vector
    DOT,
    CROSS,
    NORMALIZE,
    LENGTH,
    DISTANCE,
    REFLECT,
    REFRACT,
    FACEFORWARD,
    
    // Matrix
    TRANSPOSE,
    DETERMINANT,
    INVERSE,
    
    // Derivatives (fragment only)
    DDX,
    DDY,
    DDX_FINE,
    DDY_FINE,
    DDX_COARSE,
    DDY_COARSE,
    FWIDTH,
    
    // Texture
    SAMPLE,
    SAMPLE_LOD,
    SAMPLE_BIAS,
    SAMPLE_GRAD,
    SAMPLE_CMP,
    GATHER,
    LOAD,
    STORE,
    
    // Wave/SIMD
    WAVE_ACTIVE_SUM,
    WAVE_ACTIVE_PRODUCT,
    WAVE_ACTIVE_MIN,
    WAVE_ACTIVE_MAX,
    WAVE_ACTIVE_ALL,
    WAVE_ACTIVE_ANY,
    WAVE_BROADCAST,
    WAVE_READ_FIRST,
    
    // Atomics
    ATOMIC_ADD,
    ATOMIC_MIN,
    ATOMIC_MAX,
    ATOMIC_AND,
    ATOMIC_OR,
    ATOMIC_XOR,
    ATOMIC_EXCHANGE,
    ATOMIC_CMP_EXCHANGE,
    
    // Bit operations
    COUNT_BITS,
    REVERSE_BITS,
    FIRST_BIT_LOW,
    FIRST_BIT_HIGH,

    // Control flow
    SELECT,  // Ternary select: select(false_val, true_val, condition)

    COUNT,
    INVALID = 0xFFFF
};

// Bit flags for special handling / custom behavior
namespace IntrinsicFlags {
    constexpr u8 CUSTOM_METAL    = 0x01;
    constexpr u8 CUSTOM_HLSL     = 0x02;
    constexpr u8 CUSTOM_GLSL     = 0x04;
    constexpr u8 FRAGMENT_ONLY   = 0x08;
    constexpr u8 VERTEX_ONLY     = 0x10;
    constexpr u8 TEXTURE_OP      = 0x20;
    constexpr u8 WAVE_OP         = 0x40;
    constexpr u8 ATOMIC_OP       = 0x80;
}


// Backend name mappings
struct BackendNames {
    const char* metal;
    const char* hlsl;
    const char* glsl;
};

// Define backend names for each intrinsic
constexpr BackendNames BACKEND_NAMES[] = {
    // Math
    {"mix", "lerp", "mix"},                              // LERP
    {"smoothstep", "smoothstep", "smoothstep"},          // SMOOTHSTEP
    {"saturate", "saturate", nullptr},                   // SATURATE (custom GLSL)
    {"fract", "frac", "fract"},                          // FRACT
    {"step", "step", "step"},                            // STEP
    {"clamp", "clamp", "clamp"},                         // CLAMP
    {"sign", "sign", "sign"},                            // SIGN
    {"abs", "abs", "abs"},                               // ABS
    {"min", "min", "min"},                               // MIN
    {"max", "max", "max"},                               // MAX
    {"floor", "floor", "floor"},                         // FLOOR
    {"ceil", "ceil", "ceil"},                            // CEIL
    {"round", "round", "round"},                         // ROUND
    {"fmod", "fmod", "mod"},                             // MOD
    {"pow", "pow", "pow"},                               // POW
    {"sqrt", "sqrt", "sqrt"},                            // SQRT
    {"rsqrt", "rsqrt", "inversesqrt"},                   // RSQRT
    {"exp", "exp", "exp"},                               // EXP
    {"exp2", "exp2", "exp2"},                            // EXP2
    {"log", "log", "log"},                               // LOG
    {"log2", "log2", "log2"},                            // LOG2
    
    // Trigonometry
    {"sin", "sin", "sin"},                               // SIN
    {"cos", "cos", "cos"},                               // COS
    {"tan", "tan", "tan"},                               // TAN
    {"asin", "asin", "asin"},                            // ASIN
    {"acos", "acos", "acos"},                            // ACOS
    {"atan", "atan", "atan"},                            // ATAN
    {"atan2", "atan2", "atan"},                          // ATAN2
    {nullptr, "sincos", nullptr},                        // SINCOS (custom)
    {"degrees", "degrees", "degrees"},                   // DEGREES
    {"radians", "radians", "radians"},                   // RADIANS
    
    // Vector
    {"dot", "dot", "dot"},                               // DOT
    {"cross", "cross", "cross"},                         // CROSS
    {"normalize", "normalize", "normalize"},             // NORMALIZE
    {"length", "length", "length"},                      // LENGTH
    {"distance", "distance", "distance"},                // DISTANCE
    {"reflect", "reflect", "reflect"},                   // REFLECT
    {"refract", "refract", "refract"},                   // REFRACT
    {"faceforward", "faceforward", "faceforward"},       // FACEFORWARD
    
    // Matrix
    {"transpose", "transpose", "transpose"},             // TRANSPOSE
    {"determinant", "determinant", "determinant"},       // DETERMINANT
    {nullptr, nullptr, nullptr},                         // INVERSE (custom)
    
    // Derivatives
    {"dfdx", "ddx", "dFdx"},                            // DDX
    {"dfdy", "ddy", "dFdy"},                            // DDY
    {"dfdx_fine", "ddx_fine", "dFdxFine"},              // DDX_FINE
    {"dfdy_fine", "ddy_fine", "dFdyFine"},              // DDY_FINE
    {"dfdx_coarse", "ddx_coarse", "dFdxCoarse"},        // DDX_COARSE
    {"dfdy_coarse", "ddy_coarse", "dFdyCoarse"},        // DDY_COARSE
    {"fwidth", "fwidth", "fwidth"},                      // FWIDTH
    
    // Texture
    {"sample", ".Sample", "texture"},                    // SAMPLE
    {"sample", ".SampleLevel", "textureLod"},           // SAMPLE_LOD
    {"sample", ".SampleBias", "texture"},               // SAMPLE_BIAS
    {"sample", ".SampleGrad", "textureGrad"},           // SAMPLE_GRAD
    {"sample_compare", ".SampleCmp", "texture"},        // SAMPLE_CMP
    {"gather", ".Gather", "textureGather"},             // GATHER
    {"read", ".Load", "texelFetch"},                    // LOAD
    {"write", nullptr, "imageStore"},                   // STORE (custom HLSL)
    
    // Wave/SIMD
    {"simd_sum", "WaveActiveSum", "subgroupAdd"},       // WAVE_ACTIVE_SUM
    {"simd_product", "WaveActiveProduct", "subgroupMul"}, // WAVE_ACTIVE_PRODUCT
    {"simd_min", "WaveActiveMin", "subgroupMin"},       // WAVE_ACTIVE_MIN
    {"simd_max", "WaveActiveMax", "subgroupMax"},       // WAVE_ACTIVE_MAX
    {"simd_all", "WaveActiveAllTrue", "subgroupAll"},   // WAVE_ACTIVE_ALL
    {"simd_any", "WaveActiveAnyTrue", "subgroupAny"},   // WAVE_ACTIVE_ANY
    {"simd_broadcast", "WaveReadLaneAt", "subgroupBroadcast"}, // WAVE_BROADCAST
    {"simd_broadcast_first", "WaveReadLaneFirst", "subgroupBroadcastFirst"}, // WAVE_READ_FIRST
    
    // Atomics
    {"atomic_fetch_add", "InterlockedAdd", "atomicAdd"},     // ATOMIC_ADD
    {"atomic_fetch_min", "InterlockedMin", "atomicMin"},     // ATOMIC_MIN
    {"atomic_fetch_max", "InterlockedMax", "atomicMax"},     // ATOMIC_MAX
    {"atomic_fetch_and", "InterlockedAnd", "atomicAnd"},     // ATOMIC_AND
    {"atomic_fetch_or", "InterlockedOr", "atomicOr"},        // ATOMIC_OR
    {"atomic_fetch_xor", "InterlockedXor", "atomicXor"},     // ATOMIC_XOR
    {"atomic_exchange", "InterlockedExchange", "atomicExchange"}, // ATOMIC_EXCHANGE
    {nullptr, nullptr, nullptr},                             // ATOMIC_CMP_EXCHANGE (custom)
    
    // Bit operations
    {"popcount", "countbits", "bitCount"},              // COUNT_BITS
    {"reverse_bits", "reversebits", "bitfieldReverse"}, // REVERSE_BITS
    {"ctz", "firstbitlow", "findLSB"},                  // FIRST_BIT_LOW
    {"clz", "firstbithigh", "findMSB"},                 // FIRST_BIT_HIGH

    // Control flow
    {"select", "select", "mix"},                        // SELECT (GLSL uses mix for component-wise select)
};

struct IntrinsicData {
    u32 nameHash;
    TypeMask returnTypes;
    TypeMask paramTypes[4];
    TypeMask optionalParams[2];
    u8 enumIndex;
    u8 paramCount;
    u8 flags;
    u8 minParams;
    u8 maxParams;
    SpvMapping spv;
    u8 padding[3];
};

constexpr u8 CountParamMasks(TypeMask p1, TypeMask p2, TypeMask p3, TypeMask p4) {
    u8 count = 0;
    if (p1 != 0) count++;
    if (p2 != 0) count++;
    if (p3 != 0) count++;
    if (p4 != 0) count++;
    return count;
}

#define SPV_MAP(CORE_OP, EXT_OP) {CORE_OP, EXT_OP}

#define INTRINSIC_FIXED(ENUM_NAME, FUNC_NAME, RET_MASK, P1, P2, P3, P4, FLAGS, SPV_MAPPING) \
    {Utils::HashStr(FUNC_NAME), \
     RET_MASK, \
     {P1, P2, P3, P4}, \
     {0, 0}, \
     static_cast<u8>(Intrinsic::ENUM_NAME), \
     CountParamMasks(P1, P2, P3, P4), \
     FLAGS, \
     CountParamMasks(P1, P2, P3, P4), \
     CountParamMasks(P1, P2, P3, P4), \
     SPV_MAPPING, \
     {0, 0, 0}}

#define INTRINSIC_VAR(ENUM_NAME, FUNC_NAME, MIN, MAX, RET_MASK, P1, P2, P3, P4, OPT1, OPT2, FLAGS, SPV_MAPPING) \
    {Utils::HashStr(FUNC_NAME), \
     RET_MASK, \
     {P1, P2, P3, P4}, \
     {OPT1, OPT2}, \
     static_cast<u8>(Intrinsic::ENUM_NAME), \
     MIN, \
     FLAGS, \
     MIN, \
     MAX, \
     SPV_MAPPING, \
     {0, 0, 0}}

// Texture intrinsics use special handling in SPIR-V backend based on sampling mode
// The spv::Op stored here is the primary op, backend handles variants (Lod, Bias, Grad, etc.)
#define TEXTURE_INTRINSIC(ENUM_NAME, FUNC_NAME, MIN, MAX, FLAGS) \
    {Utils::HashStr(FUNC_NAME), \
     mask(CoreType::FLOAT4), \
     {mask(CoreType::CUSTOM), 0, 0, 0}, \
     {0, 0}, \
     static_cast<u8>(Intrinsic::ENUM_NAME), \
     MIN, \
     FLAGS | IntrinsicFlags::TEXTURE_OP, \
     MIN, \
     MAX, \
     SPV_MAP(spv::OpImageSampleImplicitLod, SPV_EXT_NONE), \
     {0, 0, 0}}


constexpr IntrinsicData INTRINSICS[] = {
    // Math - most work on all float types
    // Note: For type-dependent ops (FClamp vs SClamp), backend selects based on actual type
    INTRINSIC_FIXED(LERP, "lerp", TypeMasks::FLOAT_TYPES, TypeMasks::FLOAT_TYPES, TypeMasks::FLOAT_TYPES, TypeMasks::SCALAR_TYPES, 0, 0, SPV_MAP(SPV_OP_NONE, GLSLstd450FMix)),
    INTRINSIC_FIXED(SMOOTHSTEP, "smoothstep", TypeMasks::FLOAT_TYPES, TypeMasks::SCALAR_TYPES, TypeMasks::SCALAR_TYPES, TypeMasks::FLOAT_TYPES, 0, 0, SPV_MAP(SPV_OP_NONE, GLSLstd450SmoothStep)),
    INTRINSIC_FIXED(SATURATE, "saturate", TypeMasks::FLOAT_TYPES, TypeMasks::FLOAT_TYPES, 0, 0, 0, IntrinsicFlags::CUSTOM_GLSL, SPV_MAP(SPV_OP_NONE, GLSLstd450FClamp)), // Emitted as FClamp(x, 0, 1)
    INTRINSIC_FIXED(FRACT, "fract", TypeMasks::FLOAT_TYPES, TypeMasks::FLOAT_TYPES, 0, 0, 0, 0, SPV_MAP(SPV_OP_NONE, GLSLstd450Fract)),
    INTRINSIC_FIXED(STEP, "step", TypeMasks::FLOAT_TYPES, TypeMasks::FLOAT_TYPES, TypeMasks::FLOAT_TYPES, 0, 0, 0, SPV_MAP(SPV_OP_NONE, GLSLstd450Step)),
    INTRINSIC_FIXED(CLAMP, "clamp", TypeMasks::ANY_NUMERIC, TypeMasks::ANY_NUMERIC, TypeMasks::ANY_NUMERIC, TypeMasks::ANY_NUMERIC, 0, 0, SPV_MAP(SPV_OP_NONE, GLSLstd450FClamp)), // FClamp for float, SClamp/UClamp for int
    INTRINSIC_FIXED(SIGN, "sign", TypeMasks::ANY_NUMERIC, TypeMasks::ANY_NUMERIC, 0, 0, 0, 0, SPV_MAP(SPV_OP_NONE, GLSLstd450FSign)), // FSign for float, SSign for int
    INTRINSIC_FIXED(ABS, "abs", TypeMasks::ANY_NUMERIC, TypeMasks::ANY_NUMERIC, 0, 0, 0, 0, SPV_MAP(SPV_OP_NONE, GLSLstd450FAbs)), // FAbs for float, SAbs for int
    INTRINSIC_VAR(MIN, "min", 2, 0xFF, TypeMasks::ANY_NUMERIC, TypeMasks::ANY_NUMERIC, TypeMasks::ANY_NUMERIC, 0, 0, 0, 0, 0, SPV_MAP(SPV_OP_NONE, GLSLstd450FMin)), // FMin/SMin/UMin
    INTRINSIC_VAR(MAX, "max", 2, 0xFF, TypeMasks::ANY_NUMERIC, TypeMasks::ANY_NUMERIC, TypeMasks::ANY_NUMERIC, 0, 0, 0, 0, 0, SPV_MAP(SPV_OP_NONE, GLSLstd450FMax)), // FMax/SMax/UMax
    INTRINSIC_FIXED(FLOOR, "floor", TypeMasks::FLOAT_TYPES, TypeMasks::FLOAT_TYPES, 0, 0, 0, 0, SPV_MAP(SPV_OP_NONE, GLSLstd450Floor)),
    INTRINSIC_FIXED(CEIL, "ceil", TypeMasks::FLOAT_TYPES, TypeMasks::FLOAT_TYPES, 0, 0, 0, 0, SPV_MAP(SPV_OP_NONE, GLSLstd450Ceil)),
    INTRINSIC_FIXED(ROUND, "round", TypeMasks::FLOAT_TYPES, TypeMasks::FLOAT_TYPES, 0, 0, 0, 0, SPV_MAP(SPV_OP_NONE, GLSLstd450RoundEven)),
    INTRINSIC_FIXED(MOD, "mod", TypeMasks::FLOAT_TYPES, TypeMasks::FLOAT_TYPES, TypeMasks::FLOAT_TYPES, 0, 0, 0, SPV_MAP(spv::OpFMod, SPV_EXT_NONE)),
    INTRINSIC_FIXED(POW, "pow", TypeMasks::FLOAT_TYPES, TypeMasks::FLOAT_TYPES, TypeMasks::FLOAT_TYPES, 0, 0, 0, SPV_MAP(SPV_OP_NONE, GLSLstd450Pow)),
    INTRINSIC_FIXED(SQRT, "sqrt", TypeMasks::FLOAT_TYPES, TypeMasks::FLOAT_TYPES, 0, 0, 0, 0, SPV_MAP(SPV_OP_NONE, GLSLstd450Sqrt)),
    INTRINSIC_FIXED(RSQRT, "rsqrt", TypeMasks::FLOAT_TYPES, TypeMasks::FLOAT_TYPES, 0, 0, 0, 0, SPV_MAP(SPV_OP_NONE, GLSLstd450InverseSqrt)),
    INTRINSIC_FIXED(EXP, "exp", TypeMasks::FLOAT_TYPES, TypeMasks::FLOAT_TYPES, 0, 0, 0, 0, SPV_MAP(SPV_OP_NONE, GLSLstd450Exp)),
    INTRINSIC_FIXED(EXP2, "exp2", TypeMasks::FLOAT_TYPES, TypeMasks::FLOAT_TYPES, 0, 0, 0, 0, SPV_MAP(SPV_OP_NONE, GLSLstd450Exp2)),
    INTRINSIC_FIXED(LOG, "log", TypeMasks::FLOAT_TYPES, TypeMasks::FLOAT_TYPES, 0, 0, 0, 0, SPV_MAP(SPV_OP_NONE, GLSLstd450Log)),
    INTRINSIC_FIXED(LOG2, "log2", TypeMasks::FLOAT_TYPES, TypeMasks::FLOAT_TYPES, 0, 0, 0, 0, SPV_MAP(SPV_OP_NONE, GLSLstd450Log2)),
    
    // Trigonometry - work on all float types
    INTRINSIC_FIXED(SIN, "sin", TypeMasks::FLOAT_TYPES, TypeMasks::FLOAT_TYPES, 0, 0, 0, 0, SPV_MAP(SPV_OP_NONE, GLSLstd450Sin)),
    INTRINSIC_FIXED(COS, "cos", TypeMasks::FLOAT_TYPES, TypeMasks::FLOAT_TYPES, 0, 0, 0, 0, SPV_MAP(SPV_OP_NONE, GLSLstd450Cos)),
    INTRINSIC_FIXED(TAN, "tan", TypeMasks::FLOAT_TYPES, TypeMasks::FLOAT_TYPES, 0, 0, 0, 0, SPV_MAP(SPV_OP_NONE, GLSLstd450Tan)),
    INTRINSIC_FIXED(ASIN, "asin", TypeMasks::FLOAT_TYPES, TypeMasks::FLOAT_TYPES, 0, 0, 0, 0, SPV_MAP(SPV_OP_NONE, GLSLstd450Asin)),
    INTRINSIC_FIXED(ACOS, "acos", TypeMasks::FLOAT_TYPES, TypeMasks::FLOAT_TYPES, 0, 0, 0, 0, SPV_MAP(SPV_OP_NONE, GLSLstd450Acos)),
    INTRINSIC_FIXED(ATAN, "atan", TypeMasks::FLOAT_TYPES, TypeMasks::FLOAT_TYPES, 0, 0, 0, 0, SPV_MAP(SPV_OP_NONE, GLSLstd450Atan)),
    INTRINSIC_FIXED(ATAN2, "atan2", TypeMasks::FLOAT_TYPES, TypeMasks::FLOAT_TYPES, TypeMasks::FLOAT_TYPES, 0, 0, 0, SPV_MAP(SPV_OP_NONE, GLSLstd450Atan2)),
    INTRINSIC_FIXED(SINCOS, "sincos", mask(CoreType::VOID), mask(CoreType::FLOAT), mask(CoreType::FLOAT), mask(CoreType::FLOAT), 0, IntrinsicFlags::CUSTOM_METAL | IntrinsicFlags::CUSTOM_GLSL, SPV_MAP(SPV_OP_NONE, SPV_EXT_NONE)), // Custom: calls Sin + Cos
    INTRINSIC_FIXED(DEGREES, "degrees", TypeMasks::FLOAT_TYPES, TypeMasks::FLOAT_TYPES, 0, 0, 0, 0, SPV_MAP(SPV_OP_NONE, GLSLstd450Degrees)),
    INTRINSIC_FIXED(RADIANS, "radians", TypeMasks::FLOAT_TYPES, TypeMasks::FLOAT_TYPES, 0, 0, 0, 0, SPV_MAP(SPV_OP_NONE, GLSLstd450Radians)),
    
    // Vector - work on float vector types
    INTRINSIC_FIXED(DOT, "dot", mask(CoreType::FLOAT), TypeMasks::FLOAT_TYPES & TypeMasks::VECTOR_TYPES, TypeMasks::FLOAT_TYPES & TypeMasks::VECTOR_TYPES, 0, 0, 0, SPV_MAP(spv::OpDot, SPV_EXT_NONE)),
    INTRINSIC_FIXED(CROSS, "cross", mask(CoreType::FLOAT3), mask(CoreType::FLOAT3), mask(CoreType::FLOAT3), 0, 0, 0, SPV_MAP(SPV_OP_NONE, GLSLstd450Cross)),
    INTRINSIC_FIXED(NORMALIZE, "normalize", TypeMasks::FLOAT_TYPES & TypeMasks::VECTOR_TYPES, TypeMasks::FLOAT_TYPES & TypeMasks::VECTOR_TYPES, 0, 0, 0, 0, SPV_MAP(SPV_OP_NONE, GLSLstd450Normalize)),
    INTRINSIC_FIXED(LENGTH, "length", mask(CoreType::FLOAT), TypeMasks::FLOAT_TYPES & TypeMasks::VECTOR_TYPES, 0, 0, 0, 0, SPV_MAP(SPV_OP_NONE, GLSLstd450Length)),
    INTRINSIC_FIXED(DISTANCE, "distance", mask(CoreType::FLOAT), TypeMasks::FLOAT_TYPES & TypeMasks::VECTOR_TYPES, TypeMasks::FLOAT_TYPES & TypeMasks::VECTOR_TYPES, 0, 0, 0, SPV_MAP(SPV_OP_NONE, GLSLstd450Distance)),
    INTRINSIC_FIXED(REFLECT, "reflect", TypeMasks::FLOAT_TYPES & TypeMasks::VECTOR_TYPES, TypeMasks::FLOAT_TYPES & TypeMasks::VECTOR_TYPES, TypeMasks::FLOAT_TYPES & TypeMasks::VECTOR_TYPES, 0, 0, 0, SPV_MAP(SPV_OP_NONE, GLSLstd450Reflect)),
    INTRINSIC_FIXED(REFRACT, "refract", TypeMasks::FLOAT_TYPES & TypeMasks::VECTOR_TYPES, TypeMasks::FLOAT_TYPES & TypeMasks::VECTOR_TYPES, TypeMasks::FLOAT_TYPES & TypeMasks::VECTOR_TYPES, mask(CoreType::FLOAT), 0, 0, SPV_MAP(SPV_OP_NONE, GLSLstd450Refract)),
    INTRINSIC_FIXED(FACEFORWARD, "faceforward", TypeMasks::FLOAT_TYPES & TypeMasks::VECTOR_TYPES, TypeMasks::FLOAT_TYPES & TypeMasks::VECTOR_TYPES, TypeMasks::FLOAT_TYPES & TypeMasks::VECTOR_TYPES, TypeMasks::FLOAT_TYPES & TypeMasks::VECTOR_TYPES, 0, 0, SPV_MAP(SPV_OP_NONE, GLSLstd450FaceForward)),
    
    // Matrix
    INTRINSIC_FIXED(TRANSPOSE, "transpose", TypeMasks::MATRIX_TYPES, TypeMasks::MATRIX_TYPES, 0, 0, 0, 0, SPV_MAP(spv::OpTranspose, SPV_EXT_NONE)),
    INTRINSIC_FIXED(DETERMINANT, "determinant", mask(CoreType::FLOAT), TypeMasks::MATRIX_TYPES, 0, 0, 0, 0, SPV_MAP(SPV_OP_NONE, GLSLstd450Determinant)),
    INTRINSIC_FIXED(INVERSE, "inverse", TypeMasks::MATRIX_TYPES, TypeMasks::MATRIX_TYPES, 0, 0, 0, IntrinsicFlags::CUSTOM_HLSL, SPV_MAP(SPV_OP_NONE, GLSLstd450MatrixInverse)),
    
    // Derivatives (fragment only)
    INTRINSIC_FIXED(DDX, "ddx", TypeMasks::FLOAT_TYPES, TypeMasks::FLOAT_TYPES, 0, 0, 0, IntrinsicFlags::FRAGMENT_ONLY, SPV_MAP(spv::OpDPdx, SPV_EXT_NONE)),
    INTRINSIC_FIXED(DDY, "ddy", TypeMasks::FLOAT_TYPES, TypeMasks::FLOAT_TYPES, 0, 0, 0, IntrinsicFlags::FRAGMENT_ONLY, SPV_MAP(spv::OpDPdy, SPV_EXT_NONE)),
    INTRINSIC_FIXED(DDX_FINE, "ddx_fine", TypeMasks::FLOAT_TYPES, TypeMasks::FLOAT_TYPES, 0, 0, 0, IntrinsicFlags::FRAGMENT_ONLY, SPV_MAP(spv::OpDPdxFine, SPV_EXT_NONE)),
    INTRINSIC_FIXED(DDY_FINE, "ddy_fine", TypeMasks::FLOAT_TYPES, TypeMasks::FLOAT_TYPES, 0, 0, 0, IntrinsicFlags::FRAGMENT_ONLY, SPV_MAP(spv::OpDPdyFine, SPV_EXT_NONE)),
    INTRINSIC_FIXED(DDX_COARSE, "ddx_coarse", TypeMasks::FLOAT_TYPES, TypeMasks::FLOAT_TYPES, 0, 0, 0, IntrinsicFlags::FRAGMENT_ONLY, SPV_MAP(spv::OpDPdxCoarse, SPV_EXT_NONE)),
    INTRINSIC_FIXED(DDY_COARSE, "ddy_coarse", TypeMasks::FLOAT_TYPES, TypeMasks::FLOAT_TYPES, 0, 0, 0, IntrinsicFlags::FRAGMENT_ONLY, SPV_MAP(spv::OpDPdyCoarse, SPV_EXT_NONE)),
    INTRINSIC_FIXED(FWIDTH, "fwidth", TypeMasks::FLOAT_TYPES, TypeMasks::FLOAT_TYPES, 0, 0, 0, IntrinsicFlags::FRAGMENT_ONLY, SPV_MAP(spv::OpFwidth, SPV_EXT_NONE)),
    
    // Texture operations (SPIR-V ops handled specially based on sampling mode)
    TEXTURE_INTRINSIC(SAMPLE, "sample", 2, 6, 0),            // SpvOpImageSampleImplicitLod
    TEXTURE_INTRINSIC(SAMPLE_LOD, "sample_lod", 3, 6, 0),    // SpvOpImageSampleExplicitLod with Lod
    TEXTURE_INTRINSIC(SAMPLE_GRAD, "sample_grad", 4, 7, 0),  // SpvOpImageSampleExplicitLod with Grad
    TEXTURE_INTRINSIC(SAMPLE_BIAS, "sample_bias", 3, 5, IntrinsicFlags::FRAGMENT_ONLY), // SpvOpImageSampleImplicitLod with Bias
    TEXTURE_INTRINSIC(SAMPLE_CMP, "sample_cmp", 3, 5, 0),    // SpvOpImageSampleDrefImplicitLod
    TEXTURE_INTRINSIC(GATHER, "gather", 3, 4, 0),            // SpvOpImageGather
    TEXTURE_INTRINSIC(LOAD, "load", 3, 4, 0),                // SpvOpImageFetch
    INTRINSIC_FIXED(STORE, "store", mask(CoreType::VOID), mask(CoreType::CUSTOM), mask(CoreType::INT2), mask(CoreType::FLOAT4), 0, IntrinsicFlags::TEXTURE_OP | IntrinsicFlags::CUSTOM_HLSL, SPV_MAP(spv::OpImageWrite, SPV_EXT_NONE)),
    
    // Wave/SIMD (Subgroup operations - require SPIR-V 1.3+ and VK_KHR_shader_subgroup_*)
    // SPIR-V opcode values for GroupNonUniform ops (SPIR-V 1.3+):
    // OpGroupNonUniformFAdd = 350, OpGroupNonUniformFMul = 354, OpGroupNonUniformFMin = 356, etc.
    // Backend handles these specially when targeting Vulkan 1.1+
    INTRINSIC_FIXED(WAVE_ACTIVE_SUM, "wave_sum", TypeMasks::FLOAT_TYPES, TypeMasks::FLOAT_TYPES, 0, 0, 0, IntrinsicFlags::WAVE_OP, SPV_MAP(static_cast<spv::Op>(350), SPV_EXT_NONE)), // OpGroupNonUniformFAdd
    INTRINSIC_FIXED(WAVE_ACTIVE_PRODUCT, "wave_product", TypeMasks::FLOAT_TYPES, TypeMasks::FLOAT_TYPES, 0, 0, 0, IntrinsicFlags::WAVE_OP, SPV_MAP(static_cast<spv::Op>(354), SPV_EXT_NONE)), // OpGroupNonUniformFMul
    INTRINSIC_FIXED(WAVE_ACTIVE_MIN, "wave_min", TypeMasks::ANY_NUMERIC, TypeMasks::ANY_NUMERIC, 0, 0, 0, IntrinsicFlags::WAVE_OP, SPV_MAP(static_cast<spv::Op>(356), SPV_EXT_NONE)), // OpGroupNonUniformFMin
    INTRINSIC_FIXED(WAVE_ACTIVE_MAX, "wave_max", TypeMasks::ANY_NUMERIC, TypeMasks::ANY_NUMERIC, 0, 0, 0, IntrinsicFlags::WAVE_OP, SPV_MAP(static_cast<spv::Op>(357), SPV_EXT_NONE)), // OpGroupNonUniformFMax
    INTRINSIC_FIXED(WAVE_ACTIVE_ALL, "wave_all", mask(CoreType::BOOL), mask(CoreType::BOOL), 0, 0, 0, IntrinsicFlags::WAVE_OP, SPV_MAP(static_cast<spv::Op>(334), SPV_EXT_NONE)), // OpGroupNonUniformAll
    INTRINSIC_FIXED(WAVE_ACTIVE_ANY, "wave_any", mask(CoreType::BOOL), mask(CoreType::BOOL), 0, 0, 0, IntrinsicFlags::WAVE_OP, SPV_MAP(static_cast<spv::Op>(335), SPV_EXT_NONE)), // OpGroupNonUniformAny
    INTRINSIC_FIXED(WAVE_BROADCAST, "wave_broadcast", TypeMasks::ANY_NUMERIC, TypeMasks::ANY_NUMERIC, mask(CoreType::INT), 0, 0, IntrinsicFlags::WAVE_OP, SPV_MAP(static_cast<spv::Op>(337), SPV_EXT_NONE)), // OpGroupNonUniformBroadcast
    INTRINSIC_FIXED(WAVE_READ_FIRST, "wave_read_first", TypeMasks::ANY_NUMERIC, TypeMasks::ANY_NUMERIC, 0, 0, 0, IntrinsicFlags::WAVE_OP, SPV_MAP(static_cast<spv::Op>(339), SPV_EXT_NONE)), // OpGroupNonUniformBroadcastFirst
    
    // Atomics
    INTRINSIC_FIXED(ATOMIC_ADD, "atomic_add", mask(CoreType::INT), mask(CoreType::CUSTOM), mask(CoreType::INT), 0, 0, IntrinsicFlags::ATOMIC_OP, SPV_MAP(spv::OpAtomicIAdd, SPV_EXT_NONE)),
    INTRINSIC_FIXED(ATOMIC_MIN, "atomic_min", mask(CoreType::INT), mask(CoreType::CUSTOM), mask(CoreType::INT), 0, 0, IntrinsicFlags::ATOMIC_OP, SPV_MAP(spv::OpAtomicSMin, SPV_EXT_NONE)),
    INTRINSIC_FIXED(ATOMIC_MAX, "atomic_max", mask(CoreType::INT), mask(CoreType::CUSTOM), mask(CoreType::INT), 0, 0, IntrinsicFlags::ATOMIC_OP, SPV_MAP(spv::OpAtomicSMax, SPV_EXT_NONE)),
    INTRINSIC_FIXED(ATOMIC_AND, "atomic_and", mask(CoreType::INT), mask(CoreType::CUSTOM), mask(CoreType::INT), 0, 0, IntrinsicFlags::ATOMIC_OP, SPV_MAP(spv::OpAtomicAnd, SPV_EXT_NONE)),
    INTRINSIC_FIXED(ATOMIC_OR, "atomic_or", mask(CoreType::INT), mask(CoreType::CUSTOM), mask(CoreType::INT), 0, 0, IntrinsicFlags::ATOMIC_OP, SPV_MAP(spv::OpAtomicOr, SPV_EXT_NONE)),
    INTRINSIC_FIXED(ATOMIC_XOR, "atomic_xor", mask(CoreType::INT), mask(CoreType::CUSTOM), mask(CoreType::INT), 0, 0, IntrinsicFlags::ATOMIC_OP, SPV_MAP(spv::OpAtomicXor, SPV_EXT_NONE)),
    INTRINSIC_FIXED(ATOMIC_EXCHANGE, "atomic_exchange", mask(CoreType::INT), mask(CoreType::CUSTOM), mask(CoreType::INT), 0, 0, IntrinsicFlags::ATOMIC_OP, SPV_MAP(spv::OpAtomicExchange, SPV_EXT_NONE)),
    INTRINSIC_FIXED(ATOMIC_CMP_EXCHANGE, "atomic_cmp_exchange", mask(CoreType::INT), mask(CoreType::CUSTOM), mask(CoreType::INT), mask(CoreType::INT), 0, IntrinsicFlags::ATOMIC_OP | IntrinsicFlags::CUSTOM_METAL | IntrinsicFlags::CUSTOM_HLSL | IntrinsicFlags::CUSTOM_GLSL, SPV_MAP(spv::OpAtomicCompareExchange, SPV_EXT_NONE)),
    
    // Bit operations
    INTRINSIC_FIXED(COUNT_BITS, "count_bits", mask(CoreType::INT), mask(CoreType::INT) | mask(CoreType::UINT), 0, 0, 0, 0, SPV_MAP(spv::OpBitCount, SPV_EXT_NONE)),
    INTRINSIC_FIXED(REVERSE_BITS, "reverse_bits", mask(CoreType::INT) | mask(CoreType::UINT), mask(CoreType::INT) | mask(CoreType::UINT), 0, 0, 0, 0, SPV_MAP(spv::OpBitReverse, SPV_EXT_NONE)),
    INTRINSIC_FIXED(FIRST_BIT_LOW, "first_bit_low", mask(CoreType::INT), mask(CoreType::INT) | mask(CoreType::UINT), 0, 0, 0, 0, SPV_MAP(SPV_OP_NONE, GLSLstd450FindILsb)),
    INTRINSIC_FIXED(FIRST_BIT_HIGH, "first_bit_high", mask(CoreType::INT), mask(CoreType::INT) | mask(CoreType::UINT), 0, 0, 0, 0, SPV_MAP(SPV_OP_NONE, GLSLstd450FindSMsb)), // FindSMsb for signed, FindUMsb for unsigned

    // Control flow
    // select(false_val, true_val, condition) - returns true_val where condition is true, false_val otherwise
    INTRINSIC_FIXED(SELECT, "select", TypeMasks::ANY_NUMERIC, TypeMasks::ANY_NUMERIC, TypeMasks::ANY_NUMERIC, mask(CoreType::BOOL), 0, 0, SPV_MAP(spv::OpSelect, SPV_EXT_NONE)),
};
#undef INTRINSIC_ENTRY

// Custom implementations for special cases
struct CustomImpl {
    Intrinsic id;
    const char* metal;
    const char* hlsl;
    const char* glsl;
};

constexpr CustomImpl CUSTOM_IMPLS[] = {
    {Intrinsic::SATURATE, nullptr, nullptr, 
     "clamp(%s, 0.0, 1.0)"},
    
    {Intrinsic::SINCOS, 
     "float2 __sc = sincos(%s); %s = __sc.x; %s = __sc.y;",
     nullptr,  // HLSL has native sincos
     "%s = sin(%s); %s = cos(%s);"},
    
    {Intrinsic::INVERSE,
     "inverse(%s)",  // Metal has native inverse
     "transpose(adjugate(%s)) / determinant(%s)",  // HLSL workaround
     "inverse(%s)"},  // GLSL has native
     
    {Intrinsic::STORE,
     "%s.write(%s, %s)",
     "%s[%s] = %s",  // HLSL direct array access
     "imageStore(%s, %s, %s)"},
     
    {Intrinsic::ATOMIC_CMP_EXCHANGE,
     "atomic_compare_exchange_weak_explicit(&%s, &%s, %s, memory_order_relaxed, memory_order_relaxed)",
     "InterlockedCompareExchange(%s, %s, %s, %s)",
     "atomicCompSwap(%s, %s, %s)"},
};


inline const char* GetMetalName(const IntrinsicData* data) {
    return BACKEND_NAMES[data->enumIndex].metal;
}

inline const char* GetHLSLName(const IntrinsicData* data) {
    return BACKEND_NAMES[data->enumIndex].hlsl;
}

inline const char* GetGLSLName(const IntrinsicData* data) {
    return BACKEND_NAMES[data->enumIndex].glsl;
}

inline const char* GetCustomImpl(u8 enumIndex, BackendType backend) {
    Intrinsic id = static_cast<Intrinsic>(enumIndex);
    
    for (const auto& impl : CUSTOM_IMPLS) {
        if (impl.id == id) {
            switch (backend) {
                case BackendType::Metal: return impl.metal;
                case BackendType::HLSL:  return impl.hlsl;
                case BackendType::GLSL:  return impl.glsl;
                default: return nullptr;
            }
        }
    }
    return nullptr;
}

inline bool IsValidForStage(const IntrinsicData* data, ShaderStage stage) {
    if (data->flags & IntrinsicFlags::FRAGMENT_ONLY) {
        return stage == ShaderStage::Fragment;
    }
    if (data->flags & IntrinsicFlags::VERTEX_ONLY) {
        return stage == ShaderStage::Vertex;
    }
    return true;
}

// Pre-computed hashes for hot paths
namespace PrecomputedHashes {
    constexpr u32 LERP = Utils::HashStr("lerp");
    constexpr u32 SATURATE = Utils::HashStr("saturate");
    constexpr u32 NORMALIZE = Utils::HashStr("normalize");
    constexpr u32 DOT = Utils::HashStr("dot");
    constexpr u32 SAMPLE = Utils::HashStr("sample");
    constexpr u32 DDX = Utils::HashStr("ddx");
    constexpr u32 DDY = Utils::HashStr("ddy");
}

namespace IntrinsicLookup {
    inline const IntrinsicData* Find(u32 nameHash) {
        // Binary search would be ideal if INTRINSICS was sorted by hash
        for (const auto& intrinsic : INTRINSICS) {
            if (intrinsic.nameHash == nameHash) {
                return &intrinsic;
            }
        }
        return nullptr;
    }
    
    inline const char* GetMetalName(const IntrinsicData* data) {
        return BACKEND_NAMES[data->enumIndex].metal;
    }
    
    inline bool IsValidForStage(const IntrinsicData* data, ShaderStage stage) {
        return StdLib::IsValidForStage(data, stage);
    }
    
    inline const char* GetCustomImpl(u8 enumIndex, BackendType backend) {
        return StdLib::GetCustomImpl(enumIndex, backend);
    }
}

} // namespace StdLib
} // namespace BWSL