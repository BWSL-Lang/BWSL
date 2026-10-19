#include "phases/backends/wgsl/bwsl_wgsl_backend.cpp"
#include "phases/control_flow/bwsl_cfg.cpp"
#include "phases/ssa/bwsl_ssa.cpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <vector>

using namespace BWSL;

struct WGSLFixture {
    static constexpr u32 InstructionCapacity = 32;
    alignas(64) u8 memory[16384] = {};
    BWSL_Arena arena;
    IR::IRProgram ir = {};
    u16 opcodes[InstructionCapacity] = {};
    u16 types[InstructionCapacity] = {};
    u16 destinations[InstructionCapacity] = {};
    u16 operands[InstructionCapacity * 4] = {};
    u32 metadata[InstructionCapacity] = {};
    u16 registerTypes[32] = {};
    u32 constants[2] = {0, 7};
    WGSL::WGSLBuilder builder;

    WGSLFixture() {
        arena.Initialize(memory, sizeof(memory));
        ir.opcodes = opcodes;
        ir.types = types;
        ir.destinations = destinations;
        ir.operands = operands;
        ir.metadata = metadata;
        ir.registerTypes = registerTypes;
        ir.registerCount = 32;
        ir.uintConstants = constants;
        ir.uintCount = 2;
        assert(builder.Initialize(&arena, &ir, nullptr, nullptr, ShaderStage::Compute));
    }

    void Add(u16 opcode, u16 destination, CoreType type, u16 first = 0, u16 second = 0) {
        const u32 index = ir.instructionCount++;
        assert(index < InstructionCapacity);
        opcodes[index] = opcode;
        types[index] = static_cast<u16>(type);
        destinations[index] = destination;
        operands[index * 4] = first;
        operands[index * 4 + 1] = second;
        if (destination < 32 && opcode != IR::OP_ARRAY_STORE && opcode != IR::OP_STORE_OUTPUT) {
            registerTypes[destination] = static_cast<u16>(type);
        }
    }
};

static void TestDenseValuesAndSparseBindings() {
    WGSLFixture fixture;
    fixture.ir.bufferElementCoreTypes[5] = static_cast<u8>(CoreType::UINT);
    fixture.Add(IR::OP_STORAGE_PTR, 9, CoreType::UINT, 5);
    fixture.Add(IR::OP_LOAD_CONST, 12, CoreType::UINT, IR::MakeUintConstant(1));
    fixture.Add(IR::OP_ARRAY_STORE, 9, CoreType::UINT, IR::MakeUintConstant(0), 12);
    fixture.Add(IR::OP_RET, 0xFFFF, CoreType::VOID);
    assert(fixture.builder.Emit());
    const WGSL::WGSLBuilder& builder = fixture.builder;
    assert(builder.values.count == 2);
    assert(builder.valueOrigins.registers[0] == 9);
    assert(builder.valueOrigins.registers[1] == 12);
    assert(builder.resources.count == 1);
    assert(builder.resources.bindings[0] == 5);
    assert(builder.resourceOrigins.logicalSlots[0] == 5);
    assert(builder.resources.access[0] == WGSL::AccessWrite);
    assert(builder.resources.strides[0] == 4);
    assert(reinterpret_cast<uintptr_t>(builder.values.types) % 64 == 0);
    assert(reinterpret_cast<uintptr_t>(builder.resources.bindings) % 64 == 0);
    assert(builder.Source().find("@binding(5)") != std::string_view::npos);
    assert(builder.Source().data() >= reinterpret_cast<char*>(fixture.memory));
    assert(builder.Source().data() + builder.Source().size() <=
           reinterpret_cast<char*>(fixture.memory + sizeof(fixture.memory)));
}

