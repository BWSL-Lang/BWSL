#pragma once

#include "core/bwsl_compiler_types.h"
#include "core/bwsl_symbol_table.h"
#include "phases/control_flow/bwsl_cfg.h"
#include "phases/ir_generation/bwsl_ir_gen.h"
#include <cstddef>
#include <string_view>

namespace BWSL {
namespace WGSL {

constexpr u32 InvalidId = 0xFFFFFFFF;

enum class ValueKind : u8 {
    Value,
    StorageRoot,
    StorageElement,
    UniformAddress,
    Reference, // A path into a variable: its parent plus one accessor
};

// How a Reference value reaches into its parent.
enum class Accessor : u8 {
    Whole,
    Field,
    Index,
};

// Flags of a value that lives in a WGSL variable rather than a let.
enum VariableFlags : u8 {
    VariableMutable = 1,   // Assigned more than once or through pointers
};

enum ResourceAccess : u8 {
    AccessRead = 1,
    AccessWrite = 2,
};

// All arrays share a dense ValueId. Register allocation can contain holes;
// only actual instruction definitions consume entries in this table.
struct ValueTable {
    u16* types = nullptr;
    u32* producerInstructions = nullptr;
    u8* kinds = nullptr;
    u32* resourceIds = nullptr;
    u16* indexOperands = nullptr;
    u16* aliasOperands = nullptr; // Canonical register/constant for copy elimination
    u32* useCounts = nullptr;
    u32* useInstructions = nullptr; // Single-use consumer, for bounded deferral
    u8* inlineDepths = nullptr; // Zero materializes; nonzero bounds expression depth
    u8* functionScope = nullptr; // Phi/cross-block values; other values stay local
    u32* structTypes = nullptr; // Struct type hash of CUSTOM/ENUM values
    u32* arrayLengths = nullptr; // Nonzero for fixed-size array values
    u32* parents = nullptr; // Reference values: the value they reach into
    u8* accessors = nullptr; // Reference values: Accessor into the parent
    u8* variables = nullptr; // VariableFlags
    u32 count = 0;
};

struct ValueOrigins {
    u16* registers = nullptr;
};

// ResourceId indexes these arrays. Binding N retains BWSL logical slot N in
// group 0. Strides are the physical WGSL runtime-array element strides.
struct ResourceTable {
    u16* elementTypes = nullptr;
    u8* access = nullptr;
    u32* groups = nullptr;
    u32* bindings = nullptr;
    u32* strides = nullptr;
    u32 count = 0;
};

struct ResourceOrigins {
    u32* logicalSlots = nullptr;
};

struct UniformTable {
    u16* types = nullptr;
    u32* slots = nullptr;
    u32* byteSizes = nullptr;
    u32* arrayLengths = nullptr;
    u32* structTypes = nullptr;
    u32 count = 0;
};

struct TextureTable {
    u16* types = nullptr;
    u32* slots = nullptr;
    u8* depth = nullptr;
    u32 count = 0;
};

// Write-only rgba32float 2D storage textures, as the SPIR-V backend's
// storage images. They bind in group 0 at their logical slot.
struct StorageImageTable {
    u32* slots = nullptr;
    u32 count = 0;
};

struct SamplerTable {
    u32* bindings = nullptr;
    u8* comparison = nullptr;
    u32 count = 0;
};

// Address expressions form a dense parent chain, never allocated strings.
// These arrays are allocated only when the shader uses uniform buffers.
struct UniformAddressTable {
    u32* bases = nullptr;
    u32* byteOffsets = nullptr;
    u32* strides = nullptr;
    u32* lengths = nullptr;
    u32* structTypes = nullptr;
};

// Physical uniform layout is separate from IR's storage/value layout. A scalar
// array in a uniform needs 16-byte stride even when its IR layout is packed.
struct UniformLayoutTable {
    u32* sizes = nullptr;
    u32* alignments = nullptr;
    u8* states = nullptr;
    u32* offsets = nullptr;
    u32* strides = nullptr;
    u32* owners = nullptr;
    u32 fieldCount = 0;
};

enum class InterfaceBuiltin : u8 {
    None,
    VertexIndex,
    InstanceIndex,
    Position,
    FragDepth,
    GlobalInvocationId,
    LocalInvocationId,
    WorkgroupId,
    NumWorkgroups,
    LocalInvocationIndex,
};

const char* InterfaceBuiltinName(InterfaceBuiltin builtin);

// Each input/output has a dense InterfaceId. Locations never depend on which
// other varyings a stage happens to use, so sparse stage interfaces still link.
struct InterfaceTable {
    u16* types = nullptr;
    u32* locations = nullptr;
    u8* builtins = nullptr;
    u8* interpolations = nullptr;
    u8* samplings = nullptr;
    u32 count = 0;
};

struct InterfaceOrigins {
    u16* slots = nullptr;
    u32* instructions = nullptr;
};

// BlockId indexes the structural plan; phi indices use CSR storage. The CFG
// and IR retain cold source/edge origins. Every array is backed by the arena.
struct ControlTable {
    u32* merges = nullptr;
    u32* continues = nullptr;
    u32* kinds = nullptr;
    u32* phiOffsets = nullptr;
    u32* phis = nullptr;
    u8* phiLive = nullptr;
    u8* visited = nullptr;
    u8* reachable = nullptr;
};

struct ControlScope {
    u32 loopHeader = InvalidId;
    u32 loopContinue = InvalidId;
    u32 loopMerge = InvalidId;
    u32 switchMerge = InvalidId;
    bool continuing = false;
};

struct BackendStats {
    u32 instructionCount = 0;
    u32 valueCount = 0;
    u32 resourceCount = 0;
    size_t arenaBytes = 0;
    size_t sourceBytes = 0;
};

struct TextWriter;

struct WGSLBuilder {
    ValueTable values;
    ValueOrigins valueOrigins;
    ResourceTable resources;
    ResourceOrigins resourceOrigins;
    UniformTable uniforms;
    TextureTable textures;
    SamplerTable samplers;
    StorageImageTable storageImages;
    UniformAddressTable uniformAddresses;
    UniformLayoutTable uniformLayouts;
    InterfaceTable inputs;
    InterfaceOrigins inputOrigins;
    InterfaceTable outputs;
    InterfaceOrigins outputOrigins;
    ControlTable control;
    BackendStats stats;

