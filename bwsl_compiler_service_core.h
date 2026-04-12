#pragma once

#include "bwsl_spirv_backend.h"
#include "bwsl_ir_lowering.h"
#include "bwsl_ir_analysis.h"
#include "bwsl_cfg.h"
#include "bwsl_ssa.h"
#include "bwsl_parser_soa.h"
#include "bwsl_lexer.h"
#include "../arena_allocator.h"
#include "../render_config.h"
#include "../model_types.h"
#include <vector>
#include <string>
#include <unordered_map>
#include <functional>
#include <fstream>
#include <filesystem>
#include <cstdio>
#include <array>

namespace BWSL {

// ============================================================================
// Vertex Input Mode Configuration
// ============================================================================

enum class VertexInputMode : u8 {
    // One buffer per attribute (matches ModelData::attributeStreams from model_types.h)
    // Each attribute gets its own storage buffer binding
    SeparateBuffers,
    
    // Single unified buffer with per-attribute offset table
    // Used by current Metal shader approach with unifiedVertexBuffer
    UnifiedWithOffsets,
};

// Configuration for vertex attribute pulling
struct VertexPullingConfig {
    VertexInputMode mode = VertexInputMode::SeparateBuffers;
    
    // Which attributes are active (bitmask from VertexAttributeType)
    // e.g., AttributeMask(POSITION) | AttributeMask(NORMAL) | AttributeMask(TEXCOORD)
    u8 attributeMask = 0;
    
    // Base binding index for attribute buffers
    // For SeparateBuffers: position at baseBinding, normal at baseBinding+1, etc.
    // For UnifiedWithOffsets: unified buffer at baseBinding, offset table at baseBinding+1
    u32 baseBufferBinding = 0;
    
    // Descriptor set for vertex buffers (Vulkan-style, maps to buffer index on Metal)
    u32 descriptorSet = 0;
};

// ============================================================================
// Target Backend Configuration
// ============================================================================

enum class TargetBackend : u8 {
    SPIRV,      // Raw SPIR-V (for Vulkan or further processing)
    Metal,      // Use spirv-cross to convert to MSL
    HLSL,       // Use spirv-cross to convert to HLSL (DX12)
    GLSL,       // Use spirv-cross to convert to GLSL (OpenGL/ES)
    PSSL,       // Use spirv-cross to convert to PSSL (PlayStation Shader Language)
};

struct CompilationConfig {
    TargetBackend targetBackend = TargetBackend::SPIRV;
    VertexPullingConfig vertexPulling;
    std::string shadesPath = "shaders/";

    // Shader stage to compile
    ShaderStage stage = ShaderStage::Vertex;

    // Debug options
    bool generateDebugInfo = false;
    bool preserveBindings = true;  // Keep original binding numbers through spirv-cross

    // Validation options
    bool validateSpirv = true;     // Run spirv-val on generated SPIR-V
};

// ============================================================================
// SPIR-V Validation Utility
// ============================================================================

// Runs spirv-val on the given SPIR-V binary
// Returns empty string on success, error message on failure
inline std::string ValidateSpirvBinary(const std::vector<u32>& spirv, const std::string& stageName = "") {
    // Write to temp file
    std::string tempPath = "/tmp/bwsl_validate_" + stageName + ".spv";
    std::ofstream outFile(tempPath, std::ios::binary);
    if (!outFile.is_open()) {
        return "Failed to write temp file for validation";
    }
    outFile.write(reinterpret_cast<const char*>(spirv.data()), spirv.size() * sizeof(u32));
    outFile.close();

    // Run spirv-val
    std::string cmd = "spirv-val \"" + tempPath + "\" 2>&1";
    std::array<char, 4096> buffer;
    std::string result;

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return "Failed to run spirv-val (not installed?)";
    }

    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result += buffer.data();
    }

    int status = pclose(pipe);

    // Clean up temp file
    std::filesystem::remove(tempPath);

    // Check result - empty or no "error" means success
    if (status == 0 && (result.empty() || result.find("error") == std::string::npos)) {
        return "";  // Success
    }

    return result.empty() ? "spirv-val returned error code " + std::to_string(status) : result;
}

