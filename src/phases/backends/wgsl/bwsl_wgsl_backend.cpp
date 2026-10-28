#include "bwsl_wgsl_backend.h"
#include "phases/ir_generation/bwsl_ir_analysis.h"
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace BWSL {
namespace WGSL {

struct TextWriter {
    char* data = nullptr;
    size_t capacity = 0;
    size_t count = 0;
    bool valid = true;
    u32 indentation = 1;

    void Indent() {
        for (u32 level = 0; level < indentation; ++level) {
            Text("    ");
        }
    }


    void Text(const char* text) {
        Bytes(text, std::strlen(text));
    }

    void Bytes(const char* text, size_t length) {
        if (!valid || length > std::numeric_limits<size_t>::max() - count) {
            valid = false;
            return;
        }
        if (data != nullptr) {
            if (length > capacity - count) {
                valid = false;
                return;
            }
            std::memcpy(data + count, text, length);
        }
        count += length;
    }

    void Number(u32 value) {
        char text[16];
        std::to_chars_result result = std::to_chars(text, text + sizeof(text), value);
        if (result.ec != std::errc()) {
            valid = false;
            return;
        }
        Bytes(text, static_cast<size_t>(result.ptr - text));
    }
};

namespace {

bool IsSampledTextureOpcode(u16 opcode) {
    return opcode == IR::OP_TEX_SAMPLE || opcode == IR::OP_TEX_SAMPLE_LOD ||
           opcode == IR::OP_TEX_SAMPLE_BIAS || opcode == IR::OP_TEX_SAMPLE_GRAD ||
           opcode == IR::OP_TEX_SAMPLE_CMP || opcode == IR::OP_TEX_GATHER ||
           opcode == IR::OP_TEX_FETCH || opcode == IR::OP_TEX_SIZE || opcode == IR::OP_TEX_LEVELS;
}

u32 Width(CoreType type) {
    switch (type) {
        case CoreType::BOOL:
        case CoreType::INT:
        case CoreType::UINT:
        case CoreType::FLOAT:
            return 1;
        case CoreType::BOOL2:
        case CoreType::INT2:
        case CoreType::UINT2:
        case CoreType::FLOAT2:
            return 2;
        case CoreType::BOOL3:
        case CoreType::INT3:
        case CoreType::UINT3:
        case CoreType::FLOAT3:
            return 3;
        case CoreType::BOOL4:
        case CoreType::INT4:
        case CoreType::UINT4:
        case CoreType::FLOAT4:
            return 4;
        default:
            return 0;
    }
}

CoreType Scalar(CoreType type) {
    switch (type) {
        case CoreType::BOOL2:
        case CoreType::BOOL3:
        case CoreType::BOOL4:
            return CoreType::BOOL;
        case CoreType::INT2:
        case CoreType::INT3:
        case CoreType::INT4:
            return CoreType::INT;
        case CoreType::UINT2:
        case CoreType::UINT3:
        case CoreType::UINT4:
            return CoreType::UINT;
        case CoreType::FLOAT2:
        case CoreType::FLOAT3:
        case CoreType::FLOAT4:
            return CoreType::FLOAT;
        default:
            return type;
    }
}

CoreType Shape(CoreType scalar, u32 width) {
    if (width == 1) {
        return scalar;
    }
    CoreType first = CoreType::INVALID;
    switch (scalar) {
        case CoreType::BOOL: first = CoreType::BOOL2; break;
        case CoreType::INT: first = CoreType::INT2; break;
        case CoreType::UINT: first = CoreType::UINT2; break;
        case CoreType::FLOAT: first = CoreType::FLOAT2; break;
        default: return CoreType::INVALID;
    }
    if (width < 2 || width > 4) {
        return CoreType::INVALID;
    }
    return static_cast<CoreType>(static_cast<u32>(first) + width - 2);
}

void WriteType(TextWriter& writer, CoreType type) {
    if (type >= CoreType::MAT2 && type <= CoreType::MAT4) {
        const u32 dimension = static_cast<u32>(type) - static_cast<u32>(CoreType::MAT2) + 2;
        writer.Text("mat");
        writer.Number(dimension);
        writer.Text("x");
        writer.Number(dimension);
        writer.Text("<f32>");
        return;
    }
    u32 width = Width(type);
    if (width > 1) {
        writer.Text("vec");
        writer.Number(width);
        writer.Text("<");
    }
    switch (Scalar(type)) {
        case CoreType::BOOL: writer.Text("bool"); break;
        case CoreType::INT: writer.Text("i32"); break;
        case CoreType::UINT: writer.Text("u32"); break;
        case CoreType::FLOAT: writer.Text("f32"); break;
        default: writer.valid = false; break;
    }
    if (width > 1) {
        writer.Text(">");
    }
}

void WriteValueName(TextWriter& writer, u32 id) {
    writer.Text("v");
    writer.Number(id);
}

void WriteResourceName(TextWriter& writer, u32 slot) {
    writer.Text("bwsl_buffer_");
    writer.Number(slot);
}

const char* BuiltinName(InterfaceBuiltin builtin) {
    switch (builtin) {
        case InterfaceBuiltin::VertexIndex: return "vertex_index";
        case InterfaceBuiltin::InstanceIndex: return "instance_index";
        case InterfaceBuiltin::Position: return "position";
        case InterfaceBuiltin::FragDepth: return "frag_depth";
        case InterfaceBuiltin::GlobalInvocationId: return "global_invocation_id";
        case InterfaceBuiltin::LocalInvocationId: return "local_invocation_id";
        case InterfaceBuiltin::WorkgroupId: return "workgroup_id";
        case InterfaceBuiltin::NumWorkgroups: return "num_workgroups";
        case InterfaceBuiltin::LocalInvocationIndex: return "local_invocation_index";
        default: return nullptr;
    }
}

const char* BinaryOperator(u16 opcode) {
    switch (opcode) {
        case IR::OP_FADD:
        case IR::OP_IADD: return "+";
        case IR::OP_FSUB:
        case IR::OP_ISUB: return "-";
        case IR::OP_FMUL:
        case IR::OP_IMUL: return "*";
        case IR::OP_FDIV:
        case IR::OP_IDIV: return "/";
        case IR::OP_IMOD:
        case IR::OP_FREM: return "%";
        case IR::OP_FEQ:
        case IR::OP_IEQ: return "==";
        case IR::OP_FNE:
        case IR::OP_INE: return "!=";
        case IR::OP_FLT:
        case IR::OP_ILT:
        case IR::OP_ULT: return "<";
        case IR::OP_FLE:
        case IR::OP_ILE:
        case IR::OP_ULE: return "<=";
        case IR::OP_FGT:
        case IR::OP_IGT:
        case IR::OP_UGT: return ">";
        case IR::OP_FGE:
        case IR::OP_IGE:
        case IR::OP_UGE: return ">=";
        case IR::OP_AND: return "&";
        case IR::OP_OR: return "|";
        case IR::OP_XOR: return "^";
        case IR::OP_SHL: return "<<";
        case IR::OP_SHR:
        case IR::OP_ASR: return ">>";
        default: return nullptr;
    }
}

struct BuiltinOperation {
    const char* name;
    u8 argumentCount;
};

BuiltinOperation MathOperation(u16 opcode) {
    switch (opcode) {
        case IR::OP_DDX: return {"dpdx", 1};
        case IR::OP_DDY: return {"dpdy", 1};
        case IR::OP_DDX_FINE: return {"dpdxFine", 1};
        case IR::OP_DDY_FINE: return {"dpdyFine", 1};
        case IR::OP_DDX_COARSE: return {"dpdxCoarse", 1};
        case IR::OP_DDY_COARSE: return {"dpdyCoarse", 1};
        case IR::OP_FWIDTH: return {"fwidth", 1};
        case IR::OP_FWIDTH_FINE: return {"fwidthFine", 1};
        case IR::OP_FWIDTH_COARSE: return {"fwidthCoarse", 1};
        case IR::OP_FABS:
        case IR::OP_IABS: return {"abs", 1};
        case IR::OP_FMIN:
        case IR::OP_IMIN:
        case IR::OP_UMIN: return {"min", 2};
        case IR::OP_FMAX:
        case IR::OP_IMAX:
        case IR::OP_UMAX: return {"max", 2};
        case IR::OP_FCLAMP:
        case IR::OP_ICLAMP:
        case IR::OP_UCLAMP: return {"clamp", 3};
        case IR::OP_FLOOR: return {"floor", 1};
        case IR::OP_CEIL: return {"ceil", 1};
        case IR::OP_ROUND: return {"round", 1};
        case IR::OP_TRUNC: return {"trunc", 1};
        case IR::OP_FRACT: return {"fract", 1};
        case IR::OP_FMA: return {"fma", 3};
        case IR::OP_SIGN: return {"sign", 1};
        case IR::OP_SQRT: return {"sqrt", 1};
        case IR::OP_RSQRT: return {"inverseSqrt", 1};
        case IR::OP_POW: return {"pow", 2};
        case IR::OP_EXP: return {"exp", 1};
        case IR::OP_EXP2: return {"exp2", 1};
        case IR::OP_LOG: return {"log", 1};
        case IR::OP_LOG2: return {"log2", 1};
        case IR::OP_SIN: return {"sin", 1};
        case IR::OP_COS: return {"cos", 1};
        case IR::OP_TAN: return {"tan", 1};
        case IR::OP_ASIN: return {"asin", 1};
        case IR::OP_ACOS: return {"acos", 1};
        case IR::OP_ATAN: return {"atan", 1};
        case IR::OP_ATAN2: return {"atan2", 2};
        case IR::OP_SINH: return {"sinh", 1};
        case IR::OP_COSH: return {"cosh", 1};
        case IR::OP_TANH: return {"tanh", 1};
        case IR::OP_DOT: return {"dot", 2};
        case IR::OP_CROSS: return {"cross", 2};
        case IR::OP_LENGTH: return {"length", 1};
        case IR::OP_NORMALIZE: return {"normalize", 1};
        case IR::OP_DISTANCE: return {"distance", 2};
        case IR::OP_LERP: return {"mix", 3};
        case IR::OP_SMOOTHSTEP: return {"smoothstep", 3};
        case IR::OP_STEP: return {"step", 2};
        case IR::OP_DEGREES: return {"degrees", 1};
        case IR::OP_RADIANS: return {"radians", 1};
        case IR::OP_POPCNT: return {"countOneBits", 1};
        // first_bit_high/low are bit indices, -1 for no bit, like FindUMsb,
        // FindSMsb and FindILsb; not leading/trailing zero counts.
        case IR::OP_CLZ: return {"firstLeadingBit", 1};
        case IR::OP_CTZ: return {"firstTrailingBit", 1};
        case IR::OP_REVERSE_BITS: return {"reverseBits", 1};
        case IR::OP_ANY: return {"any", 1};
        case IR::OP_ALL: return {"all", 1};
        default: return {nullptr, 0};
    }
}

struct PackOperation {
    const char* name;
    CoreType argument;
    CoreType result;
};

// Packing builtins share GLSL.std.450's component order and rounding.
PackOperation PackingOperation(u16 opcode) {
    switch (opcode) {
        case IR::OP_PACK_UNORM4X8: return {"pack4x8unorm", CoreType::FLOAT4, CoreType::UINT};
        case IR::OP_PACK_SNORM4X8: return {"pack4x8snorm", CoreType::FLOAT4, CoreType::UINT};
        case IR::OP_PACK_UNORM2X16: return {"pack2x16unorm", CoreType::FLOAT2, CoreType::UINT};
        case IR::OP_PACK_SNORM2X16: return {"pack2x16snorm", CoreType::FLOAT2, CoreType::UINT};
        case IR::OP_PACK_HALF2X16: return {"pack2x16float", CoreType::FLOAT2, CoreType::UINT};
        case IR::OP_UNPACK_UNORM4X8: return {"unpack4x8unorm", CoreType::UINT, CoreType::FLOAT4};
        case IR::OP_UNPACK_SNORM4X8: return {"unpack4x8snorm", CoreType::UINT, CoreType::FLOAT4};
        case IR::OP_UNPACK_UNORM2X16: return {"unpack2x16unorm", CoreType::UINT, CoreType::FLOAT2};
        case IR::OP_UNPACK_SNORM2X16: return {"unpack2x16snorm", CoreType::UINT, CoreType::FLOAT2};
        case IR::OP_UNPACK_HALF2X16: return {"unpack2x16float", CoreType::UINT, CoreType::FLOAT2};
        default: return {nullptr, CoreType::INVALID, CoreType::INVALID};
    }
}

bool IsCopy(u16 opcode) {
    return opcode == IR::OP_LOAD_CONST || opcode == IR::OP_LOAD_REG ||
           opcode == IR::OP_STORE_REG;
}

// Register operands only. Builtin/buffer slots and literal component indices
// must not be interpreted as value references when counting expression uses.
u32 ValueOperandMask(u16 opcode, u32 metadata) {
    if (IsSampledTextureOpcode(opcode)) {
        return opcode == IR::OP_TEX_SAMPLE_GRAD ? 14 :
               opcode == IR::OP_TEX_LEVELS ? 0 :
               opcode == IR::OP_TEX_SAMPLE || opcode == IR::OP_TEX_SIZE ? 2 : 6;
    }
    if (opcode == IR::OP_MAT_MUL || opcode == IR::OP_MAT_VEC_MUL ||
        opcode == IR::OP_VEC_MAT_MUL || opcode == IR::OP_MAT_SCALE) {
        return 3;
    }
    if (BinaryOperator(opcode) != nullptr || opcode == IR::OP_FMOD) {
        return 3;
    }
    const BuiltinOperation function = MathOperation(opcode);
    if (function.name != nullptr) {
        return (1u << function.argumentCount) - 1;
    }
    if (PackingOperation(opcode).name != nullptr) {
        return 1;
    }
    switch (opcode) {
        case IR::OP_STORAGE_LOAD:
        case IR::OP_STORAGE_FIELD:
        case IR::OP_STRUCT_EXTRACT:
        case IR::OP_BRANCH:
        case IR::OP_SWITCH: return 1;
        case IR::OP_STORAGE_INDEX: return 3;
        case IR::OP_STRUCT_ARRAY_EXTRACT: return 5;
        case IR::OP_ARRAY_LOAD:
        case IR::OP_ARRAY_STORE:
        case IR::OP_VEC_SHUFFLE: return 3;
        case IR::OP_VEC_INSERT: return 5;
        case IR::OP_LDEXP:
        case IR::OP_REFLECT: return 3;
        case IR::OP_REFRACT:
        case IR::OP_FACEFORWARD: return 7;
        case IR::OP_BITFIELD_EXTRACT: return 7;
        case IR::OP_BITFIELD_INSERT: return 15;
        case IR::OP_VEC_INSERT_DYNAMIC:
        case IR::OP_SELECT: return 7;
        case IR::OP_VEC_CONSTRUCT: return (1u << metadata) - 1;
        case IR::OP_MAT_TRANSPOSE:
        case IR::OP_MAT_DET:
        case IR::OP_MAT_INVERSE:
        case IR::OP_VEC_EXTRACT:
        case IR::OP_FNEG:
        case IR::OP_INEG:
        case IR::OP_NOT:
        case IR::OP_SATURATE:
        case IR::OP_F2I:
        case IR::OP_I2F:
        case IR::OP_F2U:
        case IR::OP_U2F:
        case IR::OP_I2U:
        case IR::OP_U2I:
        case IR::OP_BITCAST: return 1;
        default: return 0;
    }
}

u32 MatrixDimension(CoreType type) {
    return type >= CoreType::MAT2 && type <= CoreType::MAT4
        ? static_cast<u32>(type) - static_cast<u32>(CoreType::MAT2) + 2 : 0;
}

bool HasResult(u16 opcode) {
    return opcode != IR::OP_NOP && opcode != IR::OP_RET &&
           opcode != IR::OP_STORE_OUTPUT && opcode != IR::OP_DISCARD &&
           opcode != IR::OP_ARRAY_STORE && opcode != IR::OP_LOCAL_STORE;
}

bool CanBroadcast(CoreType source, CoreType target) {
    return source == target || (Width(source) == 1 && Scalar(source) == Scalar(target));
}

} // namespace

const char* InterfaceBuiltinName(InterfaceBuiltin builtin) {
    return BuiltinName(builtin);
}

bool WGSLBuilder::Initialize(BWSL_Arena* outputArena, const IR::IRProgram* program,
                             const CFG* controlFlow, const SymbolTableData* symbolTable,
                             ShaderStage shaderStage) {
    *this = WGSLBuilder{};
    arena = outputArena;
    ir = program;
    cfg = controlFlow;
    symbols = symbolTable;
    stage = shaderStage;
    for (u32 slot = 0; slot < 32; ++slot) {
        resourceForSlot[slot] = InvalidId;
        uniformForSlot[slot] = InvalidId;
        textureForSlot[slot] = InvalidId;
        outputForSlot[slot] = InvalidId;
    }
    for (u32 slot = 0; slot < InputSlotCount; ++slot) {
        inputForSlot[slot] = InvalidId;
    }
    if (arena == nullptr || ir == nullptr) {
        return Fail("WGSL backend requires an arena and prepared IR");
    }
    arenaStart = arena->allocated;
    return true;
}

void WGSLBuilder::SetComputeWorkgroupSize(u32 x, u32 y, u32 z) {
    if (!attemptedEmission) {
        workgroupSize[0] = x;
        workgroupSize[1] = y;
        workgroupSize[2] = z;
    }
}

std::string_view WGSLBuilder::Source() const {
    return source != nullptr ? std::string_view(source, sourceLength) : std::string_view();
}

const char* WGSLBuilder::Error() const {
    return errorMessage;
}

u32 WGSLBuilder::ErrorInstruction() const {
    return errorInstruction;
}

bool WGSLBuilder::Fail(const char* message) {
    if (errorMessage[0] == '\0') {
        std::snprintf(errorMessage, sizeof(errorMessage), "%s", message);
        errorInstruction = currentInstruction;
    }
    source = nullptr;
    sourceLength = 0;
    return false;
}

bool WGSLBuilder::FailOpcode(u16 opcode) {
    char message[192];
    std::snprintf(message, sizeof(message),
                  "WGSL backend does not support IR opcode 0x%02x; "
                  "this operation requires an unsupported target feature",
                  static_cast<unsigned int>(opcode));
    return Fail(message);
}

void* WGSLBuilder::Allocate(size_t count, size_t elementSize) {
    if (count == 0) {
        return nullptr;
    }
    if (elementSize == 0 || count > std::numeric_limits<size_t>::max() / elementSize) {
        Fail("WGSL backend allocation size overflow");
        return nullptr;
    }
    size_t bytes = count * elementSize;
    if (arena == nullptr || arena->memory == nullptr || arena->allocated > arena->capacity) {
        Fail("WGSL backend arena is unavailable");
        return nullptr;
    }
    size_t address = reinterpret_cast<size_t>(arena->memory) + arena->allocated;
    size_t padding = (64 - (address & 63)) & 63;
    size_t remaining = arena->capacity - arena->allocated;
    if (padding > remaining || bytes > remaining - padding) {
        Fail("WGSL backend arena exhausted");
        return nullptr;
    }
    void* memory = arena->Allocate(bytes, 64);
    if (memory == nullptr) {
        Fail("WGSL backend arena exhausted");
    }
    return memory;
}

bool WGSLBuilder::ValueId(u16 reg, u32& valueId) {
    if (reg >= IR::RegisterLimit || reg >= ir->registerCount || registerToValue == nullptr) {
        return Fail("WGSL operand is not a valid value register");
    }
    valueId = registerToValue[reg];
    if (valueId == InvalidId || (!writingSource && values.producerInstructions[valueId] != InvalidId &&
        values.producerInstructions[valueId] >= currentInstruction)) {
        char message[128];
        std::snprintf(message, sizeof(message), "WGSL operand r%u has no preceding SSA definition", reg);
        return Fail(message);
    }
    return true;
}

bool WGSLBuilder::OperandType(u16 operand, CoreType& type) {
    if (operand == 0xFFFF || operand == IR::InvalidOperand) {
        return Fail("WGSL instruction contains an invalid or missing operand");
    }
    if (!IR::IsConstant(operand)) {
        u32 id = InvalidId;
        if (!ValueId(operand, id)) {
            return false;
        }
        if (values.kinds[id] != static_cast<u8>(ValueKind::Value)) {
            return Fail("WGSL value operand unexpectedly refers to storage");
        }
        type = static_cast<CoreType>(values.types[id]);
        return true;
    }
    if (IR::IsBoolConstant(operand)) {
        u32 index = IR::BoolConstantIndex(operand);
        if (ir->boolConstants == nullptr || index >= ir->boolCount) {
            return Fail("WGSL boolean constant index is out of bounds");
        }
        type = CoreType::BOOL;
    } else if (IR::IsFloatConstant(operand)) {
        u32 index = IR::FloatConstantIndex(operand);
        if (ir->floatConstants == nullptr || index >= ir->floatCount) {
            return Fail("WGSL floating-point constant index is out of bounds");
        }
        if (!std::isfinite(ir->floatConstants[index])) {
            return Fail("WGSL backend cannot represent a non-finite source constant");
        }
        type = CoreType::FLOAT;
    } else if (IR::IsUintConstant(operand)) {
        u32 index = IR::UintConstantIndex(operand);
        if (ir->uintConstants == nullptr || index >= ir->uintCount) {
            return Fail("WGSL unsigned constant index is out of bounds");
        }
        type = CoreType::UINT;
    } else {
        u32 index = IR::IntConstantIndex(operand);
        if (ir->intConstants == nullptr || index >= ir->intCount) {
            return Fail("WGSL signed constant index is out of bounds");
        }
        type = CoreType::INT;
    }
    return true;
}

bool WGSLBuilder::NumericOperand(u16 operand, CoreType& type) {
    if (!OperandType(operand, type)) {
        return false;
    }
    if (Width(type) == 0 || Scalar(type) == CoreType::BOOL) {
        return Fail("WGSL numeric operation requires a numeric scalar or vector");
    }
    return true;
}

bool WGSLBuilder::RequireIndex(u16 operand) {
    CoreType type = CoreType::INVALID;
    if (!OperandType(operand, type)) {
        return false;
    }
    if (type != CoreType::INT && type != CoreType::UINT) {
        return Fail("WGSL storage/vector index must be an i32 or u32 scalar");
    }
    return true;
}

bool WGSLBuilder::DefineValue(u32 instruction, CoreType type, ValueKind kind) {
    u16 reg = ir->destinations[instruction];
    if (reg >= IR::RegisterLimit || reg >= ir->registerCount) {
        return Fail("WGSL result register is out of bounds");
    }
    if (registerToValue[reg] != InvalidId) {
        return Fail("WGSL backend requires SSA: result register has multiple definitions");
    }
    if (Width(type) == 0 && !(type >= CoreType::MAT2 && type <= CoreType::MAT4) &&
        !(kind == ValueKind::UniformAddress && type == CoreType::CUSTOM)) {
        return Fail("WGSL value type is unsupported");
    }
    u32 id = values.count++;
    registerToValue[reg] = id;
    values.types[id] = static_cast<u16>(type);
    values.producerInstructions[id] = instruction;
    values.kinds[id] = static_cast<u8>(kind);
    values.resourceIds[id] = InvalidId;
    values.indexOperands[id] = 0xFFFF;
    values.aliasOperands[id] = reg;
    valueOrigins.registers[id] = reg;
    if (uniforms.count != 0) {
        uniformAddresses.bases[id] = InvalidId;
        uniformAddresses.byteOffsets[id] = 0;
        uniformAddresses.strides[id] = 0;
        uniformAddresses.lengths[id] = 0;
        uniformAddresses.structTypes[id] = 0;
    }
    return true;
}

bool WGSLBuilder::PrepareResources(u32 usedSlots) {
    u32 count = static_cast<u32>(std::popcount(usedSlots));
    resources.count = count;
    if (count == 0) {
        return true;
    }
    resources.elementTypes = static_cast<u16*>(Allocate(count, sizeof(u16)));
    resources.access = static_cast<u8*>(Allocate(count, sizeof(u8)));
    resources.groups = static_cast<u32*>(Allocate(count, sizeof(u32)));
    resources.bindings = static_cast<u32*>(Allocate(count, sizeof(u32)));
    resources.strides = static_cast<u32*>(Allocate(count, sizeof(u32)));
    resourceOrigins.logicalSlots = static_cast<u32*>(Allocate(count, sizeof(u32)));
    if (errorMessage[0] != '\0') {
        return false;
    }
    u32 id = 0;
    for (u32 slot = 0; slot < 32; ++slot) {
        if ((usedSlots & (1u << slot)) == 0) {
            continue;
        }
        CoreType type = static_cast<CoreType>(ir->bufferElementCoreTypes[slot]);
        u32 structHash = ir->bufferElementStructTypes[slot];
        if (symbols != nullptr) {
            for (u32 resource = 0; resource < symbols->resources.count; ++resource) {
                const ResourceData& declaration = symbols->resources[resource];
                if (declaration.type != ResourceBinding::StorageBuffer || declaration.bindingIndex != slot) {
                    continue;
                }
                CoreType declaredType = static_cast<CoreType>(declaration.coreType);
                if (type != CoreType::INVALID && declaredType != type) {
                    return Fail("WGSL storage element metadata disagrees with its declaration");
                }
                type = declaredType;
                structHash |= declaration.structTypeHash;
            }
        }
        if (structHash != 0 || Width(type) == 0 || Scalar(type) == CoreType::BOOL) {
            return Fail("WGSL backend requires numeric primitive storage-buffer elements");
        }
        resourceForSlot[slot] = id;
        resources.elementTypes[id] = static_cast<u16>(type);
        resources.access[id] = 0;
        resources.groups[id] = 0;
        resources.bindings[id] = slot;
        resources.strides[id] = Width(type) == 3 ? 16 : 4 * Width(type);
        resourceOrigins.logicalSlots[id] = slot;
        ++id;
    }
    return true;
}

bool WGSLBuilder::AllocateInterface(InterfaceTable& table, InterfaceOrigins& origins,
                                     u32 capacity) {
    table.types = static_cast<u16*>(Allocate(capacity, sizeof(u16)));
    table.locations = static_cast<u32*>(Allocate(capacity, sizeof(u32)));
    table.builtins = static_cast<u8*>(Allocate(capacity, sizeof(u8)));
    table.interpolations = static_cast<u8*>(Allocate(capacity, sizeof(u8)));
    table.samplings = static_cast<u8*>(Allocate(capacity, sizeof(u8)));
    origins.slots = static_cast<u16*>(Allocate(capacity, sizeof(u16)));
    origins.instructions = static_cast<u32*>(Allocate(capacity, sizeof(u32)));
    return errorMessage[0] == '\0';
}

bool WGSLBuilder::AddInterface(InterfaceTable& table, InterfaceOrigins& origins,
                               u32* slotMap, u32 instruction, u16 slot,
                               CoreType type, u32 location, InterfaceBuiltin builtin,
                               InterpolationMode interpolation,
                               InterpolationSampling sampling) {
    const u32 existing = slotMap[slot];
    if (existing != InvalidId) {
        if (table.types[existing] != static_cast<u16>(type) ||
            table.interpolations[existing] != static_cast<u8>(interpolation) ||
            table.samplings[existing] != static_cast<u8>(sampling)) {
            return Fail("WGSL stage interface slot has conflicting types or interpolation");
        }
        return true;
    }
    const u32 id = table.count++;
    slotMap[slot] = id;
    table.types[id] = static_cast<u16>(type);
    table.locations[id] = location;
    table.builtins[id] = static_cast<u8>(builtin);
    table.interpolations[id] = static_cast<u8>(interpolation);
    table.samplings[id] = static_cast<u8>(sampling);
    origins.slots[id] = slot;
    origins.instructions[id] = instruction;
    return true;
}

static bool InterfaceInterpolation(CoreType type, u8 mode, u8 sampling,
                                   InterpolationMode& result,
                                   InterpolationSampling& resultSampling) {
    if (mode > static_cast<u8>(InterpolationMode::NoPerspective) ||
        sampling > static_cast<u8>(InterpolationSampling::Centroid)) {
        return false;
    }
    result = static_cast<InterpolationMode>(mode);
    resultSampling = static_cast<InterpolationSampling>(sampling);
    const CoreType scalar = Scalar(type);
    if (scalar == CoreType::BOOL || Width(type) == 0) {
        return false;
    }
    if (scalar == CoreType::INT || scalar == CoreType::UINT) {
        if (result == InterpolationMode::NoPerspective) {
            return false;
        }
        result = InterpolationMode::Flat;
    }
    // A flat varying is not interpolated, so it has no sampling to choose.
    return result != InterpolationMode::Flat ||
           resultSampling == InterpolationSampling::Center;
}

bool WGSLBuilder::PrepareInput(u32 instruction, u16 slot, CoreType type, bool attribute) {
    if (slot >= InputSlotCount || Width(type) == 0 || Scalar(type) == CoreType::BOOL) {
        return Fail("WGSL stage inputs require a valid slot and 32-bit numeric scalar/vector type");
    }
    InterfaceBuiltin builtin = InterfaceBuiltin::None;
    CoreType expected = type;
    u32 location = InvalidId;
    InterpolationMode interpolation = InterpolationMode::Default;
    InterpolationSampling sampling = InterpolationSampling::Center;
    if (attribute) {
        if (stage != ShaderStage::Vertex || slot >= 16) {
            return Fail("WGSL attributes require vertex stage locations 0 through 15");
        }
        location = slot;
    } else if (slot >= BuiltinInputSlot::GLOBAL_INVOCATION_ID &&
               slot <= BuiltinInputSlot::LOCAL_INVOCATION_INDEX && stage == ShaderStage::Compute) {
        builtin = static_cast<InterfaceBuiltin>(static_cast<u8>(InterfaceBuiltin::GlobalInvocationId) +
                                                slot - BuiltinInputSlot::GLOBAL_INVOCATION_ID);
        expected = slot == BuiltinInputSlot::LOCAL_INVOCATION_INDEX ? CoreType::UINT : CoreType::UINT3;
    } else if (slot == BuiltinInputSlot::VERTEX_ID && stage == ShaderStage::Vertex) {
        builtin = InterfaceBuiltin::VertexIndex;
        expected = CoreType::UINT;
    } else if (slot == BuiltinInputSlot::INSTANCE_ID && stage == ShaderStage::Vertex) {
        builtin = InterfaceBuiltin::InstanceIndex;
        expected = CoreType::UINT;
    } else if (slot == BuiltinInputSlot::FRAG_COORD && stage == ShaderStage::Fragment) {
        builtin = InterfaceBuiltin::Position;
        expected = CoreType::FLOAT4;
    } else if (stage == ShaderStage::Fragment && slot >= OutputSlot::VARYING0 &&
               slot < OutputSlot::VARYING0 + 16) {
        location = slot - OutputSlot::VARYING0;
        if (!InterfaceInterpolation(type, ir->inputInterpolations[slot], ir->inputSamplings[slot],
                                    interpolation, sampling)) {
            return Fail("WGSL fragment varying has an invalid type or interpolation");
        }
    } else {
        return Fail("WGSL input is not available in this shader stage");
    }
    if (type != expected) {
        return Fail("WGSL builtin input has an incompatible type");
    }
    return AddInterface(inputs, inputOrigins, inputForSlot, instruction, slot, type,
                        location, builtin, interpolation, sampling);
}

bool WGSLBuilder::PrepareOutput(u32 instruction, u16 slot, CoreType type) {
    if (slot >= 32 || Width(type) == 0 || Scalar(type) == CoreType::BOOL) {
        return Fail("WGSL outputs require a valid slot and 32-bit numeric scalar/vector type");
    }
    InterfaceBuiltin builtin = InterfaceBuiltin::None;
    u32 location = InvalidId;
    InterpolationMode interpolation = InterpolationMode::Default;
    InterpolationSampling sampling = InterpolationSampling::Center;
    if (stage == ShaderStage::Vertex && slot == OutputSlot::POSITION) {
        if (type != CoreType::FLOAT4) {
            return Fail("WGSL vertex position must be float4");
        }
        builtin = InterfaceBuiltin::Position;
    } else if (stage == ShaderStage::Vertex && slot >= OutputSlot::VARYING0 &&
               slot < OutputSlot::VARYING0 + 16) {
        location = slot - OutputSlot::VARYING0;
        if (!InterfaceInterpolation(type, ir->outputInterpolations[slot], ir->outputSamplings[slot],
                                    interpolation, sampling)) {
            return Fail("WGSL vertex varying has an invalid type or interpolation");
        }
    } else if (stage == ShaderStage::Fragment && slot == OutputSlot::DEPTH) {
        if (type != CoreType::FLOAT) {
            return Fail("WGSL fragment depth must be float");
        }
        builtin = InterfaceBuiltin::FragDepth;
    } else if (stage == ShaderStage::Fragment && slot >= OutputSlot::COLOR &&
               slot < OutputSlot::COLOR + FragmentOutput::MAX_COLOR_ATTACHMENTS) {
        location = slot - OutputSlot::COLOR;
    } else {
        return Fail("WGSL output is not available in this shader stage");
    }
    return AddInterface(outputs, outputOrigins, outputForSlot, instruction, slot, type,
                        location, builtin, interpolation, sampling);
}

bool WGSLBuilder::Prepare() {
    if (stage != ShaderStage::Compute && stage != ShaderStage::Vertex && stage != ShaderStage::Fragment) {
        return Fail("WGSL supports compute, vertex and fragment entry points");
    }
    for (u32 axis = 0; stage == ShaderStage::Compute && axis < 3; ++axis) {
        if (workgroupSize[axis] == 0) {
            return Fail("WGSL compute workgroup dimensions must be nonzero");
        }
    }
    structured = cfg != nullptr && cfg->blockCount > 1;
    if (!structured && ir->phiCount != 0) {
        return Fail("WGSL requires a CFG for phi nodes");
    }
    if (ir->sharedVarCount != 0 || ir->localArrayCount != 0) {
        return Fail("WGSL backend does not support workgroup storage or local arrays");
    }
    if (ir->instructionCount != 0 && (ir->opcodes == nullptr || ir->destinations == nullptr ||
        ir->operands == nullptr || ir->types == nullptr || ir->metadata == nullptr)) {
        return Fail("WGSL input is missing required instruction arrays");
    }
    if (ir->undefRegCount != 0 && (ir->undefRegs == nullptr || ir->undefRegTypes == nullptr)) {
        return Fail("WGSL undefined SSA temporary metadata is missing");
    }
    if (ir->registerCount != 0 && ir->registerTypes == nullptr) {
        return Fail("WGSL input is missing register types");
    }
    u32 usedSlots = 0;
    u32 definitionCount = ir->phiCount + ir->undefRegCount;
    u32 inputCapacity = 0;
    u32 outputCapacity = 0;
    bool returned = false;
    for (u32 instruction = 0; instruction < ir->instructionCount; ++instruction) {
        currentInstruction = instruction;
        u16 opcode = ir->opcodes[instruction];
        if (opcode == IR::OP_NOP) {
            continue;
        }
        if (!structured && returned && opcode != IR::OP_RET) {
            return Fail("WGSL backend requires one straight-line entry block");
        }
        if (opcode == IR::OP_RET) {
            if (ir->destinations[instruction] != 0 && ir->destinations[instruction] != 0xFFFF) {
                return Fail("WGSL entry point IR returns cannot carry a value");
            }
            returned = true;
        }
        if (opcode == IR::OP_DISCARD) {
            returned = true;
        }
        if (opcode == IR::OP_LOAD_INPUT || opcode == IR::OP_LOAD_ATTR) {
            ++inputCapacity;
        } else if (opcode == IR::OP_STORE_OUTPUT) {
            ++outputCapacity;
        }
        if (opcode == IR::OP_STORAGE_PTR) {
            u32 slot = ir->GetOperand(instruction, 0);
            if (slot >= 32) {
                return Fail("WGSL storage-buffer logical slot is out of bounds");
            }
            usedSlots |= 1u << slot;
        }
        if (HasResult(opcode) && opcode != IR::OP_JUMP && opcode != IR::OP_BRANCH && opcode != IR::OP_SWITCH) {
            ++definitionCount;
        }
    }
    currentInstruction = InvalidId;
    if (!PrepareResources(usedSlots) || !PrepareAdditionalResources()) {
        return false;
    }
    if (!AllocateInterface(inputs, inputOrigins, inputCapacity) ||
        !AllocateInterface(outputs, outputOrigins, outputCapacity)) {
        return false;
    }
    // SSA keeps spare allocation capacity in registerCount. The operand
    // encoding limits addressable registers, so map only that domain;
    // DefineValue and ValueId validate every actual register independently.
    const u32 registerMapCount = ir->registerCount < IR::RegisterLimit
        ? ir->registerCount : IR::RegisterLimit;
    registerToValue = static_cast<u32*>(Allocate(registerMapCount, sizeof(u32)));
    if (definitionCount != 0) {
        values.types = static_cast<u16*>(Allocate(definitionCount, sizeof(u16)));
        values.producerInstructions = static_cast<u32*>(Allocate(definitionCount, sizeof(u32)));
        values.kinds = static_cast<u8*>(Allocate(definitionCount, sizeof(u8)));
        values.resourceIds = static_cast<u32*>(Allocate(definitionCount, sizeof(u32)));
        values.indexOperands = static_cast<u16*>(Allocate(definitionCount, sizeof(u16)));
        values.aliasOperands = static_cast<u16*>(Allocate(definitionCount, sizeof(u16)));
        values.useCounts = static_cast<u32*>(Allocate(definitionCount, sizeof(u32)));
        values.useInstructions = static_cast<u32*>(Allocate(definitionCount, sizeof(u32)));
        values.inlineDepths = static_cast<u8*>(Allocate(definitionCount, sizeof(u8)));
        values.functionScope = static_cast<u8*>(Allocate(definitionCount, sizeof(u8)));
        valueOrigins.registers = static_cast<u16*>(Allocate(definitionCount, sizeof(u16)));
        if (uniforms.count != 0) {
            uniformAddresses.bases = static_cast<u32*>(Allocate(definitionCount, sizeof(u32)));
            uniformAddresses.byteOffsets = static_cast<u32*>(Allocate(definitionCount, sizeof(u32)));
            uniformAddresses.strides = static_cast<u32*>(Allocate(definitionCount, sizeof(u32)));
            uniformAddresses.lengths = static_cast<u32*>(Allocate(definitionCount, sizeof(u32)));
            uniformAddresses.structTypes = static_cast<u32*>(Allocate(definitionCount, sizeof(u32)));
        }
    }
    if (errorMessage[0] != '\0') {
        return false;
    }
    for (u32 reg = 0; reg < registerMapCount; ++reg) {
        registerToValue[reg] = InvalidId;
    }
    // Undefined SSA temporaries may arise from an unobserved inlined-return
    // path. WGSL's typed zero is one valid choice for the IR's undefined value.
    // Stage output reads retain the separate definite-assignment check.
    for (u32 index = 0; index < ir->undefRegCount; ++index) {
        const u16 reg = ir->undefRegs[index];
        const CoreType type = static_cast<CoreType>(ir->undefRegTypes[index]);
        if (reg >= registerMapCount || registerToValue[reg] != InvalidId || Width(type) == 0) {
            return Fail("WGSL undefined SSA temporary has an invalid register or type");
        }
        const u32 value = values.count++;
        registerToValue[reg] = value;
        values.types[value] = static_cast<u16>(type);
        values.producerInstructions[value] = InvalidId;
        values.kinds[value] = static_cast<u8>(ValueKind::Value);
        values.resourceIds[value] = InvalidId;
        values.indexOperands[value] = 0xFFFF;
        values.aliasOperands[value] = reg;
        valueOrigins.registers[value] = reg;
    }
    if (structured && !PrepareControl()) {
        return false;
    }
    for (u32 instruction = 0; instruction < ir->instructionCount; ++instruction) {
        currentInstruction = instruction;
        if (structured && !control.reachable[cfg->instToBlock[instruction]]) {
            continue;
        }
        if (!PrepareInstruction(instruction)) {
            return false;
        }
    }
    currentInstruction = InvalidId;
    if (stage == ShaderStage::Vertex && outputForSlot[OutputSlot::POSITION] == InvalidId) {
        return Fail("WGSL vertex entry point must write output.position");
    }
    if (stage == ShaderStage::Vertex) {
        for (u32 resource = 0; resource < resources.count; ++resource) {
            if ((resources.access[resource] & AccessWrite) != 0) {
                return Fail("WGSL vertex entry points cannot write storage buffers");
            }
        }
    }
    if (structured && !ValidateOutputAssignments()) {
        return false;
    }
    PlanExpressions();
    stats.instructionCount = ir->instructionCount;
    stats.valueCount = values.count;
    stats.resourceCount = resources.count + uniforms.count + textures.count + samplers.count;
    return true;
}

u32 WGSLBuilder::OperandMask(u32 instruction) const {
    const u16 opcode = ir->opcodes[instruction];
    if (opcode == IR::OP_MAT_CONSTRUCT) {
        // One column vector per operand; metadata is not always set.
        const u16 destination = ir->destinations[instruction];
        const u32 columns = destination < ir->registerCount
            ? MatrixDimension(static_cast<CoreType>(ir->registerTypes[destination])) : 0;
        return (1u << columns) - 1;
    }
    if (opcode == IR::OP_VEC_CONSTRUCT) {
        return (1u << VectorArgumentCount(instruction)) - 1;
    }
    return ValueOperandMask(opcode, ir->metadata[instruction]);
}

u32 WGSLBuilder::VectorArgumentCount(u32 instruction) const {
    // Some lowerings leave the count out of metadata; then the operands
    // run until the first unused slot.
    const u32 metadata = ir->metadata[instruction];
    if (metadata >= 1 && metadata <= 4) {
        return metadata;
    }
    u32 count = 0;
    while (count < 4 && ir->GetOperand(instruction, count) != IR::UnusedOperand) {
        ++count;
    }
    return count;
}

void WGSLBuilder::CountUse(u16 operand, u32 count) {
    if (IR::IsConstant(operand)) return;
    operand = values.aliasOperands[registerToValue[operand]];
    if (!IR::IsConstant(operand)) {
        const u32 value = registerToValue[operand];
        const ValueKind kind = static_cast<ValueKind>(values.kinds[value]);
        if (kind == ValueKind::StorageElement) {
            CountUse(values.indexOperands[value]);
            return;
        }
        if (kind == ValueKind::UniformAddress) {
            for (u32 address = value; address != InvalidId; address = uniformAddresses.bases[address]) {
                if (values.indexOperands[address] != 0xFFFF) {
                    CountUse(values.indexOperands[address]);
                }
            }
            return;
        }
        values.useCounts[value] += count;
        values.useInstructions[value] = currentInstruction;
        const u32 producer = values.producerInstructions[value];
        if (structured && (producer == InvalidId || cfg->instToBlock[producer] != cfg->instToBlock[currentInstruction])) {
            values.functionScope[value] = 1;
        }
    }
}

void WGSLBuilder::PlanExpressions() {
    for (u32 value = 0; value < values.count; ++value) {
        values.useCounts[value] = 0;
        values.useInstructions[value] = InvalidId;
        values.inlineDepths[value] = 0;
        values.functionScope[value] = structured && values.producerInstructions[value] == InvalidId ? 1 : 0;
    }
    for (u32 instruction = 0; instruction < ir->instructionCount; ++instruction) {
        currentInstruction = instruction;
        const u16 opcode = ir->opcodes[instruction];
        if (structured && !control.reachable[cfg->instToBlock[instruction]]) {
            continue;
        }
        const u16 first = ir->GetOperand(instruction, 0);
        const u16 second = ir->GetOperand(instruction, 1);
        if (opcode == IR::OP_STORE_OUTPUT) {
            CountUse(ir->destinations[instruction]);
        } else if (HasResult(opcode) && opcode != IR::OP_JUMP && opcode != IR::OP_BRANCH &&
                   opcode != IR::OP_SWITCH &&
                   values.kinds[registerToValue[ir->destinations[instruction]]] != static_cast<u8>(ValueKind::Value)) {
            // Address plans are not emitted evaluations. Count their indices
            // at each actual load, so single-use arithmetic is not counted
            // twice or incorrectly forced into a separate WGSL declaration.
            continue;
        } else if (opcode == IR::OP_STORAGE_LOAD) {
            CountUse(first);
        } else if (opcode == IR::OP_FMOD) {
            // The floor-based expansion mentions each argument twice.
            CountUse(first, 2);
            CountUse(second, 2);
        } else if (opcode == IR::OP_VEC_SHUFFLE) {
            const u32 value = registerToValue[ir->destinations[instruction]];
            const u32 leftWidth = Width(static_cast<CoreType>(values.types[registerToValue[first]]));
            const u32 width = Width(static_cast<CoreType>(values.types[value]));
            for (u32 component = 0; component < width; ++component) {
                const u32 index = (ir->metadata[instruction] >> (4 * component)) & 0xF;
                CountUse(index < leftWidth ? first : second);
            }
        } else {
            const u32 mask = OperandMask(instruction);
            for (u32 argument = 0; argument < 4; ++argument) {
                if ((mask & (1u << argument)) != 0) {
                    CountUse(ir->GetOperand(instruction, argument));
                }
            }
        }
    }
    if (structured) {
        for (u32 phi = 0; phi < ir->phiCount; ++phi) {
            if (!control.phiLive[phi]) {
                continue;
            }
            for (u32 operand = 0; operand < ir->GetPhiOperandCount(phi); ++operand) {
                const u32 predecessor = ir->GetPhiOperandBlock(phi, operand);
                if (control.reachable[predecessor]) {
                    currentInstruction = cfg->lastInst[predecessor];
                    CountUse(ir->GetPhiOperandValue(phi, operand));
                }
            }
        }
    }
    for (u32 value = 0; value < values.count; ++value) {
        const u32 instruction = values.producerInstructions[value];
        if (instruction == InvalidId || values.functionScope[value]) {
            continue;
        }
        const u16 opcode = ir->opcodes[instruction];
        const u32 mask = OperandMask(instruction);
        if (values.useCounts[value] != 1 || mask == 0 ||
            values.kinds[value] != static_cast<u8>(ValueKind::Value) ||
            opcode == IR::OP_ARRAY_LOAD || opcode == IR::OP_STORAGE_LOAD || opcode == IR::OP_VEC_INSERT ||
            opcode == IR::OP_VEC_INSERT_DYNAMIC) {
            continue;
        }
        // Preserve distant vector accumulations at their original evaluation
        // point. Deferring a partial sum until the next accumulation can keep
        // several completed contributions live across an unrelated calculation.
        const u32 consumer = values.useInstructions[value];
        if (opcode == IR::OP_FADD && Width(static_cast<CoreType>(values.types[value])) > 1 &&
            ir->opcodes[consumer] == IR::OP_FADD && consumer - instruction > 16) {
            continue;
        }
        u32 depth = 1;
        for (u32 argument = 0; argument < 4; ++argument) {
            if ((mask & (1u << argument)) == 0) continue;
            u16 operand = ir->GetOperand(instruction, argument);
            if (IR::IsConstant(operand)) continue;
            operand = values.aliasOperands[registerToValue[operand]];
            if (!IR::IsConstant(operand)) {
                const u32 childDepth = values.inlineDepths[registerToValue[operand]];
                if (childDepth + 1 > depth) depth = childDepth + 1;
            }
        }
        // Bound recursive emission and WGSL parser nesting. Loads and mutable
        // vector construction stay at their original evaluation points.
        if (depth <= 4) values.inlineDepths[value] = static_cast<u8>(depth);
    }
    currentInstruction = InvalidId;
}

bool WGSLBuilder::PrepareInstruction(u32 instruction) {
    u16 opcode = ir->opcodes[instruction];
    if (opcode == IR::OP_NOP || opcode == IR::OP_RET || opcode == IR::OP_JUMP ||
        opcode == IR::OP_BRANCH || opcode == IR::OP_SWITCH) {
        return true;
    }
    u16 first = ir->GetOperand(instruction, 0);
    u16 second = ir->GetOperand(instruction, 1);
    u16 third = ir->GetOperand(instruction, 2);
    u16 destination = ir->destinations[instruction];
    CoreType type = destination < ir->registerCount
        ? static_cast<CoreType>(ir->registerTypes[destination]) : CoreType::INVALID;
    if (type == CoreType::INVALID || type == CoreType::VOID) {
        type = static_cast<CoreType>(ir->types[instruction]);
    }

    if (uniforms.count != 0 && destination < ir->registerCount) {
        bool handled = false;
        if (!PrepareUniformInstruction(instruction, handled)) {
            return false;
        }
        if (handled) {
            return true;
        }
    }
    if (IsSampledTextureOpcode(opcode)) {
        return PrepareTextureInstruction(instruction);
    }
    if (opcode == IR::OP_MAT_MUL || opcode == IR::OP_MAT_VEC_MUL ||
        opcode == IR::OP_VEC_MAT_MUL || opcode == IR::OP_MAT_SCALE) {
        CoreType left = CoreType::INVALID;
        CoreType right = CoreType::INVALID;
        if (!OperandType(first, left) || !OperandType(second, right)) {
            return false;
        }
        const CoreType matrix = opcode == IR::OP_VEC_MAT_MUL ? right : left;
        const u32 dimension = static_cast<u32>(matrix) - static_cast<u32>(CoreType::MAT2) + 2;
        const CoreType vector = Shape(CoreType::FLOAT, dimension);
        const bool valid = matrix >= CoreType::MAT2 && matrix <= CoreType::MAT4 &&
            (opcode == IR::OP_MAT_MUL ? right == matrix && type == matrix :
             opcode == IR::OP_MAT_SCALE ? right == CoreType::FLOAT && type == matrix :
             opcode == IR::OP_VEC_MAT_MUL ? left == vector && type == vector : right == vector && type == vector);
        return valid ? DefineValue(instruction, type, ValueKind::Value) : Fail("WGSL matrix multiplication has incompatible types");
    }
    if (opcode == IR::OP_STORAGE_PTR) {
        u32 resource = resourceForSlot[first];
        CoreType element = static_cast<CoreType>(resources.elementTypes[resource]);
        if (!DefineValue(instruction, element, ValueKind::StorageRoot)) {
            return false;
        }
        values.resourceIds[values.count - 1] = resource;
        return true;
    }
    if (IsCopy(opcode) && !IR::IsConstant(first)) {
        u32 sourceId = InvalidId;
        if (!ValueId(first, sourceId)) {
            return false;
        }
        ValueKind kind = static_cast<ValueKind>(values.kinds[sourceId]);
        if (kind != ValueKind::Value) {
            CoreType element = static_cast<CoreType>(values.types[sourceId]);
            if (!DefineValue(instruction, element, kind)) {
                return false;
            }
            u32 result = values.count - 1;
            values.resourceIds[result] = values.resourceIds[sourceId];
            values.indexOperands[result] = values.indexOperands[sourceId];
            return true;
        }
    }
    if (opcode == IR::OP_STORAGE_INDEX) {
        u32 base = InvalidId;
        if (!ValueId(first, base) || !RequireIndex(second)) {
            return false;
        }
        if (values.kinds[base] != static_cast<u8>(ValueKind::StorageRoot)) {
            return Fail("WGSL backend supports one index into a primitive storage buffer");
        }
        CoreType element = static_cast<CoreType>(values.types[base]);
        if (!DefineValue(instruction, element, ValueKind::StorageElement)) {
            return false;
        }
        u32 result = values.count - 1;
        values.resourceIds[result] = values.resourceIds[base];
        values.indexOperands[result] = second;
        return true;
    }
    if (opcode == IR::OP_STORAGE_LOAD || opcode == IR::OP_ARRAY_LOAD) {
        u32 base = InvalidId;
        if (!ValueId(first, base)) {
            return false;
        }
        ValueKind kind = static_cast<ValueKind>(values.kinds[base]);
        if (opcode == IR::OP_ARRAY_LOAD && kind == ValueKind::Value) {
            CoreType vectorType = static_cast<CoreType>(values.types[base]);
            const u32 columns = MatrixDimension(vectorType);
            if (columns != 0 && type == Shape(CoreType::FLOAT, columns) && RequireIndex(second)) {
                return DefineValue(instruction, type, ValueKind::Value);
            }
            if (Width(vectorType) < 2 || type != Scalar(vectorType) || !RequireIndex(second)) {
                return Fail("WGSL backend array loads require a storage buffer or vector");
            }
            return DefineValue(instruction, type, ValueKind::Value);
        }
        ValueKind expected = opcode == IR::OP_STORAGE_LOAD
            ? ValueKind::StorageElement : ValueKind::StorageRoot;
        if (kind != expected || (opcode == IR::OP_ARRAY_LOAD && !RequireIndex(second))) {
            return Fail("WGSL storage load has an unsupported access path");
        }
        CoreType element = static_cast<CoreType>(values.types[base]);
        if (type != element) {
            return Fail("WGSL storage load result does not match its buffer element type");
        }
        resources.access[values.resourceIds[base]] |= AccessRead;
        return DefineValue(instruction, element, ValueKind::Value);
    }
    if (opcode == IR::OP_ARRAY_STORE) {
        u32 base = InvalidId;
        CoreType storedType = CoreType::INVALID;
        if (!ValueId(destination, base) || !RequireIndex(first) || !OperandType(second, storedType)) {
            return false;
        }
        if (values.kinds[base] != static_cast<u8>(ValueKind::StorageRoot)) {
            return Fail("WGSL backend array stores require a primitive storage-buffer root");
        }
        if (storedType != static_cast<CoreType>(values.types[base])) {
            return Fail("WGSL stored value does not match its buffer element type");
        }
        resources.access[values.resourceIds[base]] |= AccessWrite;
        return true;
    }
    if (opcode == IR::OP_LOAD_INPUT || opcode == IR::OP_LOAD_ATTR) {
        return PrepareInput(instruction, first, type, opcode == IR::OP_LOAD_ATTR) &&
               DefineValue(instruction, type, ValueKind::Value);
    }
    if (opcode == IR::OP_STORE_OUTPUT) {
        CoreType storedType = CoreType::INVALID;
        return OperandType(destination, storedType) && PrepareOutput(instruction, first, storedType);
    }
    if (opcode == IR::OP_LOAD_OUTPUT) {
        if (first >= 32 || (!structured && outputForSlot[first] == InvalidId)) {
            return Fail("WGSL stage output is read before its first assignment");
        }
        if (outputForSlot[first] != InvalidId && outputs.types[outputForSlot[first]] != static_cast<u16>(type)) {
            return Fail("WGSL stage output load has an incompatible type");
        }
        return DefineValue(instruction, type, ValueKind::Value);
    }
    if (opcode == IR::OP_DISCARD) {
        return stage == ShaderStage::Fragment || Fail("WGSL discard requires a fragment entry point");
    }
    if (IsCopy(opcode)) {
        CoreType sourceType = CoreType::INVALID;
        if (!OperandType(first, sourceType)) {
            return false;
        }
        if (type != sourceType) {
            return Fail("WGSL SSA copy changes type without an explicit conversion");
        }
        if (!DefineValue(instruction, type, ValueKind::Value)) {
            return false;
        }
        values.aliasOperands[values.count - 1] = IR::IsConstant(first)
            ? first : values.aliasOperands[registerToValue[first]];
        return true;
    }

    const char* binary = BinaryOperator(opcode);
    if (binary != nullptr || opcode == IR::OP_FMOD) {
        CoreType left = CoreType::INVALID;
        CoreType right = CoreType::INVALID;
        if (!OperandType(first, left) || !OperandType(second, right)) {
            return false;
        }
        bool comparison = opcode >= IR::OP_FEQ && opcode <= IR::OP_UGE;
        bool shift = opcode == IR::OP_SHL || opcode == IR::OP_SHR || opcode == IR::OP_ASR;
        CoreType operandType = comparison ? Shape(Scalar(left), Width(type)) : type;
        if (shift) {
            if ((Scalar(type) != CoreType::INT && Scalar(type) != CoreType::UINT) ||
                left != type || (Scalar(right) != CoreType::INT && Scalar(right) != CoreType::UINT) ||
                (Width(right) != 1 && Width(right) != Width(type))) {
                return Fail("WGSL shift operands have incompatible integer types");
            }
        } else {
            if (Width(type) == 0 || !CanBroadcast(left, operandType) || !CanBroadcast(right, operandType)) {
                return Fail("WGSL binary operands have incompatible types or vector widths");
            }
            CoreType scalar = Scalar(operandType);
            if (comparison) {
                if (Scalar(type) != CoreType::BOOL ||
                    (scalar == CoreType::BOOL && opcode != IR::OP_IEQ && opcode != IR::OP_INE)) {
                    return Fail("WGSL comparison requires a compatible boolean result");
                }
            } else if (opcode == IR::OP_AND || opcode == IR::OP_OR || opcode == IR::OP_XOR) {
                if (scalar == CoreType::FLOAT || (scalar == CoreType::BOOL && opcode == IR::OP_XOR)) {
                    return Fail("WGSL bitwise operation requires integer operands");
                }
            } else if (scalar == CoreType::BOOL) {
                return Fail("WGSL arithmetic does not accept boolean operands");
            }
            bool floating = (opcode >= IR::OP_FADD && opcode <= IR::OP_FMOD) ||
                            opcode == IR::OP_FREM || (opcode >= IR::OP_FEQ && opcode <= IR::OP_FGE);
            if (floating && scalar != CoreType::FLOAT) {
                return Fail("WGSL floating-point instruction requires f32 operands");
            }
            if (opcode >= IR::OP_IADD && opcode <= IR::OP_IMOD &&
                scalar != CoreType::INT && scalar != CoreType::UINT) {
                return Fail("WGSL integer arithmetic requires i32 or u32 operands");
            }
            if (opcode >= IR::OP_ILT && opcode <= IR::OP_IGE && scalar != CoreType::INT) {
                return Fail("WGSL signed comparison requires i32 operands");
            }
            if (opcode >= IR::OP_ULT && opcode <= IR::OP_UGE && scalar != CoreType::UINT) {
                return Fail("WGSL unsigned comparison requires u32 operands");
            }
            if ((opcode == IR::OP_IEQ || opcode == IR::OP_INE) && scalar == CoreType::FLOAT) {
                return Fail("WGSL integer equality requires integer or boolean operands");
            }
        }
        return DefineValue(instruction, type, ValueKind::Value);
    }

    BuiltinOperation function = MathOperation(opcode);
    if (function.name != nullptr) {
        if (opcode >= IR::OP_DDX && opcode <= IR::OP_FWIDTH_COARSE && stage != ShaderStage::Fragment) {
            return Fail("WGSL derivatives require a fragment entry point");
        }
        CoreType argumentType = CoreType::INVALID;
        if (!OperandType(first, argumentType)) {
            return false;
        }
        bool reduction = opcode == IR::OP_DOT || opcode == IR::OP_LENGTH || opcode == IR::OP_DISTANCE;
        bool boolReduction = opcode == IR::OP_ANY || opcode == IR::OP_ALL;
        CoreType expected = reduction || boolReduction ? Scalar(argumentType) : type;
        if (type != expected || Width(type) == 0 ||
            (!reduction && !boolReduction && !CanBroadcast(argumentType, type))) {
            return Fail("WGSL builtin result has an incompatible type");
        }
        if (!reduction && !boolReduction) {
            argumentType = type;
        }
        CoreType scalar = Scalar(argumentType);
        if (boolReduction) {
            if (scalar != CoreType::BOOL || Width(argumentType) < 2) {
                return Fail("WGSL any/all require a boolean vector");
            }
        } else {
            bool integerFunction = (opcode >= IR::OP_IABS && opcode <= IR::OP_UCLAMP) ||
                                   (opcode >= IR::OP_POPCNT && opcode <= IR::OP_REVERSE_BITS);
            bool signedFunction = opcode == IR::OP_SIGN && scalar == CoreType::INT;
            if ((integerFunction && scalar != CoreType::INT && scalar != CoreType::UINT) ||
                (!integerFunction && !signedFunction && scalar != CoreType::FLOAT)) {
                return Fail("WGSL builtin operand has an incompatible scalar type");
            }
            if (opcode >= IR::OP_IABS && opcode <= IR::OP_ICLAMP && scalar != CoreType::INT) {
                return Fail("WGSL signed integer builtin requires i32 operands");
            }
            if (opcode >= IR::OP_UMIN && opcode <= IR::OP_UCLAMP && scalar != CoreType::UINT) {
                return Fail("WGSL unsigned integer builtin requires u32 operands");
            }
            if ((opcode == IR::OP_DOT || opcode == IR::OP_NORMALIZE) &&
                Width(argumentType) < 2) {
                return Fail("WGSL vector builtin requires vector operands");
            }
            if (opcode == IR::OP_CROSS && argumentType != CoreType::FLOAT3) {
                return Fail("WGSL cross requires vec3<f32> operands");
            }
        }
        for (u32 argument = 1; argument < function.argumentCount; ++argument) {
            CoreType operandType = CoreType::INVALID;
            if (!OperandType(ir->GetOperand(instruction, argument), operandType)) {
                return false;
            }
            if (!CanBroadcast(operandType, argumentType)) {
                return Fail("WGSL builtin arguments have incompatible types");
            }
        }
        return DefineValue(instruction, type, ValueKind::Value);
    }

    const PackOperation packing = PackingOperation(opcode);
    if (packing.name != nullptr) {
        CoreType argumentType = CoreType::INVALID;
        if (!OperandType(first, argumentType)) {
            return false;
        }
        if (argumentType != packing.argument || type != packing.result) {
            return Fail("WGSL packing builtin has incompatible operand or result types");
        }
        return DefineValue(instruction, type, ValueKind::Value);
    }

    switch (opcode) {
        case IR::OP_FNEG:
        case IR::OP_INEG:
        case IR::OP_NOT:
        case IR::OP_SATURATE: {
            CoreType sourceType = CoreType::INVALID;
            if (!OperandType(first, sourceType)) {
                return false;
            }
            if (opcode == IR::OP_FNEG && sourceType == type && MatrixDimension(type) != 0) {
                break;
            }
            if (sourceType != type || Width(type) == 0) {
                return Fail("WGSL unary operand has an incompatible type");
            }
            CoreType scalar = Scalar(type);
            if ((opcode == IR::OP_FNEG || opcode == IR::OP_SATURATE) && scalar != CoreType::FLOAT) {
                return Fail("WGSL floating-point unary operation requires f32");
            }
            if (opcode == IR::OP_INEG && scalar != CoreType::INT && scalar != CoreType::UINT) {
                return Fail("WGSL integer negation requires i32 or u32");
            }
            if (opcode == IR::OP_NOT && scalar == CoreType::FLOAT) {
                return Fail("WGSL inversion requires integer or boolean operands");
            }
            break;
        }
        case IR::OP_F2I:
        case IR::OP_I2F:
        case IR::OP_F2U:
        case IR::OP_U2F:
        case IR::OP_I2U:
        case IR::OP_U2I:
        case IR::OP_BITCAST: {
            CoreType sourceType = CoreType::INVALID;
            if (!NumericOperand(first, sourceType)) {
                return false;
            }
            if (Width(type) == 0 || Scalar(type) == CoreType::BOOL || Width(type) != Width(sourceType)) {
                return Fail("WGSL conversion requires equally shaped numeric operands");
            }
            CoreType expectedSource = CoreType::INVALID;
            CoreType expectedTarget = CoreType::INVALID;
            switch (opcode) {
                case IR::OP_F2I:
                    expectedSource = CoreType::FLOAT;
                    expectedTarget = CoreType::INT;
                    break;
                case IR::OP_I2F:
                    expectedSource = CoreType::INT;
                    expectedTarget = CoreType::FLOAT;
                    break;
                case IR::OP_F2U:
                    expectedSource = CoreType::FLOAT;
                    expectedTarget = CoreType::UINT;
                    break;
                case IR::OP_U2F:
                    expectedSource = CoreType::UINT;
                    expectedTarget = CoreType::FLOAT;
                    break;
                case IR::OP_I2U:
                    expectedSource = CoreType::INT;
                    expectedTarget = CoreType::UINT;
                    break;
                case IR::OP_U2I:
                    expectedSource = CoreType::UINT;
                    expectedTarget = CoreType::INT;
                    break;
                default: break;
            }
            if (opcode != IR::OP_BITCAST &&
                (Scalar(sourceType) != expectedSource || Scalar(type) != expectedTarget)) {
                return Fail("WGSL conversion opcode disagrees with its operand types");
            }
            break;
        }
        case IR::OP_SELECT: {
            CoreType whenFalse = CoreType::INVALID;
            CoreType whenTrue = CoreType::INVALID;
            CoreType condition = CoreType::INVALID;
            if (!OperandType(first, whenFalse) || !OperandType(second, whenTrue) ||
                !OperandType(third, condition)) {
                return false;
            }
            if (whenFalse != type || whenTrue != type || Scalar(condition) != CoreType::BOOL ||
                (Width(condition) != 1 && Width(condition) != Width(type))) {
                return Fail("WGSL select requires matching values and a boolean condition");
            }
            break;
        }
        case IR::OP_VEC_CONSTRUCT: {
            u32 argumentCount = VectorArgumentCount(instruction);
            if (argumentCount == 0 || argumentCount > 4 || Width(type) < 2) {
                return Fail("WGSL vector constructor has invalid argument metadata");
            }
            u32 components = 0;
            for (u32 argument = 0; argument < argumentCount; ++argument) {
                CoreType argumentType = CoreType::INVALID;
                if (!OperandType(ir->GetOperand(instruction, argument), argumentType)) {
                    return false;
                }
                components += Width(argumentType);
            }
            if (components != Width(type) && !(argumentCount == 1 && components == 1)) {
                return Fail("WGSL vector constructor has an incompatible component count");
            }
            break;
        }
        case IR::OP_VEC_EXTRACT:
        case IR::OP_VEC_INSERT:
        case IR::OP_VEC_INSERT_DYNAMIC: {
            CoreType vectorType = CoreType::INVALID;
            if (!OperandType(first, vectorType)) {
                return false;
            }
            if (Width(vectorType) < 2) {
                return Fail("WGSL component operation requires a vector");
            }
            if (opcode != IR::OP_VEC_INSERT_DYNAMIC && second >= Width(vectorType)) {
                return Fail("WGSL vector component index is out of bounds");
            }
            CoreType expected = opcode == IR::OP_VEC_EXTRACT ? Scalar(vectorType) : vectorType;
            if (type != expected) {
                return Fail("WGSL component operation result has an incompatible type");
            }
            if (opcode != IR::OP_VEC_EXTRACT) {
                CoreType insertedType = CoreType::INVALID;
                u16 inserted = opcode == IR::OP_VEC_INSERT ? third : second;
                if (!OperandType(inserted, insertedType) || insertedType != Scalar(vectorType)) {
                    return Fail("WGSL inserted component has an incompatible type");
                }
                if (opcode == IR::OP_VEC_INSERT_DYNAMIC && !RequireIndex(third)) {
                    return false;
                }
            }
            break;
        }
        case IR::OP_VEC_SHUFFLE: {
            CoreType left = CoreType::INVALID;
            CoreType right = CoreType::INVALID;
            if (!OperandType(first, left) || !OperandType(second, right)) {
                return false;
            }
            if (Width(left) < 2 || Width(right) < 2 || Width(type) == 0 ||
                Scalar(left) != Scalar(type) || Scalar(right) != Scalar(type)) {
                return Fail("WGSL shuffle requires vectors with matching scalar types");
            }
            for (u32 component = 0; component < Width(type); ++component) {
                u32 index = (ir->metadata[instruction] >> (4 * component)) & 0xF;
                if (index >= Width(left) + Width(right)) {
                    return Fail("WGSL shuffle component index is out of bounds");
                }
            }
            break;
        }
        case IR::OP_REFLECT:
        case IR::OP_REFRACT:
        case IR::OP_FACEFORWARD: {
            const u32 count = opcode == IR::OP_REFLECT ? 2 : 3;
            if (Scalar(type) != CoreType::FLOAT || Width(type) == 0) {
                return Fail("WGSL geometric builtin requires f32 operands");
            }
            for (u32 argument = 0; argument < count; ++argument) {
                CoreType argumentType = CoreType::INVALID;
                if (!OperandType(ir->GetOperand(instruction, argument), argumentType)) {
                    return false;
                }
                const bool eta = opcode == IR::OP_REFRACT && argument == 2;
                if (eta ? argumentType != CoreType::FLOAT : !CanBroadcast(argumentType, type)) {
                    return Fail("WGSL geometric builtin arguments have incompatible types");
                }
            }
            break;
        }
        case IR::OP_LDEXP: {
            CoreType valueType = CoreType::INVALID;
            CoreType exponentType = CoreType::INVALID;
            if (!OperandType(first, valueType) || !OperandType(second, exponentType)) {
                return false;
            }
            if (valueType != type || Scalar(type) != CoreType::FLOAT ||
                (Scalar(exponentType) != CoreType::INT && Scalar(exponentType) != CoreType::UINT) ||
                (Width(exponentType) != 1 && Width(exponentType) != Width(type))) {
                return Fail("WGSL ldexp requires f32 values and integer exponents");
            }
            break;
        }
        case IR::OP_BITFIELD_EXTRACT:
        case IR::OP_BITFIELD_INSERT: {
            const bool insert = opcode == IR::OP_BITFIELD_INSERT;
            CoreType baseType = CoreType::INVALID;
            CoreType insertType = CoreType::INVALID;
            CoreType offsetType = CoreType::INVALID;
            CoreType countType = CoreType::INVALID;
            if (!OperandType(first, baseType) || (insert && !OperandType(second, insertType)) ||
                !OperandType(ir->GetOperand(instruction, insert ? 2 : 1), offsetType) ||
                !OperandType(ir->GetOperand(instruction, insert ? 3 : 2), countType)) {
                return false;
            }
            const CoreType scalar = Scalar(type);
            if (baseType != type || (insert && insertType != type) ||
                (scalar != CoreType::INT && scalar != CoreType::UINT) ||
                (offsetType != CoreType::INT && offsetType != CoreType::UINT) ||
                (countType != CoreType::INT && countType != CoreType::UINT)) {
                return Fail("WGSL bitfield operation has incompatible integer operands");
            }
            break;
        }
        case IR::OP_MAT_CONSTRUCT: {
            const u32 columns = MatrixDimension(type);
            if (columns == 0) {
                return Fail("WGSL matrix constructor requires a square f32 matrix result");
            }
            for (u32 column = 0; column < columns; ++column) {
                CoreType columnType = CoreType::INVALID;
                if (!OperandType(ir->GetOperand(instruction, column), columnType)) {
                    return false;
                }
                if (columnType != Shape(CoreType::FLOAT, columns)) {
                    return Fail("WGSL matrix constructor columns must match its dimension");
                }
            }
            break;
        }
        case IR::OP_MAT_TRANSPOSE:
        case IR::OP_MAT_DET:
        case IR::OP_MAT_INVERSE: {
            CoreType matrix = CoreType::INVALID;
            if (!OperandType(first, matrix)) {
                return false;
            }
            const CoreType expected = opcode == IR::OP_MAT_DET ? CoreType::FLOAT : matrix;
            if (MatrixDimension(matrix) == 0 || type != expected) {
                return Fail("WGSL matrix function has an incompatible type");
            }
            if (opcode == IR::OP_MAT_INVERSE) {
                // WGSL has no inverse builtin; a helper per dimension.
                helperFunctions |= 1u << (MatrixDimension(matrix) - 2);
            }
            break;
        }
        default:
            return FailOpcode(opcode);
    }
    return DefineValue(instruction, type, ValueKind::Value);
}

bool WGSLBuilder::WriteOperand(TextWriter& writer, u16 operand) {
    CoreType type = CoreType::INVALID;
    if (!OperandType(operand, type)) {
        return false;
    }
    if (!IR::IsConstant(operand)) {
        operand = values.aliasOperands[registerToValue[operand]];
        if (!IR::IsConstant(operand)) {
            const u32 valueId = registerToValue[operand];
            const u32 producer = values.producerInstructions[valueId];
            if (producer == InvalidId && !structured) {
                WriteType(writer, type);
                writer.Text("()");
                return true;
            }
            if (producer != InvalidId && (ir->opcodes[producer] == IR::OP_LOAD_INPUT || ir->opcodes[producer] == IR::OP_LOAD_ATTR)) {
                return WriteExpression(writer, producer, valueId);
            } else if (values.inlineDepths[valueId] != 0) {
                const u32 savedInstruction = currentInstruction;
                currentInstruction = producer;
                const bool written = WriteExpression(writer, producer, valueId);
                currentInstruction = savedInstruction;
                return written;
            } else {
                WriteValueName(writer, valueId);
            }
            return true;
        }
    }
    if (type == CoreType::BOOL) {
        writer.Text(ir->boolConstants[IR::ConstantIndex(operand)] != 0 ? "true" : "false");
    } else if (type == CoreType::FLOAT) {
        // Exact hexadecimal f32 literals avoid locale-sensitive formatting
        // and the newer deployment target required by floating to_chars.
        float value = ir->floatConstants[IR::ConstantIndex(operand)];
        u32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        u32 exponentBits = (bits >> 23) & 0xFF;
        const u32 mantissa = bits & 0x7FFFFF;
        // Common exact integers have shorter decimal spellings. Keep the
        // general path hexadecimal so no locale or rounding is involved.
        const u32 shift = exponentBits >= 127 && exponentBits <= 150
            ? 150 - exponentBits : 0;
        if ((bits & 0x7FFFFFFFu) == 0 ||
            (exponentBits >= 127 && exponentBits <= 150 &&
             (mantissa & ((1u << shift) - 1)) == 0)) {
            writer.Text((bits & 0x80000000u) != 0 ? "(-" : "(");
            writer.Number((bits & 0x7FFFFFFFu) == 0 ? 0 : (0x800000u | mantissa) >> shift);
            writer.Text(".0f)");
            return true;
        }
        s32 exponent = exponentBits == 0 ? -126 : static_cast<s32>(exponentBits) - 127;
        u32 fraction = (bits & 0x7FFFFF) << 1;
        char fractionText[6];
        constexpr char hexDigits[] = "0123456789abcdef";
        for (u32 digit = 0; digit < 6; ++digit) {
            fractionText[digit] = hexDigits[(fraction >> (20 - digit * 4)) & 0xF];
        }
        writer.Text((bits & 0x80000000u) != 0 ? "(-0x" : "(0x");
        writer.Text(exponentBits == 0 ? "0." : "1.");
        size_t fractionLength = sizeof(fractionText);
        while (fractionLength > 1 && fractionText[fractionLength - 1] == '0') {
            --fractionLength;
        }
        writer.Bytes(fractionText, fractionLength);
        writer.Text(exponent < 0 ? "p-" : "p+");
        writer.Number(static_cast<u32>(exponent < 0 ? -exponent : exponent));
        writer.Text("f)");
    } else if (type == CoreType::UINT) {
        writer.Number(ir->uintConstants[IR::ConstantIndex(operand)]);
        writer.Text("u");
    } else {
        u32 bits = ir->intConstants[IR::ConstantIndex(operand)];
        s32 value = 0;
        std::memcpy(&value, &bits, sizeof(value));
        if (value == std::numeric_limits<s32>::min()) {
            writer.Text("bitcast<i32>(2147483648u)");
        } else {
            writer.Text("(");
            if (value < 0) {
                writer.Text("-");
                value = -value;
            }
            writer.Number(static_cast<u32>(value));
            writer.Text("i)");
        }
    }
    return true;
}

bool WGSLBuilder::WriteConvertedOperand(TextWriter& writer, u16 operand, CoreType target) {
    CoreType sourceType = CoreType::INVALID;
    if (!OperandType(operand, sourceType)) {
        return false;
    }
    if (sourceType == target) {
        return WriteOperand(writer, operand);
    }
    if (Scalar(sourceType) == CoreType::BOOL && Scalar(target) != CoreType::BOOL) {
        writer.Text("select(");
        WriteType(writer, target);
        writer.Text("(0), ");
        WriteType(writer, target);
        writer.Text("(1), ");
        if (!WriteOperand(writer, operand)) {
            return false;
        }
        writer.Text(")");
        return true;
    }
    if (Scalar(target) == CoreType::BOOL && Scalar(sourceType) != CoreType::BOOL) {
        bool broadcast = Width(target) != Width(sourceType);
        if (broadcast) {
            WriteType(writer, target);
            writer.Text("(");
        }
        writer.Text("(");
        if (!WriteOperand(writer, operand)) {
            return false;
        }
        writer.Text(" != ");
        WriteType(writer, sourceType);
        writer.Text("(0))");
        if (broadcast) {
            writer.Text(")");
        }
        return true;
    }
    WriteType(writer, target);
    writer.Text("(");
    if (!WriteOperand(writer, operand)) {
        return false;
    }
    writer.Text(")");
    return true;
}

bool WGSLBuilder::WriteStorageAddress(TextWriter& writer, u32 valueId) {
    u32 resource = values.resourceIds[valueId];
    WriteResourceName(writer, resourceOrigins.logicalSlots[resource]);
    if (values.kinds[valueId] == static_cast<u8>(ValueKind::StorageElement)) {
        writer.Text("[");
        if (!WriteOperand(writer, values.indexOperands[valueId])) {
            return false;
        }
        writer.Text("]");
    }
    return true;
}

bool WGSLBuilder::WriteExpression(TextWriter& writer, u32 instruction, u32 valueId) {
    u16 opcode = ir->opcodes[instruction];
    u16 first = ir->GetOperand(instruction, 0);
    u16 second = ir->GetOperand(instruction, 1);
    CoreType type = static_cast<CoreType>(values.types[valueId]);
    if (IsCopy(opcode)) {
        return WriteOperand(writer, first);
    }
    const char* binary = BinaryOperator(opcode);
    if (binary != nullptr) {
        CoreType operandType = type;
        if (opcode >= IR::OP_FEQ && opcode <= IR::OP_UGE) {
            CoreType left = CoreType::INVALID;
            if (!OperandType(first, left)) {
                return false;
            }
            operandType = Shape(Scalar(left), Width(type));
        }
        bool shift = opcode == IR::OP_SHL || opcode == IR::OP_SHR || opcode == IR::OP_ASR;
        CoreType shiftType = operandType;
        if (opcode == IR::OP_SHR) {
            shiftType = Shape(CoreType::UINT, Width(type));
        } else if (opcode == IR::OP_ASR) {
            shiftType = Shape(CoreType::INT, Width(type));
        }
        bool convertResult = shift && shiftType != type;
        if (convertResult) {
            WriteType(writer, type);
            writer.Text("(");
        }
        writer.Text("(");
        if (!WriteConvertedOperand(writer, first, shiftType)) {
            return false;
        }
        writer.Text(" ");
        writer.Text(binary);
        writer.Text(" ");
        CoreType rightType = shift ? Shape(CoreType::UINT, Width(type)) : operandType;
        if (!WriteConvertedOperand(writer, second, rightType)) {
            return false;
        }
        writer.Text(")");
        if (convertResult) {
            writer.Text(")");
        }
        return true;
    }
    BuiltinOperation function = MathOperation(opcode);
    if (function.name != nullptr) {
        CoreType argumentType = CoreType::INVALID;
        if (!OperandType(first, argumentType)) {
            return false;
        }
        bool reduction = opcode == IR::OP_DOT || opcode == IR::OP_LENGTH ||
                         opcode == IR::OP_DISTANCE || opcode == IR::OP_ANY || opcode == IR::OP_ALL;
        if (!reduction) {
            argumentType = type;
        }
        writer.Text(function.name);
        writer.Text("(");
        for (u32 argument = 0; argument < function.argumentCount; ++argument) {
            if (argument != 0) {
                writer.Text(", ");
            }
            if (!WriteConvertedOperand(writer, ir->GetOperand(instruction, argument), argumentType)) {
                return false;
            }
        }
        writer.Text(")");
        return true;
    }
    if (IsSampledTextureOpcode(opcode)) {
        return WriteTextureExpression(writer, instruction);
    }
    const PackOperation packing = PackingOperation(opcode);
    if (packing.name != nullptr) {
        writer.Text(packing.name);
        writer.Text("(");
        if (!WriteOperand(writer, first)) {
            return false;
        }
        writer.Text(")");
        return true;
    }
    if (uniforms.count != 0 && values.resourceIds[valueId] != InvalidId &&
        (opcode == IR::OP_LOAD_UNIFORM || opcode == IR::OP_STRUCT_EXTRACT ||
         opcode == IR::OP_STRUCT_ARRAY_EXTRACT || opcode == IR::OP_STORAGE_LOAD || opcode == IR::OP_ARRAY_LOAD)) {
        return WriteUniformLoad(writer, valueId);
    }
    if (opcode == IR::OP_MAT_MUL || opcode == IR::OP_MAT_VEC_MUL ||
        opcode == IR::OP_VEC_MAT_MUL || opcode == IR::OP_MAT_SCALE) {
        writer.Text("(");
        if (!WriteOperand(writer, first)) {
            return false;
        }
        writer.Text(" * ");
        if (!WriteOperand(writer, second)) {
            return false;
        }
        writer.Text(")");
        return true;
    }
    switch (opcode) {
        case IR::OP_LOAD_INPUT:
        case IR::OP_LOAD_ATTR: {
            const u32 input = inputForSlot[first];
            const InterfaceBuiltin builtin = static_cast<InterfaceBuiltin>(inputs.builtins[input]);
            writer.Text("bwsl_");
            if (builtin != InterfaceBuiltin::None) {
                writer.Text(BuiltinName(builtin));
            } else {
                writer.Text(stage == ShaderStage::Vertex ? "attribute_" : "varying_");
                writer.Number(inputs.locations[input]);
            }
            return true;
        }
        case IR::OP_LOAD_OUTPUT:
            writer.Text("bwsl_output.f");
            writer.Number(outputForSlot[first]);
            return true;
        case IR::OP_STORAGE_LOAD:
            return WriteStorageAddress(writer, registerToValue[first]);
        case IR::OP_ARRAY_LOAD: {
            u32 base = registerToValue[first];
            if (values.kinds[base] == static_cast<u8>(ValueKind::Value)) {
                if (!WriteOperand(writer, first)) {
                    return false;
                }
            } else if (!WriteStorageAddress(writer, base)) {
                return false;
            }
            writer.Text("[");
            if (!WriteOperand(writer, second)) {
                return false;
            }
            writer.Text("]");
            return true;
        }
        case IR::OP_FMOD:
            // SPIR-V OpFMod uses floor, while WGSL % uses truncation.
            writer.Text("(");
            if (!WriteConvertedOperand(writer, first, type)) {
                return false;
            }
            writer.Text(" - ");
            if (!WriteConvertedOperand(writer, second, type)) {
                return false;
            }
            writer.Text(" * floor(");
            if (!WriteConvertedOperand(writer, first, type)) {
                return false;
            }
            writer.Text(" / ");
            if (!WriteConvertedOperand(writer, second, type)) {
                return false;
            }
            writer.Text("))");
            return true;
        case IR::OP_FNEG:
        case IR::OP_INEG:
        case IR::OP_NOT:
            if (MatrixDimension(type) != 0) {
                // WGSL negation accepts scalars and vectors only.
                writer.Text("(");
                if (!WriteOperand(writer, first)) {
                    return false;
                }
                writer.Text(" * -1.0f)");
                return true;
            }
            writer.Text("(");
            if (opcode == IR::OP_INEG && Scalar(type) == CoreType::UINT) {
                WriteType(writer, type);
                writer.Text("(0u) - ");
            } else {
                writer.Text(opcode == IR::OP_NOT ? (Scalar(type) == CoreType::BOOL ? "!" : "~") : "-");
            }
            if (!WriteOperand(writer, first)) {
                return false;
            }
            writer.Text(")");
            return true;
        case IR::OP_SATURATE:
            writer.Text("clamp(");
            if (!WriteOperand(writer, first)) {
                return false;
            }
            writer.Text(", ");
            WriteType(writer, type);
            writer.Text("(0.0f), ");
            WriteType(writer, type);
            writer.Text("(1.0f))");
            return true;
        case IR::OP_F2I:
        case IR::OP_I2F:
        case IR::OP_F2U:
        case IR::OP_U2F:
        case IR::OP_I2U:
        case IR::OP_U2I:
            return WriteConvertedOperand(writer, first, type);
        case IR::OP_BITCAST:
            writer.Text("bitcast<");
            WriteType(writer, type);
            writer.Text(">(");
            if (!WriteOperand(writer, first)) {
                return false;
            }
            writer.Text(")");
            return true;
        case IR::OP_SELECT:
            writer.Text("select(");
            for (u32 argument = 0; argument < 3; ++argument) {
                if (argument != 0) {
                    writer.Text(", ");
                }
                if (!WriteOperand(writer, ir->GetOperand(instruction, argument))) {
                    return false;
                }
            }
            writer.Text(")");
            return true;
        case IR::OP_VEC_CONSTRUCT: {
            WriteType(writer, type);
            writer.Text("(");
            u32 argumentCount = VectorArgumentCount(instruction);
            for (u32 argument = 0; argument < argumentCount; ++argument) {
                if (argument != 0) {
                    writer.Text(", ");
                }
                u16 operand = ir->GetOperand(instruction, argument);
                CoreType sourceType = CoreType::INVALID;
                if (!OperandType(operand, sourceType)) {
                    return false;
                }
                if (!WriteConvertedOperand(writer, operand, Shape(Scalar(type), Width(sourceType)))) {
                    return false;
                }
            }
            writer.Text(")");
            return true;
        }
        case IR::OP_REFLECT:
        case IR::OP_REFRACT:
        case IR::OP_FACEFORWARD: {
            // WGSL defines these for vectors only. A scalar is the x of a
            // vector padded with zero, which adds nothing to any dot product.
            const bool scalar = Width(type) == 1;
            writer.Text(opcode == IR::OP_REFLECT ? "reflect(" :
                        opcode == IR::OP_REFRACT ? "refract(" : "faceForward(");
            const u32 count = opcode == IR::OP_REFLECT ? 2 : 3;
            for (u32 argument = 0; argument < count; ++argument) {
                if (argument != 0) {
                    writer.Text(", ");
                }
                const u16 operand = ir->GetOperand(instruction, argument);
                if (opcode == IR::OP_REFRACT && argument == 2) {
                    if (!WriteOperand(writer, operand)) {
                        return false;
                    }
                    continue;
                }
                if (scalar) {
                    writer.Text("vec2<f32>(");
                }
                if (!WriteConvertedOperand(writer, operand, type)) {
                    return false;
                }
                if (scalar) {
                    writer.Text(", 0.0f)");
                }
            }
            writer.Text(scalar ? ").x" : ")");
            return true;
        }
        case IR::OP_LDEXP: {
            // WGSL wants an i32 exponent of the value's shape.
            CoreType exponentType = CoreType::INVALID;
            if (!OperandType(second, exponentType)) {
                return false;
            }
            writer.Text("ldexp(");
            if (!WriteOperand(writer, first)) {
                return false;
            }
            writer.Text(", ");
            const bool splat = Width(exponentType) != Width(type);
            if (splat) {
                WriteType(writer, Shape(CoreType::INT, Width(type)));
                writer.Text("(");
            }
            if (!WriteConvertedOperand(writer, second, Shape(CoreType::INT, Width(exponentType)))) {
                return false;
            }
            writer.Text(splat ? "))" : ")");
            return true;
        }
        case IR::OP_BITFIELD_EXTRACT:
        case IR::OP_BITFIELD_INSERT: {
            // extractBits sign-extends i32 and zero-extends u32, like
            // OpBitFieldSExtract and OpBitFieldUExtract.
            const bool insert = opcode == IR::OP_BITFIELD_INSERT;
            writer.Text(insert ? "insertBits(" : "extractBits(");
            if (!WriteOperand(writer, first)) {
                return false;
            }
            if (insert) {
                writer.Text(", ");
                if (!WriteOperand(writer, second)) {
                    return false;
                }
            }
            for (u32 argument = insert ? 2 : 1; argument < (insert ? 4u : 3u); ++argument) {
                writer.Text(", ");
                if (!WriteConvertedOperand(writer, ir->GetOperand(instruction, argument), CoreType::UINT)) {
                    return false;
                }
            }
            writer.Text(")");
            return true;
        }
        case IR::OP_MAT_CONSTRUCT: {
            WriteType(writer, type);
            writer.Text("(");
            for (u32 column = 0; column < MatrixDimension(type); ++column) {
                if (column != 0) {
                    writer.Text(", ");
                }
                if (!WriteOperand(writer, ir->GetOperand(instruction, column))) {
                    return false;
                }
            }
            writer.Text(")");
            return true;
        }
        case IR::OP_MAT_TRANSPOSE:
        case IR::OP_MAT_DET:
        case IR::OP_MAT_INVERSE: {
            CoreType matrix = CoreType::INVALID;
            if (!OperandType(first, matrix)) {
                return false;
            }
            if (opcode == IR::OP_MAT_INVERSE) {
                writer.Text("bwsl_inverse");
                writer.Number(MatrixDimension(matrix));
                writer.Text("(");
            } else {
                writer.Text(opcode == IR::OP_MAT_DET ? "determinant(" : "transpose(");
            }
            if (!WriteOperand(writer, first)) {
                return false;
            }
            writer.Text(")");
            return true;
        }
        case IR::OP_VEC_EXTRACT:
            if (!WriteOperand(writer, first)) {
                return false;
            }
            writer.Text(".");
            writer.Bytes(&"xyzw"[second], 1);
            return true;
        case IR::OP_VEC_SHUFFLE: {
            CoreType leftType = CoreType::INVALID;
            if (!OperandType(first, leftType)) {
                return false;
            }
            u32 width = Width(type);
            if (width > 1) {
                WriteType(writer, type);
                writer.Text("(");
            }
            for (u32 component = 0; component < width; ++component) {
                if (component != 0) {
                    writer.Text(", ");
                }
                u32 index = (ir->metadata[instruction] >> (4 * component)) & 0xF;
                bool useSecond = index >= Width(leftType);
                if (!WriteOperand(writer, useSecond ? second : first)) {
                    return false;
                }
                writer.Text("[");
                writer.Number(useSecond ? index - Width(leftType) : index);
                writer.Text("u]");
            }
            if (width > 1) {
                writer.Text(")");
            }
            return true;
        }
        default:
            return FailOpcode(opcode);
    }
}

bool WGSLBuilder::WriteInstruction(TextWriter& writer, u32 instruction) {
    u16 opcode = ir->opcodes[instruction];
    if (opcode == IR::OP_NOP) {
        return true;
    }
    if (IsCopy(opcode) || opcode == IR::OP_LOAD_INPUT || opcode == IR::OP_LOAD_ATTR) {
        return true;
    }
    if (opcode == IR::OP_RET) {
        writer.Indent();
        writer.Text(outputs.count == 0 ? "return;\n" : "return bwsl_output;\n");
        return true;
    }
    if (opcode == IR::OP_DISCARD) {
        writer.Indent();
        writer.Text("discard;\n");
        return true;
    }
    u16 first = ir->GetOperand(instruction, 0);
    u16 second = ir->GetOperand(instruction, 1);
    u16 destination = ir->destinations[instruction];
    if (opcode == IR::OP_STORE_OUTPUT) {
        writer.Indent();
        writer.Text("bwsl_output.f");
        writer.Number(outputForSlot[first]);
        writer.Text(" = ");
        if (!WriteOperand(writer, destination)) {
            return false;
        }
        writer.Text(";\n");
        return true;
    }
    u32 valueId = registerToValue[destination];
    if (opcode == IR::OP_ARRAY_STORE) {
        writer.Indent();
        if (!WriteStorageAddress(writer, valueId)) {
            return false;
        }
        writer.Text("[");
        if (!WriteOperand(writer, first)) {
            return false;
        }
        writer.Text("] = ");
        if (!WriteOperand(writer, second)) {
            return false;
        }
        writer.Text(";\n");
        return true;
    }
    if (values.kinds[valueId] != static_cast<u8>(ValueKind::Value)) {
        return true;
    }
    if (values.inlineDepths[valueId] != 0) {
        return true;
    }
    bool insertion = opcode == IR::OP_VEC_INSERT || opcode == IR::OP_VEC_INSERT_DYNAMIC;
    writer.Indent();
    if (!structured || !values.functionScope[valueId]) {
        writer.Text(insertion ? "var " : "let ");
    }
    WriteValueName(writer, valueId);
    writer.Text(" = ");
    if (insertion) {
        if (!WriteOperand(writer, first)) {
            return false;
        }
        writer.Text(";\n");
        writer.Indent();
        WriteValueName(writer, valueId);
        writer.Text("[");
        u16 third = ir->GetOperand(instruction, 2);
        if (opcode == IR::OP_VEC_INSERT) {
            writer.Number(second);
            writer.Text("u");
        } else if (!WriteOperand(writer, third)) {
            return false;
        }
        writer.Text("] = ");
        if (!WriteOperand(writer, opcode == IR::OP_VEC_INSERT ? third : second)) {
            return false;
        }
    } else if (!WriteExpression(writer, instruction, valueId)) {
        return false;
    }
    writer.Text(";\n");
    return true;
}

void WGSLBuilder::WriteInterfaceAttribute(TextWriter& writer, const InterfaceTable& table,
                                           u32 id, bool varying) {
    const InterfaceBuiltin builtin = static_cast<InterfaceBuiltin>(table.builtins[id]);
    if (builtin != InterfaceBuiltin::None) {
        writer.Text("@builtin(");
        writer.Text(BuiltinName(builtin));
        writer.Text(") ");
        return;
    }
    writer.Text("@location(");
    writer.Number(table.locations[id]);
    writer.Text(") ");
    if (varying) {
        const InterpolationMode mode = static_cast<InterpolationMode>(table.interpolations[id]);
        const bool centroid =
            static_cast<InterpolationSampling>(table.samplings[id]) == InterpolationSampling::Centroid;
        if (mode == InterpolationMode::Flat) {
            writer.Text("@interpolate(flat) ");
        } else if (mode == InterpolationMode::NoPerspective) {
            writer.Text(centroid ? "@interpolate(linear, centroid) " : "@interpolate(linear, center) ");
        } else if (centroid) {
            writer.Text("@interpolate(perspective, centroid) ");
        }
    }
}

#include "bwsl_wgsl_backend_resources.inl"
#include "bwsl_wgsl_backend_control.inl"

static void WriteHelperFunctions(TextWriter& writer, u32 helpers) {
    // Adjugate over determinant, as GLSL.std.450 MatrixInverse; m[c][r].
    if (helpers & 1u) {
        writer.Text("fn bwsl_inverse2(m: mat2x2<f32>) -> mat2x2<f32> {\n"
                    "    return mat2x2<f32>(m[1][1], -m[0][1], -m[1][0], m[0][0]) * (1.0f / determinant(m));\n"
                    "}\n\n");
    }
    if (helpers & 2u) {
        writer.Text("fn bwsl_inverse3(m: mat3x3<f32>) -> mat3x3<f32> {\n"
                    "    let r = mat3x3<f32>(cross(m[1], m[2]), cross(m[2], m[0]), cross(m[0], m[1]));\n"
                    "    return transpose(r) * (1.0f / dot(m[0], r[0]));\n"
                    "}\n\n");
    }
    if (helpers & 4u) {
        writer.Text("fn bwsl_inverse4(m: mat4x4<f32>) -> mat4x4<f32> {\n"
                    "    let b00 = m[0][0] * m[1][1] - m[0][1] * m[1][0];\n"
                    "    let b01 = m[0][0] * m[1][2] - m[0][2] * m[1][0];\n"
                    "    let b02 = m[0][0] * m[1][3] - m[0][3] * m[1][0];\n"
                    "    let b03 = m[0][1] * m[1][2] - m[0][2] * m[1][1];\n"
                    "    let b04 = m[0][1] * m[1][3] - m[0][3] * m[1][1];\n"
                    "    let b05 = m[0][2] * m[1][3] - m[0][3] * m[1][2];\n"
                    "    let b06 = m[2][0] * m[3][1] - m[2][1] * m[3][0];\n"
                    "    let b07 = m[2][0] * m[3][2] - m[2][2] * m[3][0];\n"
                    "    let b08 = m[2][0] * m[3][3] - m[2][3] * m[3][0];\n"
                    "    let b09 = m[2][1] * m[3][2] - m[2][2] * m[3][1];\n"
                    "    let b10 = m[2][1] * m[3][3] - m[2][3] * m[3][1];\n"
                    "    let b11 = m[2][2] * m[3][3] - m[2][3] * m[3][2];\n"
                    "    let d = 1.0f / (b00 * b11 - b01 * b10 + b02 * b09 + b03 * b08 - b04 * b07 + b05 * b06);\n"
                    "    return mat4x4<f32>(\n"
                    "        vec4<f32>(m[1][1] * b11 - m[1][2] * b10 + m[1][3] * b09,\n"
                    "                  m[0][2] * b10 - m[0][1] * b11 - m[0][3] * b09,\n"
                    "                  m[3][1] * b05 - m[3][2] * b04 + m[3][3] * b03,\n"
                    "                  m[2][2] * b04 - m[2][1] * b05 - m[2][3] * b03) * d,\n"
                    "        vec4<f32>(m[1][2] * b08 - m[1][0] * b11 - m[1][3] * b07,\n"
                    "                  m[0][0] * b11 - m[0][2] * b08 + m[0][3] * b07,\n"
                    "                  m[3][2] * b02 - m[3][0] * b05 - m[3][3] * b01,\n"
                    "                  m[2][0] * b05 - m[2][2] * b02 + m[2][3] * b01) * d,\n"
                    "        vec4<f32>(m[1][0] * b10 - m[1][1] * b08 + m[1][3] * b06,\n"
                    "                  m[0][1] * b08 - m[0][0] * b10 - m[0][3] * b06,\n"
                    "                  m[3][0] * b04 - m[3][1] * b02 + m[3][3] * b00,\n"
                    "                  m[2][1] * b02 - m[2][0] * b04 - m[2][3] * b00) * d,\n"
                    "        vec4<f32>(m[1][1] * b07 - m[1][0] * b09 - m[1][2] * b06,\n"
                    "                  m[0][0] * b09 - m[0][1] * b07 + m[0][2] * b06,\n"
                    "                  m[3][1] * b01 - m[3][0] * b03 - m[3][2] * b00,\n"
                    "                  m[2][0] * b03 - m[2][1] * b01 + m[2][2] * b00) * d);\n"
                    "}\n\n");
    }
}

bool WGSLBuilder::WriteModule(TextWriter& writer) {
    if (!WriteAdditionalResources(writer)) {
        return false;
    }
    WriteHelperFunctions(writer, helperFunctions);
    for (u32 resource = 0; resource < resources.count; ++resource) {
        writer.Text("@group(");
        writer.Number(resources.groups[resource]);
        writer.Text(") @binding(");
        writer.Number(resources.bindings[resource]);
        writer.Text(")\nvar<storage, ");
        writer.Text((resources.access[resource] & AccessWrite) != 0 ? "read_write> " : "read> ");
        WriteResourceName(writer, resourceOrigins.logicalSlots[resource]);
        writer.Text(": array<");
        WriteType(writer, static_cast<CoreType>(resources.elementTypes[resource]));
        writer.Text(">;\n\n");
    }
    if (outputs.count != 0) {
        writer.Text("struct BWSLOutput {\n");
        for (u32 id = 0; id < outputs.count; ++id) {
            writer.Text("    ");
            WriteInterfaceAttribute(writer, outputs, id, stage == ShaderStage::Vertex);
            writer.Text("f");
            writer.Number(id);
            writer.Text(": ");
            WriteType(writer, static_cast<CoreType>(outputs.types[id]));
            writer.Text(",\n");
        }
        writer.Text("}\n\n");
    }
    if (stage == ShaderStage::Compute) {
        writer.Text("@compute @workgroup_size(");
        for (u32 axis = 0; axis < 3; ++axis) {
            if (axis != 0) {
                writer.Text(", ");
            }
            writer.Number(workgroupSize[axis]);
        }
        writer.Text(")\n");
    } else {
        writer.Text(stage == ShaderStage::Vertex ? "@vertex\n" : "@fragment\n");
    }
    writer.Text("fn main(");
    if (inputs.count != 0) {
        writer.Text("\n");
        for (u32 id = 0; id < inputs.count; ++id) {
            writer.Text("    ");
            WriteInterfaceAttribute(writer, inputs, id, stage == ShaderStage::Fragment);
            writer.Text("bwsl_");
            const InterfaceBuiltin builtin = static_cast<InterfaceBuiltin>(inputs.builtins[id]);
            if (builtin != InterfaceBuiltin::None) {
                writer.Text(BuiltinName(builtin));
            } else {
                writer.Text(stage == ShaderStage::Vertex ? "attribute_" : "varying_");
                writer.Number(inputs.locations[id]);
            }
            writer.Text(": ");
            WriteType(writer, static_cast<CoreType>(inputs.types[id]));
            writer.Text(",\n");
        }
    }
    writer.Text(")");
    if (outputs.count != 0) {
        writer.Text(" -> BWSLOutput");
    }
    writer.Text(" {\n");
    if (outputs.count != 0) {
        writer.Text("    var bwsl_output: BWSLOutput;\n");
    }
    if (structured) {
        for (u32 value = 0; value < values.count; ++value) {
            if (values.kinds[value] != static_cast<u8>(ValueKind::Value) || !values.functionScope[value]) {
                continue;
            }
            writer.Indent();
            writer.Text("var ");
            WriteValueName(writer, value);
            writer.Text(": ");
            WriteType(writer, static_cast<CoreType>(values.types[value]));
            writer.Text(";\n");
        }
        std::memset(control.visited, 0, cfg->blockCount);
        if (!WriteRegion(writer, cfg->entryBlock, InvalidId, {}, 0)) {
            return false;
        }
    } else {
        for (u32 instruction = 0; instruction < ir->instructionCount; ++instruction) {
            currentInstruction = instruction;
            if (!WriteInstruction(writer, instruction)) {
                return false;
            }
            if (ir->opcodes[instruction] == IR::OP_RET) {
                break;
            }
        }
    }
    currentInstruction = InvalidId;
    writer.Text("}\n");
    return writer.valid || Fail("WGSL output size overflow");
}

bool WGSLBuilder::Emit() {
    if (attemptedEmission) {
        return source != nullptr;
    }
    attemptedEmission = true;
    if (errorMessage[0] != '\0' || arena == nullptr || ir == nullptr) {
        return Fail("WGSL backend is not initialized");
    }
    if (!Prepare()) {
        stats.arenaBytes = arena->allocated - arenaStart;
        return false;
    }
    writingSource = true;
    TextWriter counter;
    if (!WriteModule(counter) || counter.count == std::numeric_limits<size_t>::max()) {
        return Fail("WGSL source sizing failed");
    }
    char* output = static_cast<char*>(Allocate(counter.count + 1, sizeof(char)));
    if (output == nullptr) {
        return false;
    }
    TextWriter writer;
    writer.data = output;
    writer.capacity = counter.count;
    if (!WriteModule(writer) || writer.count != counter.count) {
        return Fail("WGSL source emission disagrees with its size pass");
    }
    output[writer.count] = '\0';
    source = output;
    sourceLength = writer.count;
    stats.sourceBytes = sourceLength;
    stats.arenaBytes = arena->allocated - arenaStart;
    return true;
}

} // namespace WGSL
} // namespace BWSL
