// BWSL Direct GLSL ES 300 Backend
// Emits GLSL ES directly from IR - no SPIR-V intermediate step
// Architecture: Arena allocation, lookup tables, compile-time string interning
//
// Uses the same IR::IRProgram that SPIRVBuilder uses, but outputs GLSL text directly

#pragma once

#include "bwsl_defs.h"
#include "bwsl_arena.h"
#include "bwsl_ir_gen.h"
#include "bwsl_types.h"
#include "bwsl_cfg.h"
#include "bwsl_render_config.h"
#include "bwsl_ast_soa.h"
#include "bwsl_ir_lowering.h"  // For PassVaryingContext, VaryingInfo
#include "bwsl_ir_analysis.h"  // For IRAnalysis

namespace BWSL {
namespace GLES {

// ============================================================================
// Compile-time interned strings - zero allocation
// ============================================================================

namespace Str {
    // Type names - indexed by CoreType enum value
    // CoreType order: INVALID=0, BOOL=1, INT=2, UINT=3, FLOAT=4,
    //                 BOOL2=5, BOOL3=6, BOOL4=7, INT2=8, INT3=9, INT4=10,
    //                 UINT2=11, UINT3=12, UINT4=13, FLOAT2=14, FLOAT3=15, FLOAT4=16,
    //                 MAT2=17, MAT3=18, MAT4=19, VOID=20
    static constexpr const char* TYPE_NAMES[] = {
        "void",   // INVALID (0) - treated as void
        "bool",   // BOOL (1)
        "int",    // INT (2)
        "uint",   // UINT (3)
        "float",  // FLOAT (4)
        "bvec2",  // BOOL2 (5)
        "bvec3",  // BOOL3 (6)
        "bvec4",  // BOOL4 (7)
        "ivec2",  // INT2 (8)
        "ivec3",  // INT3 (9)
        "ivec4",  // INT4 (10)
        "uvec2",  // UINT2 (11)
        "uvec3",  // UINT3 (12)
        "uvec4",  // UINT4 (13)
        "vec2",   // FLOAT2 (14)
        "vec3",   // FLOAT3 (15)
        "vec4",   // FLOAT4 (16)
        "mat2",   // MAT2 (17)
        "mat3",   // MAT3 (18)
        "mat4",   // MAT4 (19)
        "void",   // VOID (20)
    };

