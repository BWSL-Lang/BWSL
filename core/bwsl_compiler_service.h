#pragma once

// ==============================================================================
// BWSL Compiler Service - Metal Backend
// ==============================================================================
// 
// This is the Metal-specific compiler service that wraps the platform-agnostic
// BWSLCompilerServiceCore. For other platforms (Vulkan, DirectX), use the
// appropriate middleware with BWSLCompilerServiceCore directly.
//
// The service compiles BWSL shaders to Metal, generating optimized variants
// based on which vertex attributes are actually used by each model.
//
// VERTEX ATTRIBUTE PULLING:
// BWSL is designed around vertex attribute pulling, where each attribute
// (position, normal, texcoord, etc.) comes from its own storage buffer
// indexed by the vertex ID. This eliminates vertex descriptors and allows
// maximum flexibility in buffer organization.
//
// ==============================================================================

#include "bwsl_compiler_service_core.h"
#include "middleware/bwsl_middleware_interface.h"
#include "middleware/bwsl_metal_middleware.h"
#include "../file_watcher.h"
#include "../pipeline_builder.h"
#include "../render_config.h"

#ifdef __APPLE__
#import <Metal/Metal.h>

// Metal-specific compiled variant with MTLLibrary handles
struct MetalCompiledVariant {
    std::string variantKey;      // e.g. "Character.OpaquePass.0x0F"
    std::string vertexSource;    // Generated MSL source
    std::string fragmentSource;
    std::string functionName;    // Entry point function name (default: "main")
    id<MTLLibrary> vertexLib;
    id<MTLLibrary> fragmentLib;
    u32 attributeMask;           // Which attribute streams are enabled
    
    // Reference to underlying platform-agnostic variant
    BWSL::CompiledVariant* coreVariant = nullptr;
};

// Metal-specific compiler service (wraps BWSLCompilerServiceCore)
class BWSLCompilerService {
public:
    // ========================================================================
    // Initialization
    // ========================================================================
    
    void Initialize(id<MTLDevice> mtlDevice, const RenderConfig& config) {
        device_ = mtlDevice;
        
        // Initialize platform-agnostic core
        coreService_.Initialize(config);
        
        // Initialize Metal middleware
        metalMiddleware_ = BWSL::CreateMetalMiddleware(mtlDevice);
        
        // Set up hot reload callback
        coreService_.onPipelineRecompiled = [this](const std::string& pipelineName) {
            HandlePipelineRecompiled(pipelineName);
        };
    }
    
    void Shutdown() {
        metalVariants_.clear();
        if (metalMiddleware_) {
            metalMiddleware_->Destroy();
            metalMiddleware_ = nullptr;
        }
        coreService_.Shutdown();
    }
    
    // ========================================================================
    // Variant Compilation (Main API)
    // ========================================================================
    
    // Get or compile a variant for specific attributes
    // This is the main entry point for the render system
    MetalCompiledVariant* GetOrCompileVariant(
        const std::string& pipelineName,
        const std::string& passName,
        u32 attributeMask,
        BWSL::VertexInputMode vertexMode = BWSL::VertexInputMode::SeparateBuffers
    ) {
        // Configure compilation
        BWSL::CompilationConfig config;
        config.targetBackend = BWSL::TargetBackend::Metal;
        config.vertexPulling.mode = vertexMode;
        config.vertexPulling.attributeMask = attributeMask;
        
        // Get SPIR-V from core service
        BWSL::CompiledVariant* coreVariant = coreService_.GetOrCompileVariant(
            pipelineName, passName, attributeMask, config);
        
        if (!coreVariant || !metalMiddleware_) {
            return nullptr;
        }

        const std::string variantKey = MakeVariantKey(
            pipelineName, passName, coreVariant->attributeMask);

        // Check Metal cache after the core service resolves shader-declared defaults.
        auto it = metalVariants_.find(variantKey);
        if (it != metalVariants_.end()) {
            return &it->second;
        }
        
        // Compile to Metal via middleware 
        BWSL::MiddlewareCompilationResult result = metalMiddleware_->CompileVariant(*coreVariant);
        
        if (!result.success) {
            return nullptr;
        }
        
        // Store in Metal cache
        MetalCompiledVariant& metalVariant = metalVariants_[variantKey];
        metalVariant.variantKey = variantKey;
        metalVariant.vertexSource = result.vertexSource;
        metalVariant.fragmentSource = result.fragmentSource;
        metalVariant.functionName = "main";  // Default entry point for SPIR-V compiled shaders
        metalVariant.vertexLib = (__bridge_transfer id<MTLLibrary>)result.handle.vertexShader;
        metalVariant.fragmentLib = (__bridge_transfer id<MTLLibrary>)result.handle.fragmentShader;
        metalVariant.attributeMask = coreVariant->attributeMask;
        metalVariant.coreVariant = coreVariant;
        
        return &metalVariant;
    }
    