// ============================================================================
// Compiled Variant (Platform-Agnostic)
// ============================================================================

struct CompiledVariant {
    // Variant identification
    std::string variantKey;          // e.g., "Character.Forward.0xC7"
    std::string pipelineName;
    std::string passName;
    u32 attributeMask;
    
    // Compiled SPIR-V binary (platform-agnostic intermediate)
    std::vector<u32> vertexSpirv;
    std::vector<u32> fragmentSpirv;
    
    // Generated source code (after spirv-cross, if requested)
    std::string vertexSource;        // MSL/HLSL/GLSL depending on target
    std::string fragmentSource;
    
    // Reflection data for resource binding
    struct ResourceBinding {
        std::string name;
        u32 set;
        u32 binding;
        enum class Type { UniformBuffer, StorageBuffer, Texture, Sampler } type;
    };
    std::vector<ResourceBinding> resources;
    
    // Vertex input layout (for validation/debugging)
    struct AttributeBinding {
        VertexAttributeType type;
        u32 binding;
        u32 location;
    };
    std::vector<AttributeBinding> attributeBindings;
    
    // Platform-specific handle (set by middleware after compilation)
    void* platformHandle = nullptr;
};

// ============================================================================
// Pipeline Shader Set (Caches all variants for a pipeline)
// ============================================================================

struct PipelineShaderSet {
    std::string pipelineName;
    std::string bwslPath;
    std::time_t lastModified;
    
    // Cached AST for this pipeline (avoid re-parsing)
    std::unique_ptr<CompilationContext> cachedContext;
    std::unique_ptr<Parser> cachedParser;
    bool astCached = false;
    
    // Variant cache: key = "PassName.0xAttributeMask"
    std::unordered_map<std::string, CompiledVariant> variants;
};

// ============================================================================
// BWSL Compiler Service Core (Platform-Agnostic)
// ============================================================================

class BWSLCompilerServiceCore {
public:
    // ========================================================================
    // Initialization
    // ========================================================================
    
    void Initialize(const RenderConfig& config, size_t arenaSize = 32 * 1024 * 1024, const std::string& shadersPath = "shaders/") {
        renderConfig = &config;
        shaderPath_ = shadersPath;
        
        if (!std::filesystem::exists(shaderPath_)) {
            return;
        }
         
        // Scan for BWSL files matching pipeline names
        ScanForBWSLFiles();
    }
    
    void Shutdown() {
        pipelineShaders.clear();
    }
    
    // ========================================================================
    // Variant Compilation (Core API)
    // ========================================================================
    
    // Get or compile a variant for a specific attribute configuration
    // This is the main entry point - returns cached variant or compiles on demand
    CompiledVariant* GetOrCompileVariant(
        const std::string& pipelineName,
        const std::string& passName,
        u8 attributeMask,
        const CompilationConfig& config = CompilationConfig()
    ) {
        // Find pipeline shader set
        auto pipelineIt = pipelineShaders.find(pipelineName);
        if (pipelineIt == pipelineShaders.end()) {
            return nullptr;
        }
        
        // Create variant key
        std::string variantKey = MakeVariantKey(pipelineName, passName, attributeMask);
        
        // Check if already compiled
        auto variantIt = pipelineIt->second.variants.find(variantKey);
        if (variantIt != pipelineIt->second.variants.end()) {
            // Track usage for hot reload
            TrackVariantUsage(pipelineName, passName, attributeMask);
            return &variantIt->second;
        }
        
        // Compile on demand
        CompilationConfig variantConfig = config;
        variantConfig.vertexPulling.attributeMask = attributeMask;
        
        if (!CompileVariant(pipelineIt->second, passName, variantConfig)) {
            return nullptr;
        }
        
        // Return newly compiled variant
        return &pipelineIt->second.variants[variantKey];
    }
    