    // Input must already be normalized to SSA. All inputs and the output view
    // remain owned by the caller; the arena must outlive Source() and tables.
    bool Initialize(BWSL_Arena* arena, const IR::IRProgram* ir,
                    const CFG* cfg, const SymbolTableData* symbols,
                    ShaderStage stage);
    void SetComputeWorkgroupSize(u32 x, u32 y, u32 z);
    bool Emit();
    std::string_view Source() const;
    const char* Error() const;
    u32 ErrorInstruction() const;

private:
    BWSL_Arena* arena = nullptr;
    const IR::IRProgram* ir = nullptr;
    const CFG* cfg = nullptr;
    const SymbolTableData* symbols = nullptr;
    ShaderStage stage = ShaderStage::Compute;
    u32* registerToValue = nullptr;
    u8* registerFlags = nullptr; // RegisterFlags, by register
    u32 lastDefined = InvalidId; // Value set by the latest DefineValue
    u32 resourceForSlot[32] = {};
    u32 uniformForSlot[32] = {};
    u32 textureForSlot[32] = {};
    u32 workgroupSize[3] = {1, 1, 1};
    static constexpr u32 InputSlotCount = 0x95;
    u32 inputForSlot[InputSlotCount] = {};
    u32 outputForSlot[32] = {};
    size_t arenaStart = 0;
    char* source = nullptr;
    size_t sourceLength = 0;
    char errorMessage[256] = {};
    u32 errorInstruction = InvalidId;
    u32 currentInstruction = InvalidId;
    u32 helperFunctions = 0; // Bit set of WGSL helpers the module must define
    bool attemptedEmission = false;
    bool structured = false;
    bool writingSource = false;