static void TestVectorStride() {
    WGSLFixture fixture;
    fixture.ir.bufferElementCoreTypes[3] = static_cast<u8>(CoreType::FLOAT3);
    fixture.Add(IR::OP_STORAGE_PTR, 1, CoreType::FLOAT3, 3);
    fixture.Add(IR::OP_ARRAY_LOAD, 2, CoreType::FLOAT3, 1, IR::MakeUintConstant(0));
    assert(fixture.builder.Emit());
    assert(fixture.builder.resources.strides[0] == 16);
    assert(fixture.builder.resources.access[0] == WGSL::AccessRead);
}

static void TestInvalidIRHasNoOutput() {
    WGSLFixture duplicate;
    duplicate.Add(IR::OP_LOAD_CONST, 1, CoreType::UINT, IR::MakeUintConstant(0));
    duplicate.Add(IR::OP_LOAD_CONST, 1, CoreType::UINT, IR::MakeUintConstant(1));
    assert(!duplicate.builder.Emit());
    assert(std::strstr(duplicate.builder.Error(), "SSA") != nullptr);
    assert(duplicate.builder.ErrorInstruction() == 1);
    assert(duplicate.builder.Source().empty());

    WGSLFixture undefined;
    undefined.Add(IR::OP_LOAD_REG, 2, CoreType::UINT, 7);
    assert(!undefined.builder.Emit());
    assert(std::strstr(undefined.builder.Error(), "definition") != nullptr);
    assert(undefined.builder.Source().empty());

    WGSLFixture unsupported;
    unsupported.Add(IR::OP_ATOMIC_ADD, 2, CoreType::UINT);
    assert(!unsupported.builder.Emit());
    assert(unsupported.builder.Source().empty());
}

static void TestArenaExhaustion() {
    WGSLFixture fixture;
    fixture.Add(IR::OP_LOAD_CONST, 1, CoreType::UINT, IR::MakeUintConstant(1));
    fixture.arena.Initialize(fixture.memory + 1, 63);
    assert(fixture.builder.Initialize(&fixture.arena, &fixture.ir, nullptr, nullptr, ShaderStage::Compute));
    assert(!fixture.builder.Emit());
    assert(std::strstr(fixture.builder.Error(), "exhausted") != nullptr);
    assert(fixture.builder.Source().empty());
    assert(fixture.arena.allocated <= fixture.arena.capacity);
}

static void TestSpareRegisterCapacity() {
    WGSLFixture fixture;
    std::vector<u16> types(IR::RegisterLimit + 64, static_cast<u16>(CoreType::UINT));
    std::vector<u8> memory(256 * 1024);
    fixture.ir.registerTypes = types.data();
    fixture.ir.registerCount = static_cast<u32>(types.size());
    fixture.arena.Initialize(memory.data(), memory.size());
    fixture.Add(IR::OP_LOAD_CONST, 1, CoreType::UINT, IR::MakeUintConstant(1));
    assert(fixture.builder.Initialize(&fixture.arena, &fixture.ir, nullptr, nullptr, ShaderStage::Compute));
    assert(fixture.builder.Emit());
    assert(fixture.builder.values.count == 1);

    fixture.destinations[0] = static_cast<u16>(IR::RegisterLimit);
    fixture.arena.Initialize(memory.data(), memory.size());
    assert(fixture.builder.Initialize(&fixture.arena, &fixture.ir, nullptr, nullptr, ShaderStage::Compute));
    assert(!fixture.builder.Emit());
    assert(fixture.builder.Source().empty());
}

static void TestBoundedExpressionDepth() {
    WGSLFixture fixture;
    fixture.ir.bufferElementCoreTypes[0] = static_cast<u8>(CoreType::UINT);
    fixture.Add(IR::OP_LOAD_CONST, 0, CoreType::UINT, IR::MakeUintConstant(1));
    for (u16 destination = 1; destination <= 10; ++destination) {
        fixture.Add(IR::OP_IADD, destination, CoreType::UINT, destination - 1, IR::MakeUintConstant(1));
    }
    fixture.Add(IR::OP_STORAGE_PTR, 11, CoreType::UINT, 0);
    fixture.Add(IR::OP_ARRAY_STORE, 11, CoreType::UINT, IR::MakeUintConstant(0), 10);
    fixture.Add(IR::OP_RET, 0xFFFF, CoreType::VOID);
    assert(fixture.builder.Emit());
    u32 materialized = 0;
    for (u32 value = 1; value <= 10; ++value) {
        const u8 depth = fixture.builder.values.inlineDepths[value];
        assert(depth <= 4);
        if (depth == 0) ++materialized;
    }
    assert(materialized >= 2);
    assert(fixture.builder.Source().size() < 1024);
}