    // Convenience overload using active attributes list
    CompiledVariant* GetOrCompileVariant(
        const std::string& pipelineName,
        const std::string& passName,
        const std::vector<VertexAttributeType>& activeAttributes,
        const CompilationConfig& config = CompilationConfig()
    ) {
        u8 mask = CalculateAttributeMask(activeAttributes);
        return GetOrCompileVariant(pipelineName, passName, mask, config);
    }
    
    // ========================================================================
    // Precompilation
    // ========================================================================
    
    // Precompile common variants based on usage patterns
    void PrecompileCommonVariants(const CompilationConfig& config = CompilationConfig()) {
        // Common attribute combinations
        const u8 commonMasks[] = {
            // Static meshes: position + normal + texcoord
            AttributeMask(VertexAttributeType::POSITION) | 
            AttributeMask(VertexAttributeType::NORMAL) | 
            AttributeMask(VertexAttributeType::TEXCOORD),
            
            // Static meshes with tangents (for normal mapping)
            AttributeMask(VertexAttributeType::POSITION) | 
            AttributeMask(VertexAttributeType::NORMAL) | 
            AttributeMask(VertexAttributeType::TEXCOORD) |
            AttributeMask(VertexAttributeType::TANGENT) |
            AttributeMask(VertexAttributeType::BITANGENT),
            
            // Animated meshes: full attribute set
            AttributeMask(VertexAttributeType::POSITION) | 
            AttributeMask(VertexAttributeType::NORMAL) | 
            AttributeMask(VertexAttributeType::TEXCOORD) |
            AttributeMask(VertexAttributeType::BONE_INDICES) |
            AttributeMask(VertexAttributeType::BONE_WEIGHTS),
        };
        
        for (auto& [pipelineName, shaderSet] : pipelineShaders) {
            // Get pass names from render config
            for (const auto& pass : renderConfig->passes) {
                if (pass.descriptor.pipelineName == pipelineName) {
                    for (u8 mask : commonMasks) {
                        CompilationConfig variantConfig = config;
                        variantConfig.vertexPulling.attributeMask = mask;
                        CompileVariant(shaderSet, pass.name, variantConfig);
                    }
                }
            }
        }
    }
    
    // ========================================================================
    // Hot Reload Support
    // ========================================================================
    
    void HandleFileChange(const std::string& bwslPath) {
        for (auto& [pipelineName, shaderSet] : pipelineShaders) {
            if (shaderSet.bwslPath == bwslPath) {
                // Invalidate cached AST
                shaderSet.astCached = false;
                shaderSet.cachedContext.reset();
                shaderSet.cachedParser.reset();
                
                // Clear all variants
                shaderSet.variants.clear();
                
                // Recompile recently used variants
                RecompileRecentVariants(pipelineName);
                
                // Notify listeners
                if (onPipelineRecompiled) {
                    onPipelineRecompiled(pipelineName);
                }
                
                break;
            }
        }
    }
    
    // Callback for pipeline recompilation (used by middleware)
    std::function<void(const std::string& pipelineName)> onPipelineRecompiled;
    
    // ========================================================================
    // Accessors
    // ========================================================================
    
    const RenderConfig* GetRenderConfig() const { return renderConfig; }
    
    bool HasPipeline(const std::string& name) const {
        return pipelineShaders.find(name) != pipelineShaders.end();
    }
    
    std::vector<std::string> GetPipelineNames() const {
        std::vector<std::string> names;
        names.reserve(pipelineShaders.size());
        for (const auto& [name, _] : pipelineShaders) {
            names.push_back(name);
        }
        return names;
    }

private:
    Memory::BWEMemoryArena* compilerArena = nullptr;
    const RenderConfig* renderConfig = nullptr;
    std::string shaderPath_;
    
