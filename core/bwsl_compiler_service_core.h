#pragma once

#include "bwsl_spirv_backend.h"
#include "bwsl_ir_lowering.h"
#include "bwsl_ir_analysis.h"
#include "bwsl_cfg.h"
#include "bwsl_ssa.h"
#include "bwsl_parser_soa.h"
#include "bwsl_comptime_interpreter.h"
#include "bwsl_resource_reflection.h"
#include "bwsl_lexer.h"
#include "bwsl_arena.h"
#include "bwsl_render_config.h"
#include <vector>
#include <string>
#include <unordered_map>
#include <functional>
#include <fstream>
#include <filesystem>
#include <cstdio>
#include <array>
#include <algorithm>
#include <memory>

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
    
    // Active attribute streams. Bits are assigned from the shader's attributes {}
    // declaration order; position is validated as index 0 by the parser.
    u32 attributeMask = 0;
    
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
        u8 stages = 0;
        ResourceAccessMode access = ResourceAccessMode::ReadOnly;
        bool combinedSampledImage = false;
        std::string combinedWith;
        enum class Type { UniformBuffer, StorageBuffer, Texture, Sampler, StorageImage } type;
    };
    std::vector<ResourceBinding> resources;
    
    // Vertex input layout (for validation/debugging)
    struct AttributeBinding {
        std::string name;
        u32 attributeIndex;
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
    std::string cachedSource;
    std::unique_ptr<CompilationContext> cachedContext;
    std::unique_ptr<TokenStream> cachedTokenStream;
    std::unique_ptr<Lexer> cachedLexer;
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
        u32 attributeMask,
        const CompilationConfig& config = CompilationConfig()
    ) {
        // Find pipeline shader set
        auto pipelineIt = pipelineShaders.find(pipelineName);
        if (pipelineIt == pipelineShaders.end()) {
            return nullptr;
        }

        const u32 resolvedAttributeMask =
            ResolveAttributeMaskForPass(pipelineIt->second, passName, attributeMask);
        
        // Create variant key
        std::string variantKey = MakeVariantKey(pipelineName, passName, resolvedAttributeMask);
        
        // Check if already compiled
        auto variantIt = pipelineIt->second.variants.find(variantKey);
        if (variantIt != pipelineIt->second.variants.end()) {
            // Track usage for hot reload
            TrackVariantUsage(pipelineName, passName, resolvedAttributeMask);
            return &variantIt->second;
        }
        
        // Compile on demand
        CompilationConfig variantConfig = config;
        variantConfig.vertexPulling.attributeMask = resolvedAttributeMask;
        
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
        const std::vector<std::string>& activeAttributes,
        const CompilationConfig& config = CompilationConfig()
    ) {
        auto pipelineIt = pipelineShaders.find(pipelineName);
        if (pipelineIt == pipelineShaders.end()) {
            return nullptr;
        }
        if (!EnsureASTCached(pipelineIt->second)) {
            return nullptr;
        }

        const AST& ast = pipelineIt->second.cachedContext->ast;
        if (ast.pipelines.count == 0) {
            return nullptr;
        }

        const u32 mask = BuildNamedAttributeMask(
            ast,
            ast.pipelines[0],
            activeAttributes);
        return GetOrCompileVariant(pipelineName, passName, mask, config);
    }

    // Precompile one declared-attribute variant per pass. Unlike the old
    // "common mesh" presets, this is derived only from the shader source.
    void PrecompileDeclaredVariants(const CompilationConfig& config = CompilationConfig()) {
        for (auto& [pipelineName, shaderSet] : pipelineShaders) {
            if (!EnsureASTCached(shaderSet)) {
                continue;
            }
            AST& ast = shaderSet.cachedContext->ast;
            if (ast.pipelines.count == 0) {
                continue;
            }

            const PipelineData& pipeline = ast.pipelines[0];
            for (const auto& passConfig : renderConfig->passes) {
                if (passConfig.descriptor.pipelineName != pipelineName) {
                    continue;
                }
                for (u32 i = 0; i < pipeline.passes.count; i++) {
                    const PassData& pass = ast.GetPass(pipeline.passes[i]);
                    if (pass.name.view(shaderSet.cachedParser->sourceBase()) != passConfig.name) {
                        continue;
                    }

                    CompilationConfig variantConfig = config;
                    variantConfig.vertexPulling.attributeMask =
                        BuildPassAttributeMask(ast, pipeline, pass);
                    CompileVariant(shaderSet, passConfig.name, variantConfig);
                    break;
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
                shaderSet.cachedParser.reset();
                shaderSet.cachedLexer.reset();
                shaderSet.cachedTokenStream.reset();
                shaderSet.cachedContext.reset();
                shaderSet.cachedSource.clear();
                
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
    const RenderConfig* renderConfig = nullptr;
    std::string shaderPath_;
    
    // Pipeline name -> shader set
    std::unordered_map<std::string, PipelineShaderSet> pipelineShaders;
    
    // Recent variant usage tracking (for hot reload)
    struct VariantUsage {
        std::string pipelineName;
        std::string passName;
        u32 attributeMask;
        u32 frameLastUsed;
    };
    std::vector<VariantUsage> recentVariants;
    u32 currentFrame = 0;
    
    // ========================================================================
    // Internal Implementation
    // ========================================================================
    
    static std::string MakeVariantKey(const std::string& pipeline, const std::string& pass, u32 mask) {
        char buffer[128];
        snprintf(buffer, sizeof(buffer), "%s.%s.0x%08X", pipeline.c_str(), pass.c_str(), mask);
        return buffer;
    }
    
    static bool MaskContainsAttribute(u32 mask, u32 attributeIndex) {
        return attributeIndex < 32 && (mask & (1u << attributeIndex)) != 0;
    }

    static u32 BuildPassAttributeMask(const AST& ast,
                                      const PipelineData& pipeline,
                                      const PassData& pass) {
        u32 mask = 0;

        auto addDeclaredAttribute = [&](u32 nameHash) {
            for (u32 i = 0; i < pipeline.attributes.count; i++) {
                const AttributeDeclData& attr = ast.GetAttributeDecl(pipeline.attributes[i]);
                if (attr.name.nameHash == nameHash && attr.attributeIndex < 32) {
                    mask |= (1u << attr.attributeIndex);
                    return;
                }
            }
        };

        addDeclaredAttribute(Utils::HashStr("position"));

        if (pass.usedAttributes.count > 0) {
            for (u32 i = 0; i < pass.usedAttributes.count; i++) {
                addDeclaredAttribute(pass.usedAttributes[i].nameHash);
            }
            return mask;
        }

        for (u32 i = 0; i < pipeline.attributes.count; i++) {
            const AttributeDeclData& attr = ast.GetAttributeDecl(pipeline.attributes[i]);
            if (attr.attributeIndex < 32) {
                mask |= (1u << attr.attributeIndex);
            }
        }

        return mask;
    }

    static u32 BuildNamedAttributeMask(const AST& ast,
                                       const PipelineData& pipeline,
                                       const std::vector<std::string>& activeAttributes) {
        u32 mask = 0;

        auto addDeclaredAttribute = [&](u32 nameHash) {
            for (u32 i = 0; i < pipeline.attributes.count; i++) {
                const AttributeDeclData& attr = ast.GetAttributeDecl(pipeline.attributes[i]);
                if (attr.name.nameHash == nameHash && attr.attributeIndex < 32) {
                    mask |= (1u << attr.attributeIndex);
                    return;
                }
            }
        };

        addDeclaredAttribute(Utils::HashStr("position"));
        for (const std::string& attributeName : activeAttributes) {
            addDeclaredAttribute(Utils::HashStr(attributeName.c_str()));
        }
        return mask;
    }

    u32 ResolveAttributeMaskForPass(PipelineShaderSet& shaderSet,
                                    const std::string& passName,
                                    u32 requestedMask) {
        if (requestedMask != 0) {
            return requestedMask;
        }
        if (!EnsureASTCached(shaderSet)) {
            return 0;
        }
        AST& ast = shaderSet.cachedContext->ast;
        if (ast.pipelines.count == 0) {
            return 0;
        }

        const PipelineData& pipeline = ast.pipelines[0];
        for (u32 i = 0; i < pipeline.passes.count; i++) {
            const PassData& pass = ast.GetPass(pipeline.passes[i]);
            if (pass.name.view(shaderSet.cachedParser->sourceBase()) == passName) {
                return BuildPassAttributeMask(ast, pipeline, pass);
            }
        }
        return 0;
    }
    
    void TrackVariantUsage(const std::string& pipeline, const std::string& pass, u32 mask) {
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
        
        shaderSet.cachedSource.assign(std::istreambuf_iterator<char>(file),
                                      std::istreambuf_iterator<char>());
        file.close();
        
        // Create compilation context
        shaderSet.cachedContext = std::make_unique<CompilationContext>();
        shaderSet.cachedTokenStream = std::make_unique<TokenStream>();
        shaderSet.cachedTokenStream->Init(&shaderSet.cachedContext->arena,
                                          shaderSet.cachedSource.c_str(),
                                          shaderSet.cachedSource.length());
        
        // Create lexer and parser. The parser keeps pointers to both, so the
        // shader set owns them for as long as the AST is cached.
        shaderSet.cachedLexer = std::make_unique<Lexer>(shaderSet.cachedSource,
                                                        *shaderSet.cachedTokenStream);
        shaderSet.cachedLexer->Tokenize();
        shaderSet.cachedParser = std::make_unique<Parser>();
        shaderSet.cachedParser->Init(shaderSet.cachedLexer.get(),
                                     shaderSet.cachedTokenStream.get(),
                                     shaderSet.cachedContext.get());
        
        // Parse the pipeline
        shaderSet.cachedParser->ParsePipeline();
        
        if (shaderSet.cachedParser->hadError) {
            return false;
        }

        std::string variantResolveError;
        if (!shaderSet.cachedParser->ResolveVariants(shaderSet.cachedContext->root,
                                                     &variantResolveError)) {
            return false;
        }

        std::string comptimeError;
        if (!Comptime::RunComptimeInterpreter(shaderSet.cachedContext.get(),
                                              shaderSet.cachedParser.get(),
                                              shaderSet.cachedContext->root,
                                              &comptimeError)) {
            return false;
        }
        shaderSet.cachedParser->ResolveShaderStages(shaderSet.cachedContext->root);
        
        shaderSet.astCached = true;
        return true;
    }
    
    bool CompileVariant(PipelineShaderSet& shaderSet, const std::string& passName, 
                        const CompilationConfig& config) {
        // Ensure AST is parsed and cached
        if (!EnsureASTCached(shaderSet)) {
            return false;
        }
        
        u32 attributeMask = config.vertexPulling.attributeMask;
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
        if (attributeMask == 0) {
            attributeMask = BuildPassAttributeMask(ast, pipeline, targetPass);
        }
        CompilationConfig resolvedConfig = config;
        resolvedConfig.vertexPulling.attributeMask = attributeMask;
        std::string variantKey = MakeVariantKey(shaderSet.pipelineName, passName, attributeMask);
        
        // Create the compiled variant
        CompiledVariant& variant = shaderSet.variants[variantKey];
        variant.variantKey = variantKey;
        variant.pipelineName = shaderSet.pipelineName;
        variant.passName = passName;
        variant.attributeMask = attributeMask;
        
        IRAnalysis vertexAnalysis{};
        IRAnalysis fragmentAnalysis{};
        std::vector<ExplicitSamplerUse> reflectionSamplerUses;
        bool hasVertexAnalysis = false;
        bool hasFragmentAnalysis = false;

        // Compile vertex shader if present
        if (!targetPass.vertexShader.IsNull()) {
            if (!CompileShaderStage(shaderSet, targetPass.vertexShader, 
                                    ShaderStage::Vertex, resolvedConfig, variant.vertexSpirv,
                                    &vertexAnalysis, &reflectionSamplerUses)) {
                shaderSet.variants.erase(variantKey);
                return false;
            }
            hasVertexAnalysis = true;
        }
        
        // Compile fragment shader if present
        if (!targetPass.fragmentShader.IsNull()) {
            CompilationConfig fragConfig = resolvedConfig;
            fragConfig.stage = ShaderStage::Fragment;
            if (!CompileShaderStage(shaderSet, targetPass.fragmentShader,
                                    ShaderStage::Fragment, fragConfig, variant.fragmentSpirv,
                                    &fragmentAnalysis, &reflectionSamplerUses)) {
                shaderSet.variants.erase(variantKey);
                return false;
            }
            hasFragmentAnalysis = true;
        }

        // Populate attribute bindings for reflection
        PopulateAttributeBindings(variant, ast, pipeline, shaderSet.cachedParser->sourceBase(), resolvedConfig);
        PopulateResourceBindings(shaderSet, pipeline, targetPass, resolvedConfig, variant,
                                 hasVertexAnalysis ? &vertexAnalysis : nullptr,
                                 hasFragmentAnalysis ? &fragmentAnalysis : nullptr,
                                 nullptr,
                                 reflectionSamplerUses.empty() ? nullptr : &reflectionSamplerUses);
        
        return true;
    }
    
    bool CompileShaderStage(PipelineShaderSet& shaderSet, NodeRef stageRef,
                           ShaderStage stage, const CompilationConfig& config,
                           std::vector<u32>& outSpirv,
                           IRAnalysis* outAnalysis = nullptr,
                           std::vector<ExplicitSamplerUse>* outExplicitSamplerUses = nullptr) {
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
        if (config.vertexPulling.attributeMask != 0) {
            optimizer.EliminateUnavailableAttributes(&lowering.program,
                                                     config.vertexPulling.attributeMask);
        }
        
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

        if (outExplicitSamplerUses) {
            std::vector<ExplicitSamplerUse> stageUses =
                CollectExplicitSamplerUses(lowering.program, stage);
            outExplicitSamplerUses->insert(outExplicitSamplerUses->end(),
                                           stageUses.begin(), stageUses.end());
        }
        
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
        if (outAnalysis) {
            *outAnalysis = builder.analysis;
        }

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
        SPIRVBuilder::VertexPullingConfig vpConfig;
        vpConfig.mode = (config.vertexPulling.mode == VertexInputMode::UnifiedWithOffsets)
            ? SPIRVBuilder::VertexInputMode::UnifiedWithOffsets
            : SPIRVBuilder::VertexInputMode::SeparateBuffers;
        vpConfig.attributeMask = config.vertexPulling.attributeMask;
        vpConfig.baseBufferBinding = config.vertexPulling.baseBufferBinding;
        vpConfig.descriptorSet = config.vertexPulling.descriptorSet;
        builder.SetVertexPullingConfig(vpConfig);
    }
    
    void PopulateAttributeBindings(CompiledVariant& variant,
                                   const AST& ast,
                                   const PipelineData& pipeline,
                                   const char* sourceBase,
                                   const CompilationConfig& config) {
        u32 binding = config.vertexPulling.baseBufferBinding;
        variant.attributeBindings.clear();
        
        for (u32 i = 0; i < pipeline.attributes.count; i++) {
            const AttributeDeclData& attr = ast.GetAttributeDecl(pipeline.attributes[i]);
            if (MaskContainsAttribute(variant.attributeMask, attr.attributeIndex)) {
                CompiledVariant::AttributeBinding attrBinding;
                attrBinding.name = attr.name.ToString(sourceBase);
                attrBinding.attributeIndex = attr.attributeIndex;
                attrBinding.binding = binding;
                attrBinding.location = attr.attributeIndex;
                variant.attributeBindings.push_back(attrBinding);
                
                if (config.vertexPulling.mode == VertexInputMode::SeparateBuffers) {
                    binding++;  // Each attribute gets its own binding
                }
            }
        }
    }

    static CompiledVariant::ResourceBinding::Type MapReflectedResourceType(::ResourceBinding::Type type) {
        switch (type) {
            case ::ResourceBinding::UniformBuffer:
                return CompiledVariant::ResourceBinding::Type::UniformBuffer;
            case ::ResourceBinding::StorageBuffer:
                return CompiledVariant::ResourceBinding::Type::StorageBuffer;
            case ::ResourceBinding::Texture:
                return CompiledVariant::ResourceBinding::Type::Texture;
            case ::ResourceBinding::Sampler:
                return CompiledVariant::ResourceBinding::Type::Sampler;
            case ::ResourceBinding::StorageImage:
                return CompiledVariant::ResourceBinding::Type::StorageImage;
            default:
                return CompiledVariant::ResourceBinding::Type::UniformBuffer;
        }
    }

    void PopulateResourceBindings(PipelineShaderSet& shaderSet,
                                  const PipelineData& pipeline,
                                  const PassData& pass,
                                  const CompilationConfig& config,
                                  CompiledVariant& variant,
                                  const IRAnalysis* vertexAnalysis,
                                  const IRAnalysis* fragmentAnalysis,
                                  const IRAnalysis* computeAnalysis,
                                  const std::vector<ExplicitSamplerUse>* explicitSamplerUses) {
        ResourceReflectionConfig reflectionConfig;
        reflectionConfig.vertexPullingMode = (config.vertexPulling.mode == VertexInputMode::UnifiedWithOffsets)
            ? ResourceReflectionConfig::VertexPullingMode::UnifiedWithOffsets
            : ResourceReflectionConfig::VertexPullingMode::SeparateBuffers;
        reflectionConfig.attributeMask = config.vertexPulling.attributeMask;
        if (reflectionConfig.attributeMask == 0 && vertexAnalysis) {
            reflectionConfig.attributeMask = vertexAnalysis->usedAttributeMask;
        }
        reflectionConfig.baseBufferBinding = config.vertexPulling.baseBufferBinding;
        reflectionConfig.descriptorSet = config.vertexPulling.descriptorSet;

        const std::vector<ReflectedResourceBinding> reflected =
            BuildResolvedResourceReflection(&shaderSet.cachedContext->ast,
                                           &pipeline,
                                           &pass,
                                           shaderSet.cachedParser->symbolTable,
                                           shaderSet.cachedParser->sourceBase(),
                                           vertexAnalysis,
                                           fragmentAnalysis,
                                           computeAnalysis,
                                           reflectionConfig,
                                           explicitSamplerUses);

        variant.resources.clear();
        variant.resources.reserve(reflected.size());
        for (const ReflectedResourceBinding& resource : reflected) {
            CompiledVariant::ResourceBinding binding;
            binding.name = resource.name;
            binding.set = resource.set;
            binding.binding = resource.binding;
            binding.stages = resource.stages;
            binding.access = resource.access;
            binding.combinedSampledImage = resource.combinedSampledImage;
            binding.combinedWith = resource.combinedWith;
            binding.type = MapReflectedResourceType(resource.type);
            variant.resources.push_back(std::move(binding));
        }
    }
};

} // namespace BWSL