static void TestSSAInitializesNewMetadata() {
    WGSLFixture fixture;
    u32 storageInfo[2] = {0x12340000u, 0xABCD0000u};
    fixture.ir.registerCount = 2;
    fixture.ir.registerStorageInfo = storageInfo;
    fixture.Add(IR::OP_LOAD_CONST, 0, CoreType::UINT, IR::MakeUintConstant(0));
    fixture.Add(IR::OP_LOAD_CONST, 0, CoreType::UINT, IR::MakeUintConstant(1));
    fixture.Add(IR::OP_IADD, 1, CoreType::UINT, 0, IR::MakeUintConstant(1));
    fixture.Add(IR::OP_RET, 0xFFFF, CoreType::VOID);
    // Poison unused storage: SSA must initialize its own metadata, and must
    // not read beyond the two registers initialized by lowering.
    std::vector<u8> memory(128 * 1024, 0xCD);
    BWSL_Arena arena;
    arena.Initialize(memory.data(), memory.size());
    CFGBuilder cfg;
    cfg.Init(&fixture.ir, &arena);
    cfg.Build();
    assert(SSA::ConvertToSSA(&fixture.ir, &cfg.cfg, &cfg, &arena));
    assert(fixture.ir.registerCount > 2);
    assert(fixture.ir.registerStorageInfo != storageInfo);
    assert(fixture.ir.registerStorageInfo[0] == storageInfo[0]);
    assert(fixture.ir.registerStorageInfo[1] == storageInfo[1]);
    for (u32 reg = 2; reg < fixture.ir.registerCount; ++reg) {
        assert(fixture.ir.registerStorageInfo[reg] == 0);
    }
}

static void TestDistantAccumulationIsMaterialized() {
    WGSLFixture fixture;
    fixture.ir.bufferElementCoreTypes[0] = static_cast<u8>(CoreType::FLOAT4);
    fixture.Add(IR::OP_STORAGE_PTR, 0, CoreType::FLOAT4, 0);
    fixture.Add(IR::OP_ARRAY_LOAD, 1, CoreType::FLOAT4, 0, IR::MakeUintConstant(0));
    fixture.Add(IR::OP_ARRAY_LOAD, 2, CoreType::FLOAT4, 0, IR::MakeUintConstant(1));
    fixture.Add(IR::OP_FADD, 3, CoreType::FLOAT4, 1, 2);
    // Independent work separates the result from its sole consumer. Its inputs
    // should not stay live across that work solely to save one WGSL declaration.
    for (u16 destination = 4; destination < 24; ++destination) {
        fixture.Add(IR::OP_IADD, destination, CoreType::UINT, IR::MakeUintConstant(0), IR::MakeUintConstant(1));
    }
    fixture.Add(IR::OP_FADD, 24, CoreType::FLOAT4, 3, 2);
    fixture.Add(IR::OP_ARRAY_STORE, 0, CoreType::FLOAT4, IR::MakeUintConstant(0), 24);
    fixture.Add(IR::OP_RET, 0xFFFF, CoreType::VOID);
    assert(fixture.builder.Emit());
    assert(fixture.builder.values.useCounts[3] == 1);
    assert(fixture.builder.values.inlineDepths[3] == 0);
    assert(fixture.builder.Source().find("let v3 =") != std::string_view::npos);
}

