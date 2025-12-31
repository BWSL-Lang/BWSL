// BWSL Direct GLSL ES 300 Backend - Implementation
// Emits GLSL ES directly from IR, bypassing SPIR-V entirely

#include "bwsl_gles_backend.h"

namespace BWSL {
namespace GLES {

// ============================================================================
// Analysis Pass - Count register uses for inlining decisions
// ============================================================================

void GLESBuilder::CountUses() {
    // First pass: count how many times each register is used as an operand
    for (u32 i = 0; i < ir->instructionCount; i++) {
        for (u32 j = 0; j < 4; j++) {
            u16 op = ir->GetOperand(i, j);
            // Only count real register references (not constants or invalid)
            if ((op & 0xC000) == 0 && op < regCount) {
                regInfo[op].useCount++;
            }
        }
    }

    // Second pass: determine which instructions can be inlined
    for (u32 i = 0; i < ir->instructionCount; i++) {
        u16 dest = ir->destinations[i];
        if (dest >= regCount) continue;

        regInfo[dest].defInst = static_cast<u16>(i);

        u16 opcode = ir->opcodes[i];

        // Trivial loads always inline (no temp needed)
        if (opcode == IR::OP_LOAD_CONST ||
            opcode == IR::OP_LOAD_REG ||
            opcode == IR::OP_LOAD_ATTR ||
            opcode == IR::OP_LOAD_INPUT ||
            opcode == IR::OP_LOAD_UNIFORM) {
            regInfo[dest].flags |= REG_TRIVIAL | REG_INLINEABLE;
        }
        // Pure expressions with single use can inline
        else if (regInfo[dest].useCount == 1 && !IR::HasSideEffects(static_cast<IR::OpCode>(opcode))) {
            regInfo[dest].flags |= REG_INLINEABLE;
        }
    }
}

// ============================================================================
// Header Emission
// ============================================================================

void GLESBuilder::EmitHeader() {
    out.Lit("#version 300 es\n");
    out.Lit("precision highp float;\n");
    out.Lit("precision highp int;\n");
    out.NL(0);
}

// ============================================================================
// Interface Emission - Inputs/Outputs/Uniforms
// ============================================================================

void GLESBuilder::EmitInputs() {
    if (stage == ShaderStage::Vertex) {
        // Vertex inputs come from analysis (used attribute mask and types)
        if (analysis) {
            for (u32 i = 0; i < 16; i++) {
                if (analysis->usedAttributeMask & (1 << i)) {
                    CoreType type = static_cast<CoreType>(analysis->attributeTypes[i]);
                    if (type == CoreType::VOID || type == CoreType::INVALID) {
                        type = CoreType::FLOAT4;  // Fallback
                    }

                    out.Lit("layout(location = ");
                    out.Uint(i);
                    out.Lit(") in ");
                    EmitType(static_cast<u16>(type));
                    out.Lit(" attr");
                    out.Uint(i);
                    out.Lit(";\n");
                }
            }
        }
    } else if (stage == ShaderStage::Fragment) {
        // Fragment inputs are varyings from vertex shader
        if (varyings) {
            for (u32 i = 0; i < varyings->count; i++) {
                out.Lit("in ");
                EmitType(static_cast<u16>(varyings->varyings[i].type));
                out.Lit(" v_");
                out.Str(varyings->varyings[i].name);
                out.Lit(";\n");
            }
        }
    }
    out.NL(0);
}

void GLESBuilder::EmitOutputs() {
    if (stage == ShaderStage::Vertex) {
        // Vertex outputs are varyings to fragment shader
        if (varyings) {
            for (u32 i = 0; i < varyings->count; i++) {
                out.Lit("out ");
                EmitType(static_cast<u16>(varyings->varyings[i].type));
                out.Lit(" v_");
                out.Str(varyings->varyings[i].name);
                out.Lit(";\n");
            }
        }
    } else if (stage == ShaderStage::Fragment) {
        // Fragment outputs - typically just color
        out.Lit("layout(location = 0) out vec4 fragColor;\n");
    }
    out.NL(0);
}

void GLESBuilder::EmitUniforms() {
    // Scan IR for OP_LOAD_UNIFORM to find used uniforms
    bool hasUniforms = false;
    for (u32 i = 0; i < ir->instructionCount; i++) {
        if (ir->opcodes[i] == IR::OP_LOAD_UNIFORM) {
            hasUniforms = true;
            break;
        }
    }

    if (hasUniforms) {
        out.Lit("// TODO: Uniform block declaration\n");
        out.NL(0);
    }
}

// ============================================================================
// Main Function Emission
// ============================================================================

void GLESBuilder::EmitMain() {
    out.Lit("void main() {\n");
    indent = 1;

    // Emit temp variables for multi-use registers
    for (u32 i = 0; i < regCount; i++) {
        if (regInfo[i].useCount > 1 && !(regInfo[i].flags & REG_TRIVIAL)) {
            u32 defInst = regInfo[i].defInst;
            if (defInst < ir->instructionCount && ir->registerTypes) {
                u16 regType = ir->registerTypes[i];
                if (regType != 0) {  // Skip INVALID type
                    out.NL(indent);
                    EmitType(regType);
                    out.Chr(' ');
                    EmitReg(static_cast<u16>(i));
                    out.Lit(";");
                }
            }
        }
    }
    out.NL(0);

    // Emit instructions
    for (u32 i = 0; i < ir->instructionCount; i++) {
        EmitInstruction(i);
    }

    out.NL(0);
    out.Lit("}\n");
}

// ============================================================================
// Instruction Emission
// ============================================================================

void GLESBuilder::EmitInstruction(u32 instIdx) {
    u16 opcode = ir->opcodes[instIdx];
    u16 dest = ir->destinations[instIdx];

    // Skip if this instruction's result is inlined elsewhere
    if (dest < regCount && ShouldInline(dest)) {
        return;
    }

    out.NL(indent);

    // Handle different instruction categories
    switch (opcode) {
        // ===== Control Flow =====
        case IR::OP_RET:
            out.Lit("return;");
            return;

        case IR::OP_DISCARD:
            out.Lit("discard;");
            return;

        // ===== Memory Operations =====
        case IR::OP_STORE_OUTPUT: {
            // Fragment: fragColor = value
            // Vertex: gl_Position = value or varying = value
            u16 outputIdx = Op(instIdx, 0);
            u16 valueReg = Op(instIdx, 1);

            if (stage == ShaderStage::Fragment) {
                out.Lit("fragColor = ");
            } else if (stage == ShaderStage::Vertex) {
                if (outputIdx == 0) {  // Position output
                    out.Lit("gl_Position = ");
                } else if (varyings && outputIdx <= varyings->count) {
                    out.Lit("v_");
                    out.Str(varyings->varyings[outputIdx - 1].name);
                    out.Lit(" = ");
                }
            }
            EmitExpr(valueReg);
            out.Lit(";");
            return;
        }

        case IR::OP_LOAD_CONST:
        case IR::OP_LOAD_REG:
        case IR::OP_LOAD_ATTR:
        case IR::OP_LOAD_INPUT:
        case IR::OP_LOAD_UNIFORM:
            // These are inlined when used, but if multi-use, emit assignment
            if (dest < regCount && regInfo[dest].useCount > 1) {
                EmitReg(dest);
                out.Lit(" = ");
                EmitExprForInst(instIdx);
                out.Lit(";");
            }
            return;

        // ===== Arithmetic =====
        case IR::OP_FADD: case IR::OP_IADD:
            EmitBinaryAssign(instIdx, dest, "+");
            return;
        case IR::OP_FSUB: case IR::OP_ISUB:
            EmitBinaryAssign(instIdx, dest, "-");
            return;
        case IR::OP_FMUL: case IR::OP_IMUL:
            EmitBinaryAssign(instIdx, dest, "*");
            return;
        case IR::OP_FDIV: case IR::OP_IDIV:
            EmitBinaryAssign(instIdx, dest, "/");
            return;
        case IR::OP_IMOD:
            EmitBinaryAssign(instIdx, dest, "%");
            return;
        case IR::OP_FMOD:
            EmitFuncAssign(instIdx, dest, "mod", 2);
            return;

        case IR::OP_FNEG: case IR::OP_INEG:
            EmitUnaryAssign(instIdx, dest, "-");
            return;

        // ===== Math Functions =====
        case IR::OP_FABS: case IR::OP_IABS:
            EmitFuncAssign(instIdx, dest, "abs", 1);
            return;
        case IR::OP_FMIN: case IR::OP_IMIN: case IR::OP_UMIN:
            EmitFuncAssign(instIdx, dest, "min", 2);
            return;
        case IR::OP_FMAX: case IR::OP_IMAX: case IR::OP_UMAX:
            EmitFuncAssign(instIdx, dest, "max", 2);
            return;
        case IR::OP_FCLAMP: case IR::OP_ICLAMP: case IR::OP_UCLAMP:
            EmitFuncAssign(instIdx, dest, "clamp", 3);
            return;
        case IR::OP_FLOOR:
            EmitFuncAssign(instIdx, dest, "floor", 1);
            return;
        case IR::OP_CEIL:
            EmitFuncAssign(instIdx, dest, "ceil", 1);
            return;
        case IR::OP_ROUND:
            EmitFuncAssign(instIdx, dest, "round", 1);
            return;
        case IR::OP_TRUNC:
            EmitFuncAssign(instIdx, dest, "trunc", 1);
            return;
        case IR::OP_FRACT:
            EmitFuncAssign(instIdx, dest, "fract", 1);
            return;
        case IR::OP_FMA:
            EmitFuncAssign(instIdx, dest, "fma", 3);
            return;

        case IR::OP_SQRT:
            EmitFuncAssign(instIdx, dest, "sqrt", 1);
            return;
        case IR::OP_RSQRT:
            EmitFuncAssign(instIdx, dest, "inversesqrt", 1);
            return;
        case IR::OP_POW:
            EmitFuncAssign(instIdx, dest, "pow", 2);
            return;
        case IR::OP_EXP:
            EmitFuncAssign(instIdx, dest, "exp", 1);
            return;
        case IR::OP_EXP2:
            EmitFuncAssign(instIdx, dest, "exp2", 1);
            return;
        case IR::OP_LOG:
            EmitFuncAssign(instIdx, dest, "log", 1);
            return;
        case IR::OP_LOG2:
            EmitFuncAssign(instIdx, dest, "log2", 1);
            return;
        case IR::OP_SIN:
            EmitFuncAssign(instIdx, dest, "sin", 1);
            return;
        case IR::OP_COS:
            EmitFuncAssign(instIdx, dest, "cos", 1);
            return;
        case IR::OP_TAN:
            EmitFuncAssign(instIdx, dest, "tan", 1);
            return;
        case IR::OP_ASIN:
            EmitFuncAssign(instIdx, dest, "asin", 1);
            return;
        case IR::OP_ACOS:
            EmitFuncAssign(instIdx, dest, "acos", 1);
            return;
        case IR::OP_ATAN:
            EmitFuncAssign(instIdx, dest, "atan", 1);
            return;
        case IR::OP_ATAN2:
            EmitFuncAssign(instIdx, dest, "atan", 2);
            return;
        case IR::OP_SINH:
            EmitFuncAssign(instIdx, dest, "sinh", 1);
            return;
        case IR::OP_COSH:
            EmitFuncAssign(instIdx, dest, "cosh", 1);
            return;

        // ===== Geometric =====
        case IR::OP_DOT:
            EmitFuncAssign(instIdx, dest, "dot", 2);
            return;
        case IR::OP_CROSS:
            EmitFuncAssign(instIdx, dest, "cross", 2);
            return;
        case IR::OP_LENGTH:
            EmitFuncAssign(instIdx, dest, "length", 1);
            return;
        case IR::OP_NORMALIZE:
            EmitFuncAssign(instIdx, dest, "normalize", 1);
            return;
        case IR::OP_DISTANCE:
            EmitFuncAssign(instIdx, dest, "distance", 2);
            return;
        case IR::OP_REFLECT:
            EmitFuncAssign(instIdx, dest, "reflect", 2);
            return;
        case IR::OP_REFRACT:
            EmitFuncAssign(instIdx, dest, "refract", 3);
            return;
        case IR::OP_FACEFORWARD:
            EmitFuncAssign(instIdx, dest, "faceforward", 3);
            return;

        // ===== Interpolation =====
        case IR::OP_LERP:
            EmitFuncAssign(instIdx, dest, "mix", 3);
            return;
        case IR::OP_SMOOTHSTEP:
            EmitFuncAssign(instIdx, dest, "smoothstep", 3);
            return;
        case IR::OP_STEP:
            EmitFuncAssign(instIdx, dest, "step", 2);
            return;
        case IR::OP_SATURATE:
            // GLSL ES doesn't have saturate, use clamp(x, 0.0, 1.0)
            EmitReg(dest);
            out.Lit(" = clamp(");
            EmitExpr(Op(instIdx, 0));
            out.Lit(", 0.0, 1.0);");
            return;

        // ===== Comparison =====
        case IR::OP_FEQ: case IR::OP_IEQ:
            EmitBinaryAssign(instIdx, dest, "==");
            return;
        case IR::OP_FNE: case IR::OP_INE:
            EmitBinaryAssign(instIdx, dest, "!=");
            return;
        case IR::OP_FLT: case IR::OP_ILT: case IR::OP_ULT:
            EmitBinaryAssign(instIdx, dest, "<");
            return;
        case IR::OP_FLE: case IR::OP_ILE: case IR::OP_ULE:
            EmitBinaryAssign(instIdx, dest, "<=");
            return;
        case IR::OP_FGT: case IR::OP_IGT: case IR::OP_UGT:
            EmitBinaryAssign(instIdx, dest, ">");
            return;
        case IR::OP_FGE: case IR::OP_IGE: case IR::OP_UGE:
            EmitBinaryAssign(instIdx, dest, ">=");
            return;

        // ===== Bitwise =====
        case IR::OP_AND:
            EmitBinaryAssign(instIdx, dest, "&");
            return;
        case IR::OP_OR:
            EmitBinaryAssign(instIdx, dest, "|");
            return;
        case IR::OP_XOR:
            EmitBinaryAssign(instIdx, dest, "^");
            return;
        case IR::OP_NOT:
            EmitUnaryAssign(instIdx, dest, "~");
            return;
        case IR::OP_SHL:
            EmitBinaryAssign(instIdx, dest, "<<");
            return;
        case IR::OP_SHR: case IR::OP_ASR:
            EmitBinaryAssign(instIdx, dest, ">>");
            return;

        // ===== Type Conversion =====
        case IR::OP_F2I:
            EmitReg(dest);
            out.Lit(" = int(");
            EmitExpr(Op(instIdx, 0));
            out.Lit(");");
            return;
        case IR::OP_I2F:
            EmitReg(dest);
            out.Lit(" = float(");
            EmitExpr(Op(instIdx, 0));
            out.Lit(");");
            return;
        case IR::OP_F2U:
            EmitReg(dest);
            out.Lit(" = uint(");
            EmitExpr(Op(instIdx, 0));
            out.Lit(");");
            return;
        case IR::OP_U2F:
            EmitReg(dest);
            out.Lit(" = float(");
            EmitExpr(Op(instIdx, 0));
            out.Lit(");");
            return;

        // ===== Vector Operations =====
        case IR::OP_VEC_CONSTRUCT:
            EmitVecConstruct(instIdx);
            return;

        case IR::OP_VEC_EXTRACT:
            EmitReg(dest);
            out.Lit(" = ");
            EmitExpr(Op(instIdx, 0));
            out.Chr('.');
            out.Chr(Str::SWIZZLE[Op(instIdx, 1) & 3]);
            out.Lit(";");
            return;

        case IR::OP_VEC_SHUFFLE:
            EmitSwizzle(instIdx);
            return;

        // ===== Texture =====
        case IR::OP_TEX_SAMPLE:
            EmitReg(dest);
            out.Lit(" = texture(");
            EmitExpr(Op(instIdx, 0));  // sampler
            out.Lit(", ");
            EmitExpr(Op(instIdx, 1));  // coord
            out.Lit(");");
            return;

        // ===== Derivatives (Fragment only) =====
        case IR::OP_DDX:
            EmitFuncAssign(instIdx, dest, "dFdx", 1);
            return;
        case IR::OP_DDY:
            EmitFuncAssign(instIdx, dest, "dFdy", 1);
            return;
        case IR::OP_FWIDTH:
            EmitFuncAssign(instIdx, dest, "fwidth", 1);
            return;

        // ===== Select (ternary) =====
        case IR::OP_SELECT:
            EmitReg(dest);
            out.Lit(" = ");
            EmitExpr(Op(instIdx, 0));  // condition
            out.Lit(" ? ");
            EmitExpr(Op(instIdx, 1));  // true value
            out.Lit(" : ");
            EmitExpr(Op(instIdx, 2));  // false value
            out.Lit(";");
            return;

        // ===== Matrix Operations =====
        case IR::OP_MAT_MUL:
        case IR::OP_MAT_VEC_MUL:
        case IR::OP_VEC_MAT_MUL:
            EmitReg(dest);
            out.Lit(" = ");
            EmitExpr(Op(instIdx, 0));
            out.Lit(" * ");
            EmitExpr(Op(instIdx, 1));
            out.Lit(";");
            return;

        case IR::OP_MAT_TRANSPOSE:
            EmitFuncAssign(instIdx, dest, "transpose", 1);
            return;
        case IR::OP_MAT_INVERSE:
            EmitFuncAssign(instIdx, dest, "inverse", 1);
            return;
        case IR::OP_MAT_DET:
            EmitFuncAssign(instIdx, dest, "determinant", 1);
            return;

        default:
            out.Lit("// TODO: opcode 0x");
            // Emit hex
            {
                char hex[3];
                hex[0] = "0123456789ABCDEF"[(opcode >> 4) & 0xF];
                hex[1] = "0123456789ABCDEF"[opcode & 0xF];
                hex[2] = 0;
                out.Str(hex);
            }
            return;
    }
}

// ============================================================================
// Expression Emission Helpers
// ============================================================================

void GLESBuilder::EmitExpr(u16 reg) {
    // Check for constant reference
    if (reg & 0x8000) {
        // Float constant
        u16 idx = reg & 0x7FFF;
        if (idx < ir->floatCount) {
            out.Flt(ir->floatConstants[idx]);
        }
        return;
    }
    if (reg & 0x4000) {
        // Int constant (0x4000) or Bool constant (0xC000)
        if ((reg & 0xC000) == 0xC000) {
            // Bool constant
            u16 idx = reg & 0x3FFF;
            if (idx < ir->boolCount) {
                out.Str(ir->boolConstants[idx] ? "true" : "false");
            }
        } else {
            // Int constant
            u16 idx = reg & 0x3FFF;
            if (idx < ir->intCount) {
                out.Int(ir->intConstants[idx]);
            }
        }
        return;
    }

    // Invalid register marker
    if (reg == 0x3FFF) {
        return;  // Skip invalid operand slots
    }

    // Real register - check if we should inline it
    if (reg < regCount && ShouldInline(reg)) {
        regInfo[reg].flags |= REG_EMITTED;
        EmitExprForInst(regInfo[reg].defInst);
    } else {
        EmitReg(reg);
    }
}

void GLESBuilder::EmitExprForInst(u32 instIdx) {
    u16 opcode = ir->opcodes[instIdx];

    switch (opcode) {
        case IR::OP_LOAD_CONST: {
            u16 constReg = Op(instIdx, 0);
            EmitExpr(constReg);
            return;
        }

        case IR::OP_LOAD_ATTR: {
            // Load vertex attribute by index
            u16 attrIdx = Op(instIdx, 0);
            out.Lit("attr");
            out.Uint(attrIdx);
            return;
        }

        case IR::OP_LOAD_INPUT: {
            // Load fragment input (varying)
            u16 inputIdx = Op(instIdx, 0);
            if (varyings && inputIdx < varyings->count) {
                out.Lit("v_");
                out.Str(varyings->varyings[inputIdx].name);
            } else {
                out.Lit("v_input");
                out.Uint(inputIdx);
            }
            return;
        }

        case IR::OP_LOAD_UNIFORM: {
            // Load from uniform buffer
            u16 uniformIdx = Op(instIdx, 0);
            out.Lit("u_uniform");
            out.Uint(uniformIdx);
            return;
        }

        case IR::OP_LOAD_REG: {
            EmitExpr(Op(instIdx, 0));
            return;
        }

        // Binary ops inline
        case IR::OP_FADD: case IR::OP_IADD:
            out.Chr('(');
            EmitExpr(Op(instIdx, 0));
            out.Lit(" + ");
            EmitExpr(Op(instIdx, 1));
            out.Chr(')');
            return;
        case IR::OP_FSUB: case IR::OP_ISUB:
            out.Chr('(');
            EmitExpr(Op(instIdx, 0));
            out.Lit(" - ");
            EmitExpr(Op(instIdx, 1));
            out.Chr(')');
            return;
        case IR::OP_FMUL: case IR::OP_IMUL:
            out.Chr('(');
            EmitExpr(Op(instIdx, 0));
            out.Lit(" * ");
            EmitExpr(Op(instIdx, 1));
            out.Chr(')');
            return;
        case IR::OP_FDIV: case IR::OP_IDIV:
            out.Chr('(');
            EmitExpr(Op(instIdx, 0));
            out.Lit(" / ");
            EmitExpr(Op(instIdx, 1));
            out.Chr(')');
            return;

        default:
            // For complex expressions, just reference the temp
            EmitReg(ir->destinations[instIdx]);
            return;
    }
}

void GLESBuilder::EmitBinaryOp(u32 instIdx, const char* op) {
    out.Chr('(');
    EmitExpr(Op(instIdx, 0));
    out.Chr(' ');
    out.Str(op);
    out.Chr(' ');
    EmitExpr(Op(instIdx, 1));
    out.Chr(')');
}

void GLESBuilder::EmitUnaryOp(u32 instIdx, const char* op) {
    out.Str(op);
    out.Chr('(');
    EmitExpr(Op(instIdx, 0));
    out.Chr(')');
}

void GLESBuilder::EmitFuncCall(u32 instIdx, const char* func, u32 arity) {
    out.Str(func);
    out.Chr('(');
    for (u32 i = 0; i < arity; i++) {
        if (i > 0) out.Lit(", ");
        EmitExpr(Op(instIdx, i));
    }
    out.Chr(')');
}

void GLESBuilder::EmitConstant(u32 instIdx) {
    u16 constRef = Op(instIdx, 0);
    EmitExpr(constRef);
}

void GLESBuilder::EmitSwizzle(u32 instIdx) {
    u16 dest = ir->destinations[instIdx];
    EmitReg(dest);
    out.Lit(" = ");
    EmitExpr(Op(instIdx, 0));
    out.Chr('.');

    // Swizzle mask is in operand 1, packed as 4x2-bit indices
    u16 mask = Op(instIdx, 1);
    u16 resultType = ir->types[instIdx];
    u32 components = 1;

    // Determine component count from result type
    if (resultType >= static_cast<u16>(CoreType::FLOAT2) &&
        resultType <= static_cast<u16>(CoreType::FLOAT4)) {
        components = resultType - static_cast<u16>(CoreType::FLOAT2) + 2;
    }

    for (u32 i = 0; i < components; i++) {
        u8 idx = (mask >> (i * 2)) & 0x3;
        out.Chr(Str::SWIZZLE[idx]);
    }
    out.Lit(";");
}

void GLESBuilder::EmitVecConstruct(u32 instIdx) {
    u16 dest = ir->destinations[instIdx];
    u16 type = ir->types[instIdx];

    EmitReg(dest);
    out.Lit(" = ");
    EmitType(type);
    out.Chr('(');

    // Determine how many components based on type
    u32 components = 1;
    if (type >= static_cast<u16>(CoreType::FLOAT2) &&
        type <= static_cast<u16>(CoreType::FLOAT4)) {
        components = type - static_cast<u16>(CoreType::FLOAT2) + 2;
    } else if (type >= static_cast<u16>(CoreType::INT2) &&
               type <= static_cast<u16>(CoreType::INT4)) {
        components = type - static_cast<u16>(CoreType::INT2) + 2;
    }

    for (u32 i = 0; i < components && i < 4; i++) {
        if (i > 0) out.Lit(", ");
        EmitExpr(Op(instIdx, i));
    }
    out.Lit(");");
}

// ============================================================================
// Assignment Emission Helpers
// ============================================================================

void GLESBuilder::EmitBinaryAssign(u32 instIdx, u16 dest, const char* op) {
    EmitReg(dest);
    out.Lit(" = ");
    EmitBinaryOp(instIdx, op);
    out.Lit(";");
}

void GLESBuilder::EmitUnaryAssign(u32 instIdx, u16 dest, const char* op) {
    EmitReg(dest);
    out.Lit(" = ");
    EmitUnaryOp(instIdx, op);
    out.Lit(";");
}

void GLESBuilder::EmitFuncAssign(u32 instIdx, u16 dest, const char* func, u32 arity) {
    EmitReg(dest);
    out.Lit(" = ");
    EmitFuncCall(instIdx, func, arity);
    out.Lit(";");
}

// ============================================================================
// Public API
// ============================================================================

std::string_view GLESBuilder::Emit() {
    CountUses();
    EmitHeader();
    EmitInputs();
    EmitOutputs();
    EmitUniforms();
    EmitMain();
    return out.View();
}

} // namespace GLES
} // namespace BWSL
