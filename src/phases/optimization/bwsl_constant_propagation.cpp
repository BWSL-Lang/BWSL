#include "bwsl_constant_propagation.h"
#include "bwsl_ir_edit.h"
#include "bwsl_cse.h"
#include "core/bwsl_arena.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <vector>

namespace BWSL {
namespace Optimization {

using IR::IRProgram;

// ============= Lattice =============

enum class ValueState : u8 {
    UNDEFINED,    // Not evaluated yet, or defined only by code that never runs
    CONSTANT,
    OVERDEFINED,  // Varies at run time, or isn't a value this pass folds
};

// A mat4 has the most components. Matrices are stored column by column.
constexpr u32 MAX_COMPONENTS = 16;

// A vector or matrix can be overdefined as a whole and still have constant
// components: `known` marks them, and their bits hold the constants.
// Everything that needs the whole value treats it as overdefined; what
// works component by component (construction, swizzles, component access,
// componentwise arithmetic, select and phis) keeps the known components.
struct ConstantValue {
    ValueState state;
    CoreType type;
    u8 count;                   // Components in bits: 1 for a scalar
    u32 bits[MAX_COMPONENTS];   // Per component: float bits, two's complement integer, or 0/1 for bool
    u16 known;                  // Overdefined only: components that are constants
};

constexpr ConstantValue UNDEFINED_VALUE = {ValueState::UNDEFINED, CoreType::INVALID, 0, {}, 0};
constexpr ConstantValue OVERDEFINED_VALUE = {ValueState::OVERDEFINED, CoreType::INVALID, 0, {}, 0};

// A component's possible values (see bwsl_constant_propagation_ranges.inl):
// from low to high, each end reached unless it's exclusive, and NaN or not.
// Integers keep whole, reached ends. Without KNOWN, nothing has reached it yet.
struct ValueRange {
    f64 low;
    f64 high;
    u8 flags;

    static constexpr u8 KNOWN = 1;
    static constexpr u8 LOW_EXCLUSIVE = 2;   // value > low rather than >= low
    static constexpr u8 HIGH_EXCLUSIVE = 4;  // value < high rather than <= high
    static constexpr u8 MAY_BE_NAN = 8;
};

// `reg op other` holds (or, with !holds, fails) in a block and the blocks
// it dominates, linked to the dominator's constraints through parent.
struct RangeConstraint {
    u16 reg;
    u16 op;
    u16 other;
    bool holds;
    u32 parent;
};

// Marks a definitions[] or use-list entry that names a phi, not an instruction.
constexpr u32 PHI_REFERENCE = 0x80000000;
constexpr u32 NO_DEFINITION = 0xFFFFFFFF;

struct ConstantPropagationData {
    IRProgram* ir;
    IR::IRBuilder* builder;
    CFG* cfg;
    u32 regCount;
    bool straightLine;      // One forward walk; definitions[] is the one reaching the walk

    ValueState* states;     // [regCount]
    u32* bitOffsets;        // [regCount + 1] where a register's components start in bits
    u32* bits;              // [bitOffsets[regCount]]
    u16* known;             // [regCount] an overdefined register's constant components
    bool* tracked;          // [regCount] single-definition scalar this pass may fold
    bool* undef;            // [regCount] SSA undef register ("not assigned on this path")
    u32* definitions;       // [regCount] instruction, PHI_REFERENCE | phi, or NO_DEFINITION
    u8* definitionCounts;   // [regCount] saturates at 2

    u32* useOffsets;        // [regCount + 1] CSR: instructions and phis reading a register
    u32* useData;

    bool* blockExecutable;  // [blockCount]
    bool* edgeExecutable;   // [predecessorDataCount], parallel to cfg->predecessorData
    u32* blockPhiOffsets;   // [blockCount + 1] CSR: phis by block
    u32* blockPhis;
    bool* phiRemoved;       // [phiCount] phis left with no incoming edge

    u32* blockWork;         // A block is queued once, when it first becomes executable
    u32 blockWorkHead;
    u32 blockWorkTail;
    u32* regWork;           // A register is queued each time it drops a level: at most once
                            // per component and once more
    u32 regWorkHead;
    u32 regWorkTail;

    // Loops evaluated at compile time (see FoldLoop).
    bool* pinned;           // [regCount] header phi fixed to its value on the header's last visit
    u32* pinnedBits;        // [bitOffsets[regCount]]
    u16* pinnedPhiOperand;  // [phiCount] what a pinned phi takes on every edge, else 0xFFFF
    bool* readAnywhere;     // [regCount] some instruction or phi reads the register
    bool* loopFolded;       // [blockCount] header of a loop already evaluated
    bool* inLoop;           // [blockCount] scratch: the loop being evaluated
    u32* blockStack;        // [blockCount] scratch
    ValueState* savedStates;  // [regCount] scratch: the lattice around an evaluation
    u32* savedBits;           // [bitOffsets[regCount]]
    u16* savedKnown;          // [regCount]

    // Instructions rewrites add, applied once the pass is done with
    // instruction indices.
    InstructionInsertions insertions;