static void TestGraphicsInterfaces() {
    WGSLFixture vertex;
    assert(vertex.builder.Initialize(&vertex.arena, &vertex.ir, nullptr, nullptr, ShaderStage::Vertex));
    vertex.Add(IR::OP_LOAD_ATTR, 0, CoreType::FLOAT4, 0);
    vertex.Add(IR::OP_STORE_OUTPUT, 0, CoreType::VOID, OutputSlot::POSITION);
    vertex.Add(IR::OP_LOAD_ATTR, 1, CoreType::FLOAT2, 7);
    vertex.Add(IR::OP_STORE_OUTPUT, 1, CoreType::VOID, 5);
    vertex.ir.outputInterpolations[5] = static_cast<u8>(InterpolationMode::NoPerspective);
    vertex.Add(IR::OP_LOAD_INPUT, 2, CoreType::UINT, BuiltinInputSlot::INSTANCE_ID);
    vertex.Add(IR::OP_STORE_OUTPUT, 2, CoreType::VOID, 3);
    vertex.Add(IR::OP_RET, 0xFFFF, CoreType::VOID);
    assert(vertex.builder.Emit());
    assert(vertex.builder.inputs.count == 3);
    assert(vertex.builder.outputs.count == 3);
    assert(vertex.builder.outputs.locations[1] == 3);
    assert(vertex.builder.outputs.interpolations[2] == static_cast<u8>(InterpolationMode::Flat));
    assert(vertex.builder.Source().find("@location(7)") != std::string_view::npos);
    assert(vertex.builder.Source().find("@interpolate(linear, center)") != std::string_view::npos);
    assert(vertex.builder.Source().find("return bwsl_output") != std::string_view::npos);
    assert(reinterpret_cast<uintptr_t>(vertex.builder.inputs.types) % 64 == 0);
    assert(reinterpret_cast<uintptr_t>(vertex.builder.outputOrigins.instructions) % 64 == 0);

    WGSLFixture fragment;
    assert(fragment.builder.Initialize(&fragment.arena, &fragment.ir, nullptr, nullptr, ShaderStage::Fragment));
    fragment.Add(IR::OP_LOAD_INPUT, 0, CoreType::FLOAT2, 5);
    fragment.ir.inputInterpolations[5] = static_cast<u8>(InterpolationMode::NoPerspective);
    fragment.ir.inputSamplings[5] = static_cast<u8>(InterpolationSampling::Centroid);
    fragment.Add(IR::OP_LOAD_INPUT, 1, CoreType::UINT, 3);
    fragment.Add(IR::OP_LOAD_INPUT, 2, CoreType::FLOAT4, BuiltinInputSlot::FRAG_COORD);
    fragment.Add(IR::OP_STORE_OUTPUT, 2, CoreType::VOID, OutputSlot::COLOR);
    fragment.Add(IR::OP_LOAD_OUTPUT, 3, CoreType::FLOAT4, OutputSlot::COLOR);
    fragment.Add(IR::OP_STORE_OUTPUT, 2, CoreType::VOID, OutputSlot::COLOR);
    fragment.Add(IR::OP_STORE_OUTPUT, 3, CoreType::VOID, OutputSlot::COLOR);
    fragment.Add(IR::OP_DDX, 4, CoreType::FLOAT2, 0);
    fragment.Add(IR::OP_RET, 0xFFFF, CoreType::VOID);
    assert(fragment.builder.Emit());
    assert(fragment.builder.inputs.locations[0] == 3);
    assert(fragment.builder.inputs.interpolations[1] == static_cast<u8>(InterpolationMode::Flat));
    assert(fragment.builder.Source().find("@interpolate(linear, centroid)") != std::string_view::npos);
    assert(fragment.builder.Source().find("let v3 = bwsl_output.f0;") != std::string_view::npos);
    assert(fragment.builder.Source().find("dpdx(") != std::string_view::npos);
}