    // Swizzle components
    static constexpr char SWIZZLE[] = {'x', 'y', 'z', 'w'};
}

// ============================================================================
// Opcode -> GLSL operator/function lookup table
// Indexed by (opcode - base) for each category
// ============================================================================

struct OpInfo {
    const char* str;      // Operator string or function name
    u8 arity;             // 1=unary, 2=binary, 3=ternary
    u8 isFunction;        // 0=infix operator, 1=function call
};

// Arithmetic ops: OP_FADD (0x20) through OP_FMA (0x2F)
static constexpr OpInfo ARITH_OPS[] = {
    {"+", 2, 0},          // OP_FADD
    {"-", 2, 0},          // OP_FSUB
    {"*", 2, 0},          // OP_FMUL
    {"/", 2, 0},          // OP_FDIV
    {"mod", 2, 1},        // OP_FMOD
    {"-", 1, 0},          // OP_FNEG (unary)
    {"abs", 1, 1},        // OP_FABS
    {"min", 2, 1},        // OP_FMIN
    {"max", 2, 1},        // OP_FMAX
    {"clamp", 3, 1},      // OP_FCLAMP
    {"floor", 1, 1},      // OP_FLOOR
    {"ceil", 1, 1},       // OP_CEIL
    {"round", 1, 1},      // OP_ROUND (roundEven in GLSL)
    {"trunc", 1, 1},      // OP_TRUNC
    {"fract", 1, 1},      // OP_FRACT
    {"fma", 3, 1},        // OP_FMA
};

// Integer ops: OP_IADD (0x30) through OP_UCLAMP (0x3C)
static constexpr OpInfo INT_OPS[] = {
    {"+", 2, 0},          // OP_IADD
    {"-", 2, 0},          // OP_ISUB
    {"*", 2, 0},          // OP_IMUL
    {"/", 2, 0},          // OP_IDIV
    {"%", 2, 0},          // OP_IMOD
    {"-", 1, 0},          // OP_INEG
    {"abs", 1, 1},        // OP_IABS
    {"min", 2, 1},        // OP_IMIN
    {"max", 2, 1},        // OP_IMAX
    {"clamp", 3, 1},      // OP_ICLAMP
    {"min", 2, 1},        // OP_UMIN
    {"max", 2, 1},        // OP_UMAX
    {"clamp", 3, 1},      // OP_UCLAMP
};

// Comparison ops: OP_FEQ (0x50) through OP_UGE (0x5F)
static constexpr const char* CMP_OPS[] = {
    "==", "!=", "<", "<=", ">", ">=",  // Float: FEQ, FNE, FLT, FLE, FGT, FGE
    "==", "!=", "<", "<=", ">", ">=",  // Int: IEQ, INE, ILT, ILE, IGT, IGE
    "<", "<=", ">", ">=",              // Uint: ULT, ULE, UGT, UGE
};

// Bitwise ops: OP_AND (0x40) through OP_REVERSE_BITS (0x4A)
static constexpr OpInfo BIT_OPS[] = {
    {"&", 2, 0},          // OP_AND
    {"|", 2, 0},          // OP_OR
    {"^", 2, 0},          // OP_XOR
    {"~", 1, 0},          // OP_NOT
    {"<<", 2, 0},         // OP_SHL
    {">>", 2, 0},         // OP_SHR
    {">>", 2, 0},         // OP_ASR (same syntax, signed context)
    {"bitCount", 1, 1},   // OP_POPCNT
    {nullptr, 1, 1},      // OP_CLZ (no direct GLSL)
    {nullptr, 1, 1},      // OP_CTZ (no direct GLSL)
    {"bitfieldReverse", 1, 1}, // OP_REVERSE_BITS
};

// Math functions: OP_SQRT (0x80) through OP_COSH (0x8F).
// OP_TANH is non-contiguous and handled explicitly in bwsl_gles_backend.cpp.
static constexpr const char* MATH_FUNCS[] = {
    "sqrt", "inversesqrt", "pow", "exp", "exp2", "log", "log2",
    "sin", "cos", "tan", "asin", "acos", "atan", "atan",  // atan2 uses same name
    "sinh", "cosh",
};

// Geometric: OP_DOT (0x90) through OP_FACEFORWARD (0x97)
static constexpr const char* GEOM_FUNCS[] = {
    "dot", "cross", "length", "normalize", "distance", "reflect", "refract", "faceforward",
};

// ============================================================================
// Arena-based string builder - no malloc, no std::string
// ============================================================================

struct StringBuilder {
    char* data;
    u32 len;
    u32 cap;

    void Init(Memory::BWEMemoryArena* arena, u32 capacity) {
        data = static_cast<char*>(arena->Allocate(capacity));
        len = 0;
        cap = capacity;
    }

    // Append compile-time string literal
    template<u32 N>
    void Lit(const char (&s)[N]) {
        if (len + N - 1 < cap) {
            for (u32 i = 0; i < N - 1; i++) data[len++] = s[i];
        }
    }

    // Append C string
    void Str(const char* s) {
        while (*s && len < cap - 1) data[len++] = *s++;
    }

    // Append with known length
    void Raw(const char* s, u32 length) {
        if (len + length < cap) {
            for (u32 i = 0; i < length; i++) data[len++] = s[i];
        }
    }

    // Single char
    void Chr(char c) { if (len < cap - 1) data[len++] = c; }