    // Pipeline name -> shader set
    std::unordered_map<std::string, PipelineShaderSet> pipelineShaders;
    
    // Recent variant usage tracking (for hot reload)
    struct VariantUsage {
        std::string pipelineName;
        std::string passName;
        u8 attributeMask;
        u32 frameLastUsed;
    };
    std::vector<VariantUsage> recentVariants;
    u32 currentFrame = 0;
    
    // ========================================================================
    // Internal Implementation
    // ========================================================================
    
    static std::string MakeVariantKey(const std::string& pipeline, const std::string& pass, u8 mask) {
        char buffer[128];
        snprintf(buffer, sizeof(buffer), "%s.%s.0x%02X", pipeline.c_str(), pass.c_str(), mask);
        return buffer;
    }
    
    static u8 CalculateAttributeMask(const std::vector<VertexAttributeType>& attributes) {
        u8 mask = 0;
        for (auto attr : attributes) {
            mask |= AttributeMask(attr);
        }
        return mask;
    }
    
    void TrackVariantUsage(const std::string& pipeline, const std::string& pass, u8 mask) {
        // Remove old entry if exists
        recentVariants.erase(
            std::remove_if(recentVariants.begin(), recentVariants.end(),
                [&](const VariantUsage& u) {
                    return u.pipelineName == pipeline && u.passName == pass && u.attributeMask == mask;
                }),
            recentVariants.end()
        );
        
        // Add new entry
        recentVariants.push_back({pipeline, pass, mask, currentFrame});
        
        // Keep only recent entries
        if (recentVariants.size() > 64) {
            recentVariants.erase(recentVariants.begin());
        }
    }
    
    void RecompileRecentVariants(const std::string& pipelineName) {
        auto pipelineIt = pipelineShaders.find(pipelineName);
        if (pipelineIt == pipelineShaders.end()) return;
        
        for (const auto& usage : recentVariants) {
            if (usage.pipelineName == pipelineName) {
                CompilationConfig config;
                config.vertexPulling.attributeMask = usage.attributeMask;
                CompileVariant(pipelineIt->second, usage.passName, config);
            }
        }
    }
    
    void ScanForBWSLFiles() {
        if (!std::filesystem::exists(shaderPath_)) {
            return;
        }
        
        for (const auto& entry : std::filesystem::directory_iterator(shaderPath_)) {
            if (entry.path().extension() == ".bwsl") {
                std::string filename = entry.path().stem().string();
                
                // Check if this matches a pipeline name in render config
                for (const auto& pass : renderConfig->passes) {
                    if (pass.descriptor.pipelineName == filename) {
                        PipelineShaderSet shaderSet;
                        shaderSet.pipelineName = filename;
                        shaderSet.bwslPath = entry.path().string();
                        shaderSet.lastModified = std::filesystem::last_write_time(entry.path()).time_since_epoch().count();
                        
                        pipelineShaders[filename] = std::move(shaderSet);
                        break;
                    }
                }
            }
        }
    }
    