// Element i of a texture array at slot b is texture b + i in IR. Each one
// becomes its own WGSL binding, named by the array's declaration; a
// comparison on one element makes every element a depth texture.
static void TestTextureArrayElements() {
    std::vector<u8> scratch(64 * 1024);
    BWSL_Arena symbolArena;
    symbolArena.Initialize(scratch.data(), scratch.size());
    std::unique_ptr<SymbolTableData> symbols(new SymbolTableData());
    symbols->resources.Init(&symbolArena, 2);
    ResourceData textures;
    textures.type = ResourceBinding::Texture;
    textures.coreType = static_cast<u8>(CoreType::TEXTURE2D);
    textures.bindingIndex = 1;
    textures.arraySize = 3;
    symbols->resources.Push(&symbolArena, textures);

    WGSLFixture fixture;
    assert(fixture.builder.Initialize(&fixture.arena, &fixture.ir, nullptr, symbols.get(), ShaderStage::Fragment));
    fixture.Add(IR::OP_LOAD_INPUT, 0, CoreType::FLOAT2, 5);
    fixture.Add(IR::OP_LOAD_INPUT, 1, CoreType::FLOAT, 6);
    fixture.Add(IR::OP_TEX_SAMPLE, 2, CoreType::FLOAT4, IR::MakeResourceHandle(3), 0);
    fixture.Add(IR::OP_TEX_SAMPLE_CMP, 3, CoreType::FLOAT4, IR::MakeResourceHandle(1), 0);
    fixture.operands[3 * 4 + 2] = 1;
    fixture.Add(IR::OP_FADD, 4, CoreType::FLOAT4, 2, 3);
    fixture.Add(IR::OP_STORE_OUTPUT, 4, CoreType::VOID, OutputSlot::COLOR);
    fixture.Add(IR::OP_RET, 0xFFFF, CoreType::VOID);
    assert(fixture.builder.Emit());
    const WGSL::WGSLBuilder& builder = fixture.builder;
    assert(builder.textures.count == 2);
    assert(builder.textures.slots[0] == 1 && builder.textures.slots[1] == 3);
    assert(builder.textures.depth[0] && builder.textures.depth[1]);
    const std::string_view source = builder.Source();
    assert(source.find("@group(2) @binding(1) var bwsl_texture_1: texture_depth_2d;") != std::string_view::npos);
    assert(source.find("@group(2) @binding(3) var bwsl_texture_3: texture_depth_2d;") != std::string_view::npos);
    assert(source.find("bwsl_texture_2") == std::string_view::npos);
    assert(source.find("@group(3) @binding(33) var bwsl_sampler_33: sampler_comparison;") != std::string_view::npos);
    assert(source.find("@group(3) @binding(35) var bwsl_sampler_35: sampler;") != std::string_view::npos);
}