    // Integer (no snprintf)
    void Int(s32 v) {
        if (v < 0) { Chr('-'); v = -v; }
        if (v == 0) { Chr('0'); return; }
        char buf[12]; u32 n = 0;
        while (v > 0) { buf[n++] = '0' + (v % 10); v /= 10; }
        while (n > 0) Chr(buf[--n]);
    }

    // Unsigned
    void Uint(u32 v) {
        if (v == 0) { Chr('0'); return; }
        char buf[12]; u32 n = 0;
        while (v > 0) { buf[n++] = '0' + (v % 10); v /= 10; }
        while (n > 0) Chr(buf[--n]);
    }

    // Float (6 decimal places)
    void Flt(f32 v) {
        if (v < 0) { Chr('-'); v = -v; }
        s32 intPart = static_cast<s32>(v);
        Int(intPart);
        Chr('.');
        f32 frac = v - intPart;
        for (int i = 0; i < 6; i++) {
            frac *= 10.0f;
            s32 d = static_cast<s32>(frac);
            Chr('0' + d);
            frac -= d;
        }
    }

    // Newline + indent (4 spaces per level)
    void NL(u32 indent) {
        Chr('\n');
        for (u32 i = 0; i < indent * 4; i++) Chr(' ');
    }

    std::string_view View() const { return std::string_view(data, len); }
};

// ============================================================================
// Register use tracking for SSA → expression inlining
// ============================================================================

struct RegInfo {
    u16 useCount;      // Times this register is read
    u16 defInst;       // Instruction that defines it
    u16 defBlock;      // Block where first defined (0xFFFF = unset)
    u8 flags;          // Inlineable, trivial load, etc.
};

static constexpr u8 REG_INLINEABLE      = 0x01;
static constexpr u8 REG_TRIVIAL         = 0x02;  // Simple load, always inline
static constexpr u8 REG_EMITTED         = 0x04;  // Already emitted as temp
static constexpr u8 REG_DECLARED        = 0x08;  // Type has been declared for this reg
static constexpr u8 REG_MULTI_BLOCK_DEF = 0x10;  // Defined in multiple blocks, needs hoisting

// ============================================================================
// GLSL ES Backend
// ============================================================================

struct GLESBuilder {
    // ===== Core State =====
    Memory::BWEMemoryArena* arena;
    StringBuilder out;
    const char* sourceBase;

    // ===== IR Access =====
    IR::IRProgram* ir;
    CFG* cfg;
    ShaderStage stage;

    // ===== Register Tracking =====
    RegInfo* regInfo;        // Arena-allocated, indexed by register ID
    u32 regCount;

    // ===== Emission State =====
    u32 indent;

    // ===== Interface Info (from AST/RenderConfig) =====
    const PassData* pass;                    // Current pass for attributes
    const RenderConfig* renderConfig;
    const IRAnalysis* analysis;              // For attribute/output types
    IR::PassVaryingContext* varyings;        // Vertex→Fragment varyings
    u32 workgroupSizeX;
    u32 workgroupSizeY;
    u32 workgroupSizeZ;


    void DebugDumpRegisterInfo();
    // ============= Public API =============

    void Initialize(Memory::BWEMemoryArena* arenaPtr,
                    const char* source,
                    IR::IRProgram* irProgram,
                    CFG* cfgPtr,
                    ShaderStage shaderStage,
                    const PassData* passData,
                    const RenderConfig* config,
                    const IRAnalysis* analysisData,
                    IR::PassVaryingContext* varyingCtx)
    {
        arena = arenaPtr;
        sourceBase = source;
        ir = irProgram;
        cfg = cfgPtr;
        stage = shaderStage;
        pass = passData;
        renderConfig = config;
        analysis = analysisData;
        varyings = varyingCtx;
        workgroupSizeX = 1;
        workgroupSizeY = 1;
        workgroupSizeZ = 1;

        regCount = ir->registerCount;
        regInfo = static_cast<RegInfo*>(arena->Allocate(regCount * sizeof(RegInfo)));
        for (u32 i = 0; i < regCount; i++) {
            regInfo[i] = {};
            regInfo[i].defBlock = 0xFFFF;  // Mark as unset
        }

        out.Init(arena, 64 * 1024);  // 64KB output buffer
        indent = 0;
    }

