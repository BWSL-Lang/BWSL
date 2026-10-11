// Part of bwsl_spirv_backend.cpp. Include from that file only.
// Static IR-to-SPIR-V and GLSL.std.450 lookup tables.
#pragma once
#include "bwsl_spirv_backend.cpp"

// ============= Static lookup tables =============
// This table maps BWSL IR Opcodes to core SPIR-V opcodes.
// When an opcode maps to `spv::OpExtInst`, it indicates that the instruction
// belongs to an extended instruction set (like GLSL.std.450).
// The specific extended instruction must then be looked up in the parallel
// table.

namespace BWSL {

constexpr std::array<spv::Op, 256> BuildIrToSpvOpTable() {
    std::array<spv::Op, 256> table{};
    for (auto& entry : table) entry = spv::OpNop;

    // ========== Control Flow ==========
    table[IR::OP_NOP] = spv::OpNop;
    table[IR::OP_JUMP] = spv::OpBranch;
    table[IR::OP_BRANCH] = spv::OpBranchConditional;
    table[IR::OP_CALL] = spv::OpFunctionCall;
    table[IR::OP_RET] = spv::OpReturn; // Can also be spv::OpReturnValue
    table[IR::OP_SELECT] = spv::OpSelect;
    table[IR::OP_PHI] = spv::OpPhi;
    table[IR::OP_SWITCH] = spv::OpSwitch;

    // ========== Memory Operations ==========
    table[IR::OP_LOAD_CONST] = spv::OpConstant;
    table[IR::OP_LOAD_REG] = spv::OpCopyObject;
    table[IR::OP_STORE_REG] = spv::OpStore;
    table[IR::OP_LOAD_ATTR] = spv::OpLoad;
    table[IR::OP_STORE_OUTPUT] = spv::OpStore;
    table[IR::OP_LOAD_OUTPUT] = spv::OpLoad;
    table[IR::OP_LOAD_UNIFORM] = spv::OpLoad;
    table[IR::OP_LOAD_BUFFER] = spv::OpLoad;
    table[IR::OP_STORE_BUFFER] = spv::OpStore;
    table[IR::OP_LOAD_LOCAL] = spv::OpLoad;
    table[IR::OP_STORE_LOCAL] = spv::OpStore;
    table[IR::OP_LOAD_SHARED] = spv::OpLoad;
    table[IR::OP_STORE_SHARED] = spv::OpStore;
    table[IR::OP_LOAD_INPUT] = spv::OpLoad;

    // ========== Arithmetic (Float) ==========
    table[IR::OP_FADD] = spv::OpFAdd;
    table[IR::OP_FSUB] = spv::OpFSub;
    table[IR::OP_FMUL] = spv::OpFMul;
    table[IR::OP_FDIV] = spv::OpFDiv;
    table[IR::OP_FMOD] = spv::OpFMod;
    table[IR::OP_FREM] = spv::OpFRem;
    table[IR::OP_FNEG] = spv::OpFNegate;
    table[IR::OP_FABS] = spv::OpExtInst;
    table[IR::OP_FMIN] = spv::OpExtInst;
    table[IR::OP_FMAX] = spv::OpExtInst;
    table[IR::OP_FCLAMP] = spv::OpExtInst;
    table[IR::OP_FLOOR] = spv::OpExtInst;
    table[IR::OP_CEIL] = spv::OpExtInst;
    table[IR::OP_ROUND] = spv::OpExtInst;
    table[IR::OP_TRUNC] = spv::OpExtInst;
    table[IR::OP_FRACT] = spv::OpExtInst;
    table[IR::OP_FMA] = spv::OpExtInst;

    // ========== Arithmetic (Integer) ==========
    table[IR::OP_IADD] = spv::OpIAdd;
    table[IR::OP_ISUB] = spv::OpISub;
    table[IR::OP_IMUL] = spv::OpIMul;
    table[IR::OP_IDIV] = spv::OpSDiv;
    // Match integer division's truncation toward zero and compile-time `%`.
    table[IR::OP_IMOD] = spv::OpSRem;
    table[IR::OP_INEG] = spv::OpSNegate;
    table[IR::OP_IABS] = spv::OpExtInst;
    table[IR::OP_IMIN] = spv::OpExtInst;
    table[IR::OP_IMAX] = spv::OpExtInst;
    table[IR::OP_ICLAMP] = spv::OpExtInst;
    table[IR::OP_UMIN] = spv::OpExtInst;
    table[IR::OP_UMAX] = spv::OpExtInst;
    table[IR::OP_UCLAMP] = spv::OpExtInst;

    // ========== Bitwise ==========
    table[IR::OP_CLZ] = spv::OpExtInst;
    table[IR::OP_CTZ] = spv::OpExtInst;
    table[IR::OP_BITFIELD_EXTRACT] = spv::OpBitFieldSExtract;
    table[IR::OP_BITFIELD_INSERT] = spv::OpBitFieldInsert;
    table[IR::OP_PACK_UNORM4X8] = spv::OpExtInst;
    table[IR::OP_UNPACK_UNORM4X8] = spv::OpExtInst;
    table[IR::OP_PACK_SNORM4X8] = spv::OpExtInst;

    // ========== Type Conversion ==========
    table[IR::OP_SIGN] = spv::OpExtInst;

    // ========== Math Functions ==========
    table[IR::OP_SQRT] = spv::OpExtInst;
    table[IR::OP_RSQRT] = spv::OpExtInst;
    table[IR::OP_POW] = spv::OpExtInst;
    table[IR::OP_EXP] = spv::OpExtInst;
    table[IR::OP_EXP2] = spv::OpExtInst;
    table[IR::OP_LOG] = spv::OpExtInst;
    table[IR::OP_LOG2] = spv::OpExtInst;
    table[IR::OP_LDEXP] = spv::OpExtInst;
    table[IR::OP_SIN] = spv::OpExtInst;
    table[IR::OP_COS] = spv::OpExtInst;
    table[IR::OP_TAN] = spv::OpExtInst;
    table[IR::OP_ASIN] = spv::OpExtInst;
    table[IR::OP_ACOS] = spv::OpExtInst;
    table[IR::OP_ATAN] = spv::OpExtInst;
    table[IR::OP_ATAN2] = spv::OpExtInst;
    table[IR::OP_SINH] = spv::OpExtInst;
    table[IR::OP_COSH] = spv::OpExtInst;
    table[IR::OP_TANH] = spv::OpExtInst;
    table[IR::OP_PACK_UNORM2X16] = spv::OpExtInst;
    table[IR::OP_UNPACK_UNORM2X16] = spv::OpExtInst;
    table[IR::OP_PACK_SNORM2X16] = spv::OpExtInst;
    table[IR::OP_UNPACK_SNORM2X16] = spv::OpExtInst;
    table[IR::OP_UNPACK_SNORM4X8] = spv::OpExtInst;
    table[IR::OP_PACK_HALF2X16] = spv::OpExtInst;
    table[IR::OP_UNPACK_HALF2X16] = spv::OpExtInst;
    table[IR::OP_ISNORMAL] = spv::OpNop; // Emitted portably for Shader capability
    table[IR::OP_MODF_STRUCT] = spv::OpExtInst;
    table[IR::OP_FREXP_STRUCT] = spv::OpExtInst;

    // ========== Geometric ==========
    table[IR::OP_DOT] = spv::OpDot;
    table[IR::OP_CROSS] = spv::OpExtInst;
    table[IR::OP_LENGTH] = spv::OpExtInst;
    table[IR::OP_NORMALIZE] = spv::OpExtInst;
    table[IR::OP_DISTANCE] = spv::OpExtInst;
    table[IR::OP_REFLECT] = spv::OpExtInst;
    table[IR::OP_REFRACT] = spv::OpExtInst;
    table[IR::OP_FACEFORWARD] = spv::OpExtInst;

    // ========== Matrix ==========
    table[IR::OP_MAT_MUL] = spv::OpMatrixTimesMatrix;
    table[IR::OP_MAT_TRANSPOSE] = spv::OpTranspose;
    table[IR::OP_MAT_INVERSE] = spv::OpExtInst;
    table[IR::OP_MAT_DET] = spv::OpExtInst;
    table[IR::OP_MAT_CONSTRUCT] = spv::OpCompositeConstruct;
    table[IR::OP_MAT_VEC_MUL] = spv::OpMatrixTimesVector;
    table[IR::OP_VEC_MAT_MUL] = spv::OpVectorTimesMatrix;
    table[IR::OP_MAT_SCALE] = spv::OpMatrixTimesScalar;

    // ========== Interpolation ==========
    table[IR::OP_LERP] = spv::OpExtInst;
    table[IR::OP_SMOOTHSTEP] = spv::OpExtInst;
    table[IR::OP_STEP] = spv::OpExtInst;
    table[IR::OP_SATURATE] = spv::OpExtInst;
    table[IR::OP_DEGREES] = spv::OpExtInst;
    table[IR::OP_RADIANS] = spv::OpExtInst;

    // ========== Atomics ==========
    table[IR::OP_ATOMIC_ADD] = spv::OpAtomicIAdd;
    table[IR::OP_ATOMIC_SUB] = spv::OpAtomicISub;
    table[IR::OP_ATOMIC_MIN] = spv::OpAtomicSMin;
    table[IR::OP_ATOMIC_MAX] = spv::OpAtomicSMax;
    table[IR::OP_ATOMIC_AND] = spv::OpAtomicAnd;
    table[IR::OP_ATOMIC_OR] = spv::OpAtomicOr;
    table[IR::OP_ATOMIC_XOR] = spv::OpAtomicXor;
    table[IR::OP_ATOMIC_XCHG] = spv::OpAtomicExchange;
    table[IR::OP_ATOMIC_CMP_XCHG] = spv::OpAtomicCompareExchange;

    // ========== Synchronization ==========
    table[IR::OP_BARRIER] = spv::OpControlBarrier;
    table[IR::OP_MEM_FENCE] = spv::OpMemoryBarrier;

    // ========== Derivatives ==========
    table[IR::OP_DDX] = spv::OpDPdx;
    table[IR::OP_DDY] = spv::OpDPdy;
    table[IR::OP_DDX_FINE] = spv::OpDPdxFine;
    table[IR::OP_DDY_FINE] = spv::OpDPdyFine;
    table[IR::OP_DDX_COARSE] = spv::OpDPdxCoarse;
    table[IR::OP_DDY_COARSE] = spv::OpDPdyCoarse;
    table[IR::OP_FWIDTH] = spv::OpFwidth;
    table[IR::OP_FWIDTH_FINE] = spv::OpFwidthFine;
    table[IR::OP_FWIDTH_COARSE] = spv::OpFwidthCoarse;

    // ========== Boolean reductions ==========
    table[IR::OP_ANY] = spv::OpAny;
    table[IR::OP_ALL] = spv::OpAll;

    // ========== Float classification ==========
    table[IR::OP_ISNAN] = spv::OpIsNan;
    table[IR::OP_ISINF] = spv::OpIsInf;
    table[IR::OP_ISFINITE] = spv::OpNop; // Emitted as !isnan(x) && !isinf(x)

    // ========== Wave/SIMD/Subgroup Operations ==========
    table[IR::OP_WAVE_MIN] = static_cast<spv::Op>(358);
    table[IR::OP_WAVE_MAX] = static_cast<spv::Op>(359);
    table[IR::OP_WAVE_ALL] = static_cast<spv::Op>(334);
    table[IR::OP_WAVE_ANY] = static_cast<spv::Op>(335);
    table[IR::OP_WAVE_BALLOT] = static_cast<spv::Op>(333);
    table[IR::OP_WAVE_READ_FIRST] = static_cast<spv::Op>(338);
    table[IR::OP_WAVE_READ_LANE] = static_cast<spv::Op>(337);
    table[IR::OP_WAVE_SUM] = static_cast<spv::Op>(349);
    table[IR::OP_WAVE_MUL] = static_cast<spv::Op>(353);

    return table;
}

static constexpr auto IR_TO_SPV_OP_TABLE = BuildIrToSpvOpTable();

// A sentinel value to indicate that an IR OpCode does not map to a
// GLSL.std.450 extended instruction.
constexpr u32 NO_EXT_INST = 0xFFFFFFFF;

// This table maps BWSL IR Opcodes to their corresponding GLSL.std.450 enum
// value. It should only be accessed if the IR_TO_SPV_OP_TABLE entry for the
// same opcode is `spv::OpExtInst`.
constexpr std::array<u32, 256> BuildIrToGlslStd450Table() {
    std::array<u32, 256> table{};
    for (auto& entry : table) entry = NO_EXT_INST;

    // ========== Arithmetic (Float) ==========
    table[IR::OP_FABS] = GLSLstd450FAbs;
    table[IR::OP_FMIN] = GLSLstd450FMin;
    table[IR::OP_FMAX] = GLSLstd450FMax;
    table[IR::OP_FCLAMP] = GLSLstd450FClamp;
    table[IR::OP_FLOOR] = GLSLstd450Floor;
    table[IR::OP_CEIL] = GLSLstd450Ceil;
    table[IR::OP_ROUND] = GLSLstd450RoundEven;
    table[IR::OP_TRUNC] = GLSLstd450Trunc;
    table[IR::OP_FRACT] = GLSLstd450Fract;
    table[IR::OP_FMA] = GLSLstd450Fma;

    // ========== Arithmetic (Integer) ==========
    table[IR::OP_IABS] = GLSLstd450SAbs;
    table[IR::OP_IMIN] = GLSLstd450SMin;
    table[IR::OP_IMAX] = GLSLstd450SMax;
    table[IR::OP_ICLAMP] = GLSLstd450SClamp;
    table[IR::OP_UMIN] = GLSLstd450UMin;
    table[IR::OP_UMAX] = GLSLstd450UMax;
    table[IR::OP_UCLAMP] = GLSLstd450UClamp;

    // ========== Bitwise ==========
    table[IR::OP_CLZ] = GLSLstd450FindUMsb;
    table[IR::OP_CTZ] = GLSLstd450FindILsb;
    table[IR::OP_PACK_UNORM2X16] = GLSLstd450PackUnorm2x16;
    table[IR::OP_UNPACK_UNORM2X16] = GLSLstd450UnpackUnorm2x16;
    table[IR::OP_PACK_UNORM4X8] = GLSLstd450PackUnorm4x8;
    table[IR::OP_UNPACK_UNORM4X8] = GLSLstd450UnpackUnorm4x8;
    table[IR::OP_PACK_SNORM2X16] = GLSLstd450PackSnorm2x16;
    table[IR::OP_UNPACK_SNORM2X16] = GLSLstd450UnpackSnorm2x16;
    table[IR::OP_PACK_SNORM4X8] = GLSLstd450PackSnorm4x8;

    // ========== Type Conversion ==========
    table[IR::OP_SIGN] = GLSLstd450FSign;

    // ========== Math Functions ==========
    table[IR::OP_SQRT] = GLSLstd450Sqrt;
    table[IR::OP_RSQRT] = GLSLstd450InverseSqrt;
    table[IR::OP_POW] = GLSLstd450Pow;
    table[IR::OP_EXP] = GLSLstd450Exp;
    table[IR::OP_EXP2] = GLSLstd450Exp2;
    table[IR::OP_LOG] = GLSLstd450Log;
    table[IR::OP_LOG2] = GLSLstd450Log2;
    table[IR::OP_LDEXP] = GLSLstd450Ldexp;
    table[IR::OP_SIN] = GLSLstd450Sin;
    table[IR::OP_COS] = GLSLstd450Cos;
    table[IR::OP_TAN] = GLSLstd450Tan;
    table[IR::OP_ASIN] = GLSLstd450Asin;
    table[IR::OP_ACOS] = GLSLstd450Acos;
    table[IR::OP_ATAN] = GLSLstd450Atan;
    table[IR::OP_ATAN2] = GLSLstd450Atan2;
    table[IR::OP_SINH] = GLSLstd450Sinh;
    table[IR::OP_COSH] = GLSLstd450Cosh;
    table[IR::OP_TANH] = GLSLstd450Tanh;
    table[IR::OP_UNPACK_SNORM4X8] = GLSLstd450UnpackSnorm4x8;
    table[IR::OP_PACK_HALF2X16] = GLSLstd450PackHalf2x16;
    table[IR::OP_UNPACK_HALF2X16] = GLSLstd450UnpackHalf2x16;
    table[IR::OP_MODF_STRUCT] = GLSLstd450ModfStruct;
    table[IR::OP_FREXP_STRUCT] = GLSLstd450FrexpStruct;

    // ========== Geometric ==========
    table[IR::OP_CROSS] = GLSLstd450Cross;
    table[IR::OP_LENGTH] = GLSLstd450Length;
    table[IR::OP_NORMALIZE] = GLSLstd450Normalize;
    table[IR::OP_DISTANCE] = GLSLstd450Distance;
    table[IR::OP_REFLECT] = GLSLstd450Reflect;
    table[IR::OP_REFRACT] = GLSLstd450Refract;
    table[IR::OP_FACEFORWARD] = GLSLstd450FaceForward;

    // ========== Matrix ==========
    table[IR::OP_MAT_INVERSE] = GLSLstd450MatrixInverse;
    table[IR::OP_MAT_DET] = GLSLstd450Determinant;

    // ========== Interpolation ==========
    table[IR::OP_LERP] = GLSLstd450FMix;
    table[IR::OP_SMOOTHSTEP] = GLSLstd450SmoothStep;
    table[IR::OP_STEP] = GLSLstd450Step;
    table[IR::OP_SATURATE] = GLSLstd450FClamp;
    table[IR::OP_DEGREES] = GLSLstd450Degrees;
    table[IR::OP_RADIANS] = GLSLstd450Radians;

    return table;
}

static constexpr auto IR_TO_GLSL_STD_450_TABLE = BuildIrToGlslStd450Table();

// Words a GLSL local may not be named: keywords, reserved words, types and
// built-in functions. Copied from CompilerGLSL::replace_illegal_names() in
// SPIRV-Cross, which applies it to variables, functions and types but not to
// OpNames on plain result IDs. Sorted for binary search.
static constexpr const char* GLSL_RESERVED_NAMES[] = {
    "EmitStreamVertex", "EmitVertex", "EndPrimitive", "EndStreamPrimitive",
    "abs", "acos", "acosh", "active", "all", "any", "asin", "asinh", "asm",
    "atan", "atanh", "atomicAdd", "atomicCompSwap", "atomicCounter",
    "atomicCounterDecrement", "atomicCounterIncrement", "atomicExchange",
    "atomicMax", "atomicMin", "atomicOr", "atomicXor", "atomic_uint",
    "attribute", "bitCount", "bitfieldExtract", "bitfieldInsert",
    "bitfieldReverse", "bool", "break", "buffer", "bvec2", "bvec3", "bvec4",
    "case", "cast", "ceil", "centroid", "class", "coherent", "common", "const",
    "continue", "cos", "cosh", "cross", "dFdx", "dFdxCoarse", "dFdxFine",
    "dFdy", "dFdyCoarse", "dFdyFine", "default", "degrees", "discard",
    "distance", "dmat2", "dmat2x2", "dmat2x3", "dmat2x4", "dmat3", "dmat3x2",
    "dmat3x3", "dmat3x4", "dmat4", "dmat4x2", "dmat4x3", "dmat4x4", "do",
    "dot", "double", "dvec2", "dvec3", "dvec4", "else", "enum", "equal", "exp",
    "exp2", "extern", "external", "faceforward", "false", "filter", "findLSB",
    "findMSB", "fixed", "flat", "float", "float16BitsToInt16",
    "float16BitsToUint16", "floatBitsToInt", "floatBitsToUint", "floor", "fma",
    "for", "fract", "frexp", "fvec2", "fvec3", "fvec4", "fwidth",
    "fwidthCoarse", "fwidthFine", "goto", "greaterThan", "greaterThanEqual",
    "groupMemoryBarrier", "half", "highp", "hvec2", "hvec3", "hvec4", "if",
    "iimage1D", "iimage1DArray", "iimage2D", "iimage2DArray", "iimage2DMS",
    "iimage2DMSArray", "iimage2DRect", "iimage3D", "iimageBuffer",
    "iimageCube", "iimageCubeArray", "image1D", "image1DArray", "image2D",
    "image2DArray", "image2DMS", "image2DMSArray", "image2DRect", "image3D",
    "imageAtomicAdd", "imageAtomicAnd", "imageAtomicCompSwap",
    "imageAtomicExchange", "imageAtomicMax", "imageAtomicMin", "imageAtomicOr",
    "imageAtomicXor", "imageBuffer", "imageCube", "imageCubeArray",
    "imageLoad", "imageSamples", "imageSize", "imageStore", "imulExtended",
    "in", "inline", "inout", "input", "int", "int16BitsToFloat16",
    "intBitsToFloat", "interface", "interpolateAtCentroid",
    "interpolateAtOffset", "interpolateAtSample", "invariant", "inverse",
    "inversesqrt", "isampler1D", "isampler1DArray", "isampler2D",
    "isampler2DArray", "isampler2DMS", "isampler2DMSArray", "isampler2DRect",
    "isampler3D", "isamplerBuffer", "isamplerCube", "isamplerCubeArray",
    "isinf", "isnan", "ivec2", "ivec3", "ivec4", "layout", "ldexp", "length",
    "lessThan", "lessThanEqual", "log", "log2", "long", "lowp", "mat2",
    "mat2x2", "mat2x3", "mat2x4", "mat3", "mat3x2", "mat3x3", "mat3x4", "mat4",
    "mat4x2", "mat4x3", "mat4x4", "matrixCompMult", "max", "mediump",
    "memoryBarrier", "memoryBarrierAtomicCounter", "memoryBarrierBuffer",
    "memoryBarrierImage", "memoryBarrierShared", "min", "mix", "mod", "modf",
    "namespace", "noinline", "noise", "noise1", "noise2", "noise3", "noise4",
    "noperspective", "normalize", "not", "notEqual", "out", "outerProduct",
    "output", "packDouble2x32", "packHalf2x16", "packInt2x16", "packInt4x16",
    "packSnorm2x16", "packSnorm4x8", "packUint2x16", "packUint4x16",
    "packUnorm2x16", "packUnorm4x8", "packed", "partition", "patch", "pow",
    "precise", "precision", "public", "radians", "readonly", "reflect",
    "refract", "resource", "restrict", "return", "round", "roundEven",
    "sample", "sampler1D", "sampler1DArray", "sampler1DArrayShadow",
    "sampler1DShadow", "sampler2D", "sampler2DArray", "sampler2DArrayShadow",
    "sampler2DMS", "sampler2DMSArray", "sampler2DRect", "sampler2DRectShadow",
    "sampler2DShadow", "sampler3D", "sampler3DRect", "samplerBuffer",
    "samplerCube", "samplerCubeArray", "samplerCubeArrayShadow",
    "samplerCubeShadow", "shared", "short", "sign", "sin", "sinh", "sizeof",
    "smooth", "smoothstep", "sqrt", "static", "step", "struct", "subroutine",
    "superp", "switch", "tan", "tanh", "template", "texelFetch",
    "texelFetchOffset", "texture", "textureGather", "textureGatherOffset",
    "textureGatherOffsets", "textureGrad", "textureGradOffset", "textureLod",
    "textureLodOffset", "textureOffset", "textureProj", "textureProjGrad",
    "textureProjGradOffset", "textureProjLod", "textureProjLodOffset",
    "textureProjOffset", "textureQueryLevels", "textureQueryLod",
    "textureSamples", "textureSize", "this", "transpose", "true", "trunc",
    "typedef", "uaddCarry", "uimage1D", "uimage1DArray", "uimage2D",
    "uimage2DArray", "uimage2DMS", "uimage2DMSArray", "uimage2DRect",
    "uimage3D", "uimageBuffer", "uimageCube", "uimageCubeArray", "uint",
    "uint16BitsToFloat16", "uintBitsToFloat", "umulExtended", "uniform",
    "union", "unpackDouble2x32", "unpackHalf2x16", "unpackInt2x16",
    "unpackInt4x16", "unpackSnorm2x16", "unpackSnorm4x8", "unpackUint2x16",
    "unpackUint4x16", "unpackUnorm2x16", "unpackUnorm4x8", "unsigned",
    "usampler1D", "usampler1DArray", "usampler2D", "usampler2DArray",
    "usampler2DMS", "usampler2DMSArray", "usampler2DRect", "usampler3D",
    "usamplerBuffer", "usamplerCube", "usamplerCubeArray", "using",
    "usubBorrow", "uvec2", "uvec3", "uvec4", "varying", "vec2", "vec3", "vec4",
    "void", "volatile", "while", "writeonly",
};

constexpr bool IsSortedNameTable(const char* const* names, u32 count) {
    for (u32 i = 1; i < count; i++) {
        const char* a = names[i - 1];
        const char* b = names[i];
        while (*a && *a == *b) { a++; b++; }
        if (static_cast<unsigned char>(*a) >= static_cast<unsigned char>(*b)) return false;
    }
    return true;
}
static_assert(IsSortedNameTable(GLSL_RESERVED_NAMES, std::size(GLSL_RESERVED_NAMES)),
              "GLSL_RESERVED_NAMES must stay sorted and unique");

static bool IsGlslReservedName(const char* name) {
    return std::binary_search(std::begin(GLSL_RESERVED_NAMES), std::end(GLSL_RESERVED_NAMES), name,
                              [](const char* a, const char* b) { return strcmp(a, b) < 0; });
}

// ============= Initialization =============


} // namespace BWSL