static void TestInvalidGraphicsInterfaces() {
    WGSLFixture wrongStage;
    wrongStage.Add(IR::OP_LOAD_ATTR, 0, CoreType::FLOAT4, 0);
    assert(!wrongStage.builder.Emit());

    WGSLFixture missingPosition;
    assert(missingPosition.builder.Initialize(&missingPosition.arena, &missingPosition.ir,
                                             nullptr, nullptr, ShaderStage::Vertex));
    assert(!missingPosition.builder.Emit());
    assert(std::strstr(missingPosition.builder.Error(), "output.position") != nullptr);

    WGSLFixture unassignedOutput;
    assert(unassignedOutput.builder.Initialize(&unassignedOutput.arena, &unassignedOutput.ir,
                                              nullptr, nullptr, ShaderStage::Fragment));
    unassignedOutput.Add(IR::OP_LOAD_OUTPUT, 0, CoreType::FLOAT4, OutputSlot::COLOR);
    assert(!unassignedOutput.builder.Emit());
    assert(std::strstr(unassignedOutput.builder.Error(), "before its first assignment") != nullptr);

    WGSLFixture integerLinear;
    assert(integerLinear.builder.Initialize(&integerLinear.arena, &integerLinear.ir,
                                           nullptr, nullptr, ShaderStage::Fragment));
    integerLinear.Add(IR::OP_LOAD_INPUT, 0, CoreType::UINT, 2);
    integerLinear.ir.inputInterpolations[2] = static_cast<u8>(InterpolationMode::NoPerspective);
    assert(!integerLinear.builder.Emit());

    WGSLFixture integerCentroid;
    assert(integerCentroid.builder.Initialize(&integerCentroid.arena, &integerCentroid.ir,
                                             nullptr, nullptr, ShaderStage::Fragment));
    integerCentroid.Add(IR::OP_LOAD_INPUT, 0, CoreType::UINT, 2);
    integerCentroid.ir.inputSamplings[2] = static_cast<u8>(InterpolationSampling::Centroid);
    assert(!integerCentroid.builder.Emit());

    WGSLFixture derivativeCompute;
    derivativeCompute.Add(IR::OP_LOAD_INPUT, 0, CoreType::UINT3, BuiltinInputSlot::GLOBAL_INVOCATION_ID);
    derivativeCompute.Add(IR::OP_DDX, 1, CoreType::UINT3, 0);
    assert(!derivativeCompute.builder.Emit());
    assert(std::strstr(derivativeCompute.builder.Error(), "fragment") != nullptr);

    WGSLFixture vertexWrite;
    assert(vertexWrite.builder.Initialize(&vertexWrite.arena, &vertexWrite.ir,
                                         nullptr, nullptr, ShaderStage::Vertex));
    vertexWrite.ir.bufferElementCoreTypes[0] = static_cast<u8>(CoreType::FLOAT4);
    vertexWrite.Add(IR::OP_STORAGE_PTR, 0, CoreType::FLOAT4, 0);
    vertexWrite.Add(IR::OP_ARRAY_LOAD, 1, CoreType::FLOAT4, 0, IR::MakeUintConstant(0));
    vertexWrite.Add(IR::OP_STORE_OUTPUT, 1, CoreType::VOID, OutputSlot::POSITION);
    vertexWrite.Add(IR::OP_ARRAY_STORE, 0, CoreType::FLOAT4, IR::MakeUintConstant(0), 1);
    assert(!vertexWrite.builder.Emit());
    assert(std::strstr(vertexWrite.builder.Error(), "cannot write storage") != nullptr);
}

static void TestStorageLoadKeepsItsEvaluationPoint() {
    WGSLFixture fixture;
    fixture.ir.bufferElementCoreTypes[0] = static_cast<u8>(CoreType::UINT);
    fixture.Add(IR::OP_STORAGE_PTR, 0, CoreType::UINT, 0);
    fixture.Add(IR::OP_STORAGE_INDEX, 1, CoreType::UINT, 0, IR::MakeUintConstant(0));
    fixture.Add(IR::OP_STORAGE_LOAD, 2, CoreType::UINT, 1);
    fixture.Add(IR::OP_ARRAY_STORE, 0, CoreType::UINT, IR::MakeUintConstant(0), IR::MakeUintConstant(1));
    fixture.Add(IR::OP_IADD, 3, CoreType::UINT, 2, IR::MakeUintConstant(1));
    fixture.Add(IR::OP_ARRAY_STORE, 0, CoreType::UINT, IR::MakeUintConstant(0), 3);
    assert(fixture.builder.Emit());
    assert(fixture.builder.values.inlineDepths[2] == 0);
    const std::string_view source = fixture.builder.Source();
    assert(source.find("let v2 = bwsl_buffer_0[0u]") < source.find("bwsl_buffer_0[0u] = 7u"));
}