    bool Fail(const char* message);
    bool FailOpcode(u16 opcode);
    void* Allocate(size_t count, size_t elementSize);
    bool Prepare();
    bool PrepareInstruction(u32 instruction);
    bool PrepareResources(u32 usedSlots);
    bool PrepareAdditionalResources();
    bool PrepareUniformInstruction(u32 instruction, bool& handled);
    bool PrepareTextureInstruction(u32 instruction);
    bool WriteUniformOffset(TextWriter& writer, u32 value);
    bool WriteUniformLoad(TextWriter& writer, u32 value);
    bool WriteUniformValue(TextWriter& writer, u32 value, CoreType type, u32 structHash,
                           u32 arrayLength, u32 extraBytes);
    bool WriteTextureExpression(TextWriter& writer, u32 instruction);
    bool WriteTextureOffset(TextWriter& writer, u16 operand, u32 width);
    bool WriteAdditionalResources(TextWriter& writer);
    u32 UniformTypeSize(CoreType type, u32 structHash);
    bool PrepareUniformLayout(u32 structId);
    bool PrepareControl();
    bool ValidateOutputAssignments();
    bool WriteRegion(TextWriter& writer, u32 block, u32 stop, ControlScope scope, u32 depth);
    bool WriteTransfer(TextWriter& writer, u32 from, u32 target, u32 stop,
                       ControlScope scope, u32 depth);
    bool WritePhiCopies(TextWriter& writer, u32 from, u32 to);
    bool WriteCondition(TextWriter& writer, u16 operand);
    bool AllocateInterface(InterfaceTable& table, InterfaceOrigins& origins, u32 capacity);
    bool PrepareInput(u32 instruction, u16 slot, CoreType type, bool attribute);
    bool PrepareOutput(u32 instruction, u16 slot, CoreType type);
    bool AddInterface(InterfaceTable& table, InterfaceOrigins& origins, u32* slotMap,
                      u32 instruction, u16 slot, CoreType type, u32 location,
                      InterfaceBuiltin builtin, InterpolationMode interpolation,
                      InterpolationSampling sampling);
    void WriteInterfaceAttribute(TextWriter& writer, const InterfaceTable& table,
                                 u32 id, bool varying);
    void PlanExpressions();
    u32 OperandMask(u32 instruction) const;
    u32 VectorArgumentCount(u32 instruction) const;
    void CountUse(u16 operand, u32 count = 1);
    bool WriteModule(TextWriter& writer);
    bool WriteInstruction(TextWriter& writer, u32 instruction);
    bool WriteOperand(TextWriter& writer, u16 operand);
    bool WriteConvertedOperand(TextWriter& writer, u16 operand, CoreType target);
    bool WriteStorageAddress(TextWriter& writer, u32 valueId);
    bool WriteExpression(TextWriter& writer, u32 instruction, u32 valueId);
    bool OperandType(u16 operand, CoreType& type);
    bool ValueId(u16 reg, u32& valueId);
    bool NumericOperand(u16 operand, CoreType& type);
    bool RequireIndex(u16 operand);
    // structType InvalidId takes the result register's struct type.
    bool DefineValue(u32 instruction, CoreType type, ValueKind kind,
                     u32 structType = InvalidId, u32 arrayLength = 0);
    u32 NewValue(u16 reg, CoreType type, u32 structType, u32 arrayLength, ValueKind kind);
    void CopyValueShape(u32 target, u32 source);
    u32 StructIndex(u32 structHash) const;
    bool FieldShape(u32 structHash, u32 field, CoreType& type, u32& structType, u32& arrayLength);
    bool PrepareRegisters();
    bool PrepareMemoryInstruction(u32 instruction, bool& handled);
    bool PrepareStructInstruction(u32 instruction, bool& handled);
    bool WriteFullType(TextWriter& writer, CoreType type, u32 structType, u32 arrayLength);
    bool WriteValueType(TextWriter& writer, u32 valueId);
    bool WriteReference(TextWriter& writer, u32 valueId);
    bool WriteStructDeclarations(TextWriter& writer);
    bool WriteMemoryInstruction(TextWriter& writer, u32 instruction, bool& handled);
    bool WriteStructExpression(TextWriter& writer, u32 instruction, u32 valueId, bool& handled);
    bool IsMutable(u16 operand) const;
};

} // namespace WGSL
} // namespace BWSL