    std::string_view Emit();

    void SetComputeWorkgroupSize(u32 x, u32 y, u32 z) {
        workgroupSizeX = x;
        workgroupSizeY = y;
        workgroupSizeZ = z;
    }

private:
    // ===== Analysis Pass =====
    void CountUses();

    // ===== Emission =====
    void EmitHeader();
    void EmitInputs();
    void EmitOutputs();
    void EmitUniforms();
    void EmitMain();
    void EmitBlockRecursive(u32 blockIdx, u32 stopAt, bool* emitted);
    void EmitPhiAssignments(u32 fromBlock, u32 toBlock);
    void EmitInstruction(u32 instIdx);

    void EmitPhiDeclarations();
    void EmitUndefDeclarations();
    void EmitDefaultValue(u16 type);
    void EmitStructDeclarations();
    void EmitStructTypeName(u32 typeHash);
    void EmitStructFieldName(u32 fieldHash);
    void EmitStructFieldNameByIndex(u32 structHash, u16 fieldIdx);
    void EmitRegisterType(u16 reg);
    void EmitTextureLevelsUniformName(u16 texReg);

    // ===== Expression Emission =====
    void EmitExpr(u16 reg);
    void EmitExprForInst(u32 instIdx);
    void EmitBinaryOp(u32 instIdx, const char* op);
    void EmitUnaryOp(u32 instIdx, const char* op);
    void EmitFuncCall(u32 instIdx, const char* func, u32 arity);
    void EmitConstant(u32 instIdx);
    void EmitSwizzle(u32 instIdx);
    void EmitVecConstruct(u32 instIdx);

    // ===== Assignment Helpers =====
    void EmitBinaryAssign(u32 instIdx, u16 dest, const char* op);
    void EmitUnaryAssign(u32 instIdx, u16 dest, const char* op);
    void EmitFuncAssign(u32 instIdx, u16 dest, const char* func, u32 arity);

    // ===== Helpers =====
    void EmitType(u16 typeId) {
        if (typeId >= (sizeof(Str::TYPE_NAMES) / sizeof(Str::TYPE_NAMES[0]))) {
            out.Lit("void");
            return;
        }
        out.Str(Str::TYPE_NAMES[typeId]);
    }

    void EmitReg(u16 reg) {
        out.Lit("r");
        out.Uint(reg);
    }

    // Emit register with type declaration if not yet declared
    // Returns true if type was emitted (first declaration)
    bool EmitRegWithDecl(u16 reg) {
        if (reg >= regCount) {
            EmitReg(reg);
            return false;
        }
        if (!(regInfo[reg].flags & REG_DECLARED)) {
            regInfo[reg].flags |= REG_DECLARED;
            u16 regType = ir->registerTypes ? ir->registerTypes[reg] : 0;
            if (regType != 0 && regType != static_cast<u16>(CoreType::INVALID)) {
                EmitRegisterType(reg);
                out.Chr(' ');
            }
            EmitReg(reg);
            return true;
        }
        EmitReg(reg);
        return false;
    }

    bool ShouldInline(u16 reg) const {
        return regInfo[reg].useCount == 1 &&
               (regInfo[reg].flags & REG_INLINEABLE) &&
               !(regInfo[reg].flags & REG_EMITTED);
    }

    bool IsValidOperand(u16 op) const;

    u16 Op(u32 inst, u32 idx) const { return ir->GetOperand(inst, idx); }
    u16 Opcode(u32 inst) const { return ir->opcodes[inst]; }
    u16 Dest(u32 inst) const { return ir->destinations[inst]; }
    u16 Type(u32 inst) const { return ir->types[inst]; }
};



} // namespace GLES
} // namespace BWSL