static void TestTypedNodeMapsSurviveGrowthAndUpdates() {
    std::vector<u8> memory(1024 * 1024);
    BWSL_Arena arena;
    arena.Initialize(memory.data(), memory.size());
    AST ast;
    ast.Init(&arena, 8);
    for (u32 kind = 1; kind <= 8; ++kind) {
        for (u32 index = 0; index < 128; ++index) {
            const NodeRef node = ast.MakeNodeRef(static_cast<ASTNodeType>(kind), index, index + 1, kind);
            ast.SetEndPosition(node, index + 2, kind + 1);
            ast.SetNamePosition(node, index + 3, kind + 2);
        }
    }
    for (u32 kind = 1; kind <= 8; ++kind) {
        for (u32 index = 0; index < 128; ++index) {
            const NodeRef node(static_cast<ASTNodeType>(kind), index);
            assert(ast.FindPosition(node) == AST::PackPosition(index + 1, kind));
            assert(ast.FindEndPosition(node) == AST::PackPosition(index + 2, kind + 1));
            assert(ast.GetNamePosition(node) == AST::PackPosition(index + 3, kind + 2));
            ast.SetNamePosition(node, index + 4, kind + 3);
            assert(ast.GetNamePosition(node) == AST::PackPosition(index + 4, kind + 3));
        }
    }
}

static void TestConditionalContinuingExit() {
    WGSLFixture fixture;
    u32 structure[32] = {};
    u32 continuation[32] = {};
    u32 yes[32] = {};
    u32 no[32] = {};
    fixture.ir.structureInfo = structure;
    fixture.ir.continueInfo = continuation;
    fixture.ir.branchTrueTargets = yes;
    fixture.ir.branchFalseTargets = no;
    fixture.Add(IR::OP_LOAD_CONST, 0, CoreType::UINT, IR::MakeUintConstant(1));
    fixture.Add(IR::OP_JUMP, 0xFFFF, CoreType::VOID);
    fixture.metadata[1] = 2;
    fixture.Add(IR::OP_NOP, 0xFFFF, CoreType::VOID);
    fixture.Add(IR::OP_JUMP, 0xFFFF, CoreType::VOID);
    fixture.metadata[3] = 4;
    structure[3] = IR::IRProgram::PackStructure(IR::IRProgram::STRUCT_LOOP_HEADER, 8);
    continuation[3] = 6;
    fixture.Add(IR::OP_NOP, 0xFFFF, CoreType::VOID);
    fixture.Add(IR::OP_JUMP, 0xFFFF, CoreType::VOID);
    fixture.metadata[5] = 6;
    fixture.Add(IR::OP_IEQ, 1, CoreType::BOOL, 0, IR::MakeUintConstant(1));
    fixture.Add(IR::OP_BRANCH, 0xFFFF, CoreType::VOID, 1);
    fixture.ir.SetBranchTargets(7, 8, 2);
    fixture.Add(IR::OP_RET, 0xFFFF, CoreType::VOID);
    std::vector<u8> scratch(64 * 1024);
    BWSL_Arena cfgArena;
    cfgArena.Initialize(scratch.data(), scratch.size());
    CFGBuilder cfg;
    cfg.Init(&fixture.ir, &cfgArena);
    cfg.Build();
    assert(fixture.builder.Initialize(&fixture.arena, &fixture.ir, &cfg.cfg, nullptr, ShaderStage::Compute));
    assert(fixture.builder.Emit());
    assert(fixture.builder.Source().find("break if bwsl_break_") != std::string_view::npos);
    const char* output = std::getenv("BWSL_WGSL_UNIT_OUTPUT");
    if (output != nullptr) {
        FILE* file = std::fopen(output, "wb");
        assert(file != nullptr);
        const std::string_view source = fixture.builder.Source();
        assert(std::fwrite(source.data(), 1, source.size(), file) == source.size());
        std::fclose(file);
    }
}

int main() {
    TestDenseValuesAndSparseBindings();
    TestVectorStride();
    TestInvalidIRHasNoOutput();
    TestArenaExhaustion();
    TestSpareRegisterCapacity();
    TestBoundedExpressionDepth();
    TestSSAInitializesNewMetadata();
    TestGraphicsInterfaces();
    TestInvalidGraphicsInterfaces();
    TestTextureArrayElements();
    TestDistantAccumulationIsMaterialized();
    TestStorageLoadKeepsItsEvaluationPoint();
    TestTypedNodeMapsSurviveGrowthAndUpdates();
    TestConditionalContinuingExit();
    std::puts("WGSL backend unit tests passed");
}