    // Convenience: Get variant using attribute name list
    MetalCompiledVariant* GetOrCompileVariant(
        const std::string& pipelineName,
        const std::string& passName,
        const std::vector<std::string>& activeAttributes
    ) {
        BWSL::CompilationConfig config;
        config.targetBackend = BWSL::TargetBackend::Metal;
        config.vertexPulling.mode = BWSL::VertexInputMode::SeparateBuffers;

        BWSL::CompiledVariant* coreVariant = coreService_.GetOrCompileVariant(
            pipelineName, passName, activeAttributes, config);

        if (!coreVariant || !metalMiddleware_) {
            return nullptr;
        }

        const std::string variantKey = MakeVariantKey(
            pipelineName, passName, coreVariant->attributeMask);

        auto it = metalVariants_.find(variantKey);
        if (it != metalVariants_.end()) {
            return &it->second;
        }

        BWSL::MiddlewareCompilationResult result = metalMiddleware_->CompileVariant(*coreVariant);
        if (!result.success) {
            return nullptr;
        }

        MetalCompiledVariant& metalVariant = metalVariants_[variantKey];
        metalVariant.variantKey = variantKey;
        metalVariant.vertexSource = result.vertexSource;
        metalVariant.fragmentSource = result.fragmentSource;
        metalVariant.functionName = "main";
        metalVariant.vertexLib = (__bridge_transfer id<MTLLibrary>)result.handle.vertexShader;
        metalVariant.fragmentLib = (__bridge_transfer id<MTLLibrary>)result.handle.fragmentShader;
        metalVariant.attributeMask = coreVariant->attributeMask;
        metalVariant.coreVariant = coreVariant;

        return &metalVariant;
    }
    
    // ========================================================================
    // Precompilation
    // ========================================================================
    
    void PrecompileCommonVariants() {
        BWSL::CompilationConfig config;
        config.targetBackend = BWSL::TargetBackend::Metal;
        config.vertexPulling.mode = BWSL::VertexInputMode::SeparateBuffers;
        
        coreService_.PrecompileDeclaredVariants(config);
    }
    
    // ========================================================================
    // Hot Reload
    // ========================================================================
    
    void HandleFileChange(const std::string& bwslPath) {
        coreService_.HandleFileChange(bwslPath);
    }
    
    // Callback when a pipeline is recompiled
    std::function<void(const std::string&, const std::string&, id<MTLLibrary>, id<MTLLibrary>)> 
        pipelineUpdateCallback;
    
    // Set the pipeline update callback
    void SetPipelineUpdateCallback(
        std::function<void(const std::string&, const std::string&, id<MTLLibrary>, id<MTLLibrary>)> callback) {
        pipelineUpdateCallback = std::move(callback);
    }
    
    // ========================================================================
    // Accessors
    // ========================================================================
    
    id<MTLDevice> GetDevice() const { return device_; }
    BWSL::BWSLCompilerServiceCore& GetCoreService() { return coreService_; }
    
private:
    id<MTLDevice> device_ = nil;
    BWSL::BWSLCompilerServiceCore coreService_;
    BWSL::ShaderMiddleware* metalMiddleware_ = nullptr;
    
    // Metal-specific variant cache
    std::unordered_map<std::string, MetalCompiledVariant> metalVariants_;
    
    static std::string MakeVariantKey(const std::string& pipeline, const std::string& pass, u32 mask) {
        char buffer[128];
        snprintf(buffer, sizeof(buffer), "%s.%s.0x%08X", pipeline.c_str(), pass.c_str(), mask);
        return buffer;
    }
    
    void HandlePipelineRecompiled(const std::string& pipelineName) {
        // Clear Metal cache for this pipeline
        for (auto it = metalVariants_.begin(); it != metalVariants_.end();) {
            if (it->second.variantKey.find(pipelineName) == 0) {
                // Release Metal resources
                it->second.vertexLib = nil;
                it->second.fragmentLib = nil;
                it = metalVariants_.erase(it);
            } else {
                ++it;
            }
        }
        
        // Notify listeners
        if (pipelineUpdateCallback) {
            // Note: Specific variant info would need to be tracked for full callback
        }
    }
};

#endif // __APPLE__