    // Value ranges.
    ValueRange* ranges;         // [bitOffsets[regCount]]
    u8* rangeChanges;           // [regCount] times a range grew, for widening
    u32* blockConstraints;      // [blockCount] the block's last constraint, NO_DEFINITION for none
    RangeConstraint* constraints;
    u32 constraintCount;
    u32 constraintCapacity;
    bool rangesReady;           // ranges hold a settled result (or, straight-line, the walk's)
    mutable bool sawUnknownRange;  // An evaluation read a range not known yet
    // What the stage stores to each varying, by varying index: the hull of
    // its stores' ranges, for ir->varyingBoundsOut.
    ValueRange varyingRanges[16 * 4];
    u8 varyingCounts[16];       // Components stored, 0 for a varying not stored
    OptimizationSet optimizations;
};

static bool IsEnabled(const ConstantPropagationData* data, OptimizationId id) {
    return Optimizations::IsEnabled(data->optimizations, id);
}

static ConstantValue MakeConstant(CoreType type, u32 bits) {
    ConstantValue value = {ValueState::CONSTANT, type, 1, {}, 0};
    value.bits[0] = bits;
    return value;
}

static bool HasSameBits(const ConstantValue& a, const ConstantValue& b) {
    return a.count == b.count && memcmp(a.bits, b.bits, a.count * sizeof(u32)) == 0;
}

static u16 FullMask(u32 count) {
    return static_cast<u16>(count >= MAX_COMPONENTS ? 0xFFFF : (1u << count) - 1);
}

// The components whose value is known: all of a constant's, none of an
// undefined value's.
static u16 KnownMask(const ConstantValue& value) {
    if (value.state == ValueState::CONSTANT) return FullMask(value.count);
    return value.state == ValueState::OVERDEFINED ? value.known : 0;
}

// A value with the `known` components of `bits`: a constant when that's all
// of them, overdefined when it's none.
static ConstantValue MakePartial(CoreType type, u32 count, u16 known, const u32* bits) {
    known &= FullMask(count);
    if (known == 0 || count == 0) return OVERDEFINED_VALUE;
    ConstantValue value = {known == FullMask(count) ? ValueState::CONSTANT : ValueState::OVERDEFINED,
                           type, static_cast<u8>(count), {}, 0};
    for (u32 c = 0; c < count; c++) {
        if (known & (1u << c)) value.bits[c] = bits[c];
    }
    if (value.state == ValueState::OVERDEFINED) value.known = known;
    return value;
}

// Components stay known where both values know the same constant.
static ConstantValue Meet(const ConstantValue& a, const ConstantValue& b) {
    if (a.state == ValueState::UNDEFINED) return b;
    if (b.state == ValueState::UNDEFINED) return a;
    u16 known = KnownMask(a) & KnownMask(b);
    if (known == 0 || a.type != b.type || a.count != b.count) return OVERDEFINED_VALUE;
    for (u32 c = 0; c < a.count; c++) {
        if (a.bits[c] != b.bits[c]) known &= ~(1u << c);
    }
    return MakePartial(a.type, a.count, known, a.bits);
}

// ============= Types =============

// The scalar, vector and matrix types this pass folds: their component
// type, and their component count (0 for every other type).
static CoreType GetScalarType(CoreType type) {
    switch (type) {
        case CoreType::BOOL: case CoreType::BOOL2: case CoreType::BOOL3: case CoreType::BOOL4:
            return CoreType::BOOL;
        case CoreType::INT: case CoreType::INT2: case CoreType::INT3: case CoreType::INT4:
            return CoreType::INT;
        case CoreType::UINT: case CoreType::UINT2: case CoreType::UINT3: case CoreType::UINT4:
            return CoreType::UINT;
        case CoreType::FLOAT: case CoreType::FLOAT2: case CoreType::FLOAT3: case CoreType::FLOAT4:
        case CoreType::MAT2: case CoreType::MAT3: case CoreType::MAT4:
            return CoreType::FLOAT;
        default:
            return CoreType::INVALID;
    }
}

static u32 GetComponentCount(CoreType type) {
    switch (type) {
        case CoreType::BOOL: case CoreType::INT: case CoreType::UINT: case CoreType::FLOAT:
            return 1;
        case CoreType::BOOL2: case CoreType::INT2: case CoreType::UINT2: case CoreType::FLOAT2:
            return 2;
        case CoreType::BOOL3: case CoreType::INT3: case CoreType::UINT3: case CoreType::FLOAT3:
            return 3;
        case CoreType::BOOL4: case CoreType::INT4: case CoreType::UINT4: case CoreType::FLOAT4:
        case CoreType::MAT2:
            return 4;
        case CoreType::MAT3:
            return 9;
        case CoreType::MAT4:
            return 16;
        default:
            return 0;
    }
}

// Rows (and columns) of a matrix type; 0 for anything else.
static u32 GetMatrixSize(CoreType type) {
    return type == CoreType::MAT2 ? 2 : type == CoreType::MAT3 ? 3 : type == CoreType::MAT4 ? 4 : 0;
}

static bool IsVectorType(CoreType type) {
    return GetMatrixSize(type) == 0 && GetComponentCount(type) > 1;
}

static bool IsFoldedType(CoreType type) {
    return GetComponentCount(type) != 0;
}

static bool IsIntegerType(CoreType type) {
    return type == CoreType::INT || type == CoreType::UINT;
}

static CoreType GetRegisterType(const IRProgram* ir, u16 reg) {
    return ir->registerTypes ? static_cast<CoreType>(ir->registerTypes[reg]) : CoreType::INVALID;
}

static void* AllocateZeroed(BWSL_Arena* arena, size_t bytes) {
    void* memory = arena->Allocate(std::max<size_t>(bytes, 1), 64);
    if (memory) memset(memory, 0, std::max<size_t>(bytes, 1));
    return memory;
}

static bool IsRegisterOperand(const ConstantPropagationData* data, u16 operand) {
    return operand != 0xFFFF && !IR::IsConstant(operand) && operand < data->regCount;
}

// ============= Floats =============

static float BitsToFloat(u32 bits) {
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static u32 FloatToBits(float value) {
    u32 bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// Folding stays away from values a device may compute differently: NaN and
// infinities, denormals (which GPUs may flush to zero) and -0.0 (which the
// constant pool merges with +0.0).
static bool IsFoldableFloat(float value) {
    return std::isfinite(value) && std::fpclassify(value) != FP_SUBNORMAL &&
           !(value == 0.0f && std::signbit(value));
}

static bool MakeFloatResult(float value, ConstantValue* out) {
    if (!IsFoldableFloat(value)) return false;
    *out = MakeConstant(CoreType::FLOAT, FloatToBits(value));
    return true;
}

// Library functions are evaluated in double and rounded once, which is
// at least as accurate as any GPU's float version.
static bool MakeRoundedResult(double value, ConstantValue* out) {
    return std::isfinite(value) && MakeFloatResult(static_cast<float>(value), out);
}

// Ties go to even, as RoundEven (SPIR-V) and round (WGSL) do.
// A negative input that rounds to zero gives -0.0, which doesn't fold.
static float RoundHalfEven(float value) {
    float floor = std::floor(value);
    float difference = value - floor;
    float result = difference < 0.5f ? floor
                 : difference > 0.5f ? floor + 1.0f
                 : std::fmod(floor, 2.0f) == 0.0f ? floor : floor + 1.0f;
    return std::copysign(result, value);
}

static ConstantValue DecodeConstant(const IRProgram* ir, u16 operand) {
    if (IR::IsBoolConstant(operand)) {
        u32 index = IR::BoolConstantIndex(operand);
        if (index < ir->boolCount) {
            return MakeConstant(CoreType::BOOL, ir->boolConstants[index] ? 1 : 0);
        }
    } else if (IR::IsFloatConstant(operand)) {
        u32 index = IR::FloatConstantIndex(operand);
        if (index < ir->floatCount && IsFoldableFloat(ir->floatConstants[index])) {
            return MakeConstant(CoreType::FLOAT, FloatToBits(ir->floatConstants[index]));
        }
    } else if (IR::IsIntConstant(operand)) {
        u32 index = IR::IntConstantIndex(operand);
        if (index < ir->intCount) return MakeConstant(CoreType::INT, ir->intConstants[index]);
    } else if (IR::IsUintConstant(operand)) {
        u32 index = IR::UintConstantIndex(operand);
        if (index < ir->uintCount) return MakeConstant(CoreType::UINT, ir->uintConstants[index]);
    }
    return OVERDEFINED_VALUE;
}

static ConstantValue GetRegisterValue(const ConstantPropagationData* data, u16 reg) {
    ConstantValue value = {data->states[reg], GetRegisterType(data->ir, reg), 0, {}, 0};
    if (value.state == ValueState::OVERDEFINED) value.known = data->known[reg];
    if (value.state != ValueState::CONSTANT && value.known == 0) return value;
    value.count = static_cast<u8>(data->bitOffsets[reg + 1] - data->bitOffsets[reg]);
    memcpy(value.bits, data->bits + data->bitOffsets[reg], value.count * sizeof(u32));
    return value;
}

static void SetRegisterValue(ConstantPropagationData* data, u16 reg, const ConstantValue& value) {
    data->states[reg] = value.state;
    data->known[reg] = value.state == ValueState::OVERDEFINED ? value.known : 0;
    u32* bits = data->bits + data->bitOffsets[reg];
    u32 count = data->bitOffsets[reg + 1] - data->bitOffsets[reg];
    memset(bits, 0, count * sizeof(u32));
    if (KnownMask(value) != 0) memcpy(bits, value.bits, std::min<u32>(count, value.count) * sizeof(u32));
}

static ConstantValue GetValue(const ConstantPropagationData* data, u16 operand) {
    if (operand == 0xFFFF) return OVERDEFINED_VALUE;
    if (IR::IsConstant(operand)) return DecodeConstant(data->ir, operand);
    if (operand >= data->regCount || !data->tracked[operand]) return OVERDEFINED_VALUE;
    return GetRegisterValue(data, operand);
}

// ============= Folding =============

// Operand count of the scalar operations FoldOperation evaluates; 0 for
// everything else.
static u32 GetFoldArity(u16 op) {
    switch (op) {
        case IR::OP_LOAD_REG: case IR::OP_STORE_REG:
        case IR::OP_FNEG: case IR::OP_FABS: case IR::OP_FLOOR: case IR::OP_CEIL:
        case IR::OP_TRUNC: case IR::OP_INEG: case IR::OP_IABS: case IR::OP_NOT:
        case IR::OP_F2I: case IR::OP_I2F: case IR::OP_F2U: case IR::OP_U2F:
        case IR::OP_I2U: case IR::OP_U2I:
        case IR::OP_SQRT: case IR::OP_RSQRT: case IR::OP_EXP: case IR::OP_EXP2:
        case IR::OP_LOG: case IR::OP_LOG2: case IR::OP_SIN: case IR::OP_COS:
        case IR::OP_TAN: case IR::OP_ASIN: case IR::OP_ACOS: case IR::OP_ATAN:
        case IR::OP_SINH: case IR::OP_COSH: case IR::OP_TANH:
        case IR::OP_ROUND: case IR::OP_FRACT: case IR::OP_SIGN: case IR::OP_SATURATE:
        case IR::OP_DEGREES: case IR::OP_RADIANS:
        case IR::OP_ISNAN: case IR::OP_ISINF: case IR::OP_ISFINITE: case IR::OP_ISNORMAL:
        case IR::OP_POPCNT: case IR::OP_CLZ: case IR::OP_CTZ: case IR::OP_REVERSE_BITS:
        case IR::OP_BITCAST:
            return 1;
        case IR::OP_FADD: case IR::OP_FSUB: case IR::OP_FMUL: case IR::OP_FDIV:
        case IR::OP_FMIN: case IR::OP_FMAX:
        case IR::OP_FMOD: case IR::OP_FREM: case IR::OP_POW: case IR::OP_ATAN2:
        case IR::OP_STEP: case IR::OP_LDEXP:
        case IR::OP_IADD: case IR::OP_ISUB: case IR::OP_IMUL: case IR::OP_IDIV:
        case IR::OP_IMOD: case IR::OP_IMIN: case IR::OP_IMAX: case IR::OP_UMIN:
        case IR::OP_UMAX:
        case IR::OP_AND: case IR::OP_OR: case IR::OP_XOR:
        case IR::OP_SHL: case IR::OP_SHR: case IR::OP_ASR:
        case IR::OP_FEQ: case IR::OP_FNE: case IR::OP_FLT: case IR::OP_FLE:
        case IR::OP_FGT: case IR::OP_FGE:
        case IR::OP_IEQ: case IR::OP_INE: case IR::OP_ILT: case IR::OP_ILE:
        case IR::OP_IGT: case IR::OP_IGE:
        case IR::OP_ULT: case IR::OP_ULE: case IR::OP_UGT: case IR::OP_UGE:
            return 2;
        case IR::OP_FCLAMP: case IR::OP_ICLAMP: case IR::OP_UCLAMP:
        case IR::OP_LERP: case IR::OP_SMOOTHSTEP: case IR::OP_FMA:
        case IR::OP_BITFIELD_EXTRACT:
            return 3;
        case IR::OP_BITFIELD_INSERT:
            return 4;
        default:
            return 0;
    }
}

// Operations whose scalar operands the backends splat when another operand
// is a vector.
static bool SplatsScalarOperands(u16 op) {
    switch (op) {
        case IR::OP_FADD: case IR::OP_FSUB: case IR::OP_FMUL: case IR::OP_FDIV:
        case IR::OP_FMOD: case IR::OP_FREM:
        case IR::OP_FMIN: case IR::OP_FMAX: case IR::OP_FCLAMP:
        case IR::OP_IMIN: case IR::OP_IMAX: case IR::OP_ICLAMP:
        case IR::OP_UMIN: case IR::OP_UMAX: case IR::OP_UCLAMP:
        case IR::OP_POW: case IR::OP_ATAN2: case IR::OP_STEP: case IR::OP_LDEXP:
        case IR::OP_LERP: case IR::OP_SMOOTHSTEP: case IR::OP_FMA:
            return true;
        default:
            return false;
    }
}

// Evaluates `op` on constant operands into a `resultType` scalar. Returns
// false when the result isn't one every target computes the same way
// (division by zero, overflowing conversions, negative integer division, a
// math function outside its domain, ...), or the operand types don't match
// the operation.
static bool FoldOperation(u16 op, const ConstantValue* in, CoreType resultType, ConstantValue* out) {
    CoreType type = in[0].type;
    bool sameTypes2 = in[1].type == type;
    bool sameTypes3 = sameTypes2 && in[2].type == type;
    float fa = BitsToFloat(in[0].bits[0]), fb = BitsToFloat(in[1].bits[0]);
    float fc = BitsToFloat(in[2].bits[0]);
    double da = fa, db = fb;
    s32 sa = static_cast<s32>(in[0].bits[0]), sb = static_cast<s32>(in[1].bits[0]);
    s32 sc = static_cast<s32>(in[2].bits[0]);
    u32 ua = in[0].bits[0], ub = in[1].bits[0], uc = in[2].bits[0];
    bool isFloat = type == CoreType::FLOAT;
    bool isFloat2 = isFloat && sameTypes2;
    bool isFloat3 = isFloat && sameTypes3;
    bool isInt2 = type == CoreType::INT && sameTypes2;
    bool isUint2 = type == CoreType::UINT && sameTypes2;

    switch (op) {
        case IR::OP_LOAD_REG:
        case IR::OP_STORE_REG:
            *out = in[0];
            return true;

        // Float arithmetic. Division may be less precise on the GPU; lowering
        // already folds it, so the IR's answer wins everywhere.
        case IR::OP_FADD: return isFloat2 && MakeFloatResult(fa + fb, out);
        case IR::OP_FSUB: return isFloat2 && MakeFloatResult(fa - fb, out);
        case IR::OP_FMUL: return isFloat2 && MakeFloatResult(fa * fb, out);
        case IR::OP_FDIV: return isFloat2 && fb != 0.0f && MakeFloatResult(fa / fb, out);
        case IR::OP_FMIN: return isFloat2 && MakeFloatResult(std::fmin(fa, fb), out);
        case IR::OP_FMAX: return isFloat2 && MakeFloatResult(std::fmax(fa, fb), out);
        case IR::OP_FCLAMP:
            return type == CoreType::FLOAT && sameTypes3 && fb <= fc &&
                   MakeFloatResult(std::fmin(std::fmax(fa, fb), fc), out);
        case IR::OP_FNEG: return type == CoreType::FLOAT && MakeFloatResult(-fa, out);
        case IR::OP_FABS: return type == CoreType::FLOAT && MakeFloatResult(std::fabs(fa), out);
        case IR::OP_FLOOR: return type == CoreType::FLOAT && MakeFloatResult(std::floor(fa), out);
        case IR::OP_CEIL: return type == CoreType::FLOAT && MakeFloatResult(std::ceil(fa), out);
        case IR::OP_TRUNC: return type == CoreType::FLOAT && MakeFloatResult(std::trunc(fa), out);
        case IR::OP_ROUND: return isFloat && MakeFloatResult(RoundHalfEven(fa), out);
        case IR::OP_FRACT: return isFloat && MakeFloatResult(fa - std::floor(fa), out);
        case IR::OP_SATURATE: return isFloat && MakeFloatResult(std::fmin(std::fmax(fa, 0.0f), 1.0f), out);
        // mod(x, y) = x - y * floor(x / y), and fmod truncates instead.
        case IR::OP_FMOD:
            return isFloat2 && fb != 0.0f && MakeFloatResult(fa - fb * std::floor(fa / fb), out);
        case IR::OP_FREM: return isFloat2 && fb != 0.0f && MakeFloatResult(std::fmod(fa, fb), out);
        case IR::OP_STEP: return isFloat2 && MakeFloatResult(fb < fa ? 0.0f : 1.0f, out);
        case IR::OP_LERP: return isFloat3 && MakeFloatResult(fa * (1.0f - fc) + fb * fc, out);
        case IR::OP_SMOOTHSTEP: {
            // Undefined unless edge0 < edge1.
            if (!isFloat3 || !(fa < fb)) return false;
            float t = std::fmin(std::fmax((fc - fa) / (fb - fa), 0.0f), 1.0f);
            return MakeFloatResult(t * t * (3.0f - 2.0f * t), out);
        }
        case IR::OP_FMA: return isFloat3 && MakeFloatResult(std::fma(fa, fb, fc), out);
        case IR::OP_LDEXP:
            return isFloat && in[1].type == CoreType::INT && MakeRoundedResult(std::ldexp(da, sb), out);

        // Math library functions, outside the inputs where they're undefined.
        case IR::OP_SQRT: return isFloat && fa >= 0.0f && MakeRoundedResult(std::sqrt(da), out);
        case IR::OP_RSQRT: return isFloat && fa > 0.0f && MakeRoundedResult(1.0 / std::sqrt(da), out);
        case IR::OP_EXP: return isFloat && MakeRoundedResult(std::exp(da), out);
        case IR::OP_EXP2: return isFloat && MakeRoundedResult(std::exp2(da), out);
        case IR::OP_LOG: return isFloat && fa > 0.0f && MakeRoundedResult(std::log(da), out);
        case IR::OP_LOG2: return isFloat && fa > 0.0f && MakeRoundedResult(std::log2(da), out);
        case IR::OP_POW:
            if (!isFloat2 || fa < 0.0f || (fa == 0.0f && fb <= 0.0f)) return false;
            return MakeRoundedResult(std::pow(da, db), out);
        case IR::OP_SIN: return isFloat && MakeRoundedResult(std::sin(da), out);
        case IR::OP_COS: return isFloat && MakeRoundedResult(std::cos(da), out);
        case IR::OP_TAN: return isFloat && MakeRoundedResult(std::tan(da), out);
        case IR::OP_ASIN: return isFloat && std::fabs(fa) <= 1.0f && MakeRoundedResult(std::asin(da), out);
        case IR::OP_ACOS: return isFloat && std::fabs(fa) <= 1.0f && MakeRoundedResult(std::acos(da), out);
        case IR::OP_ATAN: return isFloat && MakeRoundedResult(std::atan(da), out);
        case IR::OP_ATAN2:
            return isFloat2 && !(fa == 0.0f && fb == 0.0f) && MakeRoundedResult(std::atan2(da, db), out);
        case IR::OP_SINH: return isFloat && MakeRoundedResult(std::sinh(da), out);
        case IR::OP_COSH: return isFloat && MakeRoundedResult(std::cosh(da), out);
        case IR::OP_TANH: return isFloat && MakeRoundedResult(std::tanh(da), out);
        case IR::OP_DEGREES: return isFloat && MakeRoundedResult(da * (180.0 / 3.14159265358979323846), out);
        case IR::OP_RADIANS: return isFloat && MakeRoundedResult(da * (3.14159265358979323846 / 180.0), out);

        case IR::OP_SIGN:
            if (isFloat) return MakeFloatResult(fa > 0.0f ? 1.0f : fa < 0.0f ? -1.0f : 0.0f, out);
            if (type != CoreType::INT) return false;
            *out = MakeConstant(type, static_cast<u32>(sa > 0 ? 1 : sa < 0 ? -1 : 0));
            return true;

        // A folded float is never NaN, infinite or denormal.
        case IR::OP_ISNAN: case IR::OP_ISINF: case IR::OP_ISFINITE: case IR::OP_ISNORMAL: {
            if (!isFloat) return false;
            bool result = op == IR::OP_ISFINITE || (op == IR::OP_ISNORMAL && fa != 0.0f);
            *out = MakeConstant(CoreType::BOOL, result ? 1 : 0);
            return true;
        }

        // Bit operations. first_bit_high/low return the bit's index, or -1
        // (all ones) when there's none: FindUMsb, FindSMsb and FindILsb.
        case IR::OP_POPCNT:
            if (!IsIntegerType(type) || !IsIntegerType(resultType)) return false;
            *out = MakeConstant(resultType, static_cast<u32>(std::popcount(ua)));
            return true;
        case IR::OP_CLZ: {
            if (!IsIntegerType(type) || !IsIntegerType(resultType)) return false;
            // A signed value's highest bit that differs from its sign bit.
            u32 bits = type == CoreType::INT && sa < 0 ? ~ua : ua;
            *out = MakeConstant(resultType, bits == 0 ? 0xFFFFFFFFu : static_cast<u32>(31 - std::countl_zero(bits)));
            return true;
        }
        case IR::OP_CTZ:
            if (!IsIntegerType(type) || !IsIntegerType(resultType)) return false;
            *out = MakeConstant(resultType, ua == 0 ? 0xFFFFFFFFu : static_cast<u32>(std::countr_zero(ua)));
            return true;
        case IR::OP_REVERSE_BITS: {
            if (!IsIntegerType(type)) return false;
            u32 result = 0;
            for (u32 bit = 0; bit < 32; bit++) result |= ((ua >> bit) & 1u) << (31 - bit);
            *out = MakeConstant(type, result);
            return true;
        }
        case IR::OP_BITFIELD_EXTRACT:
        case IR::OP_BITFIELD_INSERT: {
            // (base, offset, count) and (base, insert, offset, count); the
            // result is undefined when the field leaves the 32 bits.
            bool insert = op == IR::OP_BITFIELD_INSERT;
            const ConstantValue& offsetValue = in[insert ? 2 : 1];
            const ConstantValue& countValue = in[insert ? 3 : 2];
            if (!IsIntegerType(type) || !IsIntegerType(offsetValue.type) || !IsIntegerType(countValue.type) ||
                (insert && in[1].type != type)) {
                return false;
            }
            u32 offset = offsetValue.bits[0], count = countValue.bits[0];
            if (offset > 32 || count > 32 || offset + count > 32) return false;
            u32 mask = count == 32 ? 0xFFFFFFFFu : ((1u << count) - 1u);
            u32 result;
            if (insert) {
                result = (ua & ~(mask << offset)) | ((ub & mask) << offset);
            } else if (count == 0) {
                result = 0;
            } else {
                result = (ua >> offset) & mask;
                // Signed extraction sign-extends the field.
                if (type == CoreType::INT && count < 32 && (result >> (count - 1)) & 1u) result |= ~mask;
            }
            *out = MakeConstant(type, result);
            return true;
        }
        case IR::OP_BITCAST: {
            if (GetComponentCount(type) != 1 || GetScalarType(type) == CoreType::BOOL ||
                (resultType != CoreType::FLOAT && !IsIntegerType(resultType))) {
                return false;
            }
            if (resultType == CoreType::FLOAT) return MakeFloatResult(BitsToFloat(ua), out);
            *out = MakeConstant(resultType, ua);
            return true;
        }

        // Integer arithmetic wraps, as it does on the GPU.
        case IR::OP_IADD:
        case IR::OP_ISUB:
        case IR::OP_IMUL: {
            if (!IsIntegerType(type) || !sameTypes2) return false;
            u32 result = op == IR::OP_IADD ? ua + ub : op == IR::OP_ISUB ? ua - ub : ua * ub;
            *out = MakeConstant(type, result);
            return true;
        }
        case IR::OP_IDIV:
        case IR::OP_IMOD:
            // Signed division and remainder of negative operands differ
            // between GLSL, HLSL and MSL, so only non-negative ones fold.
            if (!IsIntegerType(type) || !sameTypes2 || ub == 0) return false;
            if (type == CoreType::INT && (sa < 0 || sb < 0)) return false;
            *out = MakeConstant(type, op == IR::OP_IDIV ? ua / ub : ua % ub);
            return true;
        case IR::OP_INEG:
            if (type != CoreType::INT) return false;
            *out = MakeConstant(type, 0u - ua);
            return true;
        case IR::OP_IABS:
            if (type != CoreType::INT || sa == INT32_MIN) return false;
            *out = MakeConstant(type, sa < 0 ? 0u - ua : ua);
            return true;
        case IR::OP_IMIN:
            if (!isInt2) return false;
            *out = MakeConstant(type, static_cast<u32>(std::min(sa, sb)));
            return true;
        case IR::OP_IMAX:
            if (!isInt2) return false;
            *out = MakeConstant(type, static_cast<u32>(std::max(sa, sb)));
            return true;
        case IR::OP_ICLAMP:
            if (type != CoreType::INT || !sameTypes3 || sb > sc) return false;
            *out = MakeConstant(type, static_cast<u32>(std::min(std::max(sa, sb), sc)));
            return true;
        case IR::OP_UMIN:
            if (!isUint2) return false;
            *out = MakeConstant(type, std::min(ua, ub));
            return true;
        case IR::OP_UMAX:
            if (!isUint2) return false;
            *out = MakeConstant(type, std::max(ua, ub));
            return true;
        case IR::OP_UCLAMP:
            if (type != CoreType::UINT || !sameTypes3 || ub > uc) return false;
            *out = MakeConstant(type, std::min(std::max(ua, ub), uc));
            return true;

        // Bitwise operations are logical ones on bools, whose bits are 0 or 1.
        case IR::OP_AND:
        case IR::OP_OR:
        case IR::OP_XOR: {
            if (!sameTypes2 || !(IsIntegerType(type) || type == CoreType::BOOL)) return false;
            u32 result = op == IR::OP_AND ? ua & ub : op == IR::OP_OR ? ua | ub : ua ^ ub;
            *out = MakeConstant(type, result);
            return true;
        }
        case IR::OP_NOT:
            if (type == CoreType::BOOL) *out = MakeConstant(type, ua ^ 1u);
            else if (IsIntegerType(type)) *out = MakeConstant(type, ~ua);
            else return false;
            return true;
        case IR::OP_SHL:
        case IR::OP_SHR:
        case IR::OP_ASR: {
            // Shifting by the bit width or more is undefined.
            if (!IsIntegerType(type) || !IsIntegerType(in[1].type) || ub >= 32) return false;
            u32 result = op == IR::OP_SHL ? ua << ub
                       : op == IR::OP_SHR ? ua >> ub
                                          : static_cast<u32>(sa >> ub);
            *out = MakeConstant(type, result);
            return true;
        }

        case IR::OP_FEQ: case IR::OP_FNE: case IR::OP_FLT:
        case IR::OP_FLE: case IR::OP_FGT: case IR::OP_FGE: {
            if (!isFloat2) return false;
            bool result = op == IR::OP_FEQ ? fa == fb : op == IR::OP_FNE ? fa != fb
                        : op == IR::OP_FLT ? fa < fb : op == IR::OP_FLE ? fa <= fb
                        : op == IR::OP_FGT ? fa > fb : fa >= fb;
            *out = MakeConstant(CoreType::BOOL, result ? 1 : 0);
            return true;
        }
        case IR::OP_IEQ:
        case IR::OP_INE:
            if (!sameTypes2 || !(IsIntegerType(type) || type == CoreType::BOOL)) return false;
            *out = MakeConstant(CoreType::BOOL, (ua == ub) == (op == IR::OP_IEQ) ? 1 : 0);
            return true;
        case IR::OP_ILT: case IR::OP_ILE: case IR::OP_IGT: case IR::OP_IGE: {
            if (!isInt2) return false;
            bool result = op == IR::OP_ILT ? sa < sb : op == IR::OP_ILE ? sa <= sb
                        : op == IR::OP_IGT ? sa > sb : sa >= sb;
            *out = MakeConstant(CoreType::BOOL, result ? 1 : 0);
            return true;
        }
        case IR::OP_ULT: case IR::OP_ULE: case IR::OP_UGT: case IR::OP_UGE: {
            if (!isUint2) return false;
            bool result = op == IR::OP_ULT ? ua < ub : op == IR::OP_ULE ? ua <= ub
                        : op == IR::OP_UGT ? ua > ub : ua >= ub;
            *out = MakeConstant(CoreType::BOOL, result ? 1 : 0);
            return true;
        }

        // Conversions fold only when the result is exact and in range.
        case IR::OP_F2I: {
            if (type != CoreType::FLOAT) return false;
            double truncated = std::trunc(static_cast<double>(fa));
            if (truncated < -2147483648.0 || truncated > 2147483647.0) return false;
            *out = MakeConstant(CoreType::INT, static_cast<u32>(static_cast<s32>(truncated)));
            return true;
        }
        case IR::OP_F2U: {
            if (type != CoreType::FLOAT || fa < 0.0f) return false;
            double truncated = std::trunc(static_cast<double>(fa));
            if (truncated > 4294967295.0) return false;
            *out = MakeConstant(CoreType::UINT, static_cast<u32>(truncated));
            return true;
        }
        case IR::OP_I2F: {
            if (type != CoreType::INT) return false;
            float value = static_cast<float>(sa);
            return static_cast<double>(value) == static_cast<double>(sa) &&
                   MakeFloatResult(value, out);
        }
        case IR::OP_U2F: {
            if (type != CoreType::UINT) return false;
            float value = static_cast<float>(ua);
            return static_cast<double>(value) == static_cast<double>(ua) &&
                   MakeFloatResult(value, out);
        }
        case IR::OP_I2U:
            if (type != CoreType::INT) return false;
            *out = MakeConstant(CoreType::UINT, ua);
            return true;
        case IR::OP_U2I:
            if (type != CoreType::UINT) return false;
            *out = MakeConstant(CoreType::INT, ua);
            return true;

        default:
            return false;
    }
}

// The constant of `type` with every bit set: true for bools.
static u32 AllOnes(CoreType type) {
    return type == CoreType::BOOL ? 1u : 0xFFFFFFFFu;
}

// `x & 0` and `x | all-ones` don't depend on x, and `x & all-ones`,
// `x | 0` and `x ^ 0` are x. Returns false when `known` doesn't decide the
// result that way.
static bool EvaluateBitwiseIdentity(u16 op, const ConstantValue& known, const ConstantValue& other,
                                    CoreType type, ConstantValue* out) {
    if (known.state != ValueState::CONSTANT || known.type != type) return false;
    if (!(IsIntegerType(type) || type == CoreType::BOOL)) return false;
    u32 ones = AllOnes(type);
    u32 bits = known.bits[0];
    if ((op == IR::OP_AND && bits == 0) || (op == IR::OP_OR && bits == ones)) {
        *out = known;
        return true;
    }
    if ((op == IR::OP_AND && bits == ones) ||
        ((op == IR::OP_OR || op == IR::OP_XOR) && bits == 0)) {
        *out = other;
        return true;
    }
    return false;
}

// ============= Vectors and matrices =============

// The operation's scalar fold applied to each component. Every operand has
// the result's component count, except that a scalar operand of the float
// arithmetic the backends splat (+, -, *, /) applies to every component.
static bool FoldComponentwise(u16 op, const ConstantValue* in, u32 arity, CoreType resultType,
                              ConstantValue* out) {
    u32 count = GetComponentCount(resultType);
    if (count == 0 || GetMatrixSize(resultType) != 0) return false;
    bool splats = SplatsScalarOperands(op);
    bool full = false;
    for (u32 k = 0; k < arity; k++) {
        if (in[k].count != count && !(splats && in[k].count == 1)) return false;
        full |= in[k].count == count;
    }
    if (!full) return false;
    CoreType resultScalar = GetScalarType(resultType);
    *out = {ValueState::CONSTANT, resultType, static_cast<u8>(count), {}, 0};
    for (u32 c = 0; c < count; c++) {
        ConstantValue scalars[4] = {UNDEFINED_VALUE, UNDEFINED_VALUE, UNDEFINED_VALUE, UNDEFINED_VALUE};
        for (u32 k = 0; k < arity; k++) {
            scalars[k] = MakeConstant(GetScalarType(in[k].type), in[k].bits[in[k].count == 1 ? 0 : c]);
        }
        ConstantValue result;
        if (!FoldOperation(op, scalars, resultScalar, &result) || result.type != resultScalar) return false;
        out->bits[c] = result.bits[0];
    }
    return true;
}

// a[0]*b[0] + a[1]*b[1] + ..., with components `stride` apart, rounded to
// float after every step as the GPU would without fused multiply-adds.
static bool FoldDot(const u32* a, u32 aStride, const u32* b, u32 bStride, u32 count, u32* out) {
    float sum = 0.0f;
    for (u32 k = 0; k < count; k++) {
        float product = BitsToFloat(a[k * aStride]) * BitsToFloat(b[k * bStride]);
        if (!IsFoldableFloat(product)) return false;
        sum = k == 0 ? product : sum + product;
        if (!IsFoldableFloat(sum)) return false;
    }
    *out = FloatToBits(sum);
    return true;
}

static bool FoldMatrixOperation(u16 op, const ConstantValue& a, const ConstantValue& b,
                                CoreType resultType, ConstantValue* out) {
    *out = {ValueState::CONSTANT, resultType, static_cast<u8>(GetComponentCount(resultType)), {}, 0};
    switch (op) {
        case IR::OP_MAT_VEC_MUL: {
            // (M * v)[row] = sum over columns of M[column][row] * v[column]
            u32 n = GetMatrixSize(a.type);
            if (n == 0 || b.type != resultType || out->count != n || b.count != n) return false;
            for (u32 row = 0; row < n; row++) {
                if (!FoldDot(a.bits + row, n, b.bits, 1, n, &out->bits[row])) return false;
            }
            return true;
        }
        case IR::OP_VEC_MAT_MUL: {
            // (v * M)[column] = dot(v, M[column])
            u32 n = GetMatrixSize(b.type);
            if (n == 0 || a.type != resultType || out->count != n || a.count != n) return false;
            for (u32 column = 0; column < n; column++) {
                if (!FoldDot(a.bits, 1, b.bits + column * n, 1, n, &out->bits[column])) return false;
            }
            return true;
        }
        case IR::OP_MAT_MUL: {
            // (A * B)[column][row] = sum over k of A[k][row] * B[column][k]
            u32 n = GetMatrixSize(resultType);
            if (n == 0 || a.type != resultType || b.type != resultType) return false;
            for (u32 column = 0; column < n; column++) {
                for (u32 row = 0; row < n; row++) {
                    if (!FoldDot(a.bits + row, n, b.bits + column * n, 1, n,
                                 &out->bits[column * n + row])) {
                        return false;
                    }
                }
            }
            return true;
        }
        case IR::OP_MAT_SCALE: {
            if (GetMatrixSize(resultType) == 0 || a.type != resultType || b.type != CoreType::FLOAT) {
                return false;
            }
            for (u32 c = 0; c < out->count; c++) {
                float value = BitsToFloat(a.bits[c]) * BitsToFloat(b.bits[0]);
                if (!IsFoldableFloat(value)) return false;
                out->bits[c] = FloatToBits(value);
            }
            return true;
        }
        case IR::OP_MAT_TRANSPOSE: {
            u32 n = GetMatrixSize(resultType);
            if (n == 0 || a.type != resultType) return false;
            for (u32 column = 0; column < n; column++) {
                for (u32 row = 0; row < n; row++) out->bits[column * n + row] = a.bits[row * n + column];
            }
            return true;
        }
        case IR::OP_DOT:
            if (a.type != b.type || GetScalarType(a.type) != CoreType::FLOAT ||
                !IsVectorType(a.type) || resultType != CoreType::FLOAT) {
                return false;
            }
            return FoldDot(a.bits, 1, b.bits, 1, a.count, &out->bits[0]);
        case IR::OP_CROSS: {
            if (a.type != CoreType::FLOAT3 || b.type != CoreType::FLOAT3 ||
                resultType != CoreType::FLOAT3) {
                return false;
            }
            for (u32 c = 0; c < 3; c++) {
                u32 i = (c + 1) % 3, j = (c + 2) % 3;
                float left = BitsToFloat(a.bits[i]) * BitsToFloat(b.bits[j]);
                float right = BitsToFloat(a.bits[j]) * BitsToFloat(b.bits[i]);
                float value = left - right;
                if (!IsFoldableFloat(left) || !IsFoldableFloat(right) || !IsFoldableFloat(value)) {
                    return false;
                }
                out->bits[c] = FloatToBits(value);
            }
            return true;
        }
        default:
            return false;
    }
}

// Square root of dot(v, v) for a float scalar or vector.
static bool FoldLength(const ConstantValue& v, float* out) {
    u32 dot;
    if (!FoldDot(v.bits, 1, v.bits, 1, v.count, &dot)) return false;
    *out = std::sqrt(BitsToFloat(dot));
    return IsFoldableFloat(*out);
}

static bool StoreFloats(const float* values, u32 count, CoreType type, ConstantValue* out) {
    *out = {ValueState::CONSTANT, type, static_cast<u8>(count), {}, 0};
    for (u32 c = 0; c < count; c++) {
        if (!IsFoldableFloat(values[c])) return false;
        out->bits[c] = FloatToBits(values[c]);
    }
    return true;
}

// determinant and inverse, in double and rounded once. Returns false for a
// singular matrix.
static bool FoldMatrixInverse(const ConstantValue& m, bool inverse, CoreType resultType,
                              ConstantValue* out) {
    u32 n = GetMatrixSize(m.type);
    if (n == 0) return false;
    // Gauss-Jordan on [M | I] with partial pivoting; the determinant is the
    // product of the pivots.
    double a[4][8] = {};
    for (u32 column = 0; column < n; column++) {
        for (u32 row = 0; row < n; row++) a[row][column] = BitsToFloat(m.bits[column * n + row]);
        a[column][n + column] = 1.0;
    }
    double determinant = 1.0;
    for (u32 k = 0; k < n; k++) {
        u32 pivot = k;
        for (u32 row = k + 1; row < n; row++) {
            if (std::fabs(a[row][k]) > std::fabs(a[pivot][k])) pivot = row;
        }
        if (a[pivot][k] == 0.0) return false;
        if (pivot != k) {
            for (u32 c = 0; c < 2 * n; c++) std::swap(a[k][c], a[pivot][c]);
            determinant = -determinant;
        }
        double p = a[k][k];
        determinant *= p;
        for (u32 c = 0; c < 2 * n; c++) a[k][c] /= p;
        for (u32 row = 0; row < n; row++) {
            if (row == k || a[row][k] == 0.0) continue;
            double factor = a[row][k];
            for (u32 c = 0; c < 2 * n; c++) a[row][c] -= factor * a[k][c];
        }
    }
    if (!inverse) {
        return resultType == CoreType::FLOAT && MakeRoundedResult(determinant, out);
    }
    if (resultType != m.type) return false;
    float values[MAX_COMPONENTS];
    for (u32 column = 0; column < n; column++) {
        for (u32 row = 0; row < n; row++) values[column * n + row] = static_cast<float>(a[row][n + column]);
    }
    return StoreFloats(values, n * n, resultType, out);
}

// length, distance, normalize, reflect, refract, faceforward, any and all,
// on float (or, for any and all, bool) scalars and vectors. Every step rounds
// to float, as for dot.
static bool FoldGeometric(u16 op, const ConstantValue* in, CoreType resultType, ConstantValue* out) {
    const ConstantValue& a = in[0];
    u32 count = a.count;
    bool floats = GetScalarType(a.type) == CoreType::FLOAT && GetMatrixSize(a.type) == 0;
    float values[4];
    switch (op) {
        case IR::OP_ANY:
        case IR::OP_ALL: {
            if (GetScalarType(a.type) != CoreType::BOOL || resultType != CoreType::BOOL) return false;
            bool any = false, all = true;
            for (u32 c = 0; c < count; c++) {
                any |= a.bits[c] != 0;
                all &= a.bits[c] != 0;
            }
            *out = MakeConstant(CoreType::BOOL, (op == IR::OP_ANY ? any : all) ? 1 : 0);
            return true;
        }
        case IR::OP_LENGTH:
            return floats && resultType == CoreType::FLOAT && FoldLength(a, &values[0]) &&
                   StoreFloats(values, 1, resultType, out);
        case IR::OP_DISTANCE: {
            if (!floats || in[1].type != a.type || resultType != CoreType::FLOAT) return false;
            ConstantValue difference = a;
            for (u32 c = 0; c < count; c++) {
                float value = BitsToFloat(a.bits[c]) - BitsToFloat(in[1].bits[c]);
                if (!IsFoldableFloat(value) && value != 0.0f) return false;
                difference.bits[c] = FloatToBits(value);
            }
            return FoldLength(difference, &values[0]) && StoreFloats(values, 1, resultType, out);
        }
        case IR::OP_NORMALIZE: {
            float length;
            if (!floats || resultType != a.type || !FoldLength(a, &length) || length == 0.0f) return false;
            for (u32 c = 0; c < count; c++) values[c] = BitsToFloat(a.bits[c]) / length;
            return StoreFloats(values, count, resultType, out);
        }
        case IR::OP_REFLECT: {
            // I - 2 * dot(N, I) * N
            const ConstantValue& n = in[1];
            u32 dot;
            if (!floats || n.type != a.type || resultType != a.type ||
                !FoldDot(n.bits, 1, a.bits, 1, count, &dot)) {
                return false;
            }
            float scale = 2.0f * BitsToFloat(dot);
            for (u32 c = 0; c < count; c++) {
                values[c] = BitsToFloat(a.bits[c]) - scale * BitsToFloat(n.bits[c]);
            }
            return IsFoldableFloat(scale) && StoreFloats(values, count, resultType, out);
        }
        case IR::OP_REFRACT: {
            // k = 1 - eta^2 (1 - dot(N, I)^2); 0 when k < 0, else
            // eta I - (eta dot(N, I) + sqrt(k)) N.
            const ConstantValue& n = in[1];
            u32 dotBits;
            if (!floats || n.type != a.type || in[2].type != CoreType::FLOAT || resultType != a.type ||
                !FoldDot(n.bits, 1, a.bits, 1, count, &dotBits)) {
                return false;
            }
            float dot = BitsToFloat(dotBits), eta = BitsToFloat(in[2].bits[0]);
            float k = 1.0f - eta * eta * (1.0f - dot * dot);
            if (!IsFoldableFloat(k) && k != 0.0f) return false;
            if (k < 0.0f) {
                *out = {ValueState::CONSTANT, resultType, static_cast<u8>(count), {}, 0};
                return true;
            }
            float scale = eta * dot + std::sqrt(k);
            for (u32 c = 0; c < count; c++) {
                values[c] = eta * BitsToFloat(a.bits[c]) - scale * BitsToFloat(n.bits[c]);
            }
            return IsFoldableFloat(scale) && StoreFloats(values, count, resultType, out);
        }
        case IR::OP_FACEFORWARD: {
            // dot(Nref, I) < 0 ? N : -N
            u32 dot;
            if (!floats || in[1].type != a.type || in[2].type != a.type || resultType != a.type ||
                !FoldDot(in[2].bits, 1, in[1].bits, 1, count, &dot)) {
                return false;
            }
            bool keep = BitsToFloat(dot) < 0.0f;
            for (u32 c = 0; c < count; c++) values[c] = keep ? BitsToFloat(a.bits[c]) : -BitsToFloat(a.bits[c]);
            return StoreFloats(values, count, resultType, out);
        }
        default:
            return false;
    }
}

// Reads `count` operands. Returns false, with the instruction's result in
// `result`, when one isn't a known constant.
static bool GetConstantOperands(const ConstantPropagationData* data, u32 inst, u32 count,
                                ConstantValue* in, ConstantValue* result) {
    bool waiting = false;
    for (u32 k = 0; k < count; k++) {
        in[k] = GetValue(data, data->ir->GetOperand(inst, k));
        if (in[k].state == ValueState::OVERDEFINED) {
            *result = OVERDEFINED_VALUE;
            return false;
        }
        if (in[k].state == ValueState::UNDEFINED) waiting = true;
    }
    *result = waiting ? UNDEFINED_VALUE : OVERDEFINED_VALUE;
    return !waiting;
}

// An operand's value with its type and component count filled in from its
// register even when nothing about it is known, for the folds that work
// component by component.
static ConstantValue GetShapedValue(const ConstantPropagationData* data, u16 operand) {
    ConstantValue value = GetValue(data, operand);
    if (value.state != ValueState::UNDEFINED && value.count == 0 && IsRegisterOperand(data, operand)) {
        value.type = GetRegisterType(data->ir, operand);
        value.count = static_cast<u8>(GetComponentCount(value.type));
    }
    return value;
}

static bool IsKnown(const ConstantValue& value, u32 component) {
    return (KnownMask(value) >> component) & 1u;
}

static ConstantValue EvaluateVectorInstruction(const ConstantPropagationData* data, u32 inst) {
    const IRProgram* ir = data->ir;
    u16 op = ir->opcodes[inst];
    CoreType resultType = GetRegisterType(ir, ir->destinations[inst]);
    ConstantValue in[4];
    ConstantValue result;
    switch (op) {
        case IR::OP_VEC_CONSTRUCT: {
            // Operands up to the argument count in metadata; 0xFFFF is unused.
            // Mirrors how the backends build a vector: scalars and vector
            // components fill it in order, a single scalar is splatted, and
            // missing components are zero. Operands of another component
            // type aren't folded.
            u32 argumentCount = ir->metadata[inst];
            if (argumentCount == 0 || argumentCount > 4) argumentCount = 4;
            u32 count = GetComponentCount(resultType);
            CoreType scalar = GetScalarType(resultType);
            if (count == 0 || count > 4) return OVERDEFINED_VALUE;
            u32 bits[MAX_COMPONENTS] = {};
            u16 known = 0;
            u32 filled = 0;
            bool waiting = false;
            for (u32 k = 0; k < argumentCount && filled < count; k++) {
                u16 operand = ir->GetOperand(inst, k);
                if (operand == 0xFFFF) continue;
                ConstantValue value = GetShapedValue(data, operand);
                if (value.state == ValueState::UNDEFINED) {
                    waiting = true;
                    continue;
                }
                if (value.count == 0 || GetScalarType(value.type) != scalar ||
                    GetMatrixSize(value.type) != 0) {
                    return OVERDEFINED_VALUE;
                }
                for (u32 c = 0; c < value.count && filled < count; c++, filled++) {
                    if (!IsKnown(value, c)) continue;
                    bits[filled] = value.bits[c];
                    known |= 1u << filled;
                }
            }
            if (waiting) return UNDEFINED_VALUE;
            if (filled == 1) {
                for (u32 c = 1; c < count; c++) bits[c] = bits[0];
                known = known ? FullMask(count) : 0;
            } else {
                known |= FullMask(count) & ~FullMask(filled);  // Missing components are zero
            }
            return MakePartial(resultType, count, known, bits);
        }
        case IR::OP_VEC_EXTRACT: {
            // Operand 1 is the component index itself.
            ConstantValue vector = GetShapedValue(data, ir->GetOperand(inst, 0));
            if (vector.state == ValueState::UNDEFINED) return UNDEFINED_VALUE;
            u32 index = ir->GetOperand(inst, 1);
            if (!IsVectorType(vector.type) || index >= vector.count ||
                GetScalarType(vector.type) != resultType || !IsKnown(vector, index)) {
                return OVERDEFINED_VALUE;
            }
            return MakeConstant(resultType, vector.bits[index]);
        }
        case IR::OP_VEC_INSERT: {
            // INSERT(vector, component index, scalar)
            ConstantValue vector = GetShapedValue(data, ir->GetOperand(inst, 0));
            ConstantValue scalar = GetShapedValue(data, ir->GetOperand(inst, 2));
            if (vector.state == ValueState::UNDEFINED || scalar.state == ValueState::UNDEFINED) {
                return UNDEFINED_VALUE;
            }
            u32 index = ir->GetOperand(inst, 1);
            if (!IsVectorType(vector.type) || vector.type != resultType || index >= vector.count ||
                scalar.count != 1 || scalar.type != GetScalarType(resultType)) {
                return OVERDEFINED_VALUE;
            }
            u16 known = KnownMask(vector) & ~(1u << index);
            if (IsKnown(scalar, 0)) {
                vector.bits[index] = scalar.bits[0];
                known |= 1u << index;
            }
            return MakePartial(resultType, vector.count, known, vector.bits);
        }
        case IR::OP_VEC_SHUFFLE: {
            // SHUFFLE(first, second): indices past the first operand's
            // components pick from the second. A scalar result takes the
            // first index; for one past the first operand the backends
            // disagree (the SPIR-V backend counts the first operand as four
            // components), so that doesn't fold.
            ConstantValue first = GetShapedValue(data, ir->GetOperand(inst, 0));
            ConstantValue second = GetShapedValue(data, ir->GetOperand(inst, 1));
            u32 count = GetComponentCount(resultType);
            CoreType scalar = GetScalarType(resultType);
            if (count == 0 || count > 4 || !IsVectorType(first.type) || GetScalarType(first.type) != scalar) {
                return first.state == ValueState::UNDEFINED ? UNDEFINED_VALUE : OVERDEFINED_VALUE;
            }
            u32 bits[MAX_COMPONENTS] = {};
            u16 known = 0;
            bool waiting = false;
            for (u32 c = 0; c < count; c++) {
                u32 index = (ir->metadata[inst] >> (c * 4)) & 0xF;
                const ConstantValue* source = &first;
                if (index >= first.count) {
                    if (count == 1 || GetScalarType(second.type) != scalar) {
                        return second.state == ValueState::UNDEFINED ? UNDEFINED_VALUE : OVERDEFINED_VALUE;
                    }
                    source = &second;
                    index -= first.count;
                    if (index >= second.count) return OVERDEFINED_VALUE;
                }
                waiting |= source->state == ValueState::UNDEFINED;
                if (!IsKnown(*source, index)) continue;
                bits[c] = source->bits[index];
                known |= 1u << c;
            }
            if (waiting) return UNDEFINED_VALUE;
            return MakePartial(resultType, count, known, bits);
        }
        case IR::OP_ARRAY_LOAD: {
            // A column of a matrix value.
            ConstantValue matrix = GetShapedValue(data, ir->GetOperand(inst, 0));
            ConstantValue column = GetValue(data, ir->GetOperand(inst, 1));
            if (matrix.state == ValueState::UNDEFINED || column.state == ValueState::UNDEFINED) {
                return UNDEFINED_VALUE;
            }
            u32 n = GetMatrixSize(matrix.type);
            if (n == 0 || column.state != ValueState::CONSTANT || !IsIntegerType(column.type) ||
                column.bits[0] >= n ||
                resultType != (n == 2 ? CoreType::FLOAT2 : n == 3 ? CoreType::FLOAT3 : CoreType::FLOAT4)) {
                return OVERDEFINED_VALUE;
            }
            u32 offset = column.bits[0] * n;
            return MakePartial(resultType, n, static_cast<u16>(KnownMask(matrix) >> offset),
                               matrix.bits + offset);
        }
        case IR::OP_MAT_CONSTRUCT: {
            // Columns are vector operands; metadata holds their count.
            u32 n = GetMatrixSize(resultType);
            if (n == 0 || (ir->metadata[inst] != 0 && ir->metadata[inst] != n)) return OVERDEFINED_VALUE;
            CoreType columnType = n == 2 ? CoreType::FLOAT2 : n == 3 ? CoreType::FLOAT3 : CoreType::FLOAT4;
            u32 bits[MAX_COMPONENTS] = {};
            u16 known = 0;
            bool waiting = false;
            for (u32 column = 0; column < n; column++) {
                ConstantValue value = GetShapedValue(data, ir->GetOperand(inst, column));
                if (value.state == ValueState::UNDEFINED) {
                    waiting = true;
                    continue;
                }
                if (value.type != columnType) return OVERDEFINED_VALUE;
                for (u32 row = 0; row < n; row++) {
                    if (!IsKnown(value, row)) continue;
                    bits[column * n + row] = value.bits[row];
                    known |= 1u << (column * n + row);
                }
            }
            if (waiting) return UNDEFINED_VALUE;
            return MakePartial(resultType, n * n, known, bits);
        }
        case IR::OP_MAT_MUL: case IR::OP_MAT_VEC_MUL: case IR::OP_VEC_MAT_MUL:
        case IR::OP_MAT_SCALE: case IR::OP_DOT: case IR::OP_CROSS:
            if (!GetConstantOperands(data, inst, 2, in, &result)) return result;
            return FoldMatrixOperation(op, in[0], in[1], resultType, &result) ? result : OVERDEFINED_VALUE;
        case IR::OP_MAT_TRANSPOSE:
            if (!GetConstantOperands(data, inst, 1, in, &result)) return result;
            return FoldMatrixOperation(op, in[0], in[0], resultType, &result) ? result : OVERDEFINED_VALUE;
        case IR::OP_MAT_DET:
        case IR::OP_MAT_INVERSE:
            if (!GetConstantOperands(data, inst, 1, in, &result)) return result;
            return FoldMatrixInverse(in[0], op == IR::OP_MAT_INVERSE, resultType, &result) ? result
                                                                                          : OVERDEFINED_VALUE;
        case IR::OP_LENGTH: case IR::OP_NORMALIZE: case IR::OP_ANY: case IR::OP_ALL:
        case IR::OP_DISTANCE: case IR::OP_REFLECT:
        case IR::OP_REFRACT: case IR::OP_FACEFORWARD: {
            u32 arity = op == IR::OP_REFRACT || op == IR::OP_FACEFORWARD ? 3
                      : op == IR::OP_DISTANCE || op == IR::OP_REFLECT ? 2 : 1;
            if (!GetConstantOperands(data, inst, arity, in, &result)) return result;
            return FoldGeometric(op, in, resultType, &result) ? result : OVERDEFINED_VALUE;
        }
        default:
            return OVERDEFINED_VALUE;
    }
}

static bool IsVectorFoldOp(u16 op) {
    return op == IR::OP_VEC_CONSTRUCT || op == IR::OP_VEC_EXTRACT || op == IR::OP_VEC_INSERT ||
           op == IR::OP_VEC_SHUFFLE || op == IR::OP_ARRAY_LOAD || op == IR::OP_MAT_CONSTRUCT ||
           op == IR::OP_MAT_MUL || op == IR::OP_MAT_VEC_MUL || op == IR::OP_VEC_MAT_MUL ||
           op == IR::OP_MAT_SCALE || op == IR::OP_MAT_TRANSPOSE || op == IR::OP_DOT ||
           op == IR::OP_CROSS || op == IR::OP_MAT_DET || op == IR::OP_MAT_INVERSE ||
           (op >= IR::OP_LENGTH && op <= IR::OP_FACEFORWARD) || op == IR::OP_ANY || op == IR::OP_ALL;
}

// The operation applied to each component whose operands are known, the
// way FoldComponentwise applies it to constants. A component whose fold
// fails is unknown; the others keep their constants.
static ConstantValue FoldKnownComponents(u16 op, const ConstantValue* in, u32 arity, CoreType resultType) {
    u32 count = GetComponentCount(resultType);
    if (count < 2 || GetMatrixSize(resultType) != 0) return OVERDEFINED_VALUE;
    bool splats = SplatsScalarOperands(op);
    for (u32 k = 0; k < arity; k++) {
        if (in[k].count != count && !(splats && in[k].count == 1)) return OVERDEFINED_VALUE;
    }
    CoreType resultScalar = GetScalarType(resultType);
    u32 bits[MAX_COMPONENTS] = {};
    u16 known = 0;
    for (u32 c = 0; c < count; c++) {
        ConstantValue scalars[4] = {UNDEFINED_VALUE, UNDEFINED_VALUE, UNDEFINED_VALUE, UNDEFINED_VALUE};
        bool inputsKnown = true;
        for (u32 k = 0; k < arity && inputsKnown; k++) {
            u32 component = in[k].count == 1 ? 0 : c;
            inputsKnown = IsKnown(in[k], component);
            scalars[k] = MakeConstant(GetScalarType(in[k].type), in[k].bits[component]);
        }
        ConstantValue result;
        if (!inputsKnown || !FoldOperation(op, scalars, resultScalar, &result) || result.type != resultScalar) {
            continue;
        }
        bits[c] = result.bits[0];
        known |= 1u << c;
    }
    return MakePartial(resultType, count, known, bits);
}

// SELECT per component: a known condition component picks its operand's
// component, and an unknown one keeps a component both operands agree on.
static ConstantValue SelectKnownComponents(const ConstantValue& condition, const ConstantValue& falseValue,
                                           const ConstantValue& trueValue) {
    if (GetScalarType(condition.type) != CoreType::BOOL || falseValue.type != trueValue.type ||
        falseValue.count != trueValue.count || falseValue.count == 0 ||
        (condition.count != 1 && condition.count != falseValue.count)) {
        return Meet(falseValue, trueValue);
    }
    u32 bits[MAX_COMPONENTS] = {};
    u16 known = 0;
    for (u32 c = 0; c < falseValue.count; c++) {
        u32 conditionComponent = condition.count == 1 ? 0 : c;
        bool value;
        if (IsKnown(condition, conditionComponent)) {
            const ConstantValue& picked = condition.bits[conditionComponent] ? trueValue : falseValue;
            value = IsKnown(picked, c);
            bits[c] = picked.bits[c];
        } else {
            value = IsKnown(falseValue, c) && IsKnown(trueValue, c) && falseValue.bits[c] == trueValue.bits[c];
            bits[c] = falseValue.bits[c];
        }
        if (value) known |= 1u << c;
    }
    return MakePartial(falseValue.type, falseValue.count, known, bits);
}

static ConstantValue EvaluateInstruction(const ConstantPropagationData* data, u32 inst) {
    const IRProgram* ir = data->ir;
    u16 op = ir->opcodes[inst];
    CoreType resultType = GetRegisterType(ir, ir->destinations[inst]);

    // Copies keep any value, matrices and known components included.
    if (op == IR::OP_LOAD_REG || op == IR::OP_STORE_REG) return GetValue(data, ir->GetOperand(inst, 0));

    if (op == IR::OP_AND || op == IR::OP_OR || op == IR::OP_XOR) {
        ConstantValue a = GetValue(data, ir->GetOperand(inst, 0));
        ConstantValue b = GetValue(data, ir->GetOperand(inst, 1));
        ConstantValue result;
        if (EvaluateBitwiseIdentity(op, a, b, resultType, &result) ||
            EvaluateBitwiseIdentity(op, b, a, resultType, &result)) {
            return result;
        }
    }

    // SELECT(falseValue, trueValue, condition). A bool vector condition picks
    // per component.
    if (op == IR::OP_SELECT) {
        ConstantValue condition = GetShapedValue(data, ir->GetOperand(inst, 2));
        if (condition.state == ValueState::UNDEFINED) return UNDEFINED_VALUE;
        ConstantValue falseValue = GetShapedValue(data, ir->GetOperand(inst, 0));
        ConstantValue trueValue = GetShapedValue(data, ir->GetOperand(inst, 1));
        if (condition.state == ValueState::CONSTANT && GetScalarType(condition.type) == CoreType::BOOL) {
            bool allTrue = true, allFalse = true;
            for (u32 c = 0; c < condition.count; c++) {
                allTrue &= condition.bits[c] != 0;
                allFalse &= condition.bits[c] == 0;
            }
            if (allTrue) return trueValue;
            if (allFalse) return falseValue;
            if (falseValue.state == ValueState::CONSTANT && trueValue.state == ValueState::CONSTANT &&
                falseValue.type == trueValue.type && falseValue.count == condition.count) {
                ConstantValue result = falseValue;
                for (u32 c = 0; c < condition.count; c++) {
                    if (condition.bits[c]) result.bits[c] = trueValue.bits[c];
                }
                return result;
            }
        }
        if (falseValue.state == ValueState::UNDEFINED || trueValue.state == ValueState::UNDEFINED) {
            return Meet(falseValue, trueValue);
        }
        return SelectKnownComponents(condition, falseValue, trueValue);
    }

    if (IsVectorFoldOp(op)) return EvaluateVectorInstruction(data, inst);

    u32 arity = GetFoldArity(op);
    if (arity == 0) return OVERDEFINED_VALUE;
    ConstantValue in[4] = {UNDEFINED_VALUE, UNDEFINED_VALUE, UNDEFINED_VALUE, UNDEFINED_VALUE};
    ConstantValue result;
    if (!GetConstantOperands(data, inst, arity, in, &result)) {
        if (result.state != ValueState::OVERDEFINED) return result;
        // Some operand isn't a constant: fold the components that are.
        for (u32 k = 0; k < arity; k++) {
            in[k] = GetShapedValue(data, ir->GetOperand(inst, k));
            if (in[k].state == ValueState::UNDEFINED) return UNDEFINED_VALUE;
        }
        return FoldKnownComponents(op, in, arity, resultType);
    }
    bool scalars = true;
    for (u32 k = 0; k < arity; k++) scalars &= in[k].count == 1;
    if (scalars && GetComponentCount(resultType) == 1) {
        return FoldOperation(op, in, resultType, &result) ? result : OVERDEFINED_VALUE;
    }
    return FoldComponentwise(op, in, arity, resultType, &result) ? result : OVERDEFINED_VALUE;
}

// ============= Propagation =============

static void UpdateRegister(ConstantPropagationData* data, u16 reg, ConstantValue value) {
    CoreType type = GetRegisterType(data->ir, reg);
    if (KnownMask(value) != 0 && (value.type != type || value.count != GetComponentCount(type))) {
        value = OVERDEFINED_VALUE;
    }
    ConstantValue current = GetRegisterValue(data, reg);
    ConstantValue next = Meet(current, value);
    if (next.state == current.state && KnownMask(next) == KnownMask(current) &&
        (KnownMask(next) == 0 || HasSameBits(next, current))) {
        return;
    }
    SetRegisterValue(data, reg, next);
    data->regWork[data->regWorkTail++] = reg;
}

static bool IsEdgeExecutable(const ConstantPropagationData* data, u32 from, u32 to) {
    const CFG* cfg = data->cfg;
    for (u32 k = cfg->predecessorOffsets[to]; k < cfg->predecessorOffsets[to + 1]; k++) {
        if (cfg->predecessorData[k] == from && data->edgeExecutable[k]) return true;
    }
    return false;
}

static void EvaluatePhi(ConstantPropagationData* data, u32 phi) {
    const IRProgram* ir = data->ir;
    u16 result = ir->phiResultRegs[phi];
    if (result >= data->regCount || !data->tracked[result]) return;
    if (data->pinned[result]) {
        ConstantValue value = {ValueState::CONSTANT, GetRegisterType(ir, result),
                               static_cast<u8>(data->bitOffsets[result + 1] - data->bitOffsets[result]), {}, 0};
        memcpy(value.bits, data->pinnedBits + data->bitOffsets[result], value.count * sizeof(u32));
        UpdateRegister(data, result, value);
        return;
    }
    u32 block = ir->phiBlockIndices[phi];
    ConstantValue value = UNDEFINED_VALUE;
    for (u32 j = 0; j < ir->GetPhiOperandCount(phi); j++) {
        if (!IsEdgeExecutable(data, ir->GetPhiOperandBlock(phi, j), block)) continue;
        u16 operand = ir->GetPhiOperandValue(phi, j);
        // An undef operand stands for a path that never assigned the
        // variable, so any value will do for it.
        if (IsRegisterOperand(data, operand) && data->undef[operand]) continue;
        value = Meet(value, GetValue(data, operand));
    }
    UpdateRegister(data, result, value);
}

static void MarkEdgeExecutable(ConstantPropagationData* data, u32 from, u32 to) {
    const CFG* cfg = data->cfg;
    if (to >= cfg->blockCount) return;
    bool found = false;
    bool added = false;
    for (u32 k = cfg->predecessorOffsets[to]; k < cfg->predecessorOffsets[to + 1]; k++) {
        if (cfg->predecessorData[k] != from) continue;
        found = true;
        if (!data->edgeExecutable[k]) {
            data->edgeExecutable[k] = true;
            added = true;
        }
    }
    if (found && !added) return;
    if (!data->blockExecutable[to]) {
        data->blockExecutable[to] = true;
        data->blockWork[data->blockWorkTail++] = to;
        return;
    }
    // A new incoming edge can change the phis of a block already visited.
    for (u32 k = data->blockPhiOffsets[to]; k < data->blockPhiOffsets[to + 1]; k++) {
        EvaluatePhi(data, data->blockPhis[k]);
    }
}

static void VisitTerminator(ConstantPropagationData* data, u32 block) {
    const IRProgram* ir = data->ir;
    const CFG* cfg = data->cfg;
    u32 last = cfg->lastInst[block];
    u16 op = ir->opcodes[last];
    if (op == IR::OP_BRANCH) {
        ConstantValue condition = GetValue(data, ir->GetOperand(last, 0));
        if (condition.state == ValueState::UNDEFINED) return;
        if (condition.state == ValueState::CONSTANT && condition.count == 1) {
            // Bits are zero exactly when the value is: -0.0 never folds.
            u32 target = condition.bits[0] != 0 ? ir->GetBranchTrueTarget(last)
                                                : ir->GetBranchFalseTarget(last);
            if (target < ir->instructionCount) {
                MarkEdgeExecutable(data, block, cfg->instToBlock[target]);
                return;
            }
        }
    } else if (op == IR::OP_SWITCH) {
        ConstantValue selector = GetValue(data, ir->GetOperand(last, 0));
        if (selector.state == ValueState::UNDEFINED) return;
        u32 switchId = ir->metadata[last];
        if (selector.state == ValueState::CONSTANT && IsIntegerType(selector.type) &&
            switchId < ir->switchCount) {
            u32 target = ir->GetSwitchDefaultTarget(switchId);
            for (u32 c = 0; c < ir->GetSwitchCaseCount(switchId); c++) {
                if (static_cast<u32>(ir->GetSwitchCaseValue(switchId, c)) == selector.bits[0]) {
                    target = ir->GetSwitchCaseTarget(switchId, c);
                    break;
                }
            }
            if (target < ir->instructionCount) {
                MarkEdgeExecutable(data, block, cfg->instToBlock[target]);
                return;
            }
        }
    }
    for (u32 s = 0; s < cfg->TotalSuccessorCount(block); s++) {
        MarkEdgeExecutable(data, block, cfg->GetAnySuccessor(block, s));
    }
}

static void VisitInstruction(ConstantPropagationData* data, u32 inst) {
    const IRProgram* ir = data->ir;
    u16 op = ir->opcodes[inst];
    if (op == IR::OP_NOP) return;
    if (op == IR::OP_BRANCH || op == IR::OP_SWITCH) {
        VisitTerminator(data, data->cfg->instToBlock[inst]);
        return;
    }
    u16 dest = ir->destinations[inst];
    if (dest < data->regCount && data->tracked[dest] && data->definitions[dest] == inst) {
        if (data->pinned[dest]) {
            // A value its range fixes (see PinPointRanges).
            ConstantValue value = {ValueState::CONSTANT, GetRegisterType(ir, dest),
                                   static_cast<u8>(data->bitOffsets[dest + 1] - data->bitOffsets[dest]), {}, 0};
            memcpy(value.bits, data->pinnedBits + data->bitOffsets[dest], value.count * sizeof(u32));
            UpdateRegister(data, dest, value);
            return;
        }
        UpdateRegister(data, dest, EvaluateInstruction(data, inst));
    }
}

static void VisitBlock(ConstantPropagationData* data, u32 block) {
    const CFG* cfg = data->cfg;
    for (u32 k = data->blockPhiOffsets[block]; k < data->blockPhiOffsets[block + 1]; k++) {
        EvaluatePhi(data, data->blockPhis[k]);
    }
    for (u32 i = cfg->firstInst[block]; i <= cfg->lastInst[block]; i++) {
        u16 op = data->ir->opcodes[i];
        if (op != IR::OP_BRANCH && op != IR::OP_SWITCH) VisitInstruction(data, i);
    }
    VisitTerminator(data, block);
}

static void Propagate(ConstantPropagationData* data) {
    u32 entry = data->cfg->entryBlock;
    data->blockExecutable[entry] = true;
    data->blockWork[data->blockWorkTail++] = entry;

    while (data->blockWorkHead < data->blockWorkTail || data->regWorkHead < data->regWorkTail) {
        if (data->blockWorkHead < data->blockWorkTail) {
            VisitBlock(data, data->blockWork[data->blockWorkHead++]);
            continue;
        }
        u16 reg = static_cast<u16>(data->regWork[data->regWorkHead++]);
        for (u32 k = data->useOffsets[reg]; k < data->useOffsets[reg + 1]; k++) {
            u32 user = data->useData[k];
            if (user & PHI_REFERENCE) {
                u32 phi = user & ~PHI_REFERENCE;
                if (data->blockExecutable[data->ir->phiBlockIndices[phi]]) EvaluatePhi(data, phi);
            } else if (data->blockExecutable[data->cfg->instToBlock[user]]) {
                VisitInstruction(data, user);
            }
        }
    }
}

// ============= Loops =============

static u16 InternConstant(IR::IRBuilder* builder, CoreType type, u32 bits);
static void ComputeRanges(ConstantPropagationData* data);
static bool PinPointRanges(ConstantPropagationData* data);

// Instructions a loop evaluated at compile time may hold, beside control
// flow: values without side effects.
static bool IsLoopValueOp(u16 op) {
    return GetFoldArity(op) != 0 || IsVectorFoldOp(op) || op == IR::OP_SELECT;
}

// Evaluations of one loop's instructions, all iterations together.
constexpr u32 LOOP_EVALUATION_BUDGET = 1 << 16;

static bool Dominates(const CFG* cfg, u32 dominator, u32 block) {
    while (block < cfg->blockCount) {
        if (block == dominator) return true;
        u32 parent = cfg->immediateDominators[block];
        if (parent == block) return false;
        block = parent;
    }
    return false;
}

static bool HasBackEdge(const CFG* cfg, u32 header) {
    for (u32 k = cfg->predecessorOffsets[header]; k < cfg->predecessorOffsets[header + 1]; k++) {
        if (Dominates(cfg, header, cfg->predecessorData[k])) return true;
    }
    return false;
}

// Marks the natural loop of `header` in data->inLoop: the blocks that reach
// one of its back edges without passing through it. Returns false when the
// header has no back edge.
static bool CollectLoop(ConstantPropagationData* data, u32 header) {
    const CFG* cfg = data->cfg;
    memset(data->inLoop, 0, cfg->blockCount * sizeof(bool));
    data->inLoop[header] = true;
    u32 top = 0;
    for (u32 k = cfg->predecessorOffsets[header]; k < cfg->predecessorOffsets[header + 1]; k++) {
        u32 latch = cfg->predecessorData[k];
        if (!Dominates(cfg, header, latch) || data->inLoop[latch]) continue;
        data->inLoop[latch] = true;
        data->blockStack[top++] = latch;
    }
    if (top == 0) return false;
    while (top > 0) {
        u32 block = data->blockStack[--top];
        for (u32 k = cfg->predecessorOffsets[block]; k < cfg->predecessorOffsets[block + 1]; k++) {
            u32 predecessor = cfg->predecessorData[k];
            if (predecessor >= cfg->blockCount || data->inLoop[predecessor]) continue;
            data->inLoop[predecessor] = true;
            data->blockStack[top++] = predecessor;
        }
    }
    return true;
}

// A register the loop defines but nothing else may see: a copy into a
// variable's register (SSA keeps them for naming) that nothing reads.
static bool IsUnreadCopy(const ConstantPropagationData* data, u32 inst) {
    u16 op = data->ir->opcodes[inst];
    u16 dest = data->ir->destinations[inst];
    return (op == IR::OP_STORE_REG || op == IR::OP_LOAD_REG) && IsRegisterOperand(data, dest) &&
           !data->tracked[dest] && !data->readAnywhere[dest];
}

// Whether every instruction and phi of the marked loop is one the loop can
// be evaluated through.
static bool IsPureLoop(const ConstantPropagationData* data) {
    const IRProgram* ir = data->ir;
    const CFG* cfg = data->cfg;
    for (u32 block = 0; block < cfg->blockCount; block++) {
        if (!data->inLoop[block]) continue;
        for (u32 k = data->blockPhiOffsets[block]; k < data->blockPhiOffsets[block + 1]; k++) {
            if (!data->tracked[ir->phiResultRegs[data->blockPhis[k]]]) return false;
        }
        for (u32 i = cfg->firstInst[block]; i <= cfg->lastInst[block]; i++) {
            u16 op = ir->opcodes[i];
            if (op == IR::OP_NOP || op == IR::OP_JUMP || op == IR::OP_BRANCH || op == IR::OP_SWITCH) continue;
            if (!IsLoopValueOp(op)) return false;
            u16 dest = ir->destinations[i];
            if (!IsRegisterOperand(data, dest)) return false;
            if (!data->tracked[dest] && !IsUnreadCopy(data, i)) return false;
        }
    }
    return true;
}

// The block a terminator in `block` goes to, or NO_BLOCK when it can't tell.
static u32 GetNextBlock(const ConstantPropagationData* data, u32 block) {
    const IRProgram* ir = data->ir;
    const CFG* cfg = data->cfg;
    u32 last = cfg->lastInst[block];
    u16 op = ir->opcodes[last];
    if (op == IR::OP_BRANCH || op == IR::OP_SWITCH) {
        ConstantValue condition = GetValue(data, ir->GetOperand(last, 0));
        if (condition.state != ValueState::CONSTANT || condition.count != 1) return NO_BLOCK;
        u32 target;
        if (op == IR::OP_BRANCH) {
            target = condition.bits[0] != 0 ? ir->GetBranchTrueTarget(last) : ir->GetBranchFalseTarget(last);
        } else {
            u32 switchId = ir->metadata[last];
            if (!IsIntegerType(condition.type) || switchId >= ir->switchCount) return NO_BLOCK;
            target = ir->GetSwitchDefaultTarget(switchId);
            for (u32 c = 0; c < ir->GetSwitchCaseCount(switchId); c++) {
                if (static_cast<u32>(ir->GetSwitchCaseValue(switchId, c)) == condition.bits[0]) {
                    target = ir->GetSwitchCaseTarget(switchId, c);
                    break;
                }
            }
        }
        return target < ir->instructionCount ? cfg->instToBlock[target] : NO_BLOCK;
    }
    if (op == IR::OP_RET || op == IR::OP_DISCARD || cfg->TotalSuccessorCount(block) != 1) return NO_BLOCK;
    return cfg->GetAnySuccessor(block, 0);
}

// Runs the marked loop on the lattice's storage, from its entry until it
// leaves. Registers the loop defines hold their current value as a
// constant, or UNDEFINED before their first assignment. Returns true when
// every value it computed was a constant, with the header's phis at their
// values on its last visit.
static bool RunLoop(ConstantPropagationData* data, u32 header, u32 entry) {
    const IRProgram* ir = data->ir;
    const CFG* cfg = data->cfg;
    for (u32 block = 0; block < cfg->blockCount; block++) {
        if (!data->inLoop[block]) continue;
        for (u32 k = data->blockPhiOffsets[block]; k < data->blockPhiOffsets[block + 1]; k++) {
            SetRegisterValue(data, ir->phiResultRegs[data->blockPhis[k]], UNDEFINED_VALUE);
        }
        for (u32 i = cfg->firstInst[block]; i <= cfg->lastInst[block]; i++) {
            u16 dest = ir->destinations[i];
            if (IsLoopValueOp(ir->opcodes[i]) && IsRegisterOperand(data, dest) && data->tracked[dest]) {
                SetRegisterValue(data, dest, UNDEFINED_VALUE);
            }
        }
    }

    ConstantValue phiValues[64];
    u32 budget = LOOP_EVALUATION_BUDGET;
    u32 from = entry, block = header;
    while (true) {
        // Phis read their operands before any of them is assigned.
        u32 phiStart = data->blockPhiOffsets[block], phiEnd = data->blockPhiOffsets[block + 1];
        if (phiEnd - phiStart > 64) return false;
        for (u32 k = phiStart; k < phiEnd; k++) {
            u32 phi = data->blockPhis[k];
            phiValues[k - phiStart] = UNDEFINED_VALUE;
            for (u32 j = 0; j < ir->GetPhiOperandCount(phi); j++) {
                if (ir->GetPhiOperandBlock(phi, j) != from) continue;
                u16 operand = ir->GetPhiOperandValue(phi, j);
                if (IsRegisterOperand(data, operand) && data->undef[operand]) break;
                phiValues[k - phiStart] = GetValue(data, operand);
                if (phiValues[k - phiStart].state == ValueState::OVERDEFINED) return false;
                break;
            }
        }
        for (u32 k = phiStart; k < phiEnd; k++) {
            SetRegisterValue(data, ir->phiResultRegs[data->blockPhis[k]], phiValues[k - phiStart]);
        }

        for (u32 i = cfg->firstInst[block]; i <= cfg->lastInst[block]; i++) {
            u16 op = ir->opcodes[i];
            u16 dest = ir->destinations[i];
            if (!IsLoopValueOp(op) || !data->tracked[dest]) continue;
            if (budget-- == 0) return false;
            ConstantValue value = EvaluateInstruction(data, i);
            CoreType type = GetRegisterType(ir, dest);
            if (value.state != ValueState::CONSTANT || value.type != type ||
                value.count != GetComponentCount(type)) {
                return false;
            }
            SetRegisterValue(data, dest, value);
        }

        // Visits count too, so a loop without values can't run forever.
        if (budget-- == 0) return false;
        u32 next = GetNextBlock(data, block);
        if (next >= cfg->blockCount) return false;
        if (!data->inLoop[next]) return true;
        from = block;
        block = next;
    }
}

// Evaluates the loop at `header` when everything it reads from outside is
// a constant and it has no side effects, and pins its header phis to their
// values on the header's last visit. From there, the loop runs what's left
// of its last iteration and leaves where it did, so propagation follows
// that one path with constants and the code after the loop sees the
// results. The back edge no longer runs. A vector or matrix phi stays a
// phi of its entry operand, so one whose value changed means the loop
// isn't folded.
static bool FoldLoop(ConstantPropagationData* data, u32 header) {
    IRProgram* ir = data->ir;
    const CFG* cfg = data->cfg;
    if (!CollectLoop(data, header) || !IsPureLoop(data)) return false;

    // The one executable edge into the loop.
    u32 entry = NO_BLOCK;
    for (u32 k = cfg->predecessorOffsets[header]; k < cfg->predecessorOffsets[header + 1]; k++) {
        u32 predecessor = cfg->predecessorData[k];
        if (data->inLoop[predecessor] || !data->edgeExecutable[k]) continue;
        if (entry != NO_BLOCK && entry != predecessor) return false;
        entry = predecessor;
    }
    if (entry == NO_BLOCK) return false;

    memcpy(data->savedStates, data->states, data->regCount * sizeof(ValueState));
    memcpy(data->savedBits, data->bits, data->bitOffsets[data->regCount] * sizeof(u32));
    memcpy(data->savedKnown, data->known, data->regCount * sizeof(u16));
    bool folded = RunLoop(data, header, entry);

    u32 phiStart = data->blockPhiOffsets[header], phiEnd = data->blockPhiOffsets[header + 1];
    for (u32 k = phiStart; k < phiEnd && folded; k++) {
        u32 phi = data->blockPhis[k];
        u16 result = ir->phiResultRegs[phi];
        ConstantValue exitValue = GetRegisterValue(data, result);
        if (exitValue.state != ValueState::CONSTANT) continue;
        if (exitValue.count == 1) {
            // Interned now: a pinned phi that kept its operands would be wrong.
            data->pinnedPhiOperand[phi] = InternConstant(data->builder, exitValue.type, exitValue.bits[0]);
            folded = data->pinnedPhiOperand[phi] != 0xFFFF;
            continue;
        }
        ConstantValue entryValue = UNDEFINED_VALUE;
        u16 entryOperand = 0xFFFF;
        for (u32 j = 0; j < ir->GetPhiOperandCount(phi); j++) {
            if (ir->GetPhiOperandBlock(phi, j) != entry) continue;
            entryOperand = ir->GetPhiOperandValue(phi, j);
            // Saved: the lattice's value before the run.
            if (IsRegisterOperand(data, entryOperand) && data->tracked[entryOperand]) {
                entryValue = {data->savedStates[entryOperand], GetRegisterType(ir, entryOperand),
                              static_cast<u8>(data->bitOffsets[entryOperand + 1] - data->bitOffsets[entryOperand]), {}, 0};
                memcpy(entryValue.bits, data->savedBits + data->bitOffsets[entryOperand],
                       entryValue.count * sizeof(u32));
            }
            break;
        }
        folded = entryValue.state == ValueState::CONSTANT && HasSameBits(entryValue, exitValue);
        data->pinnedPhiOperand[phi] = entryOperand;
    }
    if (folded) {
        // Phis left undefined were never assigned, so nothing reads them.
        for (u32 k = phiStart; k < phiEnd; k++) {
            u16 result = ir->phiResultRegs[data->blockPhis[k]];
            if (data->states[result] != ValueState::CONSTANT) continue;
            data->pinned[result] = true;
            u32 count = data->bitOffsets[result + 1] - data->bitOffsets[result];
            memcpy(data->pinnedBits + data->bitOffsets[result], data->bits + data->bitOffsets[result],
                   count * sizeof(u32));
        }
    } else {
        for (u32 k = phiStart; k < phiEnd; k++) data->pinnedPhiOperand[data->blockPhis[k]] = 0xFFFF;
    }

    memcpy(data->states, data->savedStates, data->regCount * sizeof(ValueState));
    memcpy(data->bits, data->savedBits, data->bitOffsets[data->regCount] * sizeof(u32));
    memcpy(data->known, data->savedKnown, data->regCount * sizeof(u16));
    return folded;
}

static void ResetPropagation(ConstantPropagationData* data) {
    const CFG* cfg = data->cfg;
    memset(data->states, 0, data->regCount * sizeof(ValueState));
    memset(data->known, 0, data->regCount * sizeof(u16));
    memset(data->bits, 0, data->bitOffsets[data->regCount] * sizeof(u32));
    memset(data->blockExecutable, 0, cfg->blockCount * sizeof(bool));
    memset(data->edgeExecutable, 0, cfg->predecessorDataCount * sizeof(bool));
    data->blockWorkHead = data->blockWorkTail = 0;
    data->regWorkHead = data->regWorkTail = 0;
}

// Propagates, then folds the loops that turned out to have constant inputs
// and propagates again with their results, until no more loops fold: one
// loop's result can be another's input.
static void PropagateWithLoops(ConstantPropagationData* data) {
    const CFG* cfg = data->cfg;
    Propagate(data);
    if (!cfg->dominatorsComputed) return;
    for (u32 round = 0; round < 16; round++) {
        // Values their ranges fix fold like constants.
        ComputeRanges(data);
        bool anyFolded = PinPointRanges(data);
        for (u32 block = 0; block < cfg->blockCount && IsEnabled(data, OptimizationId::LOOP_EVALUATION); block++) {
            if (!data->blockExecutable[block] || data->loopFolded[block] || !HasBackEdge(cfg, block)) continue;
            if (FoldLoop(data, block)) {
                data->loopFolded[block] = true;
                anyFolded = true;
            }
        }
        if (!anyFolded) break;
        ResetPropagation(data);
        Propagate(data);
    }
    // Ranges for the rewrites, from the final propagation.
    ComputeRanges(data);
}

// ============= Rewriting =============

static bool IsUseInDestination(u16 op) {
    // Same list as SSA: these carry a value register in `dest`, not a result.
    return op == IR::OP_LOCAL_STORE || op == IR::OP_ARRAY_STORE ||
           op == IR::OP_STORE_OUTPUT || op == IR::OP_STORE_BUFFER;
}

// Same list as SSA: these hold an attribute, input or output slot in their
// operands, never a register.
static bool HasSlotOperands(u16 op) {
    return op == IR::OP_LOAD_ATTR || op == IR::OP_LOAD_INPUT || op == IR::OP_LOAD_CONST ||
           op == IR::OP_LOAD_OUTPUT || op == IR::OP_STORE_OUTPUT;
}

// Operand slots that hold a literal index rather than a register.
static bool IsLiteralSlot(u16 op, u32 slot) {
    return slot == 1 &&
           (op == IR::OP_VEC_INSERT || op == IR::OP_VEC_EXTRACT || op == IR::OP_ENUM_FIELD ||
            op == IR::OP_STRUCT_INSERT || op == IR::OP_STRUCT_EXTRACT ||
            op == IR::OP_STRUCT_ARRAY_EXTRACT || op == IR::OP_STRUCT_ARRAY_INSERT ||
            op == IR::OP_LOCAL_FIELD_PTR);
}

// Value operations without side effects, beyond the scalar ones this pass
// folds. Operands that are scalar registers may become tagged constants.
static bool IsPureValueOp(u16 op) {
    return op == IR::OP_SELECT || op == IR::OP_VEC_CONSTRUCT || op == IR::OP_VEC_INSERT ||
           op == IR::OP_MAT_CONSTRUCT || op == IR::OP_MAT_MUL || op == IR::OP_MAT_VEC_MUL ||
           op == IR::OP_VEC_MAT_MUL || op == IR::OP_MAT_SCALE || op == IR::OP_MAT_TRANSPOSE ||
           op == IR::OP_DOT || op == IR::OP_CROSS ||
           (op >= IR::OP_SQRT && op <= IR::OP_COSH) || op == IR::OP_TANH ||
           op == IR::OP_FMA || op == IR::OP_FRACT || op == IR::OP_ROUND ||
           op == IR::OP_FMOD || op == IR::OP_FREM || op == IR::OP_SIGN ||
           (op >= IR::OP_LERP && op <= IR::OP_RADIANS);
}

// Folded operations whose operands stay registers unless the whole
// instruction folds. The backends read the type of a shift's operands, and
// of these intrinsics', from the register table.
static bool KeepsRegisterOperands(u16 op) {
    switch (op) {
        case IR::OP_SHL: case IR::OP_SHR: case IR::OP_ASR:
        case IR::OP_ISNAN: case IR::OP_ISINF: case IR::OP_ISFINITE: case IR::OP_ISNORMAL:
        case IR::OP_POPCNT: case IR::OP_CLZ: case IR::OP_CTZ: case IR::OP_REVERSE_BITS:
        case IR::OP_BITCAST:
        case IR::OP_LDEXP: case IR::OP_BITFIELD_EXTRACT: case IR::OP_BITFIELD_INSERT:
            return true;
        default:
            return false;
    }
}

// Instructions whose operands may become tagged constants.
static bool IsRewritableOp(u16 op) {
    if (KeepsRegisterOperands(op)) return false;
    return GetFoldArity(op) != 0 || IsPureValueOp(op) || op == IR::OP_BRANCH ||
           op == IR::OP_SWITCH;
}

// Returns 0xFFFF when the constant pool is full.
static u16 InternConstant(IR::IRBuilder* builder, CoreType type, u32 bits) {
    switch (type) {
        case CoreType::BOOL:
            return builder->EmitConstantBool(bits != 0);
        case CoreType::INT:
        case CoreType::UINT:
            return builder->EmitConstantInteger(bits, type == CoreType::UINT);
        case CoreType::FLOAT: {
            bool wasExceeded = builder->floatConstantLimitExceeded;
            u16 constant = builder->EmitConstant(BitsToFloat(bits));
            if (builder->floatConstantLimitExceeded && !wasExceeded) {
                builder->floatConstantLimitExceeded = false;
                return 0xFFFF;
            }
            return constant;
        }
        default:
            return 0xFFFF;
    }
}

// The tagged constant for a scalar register with a constant value.
static u16 GetConstantOperand(ConstantPropagationData* data, u16 reg) {
    ConstantValue value = GetRegisterValue(data, reg);
    if (value.state != ValueState::CONSTANT || value.count != 1) return 0xFFFF;
    return InternConstant(data->builder, value.type, value.bits[0]);
}

static void RewriteOperands(ConstantPropagationData* data, u32 inst) {
    IRProgram* ir = data->ir;
    u16 op = ir->opcodes[inst];
    if (!IsRewritableOp(op)) return;
    for (u32 slot = 0; slot < 4; slot++) {
        u16 operand = ir->GetOperand(inst, slot);
        if (IsLiteralSlot(op, slot) || !IsRegisterOperand(data, operand) ||
            !data->tracked[operand]) {
            continue;
        }
        u16 constant = GetConstantOperand(data, operand);
        if (constant != 0xFFFF) ir->SetOperand(inst, slot, constant);
    }
}

// A constant result can still be read where an operand can't be a constant
// (a phi, an output store, a texture operation), so its definition becomes
// a plain constant copy.
static void MakeConstantCopy(ConstantPropagationData* data, u32 inst, u16 constant) {
    IRProgram* ir = data->ir;
    u16 op = ir->opcodes[inst];
    if (op == IR::OP_LOAD_REG || op == IR::OP_STORE_REG) return;
    ir->opcodes[inst] = IR::OP_LOAD_REG;
    ir->metadata[inst] = 0;
    ir->SetOperand(inst, 0, constant);
    for (u32 slot = 1; slot < 4; slot++) ir->SetOperand(inst, slot, 0xFFFF);
}

// A register that stands for a variable or pointer, which a copy can't alias.
static bool IsMemoryBacked(const IRProgram* ir, u16 reg) {
    return ir->registerStorageInfo && ir->registerStorageInfo[reg] != 0;
}

// Turns the definition of a register with a constant value into that
// constant: a copy of a tagged constant for a scalar, and a vector built
// from constants, which the backends emit as a constant composite, for a
// vector. A matrix keeps its instruction; what reads it folds instead.
static void MaterializeConstant(ConstantPropagationData* data, u32 inst, u16 reg) {
    IRProgram* ir = data->ir;
    ConstantValue value = GetRegisterValue(data, reg);
    u16 op = ir->opcodes[inst];
    if (value.state != ValueState::CONSTANT || op == IR::OP_LOAD_REG || op == IR::OP_STORE_REG) return;
    if (value.count == 1) {
        u16 constant = InternConstant(data->builder, value.type, value.bits[0]);
        if (constant != 0xFFFF) MakeConstantCopy(data, inst, constant);
        return;
    }
    if (!IsVectorType(value.type)) return;
    u16 components[4];
    for (u32 c = 0; c < value.count; c++) {
        components[c] = InternConstant(data->builder, GetScalarType(value.type), value.bits[c]);
        if (components[c] == 0xFFFF) return;
    }
    ir->opcodes[inst] = IR::OP_VEC_CONSTRUCT;
    ir->metadata[inst] = value.count;
    ir->types[inst] = static_cast<u16>(value.type);
    for (u32 slot = 0; slot < 4; slot++) {
        ir->SetOperand(inst, slot, slot < value.count ? components[slot] : 0xFFFF);
    }
}

// After operands are rewritten, `x & true`, `x | false` and the like (see
// EvaluateBitwiseIdentity) become copies of x, which the backends alias.
static void RewriteBitwiseIdentity(ConstantPropagationData* data, u32 inst) {
    IRProgram* ir = data->ir;
    u16 op = ir->opcodes[inst];
    if (op != IR::OP_AND && op != IR::OP_OR && op != IR::OP_XOR) return;
    u16 dest = ir->destinations[inst];
    if (!IsRegisterOperand(data, dest) || IsMemoryBacked(ir, dest)) return;
    CoreType type = GetRegisterType(ir, dest);
    for (u32 slot = 0; slot < 2; slot++) {
        u16 known = ir->GetOperand(inst, slot);
        u16 other = ir->GetOperand(inst, 1 - slot);
        if (!IR::IsConstant(known) || !IsRegisterOperand(data, other) ||
            IsMemoryBacked(ir, other) || GetRegisterType(ir, other) != type) {
            continue;
        }
        ConstantValue copy = OVERDEFINED_VALUE;
        ConstantValue result;
        if (!EvaluateBitwiseIdentity(op, DecodeConstant(ir, known), copy, type, &result) ||
            result.state != ValueState::OVERDEFINED) {
            continue;
        }
        ir->opcodes[inst] = IR::OP_LOAD_REG;
        ir->metadata[inst] = 0;
        ir->SetOperand(inst, 0, other);
        for (u32 s = 1; s < 4; s++) ir->SetOperand(inst, s, 0xFFFF);
        return;
    }
}

// ============= Algebraic identities =============

constexpr u32 FLOAT_ZERO_BITS = 0x00000000;
constexpr u32 FLOAT_ONE_BITS = 0x3F800000;
constexpr u32 FLOAT_MINUS_ONE_BITS = 0xBF800000;
constexpr u32 FLOAT_TWO_BITS = 0x40000000;
constexpr u32 FLOAT_HALF_BITS = 0x3F000000;
constexpr u32 FLOAT_MINUS_HALF_BITS = 0xBF000000;

// A tagged constant's value, or a register's when propagation found one.
static ConstantValue GetKnownValue(const ConstantPropagationData* data, u16 operand) {
    if (IR::IsConstant(operand)) return DecodeConstant(data->ir, operand);
    if (!IsRegisterOperand(data, operand) || !data->tracked[operand]) return OVERDEFINED_VALUE;
    return GetRegisterValue(data, operand);
}

// Whether `operand` is a constant scalar, or vector, with every component
// `bits` of `scalar` type.
static bool IsSplat(const ConstantPropagationData* data, u16 operand, CoreType scalar, u32 bits) {
    ConstantValue value = GetKnownValue(data, operand);
    if (value.state != ValueState::CONSTANT || GetScalarType(value.type) != scalar ||
        GetMatrixSize(value.type) != 0) {
        return false;
    }
    for (u32 c = 0; c < value.count; c++) {
        if (value.bits[c] != bits) return false;
    }
    return true;
}

// Rewrites `inst` in place as `op` on `a` (and `b`), keeping its result.
// A copy (LOAD_REG) only stands for a register of the result's type that
// no variable or pointer aliases, which the backends then alias too.
static bool RewriteAs(ConstantPropagationData* data, u32 inst, u16 op, u16 a, u16 b = 0xFFFF) {
    IRProgram* ir = data->ir;
    u16 dest = ir->destinations[inst];
    if (!IsRegisterOperand(data, a) || GetRegisterType(ir, a) != GetRegisterType(ir, dest)) return false;
    if (op == IR::OP_LOAD_REG && (IsMemoryBacked(ir, dest) || IsMemoryBacked(ir, a))) return false;
    ir->opcodes[inst] = op;
    ir->metadata[inst] = 0;
    ir->flags[inst] = 0;
    ir->SetOperand(inst, 0, a);
    ir->SetOperand(inst, 1, b);
    for (u32 slot = 2; slot < 4; slot++) ir->SetOperand(inst, slot, 0xFFFF);
    return true;
}

// ============= Local stores =============

// Stored fields store forwarding tracks at a time.
constexpr u32 LOCAL_STORES_TRACKED = 32;

// A field of a local struct, as a field pointer names it: the register its
// chain starts from (the struct, or a pointer to one) and the field indices.
struct LocalAddress {
    u16 root;
    u8 depth;
    u8 fields[4];
};

static bool SameAddress(const LocalAddress& a, const LocalAddress& b) {
    return a.root == b.root && a.depth == b.depth && memcmp(a.fields, b.fields, a.depth) == 0;
}

// Two addresses of one root that can't overlap: their paths part.
static bool Disjoint(const LocalAddress& a, const LocalAddress& b) {
    if (a.root != b.root) return false;
    for (u32 d = 0; d < a.depth && d < b.depth; d++) {
        if (a.fields[d] != b.fields[d]) return true;
    }
    return false;
}

// Instructions that don't write local memory.
static bool KeepsLocalMemory(u16 op) {
    return op == IR::OP_NOP || op == IR::OP_LOAD_REG || op == IR::OP_STORE_REG || op == IR::OP_LOCAL_LOAD ||
           op == IR::OP_LOCAL_FIELD_PTR || op == IR::OP_LOCAL_VAR_PTR || op == IR::OP_LOCAL_INDEX_PTR ||
           op == IR::OP_LOAD_UNIFORM || op == IR::OP_STRUCT_EXTRACT || op == IR::OP_STRUCT_INSERT ||
           HasSlotOperands(op) || IsLoopValueOp(op) || IsPureValueOp(op) ||
           IR::IsTerminator(static_cast<IR::OpCode>(op));
}

// A load of a struct field that a store before it on every path wrote
// becomes a copy of the value stored, so constants and ranges carry
// through the field. Each block starts with what all its predecessors
// stored. Blocks are walked in reverse postorder until that settles,
// starting from the assumption that a block not walked yet keeps
// everything, so a loop that doesn't write a field keeps it; loads are
// rewritten in one walk after that.
//
// A store through a pointer that isn't a field pointer chain forgets every
// stored value, as does anything else that may write local memory; a store
// to a field forgets what overlaps it: its own root's fields along the same
// path, and, since a pointer may point to any struct, every field when one
// of the two roots is a pointer. Two structs are separate memory. A chain's
// root is a struct, or a pointer assigned once. Reassigning a value or a
// root forgets what depends on it, and reassigning a struct a pointer
// takes the address of forgets the fields of pointer roots too.
static void ForwardLocalStores(ConstantPropagationData* data, BWSL_Arena* arena) {
    IRProgram* ir = data->ir;
    const CFG* cfg = data->cfg;
    u32 regCount = data->regCount;
    u32 blocks = data->straightLine ? 1 : cfg->blockCount;
    constexpr u32 MAX_STORED = LOCAL_STORES_TRACKED;
    struct Stored { LocalAddress address; u16 value; };
    bool* addressed = (bool*)AllocateZeroed(arena, regCount * sizeof(bool));
    bool* pointerLike = (bool*)AllocateZeroed(arena, regCount * sizeof(bool));
    u8* definitionCounts = (u8*)AllocateZeroed(arena, regCount);
    u32* definedBy = (u32*)AllocateZeroed(arena, regCount * sizeof(u32));
    u8* resolved = (u8*)AllocateZeroed(arena, regCount);  // 0 not yet, 1 an address, 2 none
    LocalAddress* addresses = (LocalAddress*)AllocateZeroed(arena, regCount * sizeof(LocalAddress));
    Stored* outs = (Stored*)AllocateZeroed(arena, size_t(blocks) * MAX_STORED * sizeof(Stored));
    u8* outCounts = (u8*)AllocateZeroed(arena, blocks);  // ALL_STORED: not walked yet, keeps everything
    constexpr u8 ALL_STORED = 0xFF;
    if (!addressed || !pointerLike || !definitionCounts || !definedBy || !resolved || !addresses || !outs ||
        !outCounts) {
        return;
    }
    memset(outCounts, ALL_STORED, blocks);
    auto isPointerOp = [](u16 op) {
        return op == IR::OP_LOCAL_VAR_PTR || op == IR::OP_LOCAL_FIELD_PTR || op == IR::OP_LOCAL_INDEX_PTR;
    };
    auto defines = [&](u32 i) {
        u16 op = ir->opcodes[i];
        return op != IR::OP_NOP && !IsUseInDestination(op) && !IR::IsTerminator(static_cast<IR::OpCode>(op)) &&
               IsRegisterOperand(data, ir->destinations[i]);
    };
    for (u32 i = 0; i < ir->instructionCount; i++) {
        u16 op = ir->opcodes[i];
        u16 base = ir->GetOperand(i, 0);
        if (isPointerOp(op) && IsRegisterOperand(data, base)) addressed[base] = true;
        if (!defines(i)) continue;
        u16 dest = ir->destinations[i];
        if (definitionCounts[dest] < 255) definitionCounts[dest]++;
        definedBy[dest] = i;
        if (isPointerOp(op)) pointerLike[dest] = true;
    }
    for (u32 phi = 0; phi < ir->phiCount; phi++) {
        u16 result = ir->phiResultRegs[phi];
        if (IsRegisterOperand(data, result) && definitionCounts[result] < 255) definitionCounts[result] += 2;
    }
    // Copies, selects and phis of pointers are pointers too.
    for (bool changed = true; changed;) {
        changed = false;
        auto mark = [&](u16 reg, u16 source) {
            if (IsRegisterOperand(data, reg) && IsRegisterOperand(data, source) && pointerLike[source] && !pointerLike[reg]) {
                pointerLike[reg] = changed = true;
            }
        };
        for (u32 i = 0; i < ir->instructionCount; i++) {
            u16 op = ir->opcodes[i];
            if (!defines(i)) continue;
            if (op == IR::OP_LOAD_REG || op == IR::OP_STORE_REG) mark(ir->destinations[i], ir->GetOperand(i, 0));
            if (op == IR::OP_SELECT) {
                mark(ir->destinations[i], ir->GetOperand(i, 1));
                mark(ir->destinations[i], ir->GetOperand(i, 2));
            }
        }
        for (u32 phi = 0; phi < ir->phiCount; phi++) {
            for (u32 j = 0; j < ir->GetPhiOperandCount(phi); j++) mark(ir->phiResultRegs[phi], ir->GetPhiOperandValue(phi, j));
        }
    }
    // The address a field pointer register holds: one assigned once, by a
    // field pointer of a struct or of a pointer assigned once.
    auto resolve = [&](auto&& self, u16 reg, u32 depth) -> const LocalAddress* {
        if (!IsRegisterOperand(data, reg) || depth > 4) return nullptr;
        if (resolved[reg] == 0) {
            resolved[reg] = 2;
            u32 i = definedBy[reg];
            if (definitionCounts[reg] == 1 && ir->opcodes[i] == IR::OP_LOCAL_FIELD_PTR) {
                u16 base = ir->GetOperand(i, 0);
                u16 field = ir->GetOperand(i, 1);
                LocalAddress address = {};
                if (const LocalAddress* parent = self(self, base, depth + 1)) {
                    address = *parent;
                } else if (IsRegisterOperand(data, base) && (!pointerLike[base] || definitionCounts[base] == 1)) {
                    address.root = base;
                } else {
                    return nullptr;
                }
                if (address.depth < 4 && field < 256) {
                    address.fields[address.depth++] = static_cast<u8>(field);
                    addresses[reg] = address;
                    resolved[reg] = 1;
                }
            }
        }
        return resolved[reg] == 1 ? &addresses[reg] : nullptr;
    };

    // One walk over the blocks; returns whether a block's result changed.
    auto walk = [&](bool rewrite) {
        bool changed = false;
        Stored stored[MAX_STORED];
        for (u32 k = blocks; k-- > 0;) {
            u32 block = data->straightLine ? 0 : cfg->postorder[k];
            if (block >= blocks) continue;
            // What every predecessor walked stored. One not walked yet keeps
            // everything; if none is walked, the block isn't reached and
            // starts with nothing.
            u32 storedCount = 0;
            bool first = true;
            if (!data->straightLine && block != cfg->entryBlock) {
                for (u32 e = cfg->predecessorOffsets[block]; e < cfg->predecessorOffsets[block + 1]; e++) {
                    u32 predecessor = cfg->predecessorData[e];
                    if (predecessor >= blocks || outCounts[predecessor] == ALL_STORED) continue;
                    const Stored* other = outs + size_t(predecessor) * MAX_STORED;
                    if (first) {
                        storedCount = outCounts[predecessor];
                        memcpy(stored, other, storedCount * sizeof(Stored));
                        first = false;
                        continue;
                    }
                    u32 kept = 0;
                    for (u32 s = 0; s < storedCount; s++) {
                        bool found = false;
                        for (u32 t = 0; t < outCounts[predecessor] && !found; t++) {
                            found = other[t].value == stored[s].value && SameAddress(other[t].address, stored[s].address);
                        }
                        if (found) stored[kept++] = stored[s];
                    }
                    storedCount = kept;
                }
            }
            u32 firstInst = data->straightLine ? 0 : cfg->firstInst[block];
            u32 lastInst = data->straightLine ? ir->instructionCount - 1 : cfg->lastInst[block];
            for (u32 i = firstInst; i <= lastInst && i < ir->instructionCount; i++) {
                u16 op = ir->opcodes[i];
                u16 dest = ir->destinations[i];
                if (op == IR::OP_LOCAL_LOAD && rewrite) {
                    const LocalAddress* address = resolve(resolve, ir->GetOperand(i, 0), 0);
                    for (u32 s = 0; address && s < storedCount; s++) {
                        if (!SameAddress(stored[s].address, *address)) continue;
                        u16 value = stored[s].value;
                        if (IR::IsConstant(value)) {
                            CoreType type = GetRegisterType(ir, dest);
                            if (GetComponentCount(type) == 1 && DecodeConstant(ir, value).type == type &&
                                !IsMemoryBacked(ir, dest)) {
                                MakeConstantCopy(data, i, value);
                            }
                        } else {
                            RewriteAs(data, i, IR::OP_LOAD_REG, value);
                        }
                        op = ir->opcodes[i];
                        break;
                    }
                }
                if (op == IR::OP_LOCAL_STORE) {
                    const LocalAddress* address = resolve(resolve, ir->GetOperand(i, 0), 0);
                    u16 value = ir->GetOperand(i, 1);
                    if (!address) {
                        storedCount = 0;
                    } else {
                        LocalAddress target = *address;
                        u32 kept = 0;
                        for (u32 s = 0; s < storedCount; s++) {
                            u16 root = stored[s].address.root;
                            bool separate = root != target.root && !pointerLike[root] && !pointerLike[target.root];
                            if (separate || Disjoint(stored[s].address, target)) stored[kept++] = stored[s];
                        }
                        storedCount = kept;
                        bool forwardable = IR::IsConstant(value) ||
                                           (IsRegisterOperand(data, value) && !addressed[value] && !pointerLike[value] &&
                                            !IsMemoryBacked(ir, value) &&
                                            (data->straightLine || definitionCounts[value] == 1));
                        if (forwardable) {
                            // When full, the oldest value goes: a later
                            // load more likely reads a recent store.
                            if (storedCount == MAX_STORED) {
                                memmove(stored, stored + 1, (MAX_STORED - 1) * sizeof(Stored));
                                storedCount--;
                            }
                            stored[storedCount++] = {target, value};
                        }
                    }
                } else if (!KeepsLocalMemory(op)) {
                    storedCount = 0;
                }
                if (defines(i)) {
                    // A struct a pointer can reach changed: so may what the
                    // fields of pointer roots hold.
                    u32 kept = 0;
                    for (u32 s = 0; s < storedCount; s++) {
                        u16 root = stored[s].address.root;
                        bool keep = stored[s].value != dest && root != dest && !(addressed[dest] && pointerLike[root]);
                        if (keep) stored[kept++] = stored[s];
                    }
                    storedCount = kept;
                }
            }
            Stored* out = outs + size_t(block) * MAX_STORED;
            bool same = outCounts[block] == storedCount;
            for (u32 s = 0; s < storedCount && same; s++) {
                same = out[s].value == stored[s].value && SameAddress(out[s].address, stored[s].address);
            }
            if (!same) {
                changed = true;
                outCounts[block] = static_cast<u8>(storedCount);
                memcpy(out, stored, storedCount * sizeof(Stored));
            }
        }
        return changed;
    };
    // Each walk can only drop entries, so it settles; a stage that hasn't
    // after many walks is left alone.
    bool settled = false;
    for (u32 pass = 0; pass < 64 && !settled; pass++) settled = !walk(false);
    if (settled) walk(true);
}

// A scalar result that doesn't depend on the operands' values.
static bool RewriteAsConstant(ConstantPropagationData* data, u32 inst, u32 bits) {
    CoreType type = GetRegisterType(data->ir, data->ir->destinations[inst]);
    if (GetComponentCount(type) != 1 || GetMatrixSize(type) != 0) return false;
    u16 constant = InternConstant(data->builder, type, bits);
    if (constant == 0xFFFF) return false;
    MakeConstantCopy(data, inst, constant);
    return true;
}

// The operand of a single-definition `op` that defines `reg`, or 0xFFFF.
static u16 GetUnaryInput(const ConstantPropagationData* data, u16 reg, u16 op) {
    if (!IsRegisterOperand(data, reg) || data->definitionCounts[reg] != 1 || IsMemoryBacked(data->ir, reg)) {
        return 0xFFFF;
    }
    u32 definition = data->definitions[reg];
    if ((definition & PHI_REFERENCE) || data->ir->opcodes[definition] != op) return 0xFFFF;
    return data->ir->GetOperand(definition, 0);
}

// For x = ±2^k with 1/x normal, sets `reciprocal` to 1/x, which is exact:
// dividing by x and multiplying by 1/x round the same real number.
static bool GetExactReciprocal(const ConstantValue& value, float* reciprocal) {
    if (value.state != ValueState::CONSTANT || value.count != 1 || value.type != CoreType::FLOAT) {
        return false;
    }
    float divisor = BitsToFloat(value.bits[0]);
    int exponent;
    if (std::fabs(std::frexp(divisor, &exponent)) != 0.5f) return false;
    *reciprocal = 1.0f / divisor;
    return IsFoldableFloat(*reciprocal) && *reciprocal * divisor == 1.0f;
}

// pow(x, n) for an integer n with 3 <= |n| <= 16, or n = -2, as
// multiplications by squaring (x^5 = (x^2)^2 * x) and, for a negative n,
// a reciprocal. Each step rounds once, so the result is at least as
// accurate as the GPU's pow, which computes exp2(n * log2(x)).
static bool RewritePowerChain(ConstantPropagationData* data, u32 inst) {
    IRProgram* ir = data->ir;
    u16 x = ir->GetOperand(inst, 0);
    u16 dest = ir->destinations[inst];
    ConstantValue exponent = GetKnownValue(data, ir->GetOperand(inst, 1));
    if (exponent.state != ValueState::CONSTANT || GetScalarType(exponent.type) != CoreType::FLOAT ||
        GetMatrixSize(exponent.type) != 0 || !IsRegisterOperand(data, x) ||
        GetRegisterType(ir, x) != GetRegisterType(ir, dest)) {
        return false;
    }
    for (u32 c = 1; c < exponent.count; c++) {
        if (exponent.bits[c] != exponent.bits[0]) return false;
    }
    float n = BitsToFloat(exponent.bits[0]);
    if (n != std::trunc(n) || std::fabs(n) > 16.0f || (std::fabs(n) < 3.0f && n != -2.0f)) return false;
    u32 power = static_cast<u32>(std::fabs(n));

    // Left to right over the exponent's bits below the top one: square,
    // then multiply by x for a set bit. The last step is `inst` itself,
    // or the reciprocal after it.
    CoreType type = GetRegisterType(ir, dest);
    u32 steps = 0;
    for (u32 bit = 31 - std::countl_zero(power); bit-- > 0;) steps += (power >> bit) & 1u ? 2 : 1;
    if (n > 0.0f) steps--;
    u16 one = n < 0.0f ? InternConstant(data->builder, CoreType::FLOAT, FLOAT_ONE_BITS) : 0;
    if (one == 0xFFFF) return false;
    u16 registers[8];
    for (u32 k = 0; k < steps; k++) {
        registers[k] = AllocateRegister(data->builder, &data->insertions, type);
        if (registers[k] == 0xFFFF) return false;
    }
    u16 value = x;
    u32 k = 0;
    // `inst` is set directly: RewriteAs checks types only up to the
    // registers the pass started with.
    auto multiply = [&](u16 a, u16 b) {
        if (k == steps) {
            ir->opcodes[inst] = IR::OP_FMUL;
            ir->metadata[inst] = 0;
            ir->SetOperand(inst, 0, a);
            ir->SetOperand(inst, 1, b);
            return;
        }
        QueueInstruction(data->builder, &data->insertions, inst, IR::OP_FMUL, registers[k], a, b);
        value = registers[k++];
    };
    for (u32 bit = 31 - std::countl_zero(power); bit-- > 0;) {
        multiply(value, value);
        if ((power >> bit) & 1u) multiply(value, x);
    }
    if (n < 0.0f) {
        ir->opcodes[inst] = IR::OP_FDIV;
        ir->metadata[inst] = 0;
        ir->SetOperand(inst, 0, one);
        ir->SetOperand(inst, 1, value);
    }
    return true;
}

// The instruction whose value `reg` holds where it's read: its only
// definition, or in a straight-line walk the latest one. NO_DEFINITION
// otherwise.
static u32 GetReachingDefinition(const ConstantPropagationData* data, u16 reg) {
    if (!IsRegisterOperand(data, reg)) return NO_DEFINITION;
    if (!data->straightLine && data->definitionCounts[reg] != 1) return NO_DEFINITION;
    u32 definition = data->definitions[reg];
    return definition & PHI_REFERENCE ? NO_DEFINITION : definition;
}

// Whether `source` still holds, where it's read now, the value it had at
// instruction `definition`: nothing assigned it since.
static bool IsUnchangedSince(const ConstantPropagationData* data, u16 source, u32 definition) {
    if (!IsRegisterOperand(data, source) || IsMemoryBacked(data->ir, source)) return false;
    if (!data->straightLine) return data->definitionCounts[source] <= 1;
    u32 written = data->definitions[source];
    return written == NO_DEFINITION || written < definition;
}

// A component read from a vector that was just put together reads where
// the component came from instead: `float3(a, b, c).y` is b, a component
// a VEC_INSERT wrote is its scalar, one it didn't is the inner vector's,
// a swizzle's is its source's, and a copy's is the original's. Once nothing
// reads the whole vector any more, it's removed as dead.
static void RewriteComponentRead(ConstantPropagationData* data, u32 inst) {
    IRProgram* ir = data->ir;
    for (u32 step = 0; step < 8; step++) {
        u16 vector = ir->GetOperand(inst, 0);
        u32 index = ir->GetOperand(inst, 1);
        // A vector a pointer can reach may have changed since it was built.
        if (IsMemoryBacked(ir, vector)) return;
        u32 definition = GetReachingDefinition(data, vector);
        if (definition == NO_DEFINITION || index >= GetComponentCount(GetRegisterType(ir, vector))) return;
        u16 op = ir->opcodes[definition];
        u16 source = 0xFFFF;
        u32 sourceIndex = 0;
        if (op == IR::OP_VEC_CONSTRUCT) {
            // Arguments fill the components in order; a lone scalar is a splat.
            u32 argumentCount = ir->metadata[definition];
            if (argumentCount == 0 || argumentCount > 4) argumentCount = 4;
            u32 filled = 0, arguments = 0;
            for (u32 k = 0; k < argumentCount && source == 0xFFFF; k++) {
                u16 operand = ir->GetOperand(definition, k);
                if (operand == 0xFFFF) continue;
                arguments++;
                u32 width = IR::IsConstant(operand) ? 1 : GetComponentCount(GetRegisterType(ir, operand));
                if (width == 0) return;
                if (index < filled + width) {
                    source = operand;
                    sourceIndex = index - filled;
                }
                filled += width;
            }
            if (filled == 1 && arguments == 1) {
                source = ir->GetOperand(definition, 0);
                sourceIndex = 0;
            }
        } else if (op == IR::OP_VEC_INSERT) {
            if (ir->GetOperand(definition, 1) == index) {
                source = ir->GetOperand(definition, 2);
            } else {
                source = ir->GetOperand(definition, 0);
                sourceIndex = index;
            }
        } else if (op == IR::OP_LOAD_REG || op == IR::OP_STORE_REG) {
            source = ir->GetOperand(definition, 0);
            sourceIndex = index;
        } else if (op == IR::OP_VEC_SHUFFLE) {
            u16 first = ir->GetOperand(definition, 0);
            u32 firstWidth = IsRegisterOperand(data, first) ? GetComponentCount(GetRegisterType(ir, first)) : 0;
            u32 picked = (ir->metadata[definition] >> (index * 4)) & 0xF;
            if (firstWidth == 0 || picked >= firstWidth) return;
            source = first;
            sourceIndex = picked;
        } else {
            return;
        }
        if (!IsUnchangedSince(data, source, definition)) return;
        CoreType sourceType = GetRegisterType(ir, source);
        CoreType resultType = GetRegisterType(ir, ir->destinations[inst]);
        if (GetComponentCount(sourceType) == 1) {
            if (sourceType == resultType) RewriteAs(data, inst, IR::OP_LOAD_REG, source);
            return;
        }
        if (!IsVectorType(sourceType) || GetScalarType(sourceType) != resultType) return;
        ir->SetOperand(inst, 0, source);
        ir->SetOperand(inst, 1, static_cast<u16>(sourceIndex));
    }
}

// Simplifies an instruction one of whose operands is a constant that
// decides it, or whose operands are the same register, into a copy, a
// cheaper operation or a constant. Only rewrites that give the same value
// for every input apply: `x * 0.0` stays, since x may be infinite or NaN.
// `x + 0.0` and `0.0 - x` only differ from x and -x in the sign of a zero,
// which the targets don't preserve unless asked to. `pow` with an exponent
// of 1, 2, 0.5, -0.5 or -1 becomes the operation it amounts to; GLSL leaves
// pow of a negative base undefined, so x * x for a negative x is one of
// its results.
static void RewriteAlgebraicIdentity(ConstantPropagationData* data, u32 inst) {
    IRProgram* ir = data->ir;
    u16 op = ir->opcodes[inst];
    u16 dest = ir->destinations[inst];
    if (!IsRegisterOperand(data, dest)) return;
    u16 a = ir->GetOperand(inst, 0), b = ir->GetOperand(inst, 1);
    CoreType f = CoreType::FLOAT;
    CoreType scalar = GetScalarType(GetRegisterType(ir, dest));
    bool isInt = scalar == CoreType::INT, isInteger = isInt || scalar == CoreType::UINT;
    auto splat = [&](u16 operand, u32 bits) { return IsSplat(data, operand, scalar, bits); };
    switch (op) {
        case IR::OP_FADD:
            if (splat(b, FLOAT_ZERO_BITS) && RewriteAs(data, inst, IR::OP_LOAD_REG, a)) return;
            if (splat(a, FLOAT_ZERO_BITS)) RewriteAs(data, inst, IR::OP_LOAD_REG, b);
            return;
        case IR::OP_FSUB:
            if (splat(b, FLOAT_ZERO_BITS) && RewriteAs(data, inst, IR::OP_LOAD_REG, a)) return;
            if (splat(a, FLOAT_ZERO_BITS)) RewriteAs(data, inst, IR::OP_FNEG, b);
            return;
        case IR::OP_FMUL:
            for (u32 slot = 0; slot < 2; slot++) {
                u16 known = slot == 0 ? b : a, other = slot == 0 ? a : b;
                if (splat(known, FLOAT_ONE_BITS) && RewriteAs(data, inst, IR::OP_LOAD_REG, other)) return;
                if (splat(known, FLOAT_MINUS_ONE_BITS) && RewriteAs(data, inst, IR::OP_FNEG, other)) return;
            }
            return;
        case IR::OP_FDIV: {
            if (splat(b, FLOAT_ONE_BITS) && RewriteAs(data, inst, IR::OP_LOAD_REG, a)) return;
            if (splat(b, FLOAT_MINUS_ONE_BITS) && RewriteAs(data, inst, IR::OP_FNEG, a)) return;
            float reciprocal;
            ConstantValue divisor = GetKnownValue(data, b);
            if (divisor.state == ValueState::CONSTANT && divisor.count > 1 &&
                IsSplat(data, b, f, divisor.bits[0])) {
                divisor = MakeConstant(f, divisor.bits[0]);
            }
            if (!GetExactReciprocal(divisor, &reciprocal)) return;
            u16 constant = InternConstant(data->builder, f, FloatToBits(reciprocal));
            if (constant != 0xFFFF) RewriteAs(data, inst, IR::OP_FMUL, a, constant);
            return;
        }
        case IR::OP_POW: {
            if (scalar != f) return;
            if (IsEnabled(data, OptimizationId::POW_EXPANSION) && RewritePowerChain(data, inst)) return;
            if (IsSplat(data, b, f, FLOAT_ONE_BITS)) RewriteAs(data, inst, IR::OP_LOAD_REG, a);
            else if (IsSplat(data, b, f, FLOAT_TWO_BITS)) RewriteAs(data, inst, IR::OP_FMUL, a, a);
            else if (IsSplat(data, b, f, FLOAT_HALF_BITS)) RewriteAs(data, inst, IR::OP_SQRT, a);
            else if (IsSplat(data, b, f, FLOAT_MINUS_HALF_BITS)) RewriteAs(data, inst, IR::OP_RSQRT, a);
            else if (IsSplat(data, b, f, FLOAT_ZERO_BITS)) RewriteAsConstant(data, inst, FLOAT_ONE_BITS);
            else if (IsSplat(data, b, f, FLOAT_MINUS_ONE_BITS)) {
                // 1 / x, with the scalar 1 splatted.
                u16 one = InternConstant(data->builder, f, FLOAT_ONE_BITS);
                if (one == 0xFFFF || !IsRegisterOperand(data, a) ||
                    GetRegisterType(ir, a) != GetRegisterType(ir, dest)) {
                    return;
                }
                ir->opcodes[inst] = IR::OP_FDIV;
                ir->metadata[inst] = 0;
                ir->SetOperand(inst, 0, one);
                ir->SetOperand(inst, 1, a);
            }
            return;
        }
        case IR::OP_IADD:
            if (!isInteger) return;
            if (splat(b, 0) && RewriteAs(data, inst, IR::OP_LOAD_REG, a)) return;
            if (splat(a, 0)) RewriteAs(data, inst, IR::OP_LOAD_REG, b);
            return;
        case IR::OP_ISUB:
            if (!isInteger) return;
            if (a == b && IsRegisterOperand(data, a) && RewriteAsConstant(data, inst, 0)) return;
            if (splat(b, 0) && RewriteAs(data, inst, IR::OP_LOAD_REG, a)) return;
            if (isInt && splat(a, 0)) RewriteAs(data, inst, IR::OP_INEG, b);
            return;
        case IR::OP_IMUL:
            if (!isInteger) return;
            for (u32 slot = 0; slot < 2; slot++) {
                u16 known = slot == 0 ? b : a, other = slot == 0 ? a : b;
                if (splat(known, 1) && RewriteAs(data, inst, IR::OP_LOAD_REG, other)) return;
                if (splat(known, 0) && RewriteAsConstant(data, inst, 0)) return;
                if (isInt && splat(known, 0xFFFFFFFF) && RewriteAs(data, inst, IR::OP_INEG, other)) return;
            }
            return;
        case IR::OP_IDIV:
            if (isInteger && splat(b, 1)) RewriteAs(data, inst, IR::OP_LOAD_REG, a);
            return;
        case IR::OP_SHL: case IR::OP_SHR: case IR::OP_ASR: {
            ConstantValue amount = GetKnownValue(data, b);
            if (amount.state == ValueState::CONSTANT && IsIntegerType(GetScalarType(amount.type)) &&
                IsSplat(data, b, GetScalarType(amount.type), 0)) {
                RewriteAs(data, inst, IR::OP_LOAD_REG, a);
            }
            return;
        }
        case IR::OP_AND: case IR::OP_OR:
            if (a == b) RewriteAs(data, inst, IR::OP_LOAD_REG, a);
            return;
        case IR::OP_XOR:
            if (a == b && IsRegisterOperand(data, a) && (isInteger || scalar == CoreType::BOOL)) {
                RewriteAsConstant(data, inst, 0);
            }
            return;
        case IR::OP_FMIN: case IR::OP_FMAX: case IR::OP_IMIN: case IR::OP_IMAX:
        case IR::OP_UMIN: case IR::OP_UMAX:
            if (a == b) RewriteAs(data, inst, IR::OP_LOAD_REG, a);
            return;
        case IR::OP_SELECT: {
            if (a == b && RewriteAs(data, inst, IR::OP_LOAD_REG, a)) return;
            // A condition that's all true or all false picks one operand.
            ConstantValue condition = GetKnownValue(data, ir->GetOperand(inst, 2));
            if (condition.state != ValueState::CONSTANT || GetScalarType(condition.type) != CoreType::BOOL) return;
            u32 set = 0;
            for (u32 c = 0; c < condition.count; c++) set += condition.bits[c] != 0;
            if (set == 0) RewriteAs(data, inst, IR::OP_LOAD_REG, a);
            else if (set == condition.count) RewriteAs(data, inst, IR::OP_LOAD_REG, b);
            return;
        }
        case IR::OP_VEC_EXTRACT:
            RewriteComponentRead(data, inst);
            return;
        case IR::OP_NOT: case IR::OP_FNEG: case IR::OP_INEG: {
            u16 inner = GetUnaryInput(data, a, op);
            if (inner != 0xFFFF) RewriteAs(data, inst, IR::OP_LOAD_REG, inner);
            return;
        }
        default:
            return;
    }
}

#include "bwsl_constant_propagation_ranges.inl"

static void RewriteConstants(ConstantPropagationData* data) {
    IRProgram* ir = data->ir;
    for (u32 i = 0; i < ir->instructionCount; i++) {
        RewriteOperands(data, i);
        u16 dest = ir->destinations[i];
        if (!IsRegisterOperand(data, dest) || data->states[dest] != ValueState::CONSTANT) {
            if (IsEnabled(data, OptimizationId::ALGEBRAIC_IDENTITIES)) {
                RewriteBitwiseIdentity(data, i);
                RewriteAlgebraicIdentity(data, i);
            }
            if (data->rangesReady && data->blockExecutable[data->cfg->instToBlock[i]]) {
                RewriteWithRanges(data, i, data->cfg->instToBlock[i]);
            }
        }
    }

    // A phi with a constant scalar result gets the constant on every edge,
    // which the backends treat as a copy of it. A vector phi keeps its
    // operands, whose definitions become constants themselves, except that
    // a pinned one (see FoldLoop) takes its entry operand on every edge: its
    // loop's back edge no longer runs.
    for (u32 reg = 0; reg < data->regCount; reg++) {
        if (!data->tracked[reg] || data->states[reg] != ValueState::CONSTANT) continue;
        u32 definition = data->definitions[reg];
        if (definition & PHI_REFERENCE) {
            u32 phi = definition & ~PHI_REFERENCE;
            u16 constant = data->pinned[reg] ? data->pinnedPhiOperand[phi]
                                             : GetConstantOperand(data, static_cast<u16>(reg));
            if (constant == 0xFFFF) continue;
            for (u32 j = 0; j < ir->GetPhiOperandCount(phi); j++) {
                ir->SetPhiOperandValue(phi, j, constant);
            }
            continue;
        }
        MaterializeConstant(data, definition, static_cast<u16>(reg));
    }
}

static void AddUndefRegister(ConstantPropagationData* data, u16 reg, u16 type) {
    IRProgram* ir = data->ir;
    if (ir->undefRegCount == ir->undefRegCapacity) {
        u32 capacity = std::max<u32>(ir->undefRegCapacity * 2, 16);
        u16* regs = (u16*)data->builder->pool->Allocate(capacity * sizeof(u16), 64);
        u16* types = (u16*)data->builder->pool->Allocate(capacity * sizeof(u16), 64);
        if (ir->undefRegCount > 0) {
            memcpy(regs, ir->undefRegs, ir->undefRegCount * sizeof(u16));
            memcpy(types, ir->undefRegTypes, ir->undefRegCount * sizeof(u16));
        }
        ir->undefRegs = regs;
        ir->undefRegTypes = types;
        ir->undefRegCapacity = capacity;
    }
    ir->undefRegs[ir->undefRegCount] = reg;
    ir->undefRegTypes[ir->undefRegCount] = type;
    ir->undefRegCount++;
    data->undef[reg] = true;
}

// The backends emit a branch or switch with a constant condition as a jump
// (see IR::TryGetConstantTarget), so the edges to the targets it skips
// disappear. Phis lose their operands for those edges. One left with none
// is in a block nothing branches to any more, and its result becomes an
// undef register.
static void DropFoldedBranchEdges(ConstantPropagationData* data, BWSL_Arena* arena) {
    IRProgram* ir = data->ir;
    const CFG* cfg = data->cfg;
    if (ir->phiCount == 0) return;
    bool* dropped = (bool*)AllocateZeroed(arena, ir->phiOperandOffsets[ir->phiCount] * sizeof(bool));
    bool anyDropped = false;
    for (u32 block = 0; block < cfg->blockCount; block++) {
        u32 target;
        if (!IR::TryGetConstantTarget(ir, cfg->lastInst[block], &target)) continue;
        u32 takenBlock = cfg->instToBlock[target];
        for (u32 s = 0; s < cfg->TotalSuccessorCount(block); s++) {
            u32 skippedBlock = cfg->GetAnySuccessor(block, s);
            if (skippedBlock == takenBlock || skippedBlock >= cfg->blockCount) continue;
            for (u32 k = data->blockPhiOffsets[skippedBlock]; k < data->blockPhiOffsets[skippedBlock + 1]; k++) {
                u32 phi = data->blockPhis[k];
                for (u32 j = ir->phiOperandOffsets[phi]; j < ir->phiOperandOffsets[phi + 1]; j++) {
                    if (ir->phiOperandBlocks[j] != block) continue;
                    dropped[j] = true;
                    anyDropped = true;
                }
            }
        }
    }
    if (!anyDropped) return;

    u32 kept = 0;
    for (u32 p = 0; p < ir->phiCount; p++) {
        u32 start = ir->phiOperandOffsets[p];
        u32 end = ir->phiOperandOffsets[p + 1];
        ir->phiOperandOffsets[p] = kept;
        for (u32 j = start; j < end; j++) {
            if (dropped[j]) continue;
            ir->phiOperandValues[kept] = ir->phiOperandValues[j];
            ir->phiOperandBlocks[kept] = ir->phiOperandBlocks[j];
            kept++;
        }
        if (kept == ir->phiOperandOffsets[p]) {
            data->phiRemoved[p] = true;
            AddUndefRegister(data, ir->phiResultRegs[p], ir->phiTypes[p]);
        }
    }
    ir->phiOperandOffsets[ir->phiCount] = kept;
}

// ============= Dead loops =============

// The instruction a block's terminator goes to when that's decided: a jump,
// a fallthrough, or a branch or switch on a constant, loop headers
// included. NO_BLOCK otherwise.
static u32 GetDecidedTarget(const IRProgram* ir, const CFG* cfg, u32 block) {
    u32 last = cfg->lastInst[block];
    u16 op = ir->opcodes[last];
    if (op == IR::OP_JUMP) return ir->metadata[last];
    if (op == IR::OP_BRANCH) {
        bool condition;
        if (!IR::TryGetConstantCondition(ir, ir->GetOperand(last, 0), &condition)) return NO_BLOCK;
        return condition ? ir->GetBranchTrueTarget(last) : ir->GetBranchFalseTarget(last);
    }
    if (op == IR::OP_SWITCH) {
        u32 selector;
        u32 switchId = ir->metadata[last];
        if (!IR::TryGetConstantInteger(ir, ir->GetOperand(last, 0), &selector) ||
            switchId >= ir->switchCount) {
            return NO_BLOCK;
        }
        u32 target = ir->GetSwitchDefaultTarget(switchId);
        for (u32 c = 0; c < ir->GetSwitchCaseCount(switchId); c++) {
            if (static_cast<u32>(ir->GetSwitchCaseValue(switchId, c)) == selector) {
                return ir->GetSwitchCaseTarget(switchId, c);
            }
        }
        return target;
    }
    if (op == IR::OP_RET || op == IR::OP_DISCARD || IR::IsTerminator(static_cast<IR::OpCode>(op))) return NO_BLOCK;
    // A block without a terminator falls through to the next one.
    return block + 1 < cfg->blockCount ? cfg->firstInst[block + 1] : NO_BLOCK;
}

// The loop header annotation in a block, or the instruction count.
static u32 FindLoopAnnotation(const IRProgram* ir, const CFG* cfg, u32 block) {
    for (u32 i = cfg->firstInst[block]; i <= cfg->lastInst[block]; i++) {
        if ((ir->structureInfo[i] & IR::IRProgram::STRUCT_TYPE_MASK) == IR::IRProgram::STRUCT_LOOP_HEADER) {
            return i;
        }
    }
    return ir->instructionCount;
}

// A loop runs at most once when every branch from its header on is decided
// and leads to its merge block without coming back: a condition that's
// constant on entry, or a body that always breaks (both left by constant
// propagation and loop evaluation). The blocks on that path become a chain
// of jumps without structure, so the backends emit them as plain code; the
// rest of the loop is unreachable. Loops are taken innermost first, so an
// outer loop's path can run through what was an inner loop.
static void RemoveDeadLoops(ConstantPropagationData* data, BWSL_Arena* arena) {
    IRProgram* ir = data->ir;
    CFG* cfg = data->cfg;
    if (!ir->structureInfo || !ir->continueInfo) return;
    u32* path = (u32*)AllocateZeroed(arena, cfg->blockCount * sizeof(u32));
    u32* next = (u32*)AllocateZeroed(arena, cfg->blockCount * sizeof(u32));
    bool* onPath = (bool*)AllocateZeroed(arena, cfg->blockCount * sizeof(bool));
    if (!path || !next || !onPath) return;

    bool anyRemoved = false;
    for (u32 header = cfg->blockCount; header-- > 0;) {
        u32 annotation = FindLoopAnnotation(ir, cfg, header);
        if (annotation >= ir->instructionCount) continue;
        u32 mergeInst = ir->structureInfo[annotation] & IR::IRProgram::STRUCT_TARGET_MASK;
        if (mergeInst >= ir->instructionCount) continue;
        u32 merge = cfg->instToBlock[mergeInst];

        // The blocks of a structured loop lie between its header and merge.
        u32 length = 0;
        u32 block = header;
        bool dead = true;
        while (block != merge) {
            if (block < header || block >= merge || onPath[block] ||
                (block != header && FindLoopAnnotation(ir, cfg, block) < ir->instructionCount)) {
                dead = false;
                break;
            }
            u32 target = GetDecidedTarget(ir, cfg, block);
            if (target >= ir->instructionCount) {
                dead = false;
                break;
            }
            onPath[block] = true;
            path[length] = block;
            next[length++] = cfg->instToBlock[target];
            block = cfg->instToBlock[target];
        }
        for (u32 k = 0; k < length; k++) onPath[path[k]] = false;
        if (!dead) continue;

        for (u32 k = 0; k < length; k++) {
            u32 b = path[k];
            for (u32 i = cfg->firstInst[b]; i <= cfg->lastInst[b]; i++) {
                ir->structureInfo[i] = 0;
                ir->continueInfo[i] = 0xFFFFFFFF;
            }
            u32 last = cfg->lastInst[b];
            u16 op = ir->opcodes[last];
            if (op == IR::OP_BRANCH || op == IR::OP_SWITCH) {
                ir->opcodes[last] = IR::OP_JUMP;
                ir->metadata[last] = cfg->firstInst[next[k]];
                for (u32 slot = 0; slot < 4; slot++) ir->SetOperand(last, slot, 0);
                ir->ClearBranchTargets(last);
            }
            cfg->successorCount[b] = 1;
            cfg->successors[b * 2] = next[k];
            cfg->successors[b * 2 + 1] = NO_BLOCK;
        }
        anyRemoved = true;
    }

    // A selection whose branch or switch is constant becomes a jump to the
    // arm it takes, so every backend sees just that arm (WGSL writes out
    // both otherwise, and the phis no longer have the other's edge). Loop
    // exits and `until` conditions keep their branch: WGSL's continuing
    // block needs it.
    for (u32 b = 0; b < cfg->blockCount; b++) {
        u32 last = cfg->lastInst[b];
        u32 kind = ir->structureInfo[last] & IR::IRProgram::STRUCT_TYPE_MASK;
        if (kind != IR::IRProgram::STRUCT_IF_HEADER && kind != IR::IRProgram::STRUCT_SWITCH_HEADER) continue;
        u32 target;
        if (!IR::TryGetConstantTarget(ir, last, &target) || target >= ir->instructionCount) continue;
        ir->opcodes[last] = IR::OP_JUMP;
        ir->metadata[last] = target;
        for (u32 slot = 0; slot < 4; slot++) ir->SetOperand(last, slot, 0);
        ir->ClearBranchTargets(last);
        ir->structureInfo[last] = 0;
        ir->continueInfo[last] = 0xFFFFFFFF;
        cfg->successorCount[b] = 1;
        cfg->successors[b * 2] = cfg->instToBlock[target];
        cfg->successors[b * 2 + 1] = NO_BLOCK;
        anyRemoved = true;
    }
    if (!anyRemoved) return;

    // Predecessors from the new successors, in place: there are fewer.
    u32* counts = (u32*)AllocateZeroed(arena, (cfg->blockCount + 1) * sizeof(u32));
    if (!counts) return;
    for (u32 b = 0; b < cfg->blockCount; b++) {
        for (u32 s = 0; s < cfg->TotalSuccessorCount(b); s++) {
            u32 successor = cfg->GetAnySuccessor(b, s);
            if (successor < cfg->blockCount) counts[successor + 1]++;
        }
    }
    for (u32 b = 0; b < cfg->blockCount; b++) counts[b + 1] += counts[b];
    memcpy(cfg->predecessorOffsets, counts, (cfg->blockCount + 1) * sizeof(u32));
    cfg->predecessorDataCount = counts[cfg->blockCount];
    for (u32 b = 0; b < cfg->blockCount; b++) {
        for (u32 s = 0; s < cfg->TotalSuccessorCount(b); s++) {
            u32 successor = cfg->GetAnySuccessor(b, s);
            if (successor < cfg->blockCount) cfg->predecessorData[counts[successor]++] = b;
        }
    }

    // Phis keep only the operands of edges that still exist. One left with
    // none is in a block nothing reaches any more.
    for (u32 p = 0; p < ir->phiCount; p++) {
        if (data->phiRemoved[p]) continue;
        u32 block = ir->phiBlockIndices[p];
        u32 kept = ir->phiOperandOffsets[p];
        for (u32 j = ir->phiOperandOffsets[p]; j < ir->phiOperandOffsets[p + 1]; j++) {
            bool edge = false;
            for (u32 k = cfg->predecessorOffsets[block]; k < cfg->predecessorOffsets[block + 1]; k++) {
                edge |= cfg->predecessorData[k] == ir->phiOperandBlocks[j];
            }
            if (!edge) continue;
            ir->phiOperandValues[kept] = ir->phiOperandValues[j];
            ir->phiOperandBlocks[kept] = ir->phiOperandBlocks[j];
            kept++;
        }
        // Shift the later phis' operands down over the dropped ones.
        u32 dropped = ir->phiOperandOffsets[p + 1] - kept;
        if (dropped == 0) continue;
        u32 end = ir->phiOperandOffsets[ir->phiCount];
        for (u32 j = ir->phiOperandOffsets[p + 1]; j < end; j++) {
            ir->phiOperandValues[j - dropped] = ir->phiOperandValues[j];
            ir->phiOperandBlocks[j - dropped] = ir->phiOperandBlocks[j];
        }
        for (u32 q = p + 1; q <= ir->phiCount; q++) ir->phiOperandOffsets[q] -= dropped;
        if (kept == ir->phiOperandOffsets[p]) {
            data->phiRemoved[p] = true;
            AddUndefRegister(data, ir->phiResultRegs[p], ir->phiTypes[p]);
        }
    }
}

// A phi whose emitted incoming edges all carry one value is a copy of it.
// Giving it that value on every edge lets the backends alias the two, so no
// phi is emitted. The value reaches the phi's block on every emitted path,
// so it dominates every use. Edges count by what the backends emit, not by
// what can run: a loop header keeps every edge, even when its condition is
// constant.
static void CollapseSingleValuePhis(ConstantPropagationData* data, BWSL_Arena* arena) {
    IRProgram* ir = data->ir;
    const CFG* cfg = data->cfg;
    if (ir->phiCount == 0) return;
    bool* reachable = (bool*)AllocateZeroed(arena, cfg->blockCount * sizeof(bool));
    bool* edgeEmitted = (bool*)AllocateZeroed(arena, cfg->predecessorDataCount * sizeof(bool));
    u32* queue = (u32*)AllocateZeroed(arena, cfg->blockCount * sizeof(u32));
    u32 head = 0, tail = 0;
    auto emitEdge = [&](u32 from, u32 to) {
        if (to >= cfg->blockCount) return;
        for (u32 k = cfg->predecessorOffsets[to]; k < cfg->predecessorOffsets[to + 1]; k++) {
            if (cfg->predecessorData[k] == from) edgeEmitted[k] = true;
        }
        if (!reachable[to]) {
            reachable[to] = true;
            queue[tail++] = to;
        }
    };
    auto isEdgeEmitted = [&](u32 from, u32 to) {
        for (u32 k = cfg->predecessorOffsets[to]; k < cfg->predecessorOffsets[to + 1]; k++) {
            if (cfg->predecessorData[k] == from && edgeEmitted[k]) return true;
        }
        return false;
    };

    // Mirrors SPIRVBuilder::EmitBranch and the direct GLES emitter.
    reachable[cfg->entryBlock] = true;
    queue[tail++] = cfg->entryBlock;
    while (head < tail) {
        u32 block = queue[head++];
        u32 target;
        if (IR::TryGetConstantTarget(ir, cfg->lastInst[block], &target)) {
            emitEdge(block, cfg->instToBlock[target]);
            continue;
        }
        for (u32 s = 0; s < cfg->TotalSuccessorCount(block); s++) {
            emitEdge(block, cfg->GetAnySuccessor(block, s));
        }
    }

    // Collapsing one phi can make another single-valued.
    bool changed = true;
    while (changed) {
        changed = false;
        for (u32 p = 0; p < ir->phiCount; p++) {
            u16 result = ir->phiResultRegs[p];
            u32 block = ir->phiBlockIndices[p];
            u16 value = 0xFFFF;
            bool single = true;
            for (u32 j = 0; j < ir->GetPhiOperandCount(p) && single; j++) {
                u16 operand = ir->GetPhiOperandValue(p, j);
                if (operand == result || !isEdgeEmitted(ir->GetPhiOperandBlock(p, j), block)) continue;
                if (value == 0xFFFF) value = operand;
                else single = operand == value;
            }
            if (!single || value == 0xFFFF) continue;
            for (u32 j = 0; j < ir->GetPhiOperandCount(p); j++) {
                if (ir->GetPhiOperandValue(p, j) == value) continue;
                ir->SetPhiOperandValue(p, j, value);
                changed = true;
            }
        }
    }
}

// A single-block stage skips SSA, so a local assigned twice keeps one
// register. One forward walk tracks each register's current value instead,
// rewriting operands as it goes.
static void PropagateStraightLine(ConstantPropagationData* data) {
    IRProgram* ir = data->ir;
    // A register read before any write holds whatever the backend set up.
    for (u32 reg = 0; reg < data->regCount; reg++) data->states[reg] = ValueState::OVERDEFINED;
    // definitions[] follows the walk: the assignment reaching each
    // instruction. It ends where FindDefinitions left it.
    memset(data->definitions, 0xFF, data->regCount * sizeof(u32));

    for (u32 i = 0; i < ir->instructionCount; i++) {
        u16 op = ir->opcodes[i];
        if (op == IR::OP_NOP) continue;
        RewriteOperands(data, i);
        if (IsEnabled(data, OptimizationId::ALGEBRAIC_IDENTITIES)) {
            RewriteBitwiseIdentity(data, i);
            RewriteAlgebraicIdentity(data, i);
        }
        if (data->rangesReady) {
            RecordVaryingRange(data, i, 0);
            RewriteWithRanges(data, i, 0);
        }
        u16 dest = ir->destinations[i];
        if (IsRegisterOperand(data, dest) && !IsUseInDestination(op) && op != IR::OP_JUMP &&
            op != IR::OP_BRANCH && op != IR::OP_SWITCH && op != IR::OP_RET) {
            data->definitions[dest] = i;
        }
        if (!IsRegisterOperand(data, dest) || !data->tracked[dest] || IsUseInDestination(op) ||
            op == IR::OP_JUMP || op == IR::OP_BRANCH || op == IR::OP_SWITCH || op == IR::OP_RET) {
            continue;
        }
        ConstantValue value = EvaluateInstruction(data, i);
        CoreType type = GetRegisterType(ir, dest);
        bool fits = value.type == type && value.count == GetComponentCount(type);
        bool isConstant = value.state == ValueState::CONSTANT && fits;
        // A vector with some known components keeps them too.
        bool isPartial = value.state == ValueState::OVERDEFINED && value.known != 0 && fits;
        SetRegisterValue(data, dest, isConstant || isPartial ? value : OVERDEFINED_VALUE);
        // The walk's range for the register from here on; one that's a
        // single number is a constant. Float zeros aren't: they could be -0.0.
        if (data->rangesReady && HasRange(data, dest)) {
            u32 offset = data->bitOffsets[dest];
            u32 count = data->bitOffsets[dest + 1] - offset;
            CoreType scalar = GetScalarType(type);
            ConstantValue fixed = {ValueState::CONSTANT, type, static_cast<u8>(count), {}, 0};
            bool point = !isConstant && count == GetComponentCount(type);
            for (u32 c = 0; c < count; c++) {
                ValueRange range = isConstant ? PointRange(scalar, value.bits[c]) : EvaluateComponentRange(data, i, c, 0);
                data->ranges[offset + c] = range;
                point = point && IsPoint(range) && !(IsFloatScalar(scalar) && range.low == 0.0);
                if (point) {
                    fixed.bits[c] = IsFloatScalar(scalar) ? FloatToBits(static_cast<float>(range.low))
                                  : scalar == CoreType::INT ? static_cast<u32>(static_cast<s32>(range.low))
                                                            : static_cast<u32>(range.low);
                }
            }
            if (point) {
                SetRegisterValue(data, dest, fixed);
                isConstant = true;
            }
        }
        if (isConstant) MaterializeConstant(data, i, dest);
    }
}

// ============= Dead code =============

static bool IsRemovableDefinition(const ConstantPropagationData* data, u16 reg) {
    if (data->definitionCounts[reg] != 1) return false;
    if (data->ir->registerStorageInfo && data->ir->registerStorageInfo[reg] != 0) return false;
    u32 definition = data->definitions[reg];
    if (definition & PHI_REFERENCE) return true;
    u16 op = data->ir->opcodes[definition];
    return GetFoldArity(op) != 0 || IsPureValueOp(op) || op == IR::OP_MAT_DET ||
           op == IR::OP_MAT_INVERSE || (op >= IR::OP_LENGTH && op <= IR::OP_FACEFORWARD) ||
           op == IR::OP_ANY || op == IR::OP_ALL || op == IR::OP_STRUCT_ARRAY_EXTRACT ||
           op == IR::OP_VEC_EXTRACT || op == IR::OP_VEC_SHUFFLE;
}

// Removes the value instructions and phis that nothing reads any more,
// including ones that only fed instructions removed here.
static void RemoveDeadValues(ConstantPropagationData* data, BWSL_Arena* arena) {
    IRProgram* ir = data->ir;
    u32 regCount = data->regCount;
    u32* useCounts = (u32*)arena->Allocate(regCount * sizeof(u32), 64);
    bool* queued = (bool*)arena->Allocate(regCount * sizeof(bool), 64);
    u32* work = (u32*)arena->Allocate(regCount * sizeof(u32), 64);
    bool* phiDead = (bool*)arena->Allocate(std::max<u32>(ir->phiCount, 1) * sizeof(bool), 64);
    memset(useCounts, 0, regCount * sizeof(u32));
    memset(queued, 0, regCount * sizeof(bool));
    memset(phiDead, 0, std::max<u32>(ir->phiCount, 1) * sizeof(bool));
    if (data->phiRemoved) memcpy(phiDead, data->phiRemoved, ir->phiCount * sizeof(bool));

    // Count operands as uses, literal indices included: an over-count only
    // keeps a value alive. A destination counts too unless it's the
    // register's recorded definition: STORE_OUTPUT and LOCAL_STORE carry a
    // value register there.
    for (u32 i = 0; i < ir->instructionCount; i++) {
        if (ir->opcodes[i] == IR::OP_NOP) continue;
        for (u32 slot = 0; slot < 4 && !HasSlotOperands(ir->opcodes[i]); slot++) {
            u16 operand = ir->GetOperand(i, slot);
            if (IsRegisterOperand(data, operand)) useCounts[operand]++;
        }
        u16 dest = ir->destinations[i];
        if (IsRegisterOperand(data, dest) && data->definitions[dest] != i) useCounts[dest]++;
    }
    for (u32 p = 0; p < ir->phiCount; p++) {
        for (u32 j = 0; j < ir->GetPhiOperandCount(p); j++) {
            u16 operand = ir->GetPhiOperandValue(p, j);
            if (IsRegisterOperand(data, operand)) useCounts[operand]++;
        }
    }
    // Instructions still to be inserted read their operands too.
    for (u32 k = 0; k < data->insertions.count; k++) {
        for (u32 slot = 0; slot < 4; slot++) {
            u16 operand = data->insertions.items[k].operands[slot];
            if (IsRegisterOperand(data, operand)) useCounts[operand]++;
        }
    }

    // Without DEAD_VALUES only the phis DropFoldedBranchEdges emptied go.
    u32 workCount = 0;
    bool removeDead = IsEnabled(data, OptimizationId::DEAD_VALUES);
    for (u32 reg = 0; reg < regCount && removeDead; reg++) {
        if (useCounts[reg] == 0 && IsRemovableDefinition(data, static_cast<u16>(reg))) {
            queued[reg] = true;
            work[workCount++] = reg;
        }
    }
    auto release = [&](u16 operand) {
        if (!IsRegisterOperand(data, operand) || useCounts[operand] == 0) return;
        if (--useCounts[operand] == 0 && !queued[operand] && IsRemovableDefinition(data, operand)) {
            queued[operand] = true;
            work[workCount++] = operand;
        }
    };

    while (workCount > 0) {
        u32 reg = work[--workCount];
        u32 definition = data->definitions[reg];
        if (definition & PHI_REFERENCE) {
            u32 phi = definition & ~PHI_REFERENCE;
            phiDead[phi] = true;
            for (u32 j = 0; j < ir->GetPhiOperandCount(phi); j++) release(ir->GetPhiOperandValue(phi, j));
            continue;
        }
        for (u32 slot = 0; slot < 4; slot++) release(ir->GetOperand(definition, slot));
        ir->opcodes[definition] = IR::OP_NOP;
        ir->destinations[definition] = 0;
        for (u32 slot = 0; slot < 4; slot++) ir->SetOperand(definition, slot, 0xFFFF);
    }

    // Values that only read each other keep each other's use counts up: a
    // loop variable that its own update and its phis read, and nothing else
    // (a ray's hit index when only the distance is used). Mark what the rest
    // reads live, and what live values read; removable values left unmarked
    // are dead too.
    bool* live = (bool*)arena->Allocate(regCount * sizeof(bool), 64);
    memset(live, 0, regCount * sizeof(bool));
    workCount = 0;
    auto markLive = [&](u16 operand) {
        if (IsRegisterOperand(data, operand) && !live[operand]) {
            live[operand] = true;
            work[workCount++] = operand;
        }
    };
    auto markOperands = [&](u32 inst) {
        for (u32 slot = 0; slot < 4 && !HasSlotOperands(ir->opcodes[inst]); slot++) markLive(ir->GetOperand(inst, slot));
    };
    // A copy into a variable's register (SSA keeps those for naming, so the
    // register may have several) reads its source only for whoever reads
    // the register.
    auto isCopyInto = [&](u32 inst) {
        u16 op = ir->opcodes[inst];
        u16 dest = ir->destinations[inst];
        return (op == IR::OP_STORE_REG || op == IR::OP_LOAD_REG) && IsRegisterOperand(data, dest) &&
               (!ir->registerStorageInfo || ir->registerStorageInfo[dest] == 0);
    };
    for (u32 i = 0; i < ir->instructionCount; i++) {
        if (ir->opcodes[i] == IR::OP_NOP || isCopyInto(i)) continue;
        u16 dest = ir->destinations[i];
        bool definesValue = IsRegisterOperand(data, dest) && data->definitions[dest] == i;
        if (definesValue && IsRemovableDefinition(data, dest)) continue;  // Live only if read
        markOperands(i);
        if (IsRegisterOperand(data, dest) && !definesValue) markLive(dest);
    }
    for (u32 p = 0; p < ir->phiCount; p++) {
        u16 result = ir->phiResultRegs[p];
        if (phiDead[p] || (IsRegisterOperand(data, result) && IsRemovableDefinition(data, result))) continue;
        for (u32 j = 0; j < ir->GetPhiOperandCount(p); j++) markLive(ir->GetPhiOperandValue(p, j));
    }
    for (u32 k = 0; k < data->insertions.count; k++) {
        for (u32 slot = 0; slot < 4; slot++) markLive(data->insertions.items[k].operands[slot]);
    }
    for (bool more = true; more;) {
        while (workCount > 0) {
            u32 reg = work[--workCount];
            u32 definition = data->definitions[reg];
            if (definition == NO_DEFINITION || data->definitionCounts[reg] != 1) continue;
            if (definition & PHI_REFERENCE) {
                u32 phi = definition & ~PHI_REFERENCE;
                if (phiDead[phi]) continue;
                for (u32 j = 0; j < ir->GetPhiOperandCount(phi); j++) markLive(ir->GetPhiOperandValue(phi, j));
            } else if (ir->opcodes[definition] != IR::OP_NOP) {
                markOperands(definition);
            }
        }
        // Copies into registers that became live, whichever definition.
        for (u32 i = 0; i < ir->instructionCount; i++) {
            if (isCopyInto(i) && live[ir->destinations[i]]) markOperands(i);
        }
        more = workCount > 0;
    }
    // Copies into registers nothing reads. A copy of a constant stays: the
    // backends name the constant after the local (see
    // RemoveUnreadAssignments), and it keeps nothing else alive.
    for (u32 i = 0; i < ir->instructionCount; i++) {
        if (!isCopyInto(i) || live[ir->destinations[i]] || IR::IsConstant(ir->GetOperand(i, 0))) continue;
        ir->opcodes[i] = IR::OP_NOP;
        ir->destinations[i] = 0;
        for (u32 slot = 0; slot < 4; slot++) ir->SetOperand(i, slot, 0xFFFF);
    }
    for (u32 reg = 0; reg < regCount; reg++) {
        if (live[reg] || !IsRemovableDefinition(data, static_cast<u16>(reg))) continue;
        u32 definition = data->definitions[reg];
        if (definition & PHI_REFERENCE) {
            phiDead[definition & ~PHI_REFERENCE] = true;
        } else if (ir->opcodes[definition] != IR::OP_NOP) {
            ir->opcodes[definition] = IR::OP_NOP;
            ir->destinations[definition] = 0;
            for (u32 slot = 0; slot < 4; slot++) ir->SetOperand(definition, slot, 0xFFFF);
        }
    }

    // Compact the phi arrays in place, keeping their order.
    u32 keptPhis = 0;
    u32 keptOperands = 0;
    for (u32 p = 0; p < ir->phiCount; p++) {
        if (phiDead[p]) continue;
        u32 opStart = ir->phiOperandOffsets[p];
        u32 opEnd = ir->phiOperandOffsets[p + 1];
        ir->phiBlockIndices[keptPhis] = ir->phiBlockIndices[p];
        ir->phiResultRegs[keptPhis] = ir->phiResultRegs[p];
        ir->phiTypes[keptPhis] = ir->phiTypes[p];
        ir->phiOperandOffsets[keptPhis] = keptOperands;
        for (u32 k = opStart; k < opEnd; k++) {
            ir->phiOperandValues[keptOperands] = ir->phiOperandValues[k];
            ir->phiOperandBlocks[keptOperands] = ir->phiOperandBlocks[k];
            keptOperands++;
        }
        keptPhis++;
    }
    if (ir->phiCount > 0) {
        ir->phiOperandOffsets[keptPhis] = keptOperands;
        ir->phiCount = keptPhis;
    }
}

// A straight-line stage assigns a variable's register more than once, so
// RemoveDeadValues, which needs one definition, leaves it alone. Walking
// back from the end instead, an assignment whose value nothing after it
// reads is dead: the value instructions among those are removed. An
// assignment that reads the register it writes, like `v.w = 4.0`, is no
// reason to keep the ones before it alive by itself.
//
// A dead copy of a constant into a named local stays, without making
// anything else live: the backends name the constant after it.
static void RemoveUnreadAssignments(ConstantPropagationData* data, BWSL_Arena* arena) {
    IRProgram* ir = data->ir;
    bool* live = (bool*)AllocateZeroed(arena, data->regCount * sizeof(bool));
    if (!live) return;
    auto markRead = [&](u16 operand) {
        if (IsRegisterOperand(data, operand)) live[operand] = true;
    };
    u32 next = data->insertions.count;
    for (u32 i = ir->instructionCount; i-- > 0;) {
        u16 op = ir->opcodes[i];
        u16 dest = ir->destinations[i];
        if (op != IR::OP_NOP) {
            // A constant copied into a named local names the constant in the
            // debug info, and costs nothing, so it stays.
            bool namesConstant = (op == IR::OP_STORE_REG || op == IR::OP_LOAD_REG) &&
                                 IR::IsConstant(ir->GetOperand(i, 0)) && ir->registerNameHashes &&
                                 IsRegisterOperand(data, dest) && ir->registerNameHashes[dest] != 0;
            bool removable = !IsUseInDestination(op) && IsRegisterOperand(data, dest) &&
                             !IsMemoryBacked(ir, dest) && !namesConstant &&
                             (GetFoldArity(op) != 0 || IsPureValueOp(op) || IsVectorFoldOp(op));
            if (removable && !live[dest]) {
                ir->opcodes[i] = IR::OP_NOP;
                ir->destinations[i] = 0;
                for (u32 slot = 0; slot < 4; slot++) ir->SetOperand(i, slot, 0xFFFF);
            } else {
                if (removable) live[dest] = false;  // Overwritten here
                // Jumps, returns and discards carry no value; their operand
                // slots aren't registers.
                bool carriesValues = op != IR::OP_JUMP && op != IR::OP_RET && op != IR::OP_DISCARD;
                for (u32 slot = 0; slot < 4 && carriesValues && !HasSlotOperands(op); slot++) {
                    markRead(ir->GetOperand(i, slot));
                }
                if (IsUseInDestination(op)) markRead(dest);
            }
        }
        // Instructions still to be inserted in front of this one read too.
        while (next > 0 && data->insertions.items[next - 1].before == i) {
            const InsertedInstruction& item = data->insertions.items[--next];
            for (u32 slot = 0; slot < 4; slot++) markRead(item.operands[slot]);
        }
    }
}

// ============= Setup =============

// Straight-line stages track registers assigned more than once, too: the
// forward walk knows which assignment is current.
static void FindDefinitions(ConstantPropagationData* data, bool straightLine) {
    IRProgram* ir = data->ir;
    for (u32 i = 0; i < ir->instructionCount; i++) {
        u16 op = ir->opcodes[i];
        if (op == IR::OP_NOP || op == IR::OP_JUMP || op == IR::OP_BRANCH ||
            op == IR::OP_SWITCH || op == IR::OP_RET || IsUseInDestination(op)) {
            continue;
        }
        u16 dest = ir->destinations[i];
        if (!IsRegisterOperand(data, dest)) continue;
        if (data->definitionCounts[dest] < 2) data->definitionCounts[dest]++;
        data->definitions[dest] = i;
    }
    for (u32 p = 0; p < ir->phiCount; p++) {
        u16 result = ir->phiResultRegs[p];
        if (!IsRegisterOperand(data, result)) continue;
        if (data->definitionCounts[result] < 2) data->definitionCounts[result]++;
        data->definitions[result] = PHI_REFERENCE | p;
    }
    for (u32 i = 0; ir->undefRegs && i < ir->undefRegCount; i++) {
        if (ir->undefRegs[i] < data->regCount) data->undef[ir->undefRegs[i]] = true;
    }
    for (u32 reg = 0; reg < data->regCount; reg++) {
        data->tracked[reg] = (straightLine || data->definitionCounts[reg] == 1) && !data->undef[reg] &&
            IsFoldedType(GetRegisterType(ir, static_cast<u16>(reg))) &&
            !(ir->registerStorageInfo && ir->registerStorageInfo[reg] != 0);
    }
}

static void BuildUseLists(ConstantPropagationData* data) {
    IRProgram* ir = data->ir;
    u32* offsets = data->useOffsets;
    for (u32 i = 0; i < ir->instructionCount; i++) {
        if (ir->opcodes[i] == IR::OP_NOP) continue;
        for (u32 slot = 0; slot < 4; slot++) {
            u16 operand = ir->GetOperand(i, slot);
            if (IsRegisterOperand(data, operand)) offsets[operand + 1]++;
        }
    }
    for (u32 p = 0; p < ir->phiCount; p++) {
        for (u32 j = 0; j < ir->GetPhiOperandCount(p); j++) {
            u16 operand = ir->GetPhiOperandValue(p, j);
            if (IsRegisterOperand(data, operand)) offsets[operand + 1]++;
        }
    }
    for (u32 reg = 0; reg < data->regCount; reg++) offsets[reg + 1] += offsets[reg];

    // Fill using the offsets as cursors, then shift them back.
    for (u32 i = 0; i < ir->instructionCount; i++) {
        if (ir->opcodes[i] == IR::OP_NOP) continue;
        for (u32 slot = 0; slot < 4; slot++) {
            u16 operand = ir->GetOperand(i, slot);
            if (IsRegisterOperand(data, operand)) data->useData[offsets[operand]++] = i;
        }
    }
    for (u32 p = 0; p < ir->phiCount; p++) {
        for (u32 j = 0; j < ir->GetPhiOperandCount(p); j++) {
            u16 operand = ir->GetPhiOperandValue(p, j);
            if (IsRegisterOperand(data, operand)) data->useData[offsets[operand]++] = PHI_REFERENCE | p;
        }
    }
    for (u32 reg = data->regCount; reg > 0; reg--) offsets[reg] = offsets[reg - 1];
    offsets[0] = 0;
}

// Every register something reads: as an operand, a phi operand, or the
// value an instruction like STORE_OUTPUT carries in its destination.
static void FindReads(ConstantPropagationData* data) {
    const IRProgram* ir = data->ir;
    for (u32 i = 0; i < ir->instructionCount; i++) {
        u16 op = ir->opcodes[i];
        if (op == IR::OP_NOP) continue;
        for (u32 slot = 0; slot < 4 && !HasSlotOperands(op); slot++) {
            u16 operand = ir->GetOperand(i, slot);
            if (IsRegisterOperand(data, operand)) data->readAnywhere[operand] = true;
        }
        u16 dest = ir->destinations[i];
        if (IsRegisterOperand(data, dest) && data->definitions[dest] != i) data->readAnywhere[dest] = true;
    }
    for (u32 p = 0; p < ir->phiCount; p++) {
        for (u32 j = 0; j < ir->GetPhiOperandCount(p); j++) {
            u16 operand = ir->GetPhiOperandValue(p, j);
            if (IsRegisterOperand(data, operand)) data->readAnywhere[operand] = true;
        }
    }
}

static void GroupPhisByBlock(ConstantPropagationData* data) {
    const IRProgram* ir = data->ir;
    u32 blockCount = data->cfg->blockCount;
    u32* offsets = data->blockPhiOffsets;
    for (u32 p = 0; p < ir->phiCount; p++) {
        if (ir->phiBlockIndices[p] < blockCount) offsets[ir->phiBlockIndices[p] + 1]++;
    }
    for (u32 b = 0; b < blockCount; b++) offsets[b + 1] += offsets[b];
    for (u32 p = 0; p < ir->phiCount; p++) {
        u32 block = ir->phiBlockIndices[p];
        if (block < blockCount) data->blockPhis[offsets[block]++] = p;
    }
    for (u32 b = blockCount; b > 0; b--) offsets[b] = offsets[b - 1];
    offsets[0] = 0;
}

void PropagateConstants(IR::IRBuilder* builder, CFG* cfg, OptimizationSet optimizations) {
    IRProgram* ir = builder->program;
    if (ir->instructionCount == 0 || !ir->registerTypes) return;
    if (cfg && cfg->blockCount == 0) cfg = nullptr;
    if (!Optimizations::IsEnabled(optimizations, OptimizationId::CONSTANT_PROPAGATION)) {
        if (Optimizations::IsEnabled(optimizations, OptimizationId::COMMON_SUBEXPRESSIONS)) {
            EliminateCommonSubexpressions(builder, cfg);
        }
        return;
    }
    bool straightLine = !cfg || cfg->blockCount == 1;
    if (straightLine && ir->phiCount > 0) return;

    u32 regCount = std::min<u32>(ir->registerCount, IR::RegisterLimit);
    u32 phiOperandCount = ir->phiCount > 0 ? ir->phiOperandOffsets[ir->phiCount] : 0;
    u32 maxUses = ir->instructionCount * 4 + phiOperandCount;
    u32 blockCount = cfg ? cfg->blockCount : 0;
    u32 edgeCount = cfg ? cfg->predecessorDataCount : 0;
    u32 bitCount = 0;
    for (u32 reg = 0; reg < regCount; reg++) {
        bitCount += std::max<u32>(GetComponentCount(GetRegisterType(ir, static_cast<u16>(reg))), 1);
    }

    // Everything is sized up front, so the arena can't run out.
    size_t bytes = size_t(regCount) * (sizeof(ValueState) + sizeof(u32) * 4 + sizeof(bool) * 3 + 1) +
                   size_t(bitCount + regCount * 2) * sizeof(u32) +  // regWork
                   size_t(regCount) * sizeof(u16) * 2 +  // known, savedKnown
                   size_t(regCount) * sizeof(bool) + 64 +  // RemoveUnreadAssignments
                   size_t(regCount + 1) * sizeof(u32) + size_t(maxUses) * sizeof(u32) +
                   size_t(blockCount) * (sizeof(bool) * 2 + sizeof(u32) * 3) + 2 * sizeof(u32) +
                   size_t(edgeCount) * sizeof(bool) * 2 + size_t(phiOperandCount) * sizeof(bool) +
                   size_t(ir->phiCount) * (sizeof(u32) + sizeof(bool) * 2) +
                   size_t(regCount + 1 + bitCount) * sizeof(u32) +
                   // Loops: pinned, readAnywhere, savedStates; pinnedBits, savedBits;
                   // pinnedPhiOperand; loopFolded, inLoop, blockStack.
                   size_t(regCount) * (sizeof(bool) * 2 + sizeof(ValueState)) +
                   size_t(bitCount) * sizeof(u32) * 2 + size_t(ir->phiCount) * sizeof(u16) +
                   size_t(blockCount) * (sizeof(bool) * 2 + sizeof(u32)) +
                   // Dead loops: path, next, onPath, predecessor counts.
                   size_t(blockCount) * (sizeof(u32) * 3 + sizeof(bool)) + sizeof(u32) +
                   // Store forwarding: per register flags, counts, definitions and
                   // addresses; per block what it stored.
                   size_t(regCount) * (sizeof(bool) * 2 + 2 + sizeof(u32) + sizeof(LocalAddress)) +
                   size_t(std::max<u32>(blockCount, 1)) * (LOCAL_STORES_TRACKED * (sizeof(LocalAddress) + sizeof(u16) + 2) + 2) + 9 * 64 +
                   // Ranges: ranges, rangeChanges; blockConstraints, constraints.
                   size_t(bitCount) * sizeof(ValueRange) + size_t(regCount) +
                   size_t(blockCount) * sizeof(u32) + size_t(blockCount * 8 + 16) * sizeof(RangeConstraint) +
                   56 * 64;
    std::vector<char> memory(bytes);
    BWSL_Arena arena;
    arena.Initialize(memory.data(), memory.size());

    ConstantPropagationData data = {};
    data.ir = ir;
    data.builder = builder;
    data.cfg = cfg;
    data.regCount = regCount;
    data.straightLine = straightLine;
    data.optimizations = optimizations;
    data.states = (ValueState*)AllocateZeroed(&arena, regCount * sizeof(ValueState));
    data.bitOffsets = (u32*)AllocateZeroed(&arena, (regCount + 1) * sizeof(u32));
    data.bits = (u32*)AllocateZeroed(&arena, bitCount * sizeof(u32));
    data.tracked = (bool*)AllocateZeroed(&arena, regCount * sizeof(bool));
    data.undef = (bool*)AllocateZeroed(&arena, regCount * sizeof(bool));
    data.definitions = (u32*)AllocateZeroed(&arena, regCount * sizeof(u32));
    data.definitionCounts = (u8*)AllocateZeroed(&arena, regCount);
    data.useOffsets = (u32*)AllocateZeroed(&arena, (regCount + 1) * sizeof(u32));
    data.useData = (u32*)AllocateZeroed(&arena, maxUses * sizeof(u32));
    data.regWork = (u32*)AllocateZeroed(&arena, (bitCount + regCount * 2) * sizeof(u32));
    data.known = (u16*)AllocateZeroed(&arena, regCount * sizeof(u16));
    data.savedKnown = (u16*)AllocateZeroed(&arena, regCount * sizeof(u16));
    data.ranges = (ValueRange*)AllocateZeroed(&arena, bitCount * sizeof(ValueRange));
    data.rangeChanges = (u8*)AllocateZeroed(&arena, regCount);
    data.blockConstraints = (u32*)AllocateZeroed(&arena, blockCount * sizeof(u32));
    data.constraintCapacity = blockCount * 8 + 16;
    data.constraints = (RangeConstraint*)AllocateZeroed(&arena, data.constraintCapacity * sizeof(RangeConstraint));
    data.rangesReady = straightLine && IsEnabled(&data, OptimizationId::VALUE_RANGES);
    data.blockExecutable = (bool*)AllocateZeroed(&arena, blockCount * sizeof(bool));
    data.edgeExecutable = (bool*)AllocateZeroed(&arena, edgeCount * sizeof(bool));
    data.blockPhiOffsets = (u32*)AllocateZeroed(&arena, (blockCount + 1) * sizeof(u32));
    data.blockPhis = (u32*)AllocateZeroed(&arena, ir->phiCount * sizeof(u32));
    data.phiRemoved = (bool*)AllocateZeroed(&arena, ir->phiCount * sizeof(bool));
    data.blockWork = (u32*)AllocateZeroed(&arena, blockCount * sizeof(u32));
    data.pinned = (bool*)AllocateZeroed(&arena, regCount * sizeof(bool));
    data.pinnedBits = (u32*)AllocateZeroed(&arena, bitCount * sizeof(u32));
    data.pinnedPhiOperand = (u16*)AllocateZeroed(&arena, ir->phiCount * sizeof(u16));
    data.readAnywhere = (bool*)AllocateZeroed(&arena, regCount * sizeof(bool));
    data.loopFolded = (bool*)AllocateZeroed(&arena, blockCount * sizeof(bool));
    data.inLoop = (bool*)AllocateZeroed(&arena, blockCount * sizeof(bool));
    data.blockStack = (u32*)AllocateZeroed(&arena, blockCount * sizeof(u32));
    data.savedStates = (ValueState*)AllocateZeroed(&arena, regCount * sizeof(ValueState));
    data.savedBits = (u32*)AllocateZeroed(&arena, bitCount * sizeof(u32));
    if (!data.states || !data.bitOffsets || !data.bits || !data.tracked || !data.undef || !data.definitions ||
        !data.definitionCounts || !data.useOffsets || !data.useData || !data.regWork ||
        !data.blockExecutable || !data.edgeExecutable || !data.blockPhiOffsets ||
        !data.blockPhis || !data.phiRemoved || !data.blockWork || !data.pinned || !data.pinnedBits ||
        !data.pinnedPhiOperand || !data.readAnywhere || !data.loopFolded || !data.inLoop ||
        !data.blockStack || !data.savedStates || !data.savedBits || !data.known || !data.savedKnown ||
        !data.ranges || !data.rangeChanges || !data.blockConstraints || !data.constraints) {
        return;
    }
    memset(data.definitions, 0xFF, regCount * sizeof(u32));
    memset(data.pinnedPhiOperand, 0xFF, ir->phiCount * sizeof(u16));
    for (u32 reg = 0; reg < regCount; reg++) {
        u32 count = std::max<u32>(GetComponentCount(GetRegisterType(ir, static_cast<u16>(reg))), 1);
        data.bitOffsets[reg + 1] = data.bitOffsets[reg] + count;
    }

    if (IsEnabled(&data, OptimizationId::STORE_FORWARDING)) ForwardLocalStores(&data, &arena);
    FindDefinitions(&data, straightLine);
    if (straightLine) {
        PropagateStraightLine(&data);
    } else {
        BuildUseLists(&data);
        FindReads(&data);
        GroupPhisByBlock(&data);
        PropagateWithLoops(&data);
        RecordVaryingRanges(&data);
        RewriteConstants(&data);
        DropFoldedBranchEdges(&data, &arena);
        if (IsEnabled(&data, OptimizationId::SINGLE_ITERATION_LOOPS)) RemoveDeadLoops(&data, &arena);
        CollapseSingleValuePhis(&data, &arena);
    }
    ExportVaryingRanges(&data);
    RemoveDeadValues(&data, &arena);
    if (straightLine && IsEnabled(&data, OptimizationId::DEAD_VALUES)) {
        RemoveUnreadAssignments(&data, &arena);
    }
    ApplyInsertions(builder, cfg, &data.insertions);
    if (IsEnabled(&data, OptimizationId::COMMON_SUBEXPRESSIONS)) EliminateCommonSubexpressions(builder, cfg);
}

}  // namespace Optimization
}  // namespace BWSL