    bool EnsureASTCached(PipelineShaderSet& shaderSet) {
        if (shaderSet.astCached) {
            return true;
        }
        
        // Read BWSL source
        std::ifstream file(shaderSet.bwslPath);
        if (!file.is_open()) {
            return false;
        }
        
        std::string source((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
        file.close();
        
        // Create compilation context
        shaderSet.cachedContext = std::make_unique<CompilationContext>();
        
        // Create lexer and parser
        auto lexer = std::make_unique<Lexer>(source);
        shaderSet.cachedParser = std::make_unique<Parser>();
        shaderSet.cachedParser->Init(lexer.get(), shaderSet.cachedContext.get());
        
        // Initialize symbol table with render config
        SymbolTable::InitFromRenderConfig(&shaderSet.cachedParser->symbolTable, *renderConfig);
        
        // Parse the pipeline
        shaderSet.cachedParser->ParsePipeline();
        
        if (shaderSet.cachedParser->hadError) {
            return false;
        }
        
        shaderSet.astCached = true;
        return true;
    }
    
    bool CompileVariant(PipelineShaderSet& shaderSet, const std::string& passName, 
                        const CompilationConfig& config) {
        // Ensure AST is parsed and cached
        if (!EnsureASTCached(shaderSet)) {
            return false;
        }
        
        u8 attributeMask = config.vertexPulling.attributeMask;
        std::string variantKey = MakeVariantKey(shaderSet.pipelineName, passName, attributeMask);
        
        // Find the pass in AST
        AST& ast = shaderSet.cachedContext->ast;
        if (ast.pipelines.count == 0) {
            return false;
        }
        
        const PipelineData& pipeline = ast.pipelines[0];
        NodeRef targetPassRef;
        
        for (u32 i = 0; i < pipeline.passes.count; i++) {
            const PassData& pass = ast.GetPass(pipeline.passes[i]);
            if (pass.name.view(shaderSet.cachedParser->sourceBase()) == passName) {
                targetPassRef = pipeline.passes[i];
                break;
            }
        }
        
        if (targetPassRef.IsNull()) {
            return false;
        }
        
        const PassData& targetPass = ast.GetPass(targetPassRef);
        
        // Create the compiled variant
        CompiledVariant& variant = shaderSet.variants[variantKey];
        variant.variantKey = variantKey;
        variant.pipelineName = shaderSet.pipelineName;
        variant.passName = passName;
        variant.attributeMask = attributeMask;
        
        // Compile vertex shader if present
        if (!targetPass.vertexShader.IsNull()) {
            if (!CompileShaderStage(shaderSet, targetPass.vertexShader, 
                                    ShaderStage::Vertex, config, variant.vertexSpirv)) {
                shaderSet.variants.erase(variantKey);
                return false;
            }
        }
        
        // Compile fragment shader if present
        if (!targetPass.fragmentShader.IsNull()) {
            CompilationConfig fragConfig = config;
            fragConfig.stage = ShaderStage::Fragment;
            if (!CompileShaderStage(shaderSet, targetPass.fragmentShader,
                                    ShaderStage::Fragment, fragConfig, variant.fragmentSpirv)) {
                shaderSet.variants.erase(variantKey);
                return false;
            }
        }
        
        // Populate attribute bindings for reflection
        PopulateAttributeBindings(variant, config);
        
        return true;
    }
    
    bool CompileShaderStage(PipelineShaderSet& shaderSet, NodeRef stageRef,
                           ShaderStage stage, const CompilationConfig& config,
                           std::vector<u32>& outSpirv) {
        AST& ast = shaderSet.cachedContext->ast;
        const ShaderStageData& shaderStage = ast.GetShaderStage(stageRef);
        
        if (shaderStage.body.IsNull()) {
            return false;
        }
        
        // Create IR memory pool
        IRMemoryPool irPool;
        
        // Lower AST to IR
        IR::IRLowering lowering;
        lowering.Initialize(&irPool, &shaderSet.cachedParser->symbolTable, &ast);
        lowering.currentStage = stage;

        const PassData* owningPass = nullptr;
        if (ast.pipelines.count > 0) {
            const PipelineData& pipeline = ast.pipelines[0];
            for (u32 i = 0; i < pipeline.passes.count; i++) {
                const PassData& pass = ast.GetPass(pipeline.passes[i]);
                if (pass.vertexShader == stageRef ||
                    pass.fragmentShader == stageRef ||
                    pass.computeShader == stageRef) {
                    owningPass = &pass;
                    break;
                }
            }
        }

        if (owningPass) {
            for (u32 i = 0; i < owningPass->consts.count; i++) {
                lowering.LowerStatement(owningPass->consts[i]);
            }
        }
        
        // Lower the shader body
        const BlockData& block = ast.GetBlock(shaderStage.body);
        for (u32 i = 0; i < block.statements.count; i++) {
            lowering.LowerStatement(block.statements[i]);
        }
        
        // Ensure return at end
        if (lowering.program.instructionCount == 0 ||
            lowering.program.opcodes[lowering.program.instructionCount - 1] != IR::OP_RET) {
            lowering.builder.EmitInstruction(IR::OP_RET, 0, 0);
        }
        
        // Specialize for this variant's attribute mask
        IR::OptimizationPass optimizer;
        optimizer.SpecializeForVariant(&lowering.program, config.vertexPulling.attributeMask);
        
        // Dead code elimination - removes unused attribute code paths
        optimizer.EliminateDeadCode(&lowering.program);
        
        // --- CFG Construction ---
        Memory::BWEMemoryArena cfgArena;
        char cfgMem[512 * 1024];
        cfgArena.Initialize(cfgMem, sizeof(cfgMem));
        
        CFGBuilder cfgBuilder;
        cfgBuilder.Init(&lowering.program, &cfgArena);
        cfgBuilder.Build();
        
        // --- Dead Block Elimination (removes unreachable blocks after specialization) ---
        optimizer.EliminateDeadBlocks(&lowering.program, &cfgBuilder.cfg);
        
        // --- Rebuild CFG after dead block elimination (block structure changed) ---
        cfgBuilder.Init(&lowering.program, &cfgArena);
        cfgBuilder.Build();
        
        // --- SSA Conversion ---
        SSA::ConvertToSSA(&lowering.program, &cfgBuilder.cfg, &cfgBuilder, &cfgArena);
        
        // --- Generate SPIR-V ---
        Memory::BWEMemoryArena spirvArena;
        char spirvMem[512 * 1024];
        spirvArena.Initialize(spirvMem, sizeof(spirvMem));
        
        SPIRVBuilder builder;
        builder.Initialize(&spirvArena, &lowering.program, stage,
                          &shaderSet.cachedParser->symbolTable, &cfgBuilder.cfg);

        // Configure vertex pulling mode
        ConfigureVertexPulling(builder, config);

        // Generate shader
        builder.EmitFunction();

        outSpirv = builder.Finalize();

        if (outSpirv.size() <= 5) {
            return false;  // Invalid SPIR-V (too small)
        }

        // Validate SPIR-V if enabled
        if (config.validateSpirv) {
            std::string stageName = (stage == ShaderStage::Vertex) ? "vertex" : "fragment";
            std::string validationError = ValidateSpirvBinary(outSpirv, stageName);
            if (!validationError.empty()) {
                fprintf(stderr, "BWSL Compiler Error: SPIR-V validation failed for %s shader:\n%s\n",
                        stageName.c_str(), validationError.c_str());
                return false;
            }
        }

        return true;
    }
    
    void ConfigureVertexPulling(SPIRVBuilder& builder, const CompilationConfig& config) {
        // This will be expanded in the spirv-vertex-pulling todo
        // For now, the builder uses its default attribute handling
        (void)builder;
        (void)config;
    }
    
    void PopulateAttributeBindings(CompiledVariant& variant, const CompilationConfig& config) {
        u32 binding = config.vertexPulling.baseBufferBinding;
        
        for (u8 i = 0; i < static_cast<u8>(VertexAttributeType::COUNT); i++) {
            if (variant.attributeMask & (1 << i)) {
                CompiledVariant::AttributeBinding attrBinding;
                attrBinding.type = static_cast<VertexAttributeType>(i);
                attrBinding.binding = binding;
                attrBinding.location = i;
                variant.attributeBindings.push_back(attrBinding);
                
                if (config.vertexPulling.mode == VertexInputMode::SeparateBuffers) {
                    binding++;  // Each attribute gets its own binding
                }
            }
        }
    }
};

} // namespace BWSL
