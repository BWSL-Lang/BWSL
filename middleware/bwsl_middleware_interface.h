#pragma once

#include "../bwsl_compiler_service_core.h"
#include <string>
#include <vector>
#include <memory>

namespace BWSL {

// ============================================================================
// Platform Shader Handle
// ============================================================================

// Opaque handle to a compiled platform-specific shader
// The actual type depends on the backend (MTLLibrary*, VkShaderModule, etc.)
struct PlatformShaderHandle {
    void* vertexShader = nullptr;
    void* fragmentShader = nullptr;
    void* computeShader = nullptr;
    
    // Platform identifier for type safety
    TargetBackend platform = TargetBackend::SPIRV;
    
    bool IsValid() const {
        return vertexShader != nullptr || fragmentShader != nullptr || computeShader != nullptr;
    }
};

// ============================================================================
// Compilation Result
// ============================================================================

struct MiddlewareCompilationResult {
    bool success = false;
    std::string errorMessage;
    
    // Compiled handles
    PlatformShaderHandle handle;
    
    // Generated source code (if requested)
    std::string vertexSource;
    std::string fragmentSource;
    
    // Reflection info (remapped bindings after spirv-cross)
    struct BindingRemap {
        u32 originalSet;
        u32 originalBinding;
        u32 newBinding;       // Platform-specific binding (e.g., Metal buffer index)
        std::string name;
    };
    std::vector<BindingRemap> bindingRemaps;
};

// ============================================================================
// Middleware Configuration
// ============================================================================

struct MiddlewareConfig {
    // Whether to keep generated source code
    bool preserveSourceCode = true;
    
    // Whether to emit debug info
    bool debugMode = false;
    
    // Optimization level (0 = none, 1 = basic, 2 = aggressive)
    u32 optimizationLevel = 1;
    
    // Entry point names
    std::string vertexEntryPoint = "main";
    std::string fragmentEntryPoint = "main";
    std::string computeEntryPoint = "main";
};

// ============================================================================
// Shader Middleware (Function Pointer Table - No Virtual Overhead)
// ============================================================================

// Forward declaration for the middleware context
struct ShaderMiddleware;

// Function pointer types for middleware operations
using FnCompileVariant = MiddlewareCompilationResult (*)(
    ShaderMiddleware* ctx,
    const CompiledVariant& variant,
    const MiddlewareConfig& config
);

using FnCompileSPIRV = MiddlewareCompilationResult (*)(
    ShaderMiddleware* ctx,
    const std::vector<u32>& spirv,
    ShaderStage stage,
    const MiddlewareConfig& config
);

using FnReleaseShader = void (*)(
    ShaderMiddleware* ctx,
    PlatformShaderHandle& handle
);

using FnConvertToSource = std::string (*)(
    ShaderMiddleware* ctx,
    const std::vector<u32>& spirv,
    ShaderStage stage
);

using FnSupportsFeature = bool (*)(
    ShaderMiddleware* ctx,
    const char* feature
);

using FnGetMaxShaderVersion = u32 (*)(ShaderMiddleware* ctx);

using FnDestroy = void (*)(ShaderMiddleware* ctx);

// Function pointer table for middleware operations
struct ShaderMiddlewareVTable {
    FnCompileVariant compileVariant;
    FnCompileSPIRV compileSPIRV;
    FnReleaseShader releaseShader;
    FnConvertToSource convertToSource;
    FnSupportsFeature supportsFeature;
    FnGetMaxShaderVersion getMaxShaderVersion;
    FnDestroy destroy;
};

// Middleware context - holds vtable + platform-specific data
struct ShaderMiddleware {
    const ShaderMiddlewareVTable* vtable;
    TargetBackend platform;
    const char* platformName;
    
    // Platform-specific data follows (allocated by platform implementation)
    // Use GetPlatformData<T>() to access
    
    template<typename T>
    T* GetPlatformData() {
        return reinterpret_cast<T*>(this + 1);
    }
    
    template<typename T>
    const T* GetPlatformData() const {
        return reinterpret_cast<const T*>(this + 1);
    }
    
    // ========================================================================
    // Convenience wrappers (inline, no overhead)
    // ========================================================================
    
    MiddlewareCompilationResult CompileVariant(
        const CompiledVariant& variant,
        const MiddlewareConfig& config = MiddlewareConfig()
    ) {
        return vtable->compileVariant(this, variant, config);
    }
    
    MiddlewareCompilationResult CompileSPIRV(
        const std::vector<u32>& spirv,
        ShaderStage stage,
        const MiddlewareConfig& config = MiddlewareConfig()
    ) {
        return vtable->compileSPIRV(this, spirv, stage, config);
    }
    
    void ReleaseShader(PlatformShaderHandle& handle) {
        vtable->releaseShader(this, handle);
    }
    
    std::string ConvertToSource(const std::vector<u32>& spirv, ShaderStage stage) {
        return vtable->convertToSource(this, spirv, stage);
    }
    
    bool SupportsFeature(const char* feature) const {
        return vtable->supportsFeature(const_cast<ShaderMiddleware*>(this), feature);
    }
    
    u32 GetMaxShaderVersion() const {
        return vtable->getMaxShaderVersion(const_cast<ShaderMiddleware*>(this));
    }
    
    TargetBackend GetPlatform() const { return platform; }
    const char* GetPlatformName() const { return platformName; }
    
    void Destroy() {
        if (vtable->destroy) {
            vtable->destroy(this);
        }
    }
};

// ============================================================================
// Middleware Factory
// ============================================================================

// Create middleware for a specific platform
// Returns nullptr if platform is not supported on this build
// Caller owns the returned pointer and must call Destroy() when done
ShaderMiddleware* CreateMiddleware(TargetBackend platform);

// Get list of available middleware platforms on this build
std::vector<TargetBackend> GetAvailableMiddlewarePlatforms();

// ============================================================================
// RAII Wrapper (Optional convenience)
// ============================================================================

struct MiddlewareHandle {
    ShaderMiddleware* middleware = nullptr;
    
    MiddlewareHandle() = default;
    explicit MiddlewareHandle(ShaderMiddleware* m) : middleware(m) {}
    ~MiddlewareHandle() { if (middleware) middleware->Destroy(); }
    
    // Move only
    MiddlewareHandle(const MiddlewareHandle&) = delete;
    MiddlewareHandle& operator=(const MiddlewareHandle&) = delete;
    MiddlewareHandle(MiddlewareHandle&& other) noexcept : middleware(other.middleware) { other.middleware = nullptr; }
    MiddlewareHandle& operator=(MiddlewareHandle&& other) noexcept {
        if (this != &other) {
            if (middleware) middleware->Destroy();
            middleware = other.middleware;
            other.middleware = nullptr;
        }
        return *this;
    }
    
    ShaderMiddleware* operator->() { return middleware; }
    const ShaderMiddleware* operator->() const { return middleware; }
    ShaderMiddleware& operator*() { return *middleware; }
    const ShaderMiddleware& operator*() const { return *middleware; }
    explicit operator bool() const { return middleware != nullptr; }
};

} // namespace BWSL
