#pragma once

#include "bwsl_middleware_interface.h"

#ifdef __APPLE__
#import <Metal/Metal.h>

namespace BWSL {

// ============================================================================
// Metal Middleware Configuration
// ============================================================================

struct MetalMiddlewareConfig : public MiddlewareConfig {
    // Metal-specific options
    bool fastMathEnabled = true;
    bool preserveInvariance = false;  // Enable in debug builds
    
    // Metal language version (e.g., 20400 for Metal 2.4)
    u32 metalLanguageVersion = 20400;
    
    // Argument buffer tier (0 = none, 1 = tier 1, 2 = tier 2)
    u32 argumentBufferTier = 0;
    
    // Force buffer bindings to specific indices
    bool useExplicitBufferBindings = true;
};

// ============================================================================
// Metal Middleware Platform Data
// ============================================================================

struct MetalMiddlewareData {
    id<MTLDevice> device;
    MetalMiddlewareConfig config;
};

// ============================================================================
// Metal Middleware Factory
// ============================================================================

// Create a Metal shader middleware instance
// Returns nullptr if Metal is not available
// Caller must call middleware->Destroy() when done
ShaderMiddleware* CreateMetalMiddleware(id<MTLDevice> device);

// ============================================================================
// Metal-Specific Helper Functions
// ============================================================================

// Get the Metal device from a middleware instance
inline id<MTLDevice> GetMetalDevice(ShaderMiddleware* middleware) {
    if (!middleware || middleware->platform != TargetBackend::Metal) {
        return nil;
    }
    return middleware->GetPlatformData<MetalMiddlewareData>()->device;
}

// Compile MSL source directly (for debugging/testing)
id<MTLLibrary> CompileMSLSource(
    ShaderMiddleware* middleware,
    const std::string& source,
    const MetalMiddlewareConfig& config,
    std::string* outError = nullptr
);

// Compile with Metal-specific config
MiddlewareCompilationResult CompileVariantMetal(
    ShaderMiddleware* middleware,
    const CompiledVariant& variant,
    const MetalMiddlewareConfig& config
);

} // namespace BWSL

#endif // __APPLE__
