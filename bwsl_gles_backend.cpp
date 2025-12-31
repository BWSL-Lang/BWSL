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
    // Emit uniform buffer declarations from render config
    if (renderConfig) {
        // Emit uniform buffers as individual uniforms (GLSL ES 300 style)
        for (const auto& ub : renderConfig->uniformBuffers) {
            // Check if this uniform is used in the current shader stage
            bool isVertex = (stage == ShaderStage::Vertex);
            bool isFragment = (stage == ShaderStage::Fragment);
            bool stageMatch = (isVertex && (ub.stages & 1)) || (isFragment && (ub.stages & 2));

            if (!stageMatch) continue;

            // Emit as std140 uniform block
            out.Lit("layout(std140) uniform UB_");
            out.Str(ub.name.c_str());
            out.Lit(" {\n");
            out.Lit("    ");

            // Map type name to GLSL type
            const char* glslType = "float";
            if (ub.typeName == "mat4") glslType = "mat4";
            else if (ub.typeName == "mat3") glslType = "mat3";
            else if (ub.typeName == "float4" || ub.typeName == "vec4") glslType = "vec4";
            else if (ub.typeName == "float3" || ub.typeName == "vec3") glslType = "vec3";
            else if (ub.typeName == "float2" || ub.typeName == "vec2") glslType = "vec2";
            else if (ub.typeName == "int") glslType = "int";
            else if (ub.typeName == "uint") glslType = "uint";

            out.Str(glslType);
            out.Lit(" u_");
            out.Str(ub.name.c_str());
            out.Lit(";\n} ub_");
            out.Str(ub.name.c_str());
            out.Lit(";\n");
        }

        // Emit samplers
        for (const auto& tex : renderConfig->textures) {
            bool isVertex = (stage == ShaderStage::Vertex);
            bool isFragment = (stage == ShaderStage::Fragment);
            bool stageMatch = (isVertex && (tex.stages & 1)) || (isFragment && (tex.stages & 2));

            if (!stageMatch) continue;

            out.Lit("uniform ");
            if (tex.isCubemap) {
                out.Lit("samplerCube");
            } else if (tex.isArray) {
                out.Lit("sampler2DArray");
            } else {
                out.Lit("sampler2D");
            }
            out.Lit(" u_");
            out.Str(tex.name.c_str());
            out.Lit(";\n");
        }
        out.NL(0);
    } else {
        // Fallback: scan IR for OP_LOAD_UNIFORM to find used uniforms
        bool hasUniforms = false;
        for (u32 i = 0; i < ir->instructionCount; i++) {
            if (ir->opcodes[i] == IR::OP_LOAD_UNIFORM) {
                hasUniforms = true;
                break;
            }
        }

        if (hasUniforms) {
            out.Lit("// TODO: Uniform block declaration (no render config)\n");
            out.NL(0);
        }
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
        case IR::OP_NOP:
            // No operation - skip
            return;

        case IR::OP_JUMP:
            // Unconditional jump - handled by structured control flow
            // In structured GLSL, this becomes implicit fall-through or break/continue
            return;

        case IR::OP_BRANCH: {
            // Conditional branch - emit as if statement
            // The CFG should provide structure info for proper reconstruction
            u16 condition = Op(instIdx, 0);
            out.Lit("if (");
            EmitExpr(condition);
            out.Lit(") {");
            indent++;
            return;
        }

        case IR::OP_PHI: {
            // SSA phi node - in structured code, this becomes assignments
            // at the end of predecessor blocks. For now, emit as a comment.
            // The phi resolution should happen during pre-processing.
            out.Lit("// phi: r");
            out.Uint(dest);
            out.Lit(" = merge of values from predecessors");
            return;
        }

        case IR::OP_SWITCH: {
            // Switch statement
            u16 selector = Op(instIdx, 0);
            out.Lit("switch (");
            EmitExpr(selector);
            out.Lit(") {");
            indent++;
            return;
        }

        case IR::OP_RET:
            out.Lit("return;");
            return;

        case IR::OP_DISCARD:
            out.Lit("discard;");
            return;

        // ===== Memory Operations =====
        case IR::OP_STORE_REG: {
            // Register-to-register copy/store
            u16 srcReg = Op(instIdx, 0);
            EmitReg(dest);
            out.Lit(" = ");
            EmitExpr(srcReg);
            out.Lit(";");
            return;
        }

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

        case IR::OP_LOAD_OUTPUT: {
            // Load from a previously written output (rare, for reading gl_Position etc.)
            u16 outputIdx = Op(instIdx, 0);
            EmitReg(dest);
            out.Lit(" = ");
            if (stage == ShaderStage::Vertex && outputIdx == 0) {
                out.Lit("gl_Position");
            } else if (varyings && outputIdx <= varyings->count) {
                out.Lit("v_");
                out.Str(varyings->varyings[outputIdx - 1].name);
            } else {
                out.Lit("output");
                out.Uint(outputIdx);
            }
            out.Lit(";");
            return;
        }

        case IR::OP_LOAD_LOCAL:
        case IR::OP_STORE_LOCAL:
            // Thread-local storage - emit as local variable access
            EmitReg(dest);
            out.Lit(" = local");
            out.Uint(Op(instIdx, 0));
            out.Lit(";");
            return;

        case IR::OP_LOAD_BUFFER:
        case IR::OP_STORE_BUFFER:
            // Storage buffer access - GLSL ES 300 doesn't have SSBOs
            out.Lit("// Buffer ops require GLSL ES 310+");
            return;

        case IR::OP_LOAD_SHARED:
        case IR::OP_STORE_SHARED:
            // Shared memory - only in compute shaders
            out.Lit("// Shared memory ops require compute shader");
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
        case IR::OP_DEGREES:
            EmitFuncAssign(instIdx, dest, "degrees", 1);
            return;
        case IR::OP_RADIANS:
            EmitFuncAssign(instIdx, dest, "radians", 1);
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
        case IR::OP_POPCNT:
            // GLSL ES 300 has bitCount
            EmitFuncAssign(instIdx, dest, "bitCount", 1);
            return;
        case IR::OP_CLZ: {
            // GLSL ES 300: use findMSB and compute 31 - findMSB(x)
            EmitReg(dest);
            out.Lit(" = (");
            EmitExpr(Op(instIdx, 0));
            out.Lit(" == 0) ? 32 : (31 - findMSB(");
            EmitExpr(Op(instIdx, 0));
            out.Lit("));");
            return;
        }
        case IR::OP_CTZ: {
            // GLSL ES 300: use findLSB
            EmitReg(dest);
            out.Lit(" = (");
            EmitExpr(Op(instIdx, 0));
            out.Lit(" == 0) ? 32 : findLSB(");
            EmitExpr(Op(instIdx, 0));
            out.Lit(");");
            return;
        }
        case IR::OP_REVERSE_BITS:
            EmitFuncAssign(instIdx, dest, "bitfieldReverse", 1);
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
        case IR::OP_I2U:
            EmitReg(dest);
            out.Lit(" = uint(");
            EmitExpr(Op(instIdx, 0));
            out.Lit(");");
            return;
        case IR::OP_U2I:
            EmitReg(dest);
            out.Lit(" = int(");
            EmitExpr(Op(instIdx, 0));
            out.Lit(");");
            return;
        case IR::OP_BITCAST: {
            // Reinterpret bits - use GLSL bitcast functions
            u16 srcType = ir->registerTypes[Op(instIdx, 0)];
            u16 dstType = ir->types[instIdx];
            if (srcType == static_cast<u16>(CoreType::FLOAT) && dstType == static_cast<u16>(CoreType::INT)) {
                EmitReg(dest);
                out.Lit(" = floatBitsToInt(");
                EmitExpr(Op(instIdx, 0));
                out.Lit(");");
            } else if (srcType == static_cast<u16>(CoreType::FLOAT) && dstType == static_cast<u16>(CoreType::UINT)) {
                EmitReg(dest);
                out.Lit(" = floatBitsToUint(");
                EmitExpr(Op(instIdx, 0));
                out.Lit(");");
            } else if (srcType == static_cast<u16>(CoreType::INT) && dstType == static_cast<u16>(CoreType::FLOAT)) {
                EmitReg(dest);
                out.Lit(" = intBitsToFloat(");
                EmitExpr(Op(instIdx, 0));
                out.Lit(");");
            } else if (srcType == static_cast<u16>(CoreType::UINT) && dstType == static_cast<u16>(CoreType::FLOAT)) {
                EmitReg(dest);
                out.Lit(" = uintBitsToFloat(");
                EmitExpr(Op(instIdx, 0));
                out.Lit(");");
            } else {
                // Fallback - just copy
                EmitReg(dest);
                out.Lit(" = ");
                EmitExpr(Op(instIdx, 0));
                out.Lit(";");
            }
            return;
        }
        case IR::OP_SIGN:
            EmitFuncAssign(instIdx, dest, "sign", 1);
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

        case IR::OP_VEC_INSERT: {
            // Insert a component into a vector
            // dest = vec with component[index] = value
            u16 vecReg = Op(instIdx, 0);
            u16 componentIdx = Op(instIdx, 1);
            u16 valueReg = Op(instIdx, 2);
            EmitReg(dest);
            out.Lit(" = ");
            EmitExpr(vecReg);
            out.Lit(";\n");
            out.NL(indent);
            EmitReg(dest);
            out.Chr('.');
            out.Chr(Str::SWIZZLE[componentIdx & 3]);
            out.Lit(" = ");
            EmitExpr(valueReg);
            out.Lit(";");
            return;
        }

        // ===== Texture =====
        case IR::OP_TEX_SAMPLE:
            EmitReg(dest);
            out.Lit(" = texture(");
            EmitExpr(Op(instIdx, 0));  // sampler
            out.Lit(", ");
            EmitExpr(Op(instIdx, 1));  // coord
            out.Lit(");");
            return;

        case IR::OP_TEX_SAMPLE_LOD:
            EmitReg(dest);
            out.Lit(" = textureLod(");
            EmitExpr(Op(instIdx, 0));  // sampler
            out.Lit(", ");
            EmitExpr(Op(instIdx, 1));  // coord
            out.Lit(", ");
            EmitExpr(Op(instIdx, 2));  // lod
            out.Lit(");");
            return;

        case IR::OP_TEX_SAMPLE_BIAS:
            // GLSL ES 300 has texture with bias
            EmitReg(dest);
            out.Lit(" = texture(");
            EmitExpr(Op(instIdx, 0));  // sampler
            out.Lit(", ");
            EmitExpr(Op(instIdx, 1));  // coord
            out.Lit(", ");
            EmitExpr(Op(instIdx, 2));  // bias
            out.Lit(");");
            return;

        case IR::OP_TEX_SAMPLE_GRAD:
            EmitReg(dest);
            out.Lit(" = textureGrad(");
            EmitExpr(Op(instIdx, 0));  // sampler
            out.Lit(", ");
            EmitExpr(Op(instIdx, 1));  // coord
            out.Lit(", ");
            EmitExpr(Op(instIdx, 2));  // dPdx
            out.Lit(", ");
            EmitExpr(Op(instIdx, 3));  // dPdy
            out.Lit(");");
            return;

        case IR::OP_TEX_FETCH:
            EmitReg(dest);
            out.Lit(" = texelFetch(");
            EmitExpr(Op(instIdx, 0));  // sampler
            out.Lit(", ");
            EmitExpr(Op(instIdx, 1));  // coord (ivec)
            out.Lit(", ");
            EmitExpr(Op(instIdx, 2));  // lod
            out.Lit(");");
            return;

        case IR::OP_TEX_SIZE:
            EmitReg(dest);
            out.Lit(" = textureSize(");
            EmitExpr(Op(instIdx, 0));  // sampler
            out.Lit(", ");
            EmitExpr(Op(instIdx, 1));  // lod
            out.Lit(");");
            return;

        case IR::OP_TEX_GATHER:
            EmitReg(dest);
            out.Lit(" = textureGather(");
            EmitExpr(Op(instIdx, 0));  // sampler
            out.Lit(", ");
            EmitExpr(Op(instIdx, 1));  // coord
            out.Lit(");");
            return;

        case IR::OP_LOAD_TEX_HANDLE:
            // Bindless textures - emit as sampler reference
            EmitReg(dest);
            out.Lit(" = sampler");
            out.Uint(Op(instIdx, 0));
            out.Lit(";");
            return;

        // ===== Derivatives (Fragment only) =====
        case IR::OP_DDX:
            EmitFuncAssign(instIdx, dest, "dFdx", 1);
            return;
        case IR::OP_DDY:
            EmitFuncAssign(instIdx, dest, "dFdy", 1);
            return;
        case IR::OP_DDX_FINE:
            // GLSL ES 300 doesn't have fine derivatives, use regular
            EmitFuncAssign(instIdx, dest, "dFdx", 1);
            return;
        case IR::OP_DDY_FINE:
            EmitFuncAssign(instIdx, dest, "dFdy", 1);
            return;
        case IR::OP_DDX_COARSE:
            EmitFuncAssign(instIdx, dest, "dFdx", 1);
            return;
        case IR::OP_DDY_COARSE:
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

        case IR::OP_MAT_CONSTRUCT: {
            // Build matrix from values
            u16 type = ir->types[instIdx];
            EmitReg(dest);
            out.Lit(" = ");
            EmitType(type);
            out.Chr('(');
            // Matrix constructors take columns, count depends on type
            u32 cols = 2;
            if (type == static_cast<u16>(CoreType::MAT3)) cols = 3;
            else if (type == static_cast<u16>(CoreType::MAT4)) cols = 4;
            for (u32 i = 0; i < cols && i < 4; i++) {
                if (i > 0) out.Lit(", ");
                EmitExpr(Op(instIdx, i));
            }
            out.Lit(");");
            return;
        }

        case IR::OP_MAT_SCALE:
            // Matrix * Scalar
            EmitReg(dest);
            out.Lit(" = ");
            EmitExpr(Op(instIdx, 0));
            out.Lit(" * ");
            EmitExpr(Op(instIdx, 1));
            out.Lit(";");
            return;

        case IR::OP_MAT_IDENTITY: {
            // Identity matrix
            u16 type = ir->types[instIdx];
            EmitReg(dest);
            out.Lit(" = ");
            EmitType(type);
            out.Lit("(1.0);");
            return;
        }

        case IR::OP_MAT_ZERO: {
            // Zero matrix
            u16 type = ir->types[instIdx];
            EmitReg(dest);
            out.Lit(" = ");
            EmitType(type);
            out.Lit("(0.0);");
            return;
        }

        // ===== Struct Operations =====
        case IR::OP_STRUCT_CONSTRUCT: {
            // Build struct from field values - emit as struct constructor
            EmitReg(dest);
            out.Lit(" = /* struct construct */;");
            return;
        }

        case IR::OP_STRUCT_EXTRACT: {
            // Extract field from struct
            u16 structReg = Op(instIdx, 0);
            u16 fieldIdx = Op(instIdx, 1);
            EmitReg(dest);
            out.Lit(" = ");
            EmitExpr(structReg);
            out.Lit(".field");
            out.Uint(fieldIdx);
            out.Lit(";");
            return;
        }

        case IR::OP_STRUCT_INSERT: {
            // Insert field into struct
            u16 structReg = Op(instIdx, 0);
            u16 fieldIdx = Op(instIdx, 1);
            u16 valueReg = Op(instIdx, 2);
            EmitReg(dest);
            out.Lit(" = ");
            EmitExpr(structReg);
            out.Lit(";\n");
            out.NL(indent);
            EmitReg(dest);
            out.Lit(".field");
            out.Uint(fieldIdx);
            out.Lit(" = ");
            EmitExpr(valueReg);
            out.Lit(";");
            return;
        }

        // ===== Array Operations =====
        // Note: OP_ALLOC_ARRAY (0x1C) shares value with OP_LOAD_INPUT (0x1C)
        // OP_LOAD_INPUT is handled above with the other load operations

        case IR::OP_ARRAY_ACCESS: {
            // Get element address - emit as array indexing
            u16 arrayReg = Op(instIdx, 0);
            u16 indexReg = Op(instIdx, 1);
            EmitReg(dest);
            out.Lit(" = ");
            EmitExpr(arrayReg);
            out.Chr('[');
            EmitExpr(indexReg);
            out.Lit("];");
            return;
        }

        case IR::OP_ARRAY_LOAD: {
            // Load from array element
            u16 arrayReg = Op(instIdx, 0);
            u16 indexReg = Op(instIdx, 1);
            EmitReg(dest);
            out.Lit(" = ");
            EmitExpr(arrayReg);
            out.Chr('[');
            EmitExpr(indexReg);
            out.Lit("];");
            return;
        }

        case IR::OP_ARRAY_STORE: {
            // Store to array element
            u16 arrayReg = Op(instIdx, 0);
            u16 indexReg = Op(instIdx, 1);
            u16 valueReg = Op(instIdx, 2);
            EmitExpr(arrayReg);
            out.Chr('[');
            EmitExpr(indexReg);
            out.Lit("] = ");
            EmitExpr(valueReg);
            out.Lit(";");
            return;
        }

        case IR::OP_ARRAY_CONSTRUCT: {
            // Build array from elements
            u16 type = ir->types[instIdx];
            EmitReg(dest);
            out.Lit(" = ");
            EmitType(type);
            out.Lit("[](");
            for (u32 i = 0; i < 4; i++) {
                u16 op = Op(instIdx, i);
                if (op == 0x3FFF) break;  // Invalid marker
                if (i > 0) out.Lit(", ");
                EmitExpr(op);
            }
            out.Lit(");");
            return;
        }

        // ===== Synchronization =====
        case IR::OP_BARRIER:
            // GLSL ES 300 compute shaders have barrier()
            out.Lit("barrier();");
            return;

        case IR::OP_MEM_FENCE:
            // GLSL ES 300: memoryBarrier variants
            out.Lit("memoryBarrier();");
            return;

        // ===== Enum Operations (emit as int) =====
        case IR::OP_ENUM_CONSTRUCT:
            EmitReg(dest);
            out.Lit(" = ");
            EmitExpr(Op(instIdx, 0));
            out.Lit(";");
            return;

        case IR::OP_ENUM_TAG:
            EmitReg(dest);
            out.Lit(" = ");
            EmitExpr(Op(instIdx, 0));
            out.Lit(";");
            return;

        case IR::OP_ENUM_FIELD:
            EmitReg(dest);
            out.Lit(" = ");
            EmitExpr(Op(instIdx, 0));
            out.Lit(";");
            return;

        // ===== Storage Buffer Operations =====
        case IR::OP_STORAGE_PTR:
        case IR::OP_STORAGE_FIELD:
        case IR::OP_STORAGE_INDEX:
        case IR::OP_STORAGE_LOAD:
            // These require SSBO support - emit as placeholder
            out.Lit("// Storage buffer op not fully supported in GLES 300");
            return;

        // ===== Atomics (not supported in GLSL ES 300 for non-compute) =====
        case IR::OP_ATOMIC_ADD:
        case IR::OP_ATOMIC_SUB:
        case IR::OP_ATOMIC_MIN:
        case IR::OP_ATOMIC_MAX:
        case IR::OP_ATOMIC_AND:
        case IR::OP_ATOMIC_OR:
        case IR::OP_ATOMIC_XOR:
        case IR::OP_ATOMIC_XCHG:
        case IR::OP_ATOMIC_CMP_XCHG:
            out.Lit("// Atomic ops require compute shader support");
            return;

        // ===== Wave/Subgroup Operations (not in GLSL ES 300) =====
        case IR::OP_WAVE_MIN:
        case IR::OP_WAVE_MAX:
        case IR::OP_WAVE_ALL:
        case IR::OP_WAVE_ANY:
        case IR::OP_WAVE_BALLOT:
        case IR::OP_WAVE_READ_FIRST:
        case IR::OP_WAVE_READ_LANE:
        case IR::OP_WAVE_SUM:
        case IR::OP_WAVE_MUL:
            out.Lit("// Wave ops not supported in GLSL ES 300");
            return;

        // ===== Call (function calls - should be inlined) =====
        case IR::OP_CALL:
            out.Lit("// Function call - should be inlined");
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
            // If we have render config, use the actual uniform name
            if (renderConfig && uniformIdx < renderConfig->uniformBuffers.size()) {
                const auto& ub = renderConfig->uniformBuffers[uniformIdx];
                out.Lit("ub_");
                out.Str(ub.name.c_str());
                out.Lit(".u_");
                out.Str(ub.name.c_str());
            } else {
                out.Lit("u_uniform");
                out.Uint(uniformIdx);
            }
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

    // Count valid operands to determine component count
    u32 components = 0;
    for (u32 i = 0; i < 4; i++) {
        u16 op = Op(instIdx, i);
        if (op != 0x3FFF && op != 0) {  // Not invalid marker
            components++;
        } else {
            break;
        }
    }
    if (components == 0) components = 1;

    // Fix type if it's VOID or INVALID - infer from component count and first operand
    if (type == static_cast<u16>(CoreType::VOID) ||
        type == static_cast<u16>(CoreType::INVALID) ||
        type == 0) {
        // Check first operand type to determine base type
        u16 firstOp = Op(instIdx, 0);
        bool isInt = false;
        bool isUint = false;

        // Check if first operand is an int constant (0x4000 prefix)
        if ((firstOp & 0x4000) && !(firstOp & 0x8000)) {
            isInt = true;
        }

        // Default to float vectors
        if (components == 1) {
            type = isInt ? static_cast<u16>(CoreType::INT) :
                   isUint ? static_cast<u16>(CoreType::UINT) :
                   static_cast<u16>(CoreType::FLOAT);
        } else if (components == 2) {
            type = isInt ? static_cast<u16>(CoreType::INT2) :
                   isUint ? static_cast<u16>(CoreType::UINT2) :
                   static_cast<u16>(CoreType::FLOAT2);
        } else if (components == 3) {
            type = isInt ? static_cast<u16>(CoreType::INT3) :
                   isUint ? static_cast<u16>(CoreType::UINT3) :
                   static_cast<u16>(CoreType::FLOAT3);
        } else {
            type = isInt ? static_cast<u16>(CoreType::INT4) :
                   isUint ? static_cast<u16>(CoreType::UINT4) :
                   static_cast<u16>(CoreType::FLOAT4);
        }
    }

    EmitType(type);
    out.Chr('(');

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
