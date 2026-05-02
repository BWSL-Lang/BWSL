#include "bwsl_metal_middleware.h"

#ifdef __APPLE__

// Include spirv-cross for SPIR-V to MSL conversion
// Undefine macros that conflict with spirv_cross's member variable names
#ifdef f32
#define BWSL_HAD_F32 1
#undef f32
#endif
#ifdef f64
#define BWSL_HAD_F64 1
#undef f64
#endif

#if __has_include(<spirv_cross/spirv_msl.hpp>)
#include <spirv_cross/spirv_msl.hpp>
#define HAS_SPIRV_CROSS 1
#else
#define HAS_SPIRV_CROSS 0
#endif

// Restore the macros after spirv_cross
#ifdef BWSL_HAD_F32
#define f32 float
#undef BWSL_HAD_F32
#endif
#ifdef BWSL_HAD_F64
#define f64 double
#undef BWSL_HAD_F64
#endif

#import <Foundation/Foundation.h>
#include <cstdlib>

namespace BWSL {

// ============================================================================
// Forward Declarations
// ============================================================================

static std::string ConvertSPIRVToMSL(
    MetalMiddlewareData* data,
    const std::vector<u32>& spirv,
    ShaderStage stage,
    std::vector<MiddlewareCompilationResult::BindingRemap>* outRemaps
);

static id<MTLLibrary> CompileMSLInternal(
    id<MTLDevice> device,
    const std::string& mslSource,
    const MetalMiddlewareConfig& config,
    std::string* outError
);

// ============================================================================
// VTable Function Implementations
// ============================================================================

static MiddlewareCompilationResult Metal_CompileVariant(
    ShaderMiddleware* ctx,
    const CompiledVariant& variant,
    const MiddlewareConfig& config
) {
    MetalMiddlewareData* data = ctx->GetPlatformData<MetalMiddlewareData>();
    
    MetalMiddlewareConfig metalConfig = data->config;
    metalConfig.preserveSourceCode = config.preserveSourceCode;
    metalConfig.debugMode = config.debugMode;
    metalConfig.optimizationLevel = config.optimizationLevel;
    metalConfig.vertexEntryPoint = config.vertexEntryPoint;
    metalConfig.fragmentEntryPoint = config.fragmentEntryPoint;
    
    return CompileVariantMetal(ctx, variant, metalConfig);
}

static MiddlewareCompilationResult Metal_CompileSPIRV(
    ShaderMiddleware* ctx,
    const std::vector<u32>& spirv,
    ShaderStage stage,
    const MiddlewareConfig& config
) {
    MetalMiddlewareData* data = ctx->GetPlatformData<MetalMiddlewareData>();
    MiddlewareCompilationResult result;
    result.success = false;
    
    if (!data->device) {
        result.errorMessage = "Metal device not initialized";
        return result;
    }
    
    MetalMiddlewareConfig metalConfig = data->config;
    metalConfig.preserveSourceCode = config.preserveSourceCode;
    metalConfig.debugMode = config.debugMode;
    metalConfig.optimizationLevel = config.optimizationLevel;
    
    std::string msl = ConvertSPIRVToMSL(data, spirv, stage, &result.bindingRemaps);
    
    if (msl.empty()) {
        result.errorMessage = "Failed to convert SPIR-V to MSL";
        return result;
    }
    
    if (config.preserveSourceCode) {
        if (stage == ShaderStage::Vertex) {
            result.vertexSource = msl;
        } else if (stage == ShaderStage::Fragment) {
            result.fragmentSource = msl;
        }
    }
    
    std::string error;
    id<MTLLibrary> lib = CompileMSLInternal(data->device, msl, metalConfig, &error);
    
    if (!lib) {
        result.errorMessage = "MSL compilation failed: " + error;
        return result;
    }
    
    if (stage == ShaderStage::Vertex) {
        result.handle.vertexShader = (__bridge_retained void*)lib;
    } else if (stage == ShaderStage::Fragment) {
        result.handle.fragmentShader = (__bridge_retained void*)lib;
    } else if (stage == ShaderStage::Compute) {
        result.handle.computeShader = (__bridge_retained void*)lib;
    }
    
    result.handle.platform = TargetBackend::Metal;
    result.success = true;
    
    return result;
}

static void Metal_ReleaseShader(ShaderMiddleware* ctx, PlatformShaderHandle& handle) {
    (void)ctx;
    
    if (handle.platform != TargetBackend::Metal) {
        return;
    }
    
    if (handle.vertexShader) {
        id<MTLLibrary> lib = (__bridge_transfer id<MTLLibrary>)handle.vertexShader;
        lib = nil;
        handle.vertexShader = nullptr;
    }
    
    if (handle.fragmentShader) {
        id<MTLLibrary> lib = (__bridge_transfer id<MTLLibrary>)handle.fragmentShader;
        lib = nil;
        handle.fragmentShader = nullptr;
    }
    
    if (handle.computeShader) {
        id<MTLLibrary> lib = (__bridge_transfer id<MTLLibrary>)handle.computeShader;
        lib = nil;
        handle.computeShader = nullptr;
    }
}

static std::string Metal_ConvertToSource(
    ShaderMiddleware* ctx,
    const std::vector<u32>& spirv,
    ShaderStage stage
) {
    MetalMiddlewareData* data = ctx->GetPlatformData<MetalMiddlewareData>();
    return ConvertSPIRVToMSL(data, spirv, stage, nullptr);
}

static bool Metal_SupportsFeature(ShaderMiddleware* ctx, const char* feature) {
    MetalMiddlewareData* data = ctx->GetPlatformData<MetalMiddlewareData>();
    
    if (!data->device) return false;
    
    if (strcmp(feature, "argument_buffers") == 0) {
        return [data->device supportsFamily:MTLGPUFamilyApple4];
    }
    if (strcmp(feature, "simd_group") == 0) {
        return [data->device supportsFamily:MTLGPUFamilyApple4];
    }
    if (strcmp(feature, "raytracing") == 0) {
        if (@available(macOS 11.0, iOS 14.0, *)) {
            return [data->device supportsRaytracing];
        }
        return false;
    }
    
    return false;
}

static u32 Metal_GetMaxShaderVersion(ShaderMiddleware* ctx) {
    (void)ctx;
    return 20400;  // Metal 2.4
}

static void Metal_Destroy(ShaderMiddleware* ctx) {
    MetalMiddlewareData* data = ctx->GetPlatformData<MetalMiddlewareData>();
    data->device = nil;
    
    // Free the entire allocation (middleware struct + platform data)
    free(ctx);
}

// ============================================================================
// VTable Definition
// ============================================================================

static const ShaderMiddlewareVTable g_MetalVTable = {
    .compileVariant = Metal_CompileVariant,
    .compileSPIRV = Metal_CompileSPIRV,
    .releaseShader = Metal_ReleaseShader,
    .convertToSource = Metal_ConvertToSource,
    .supportsFeature = Metal_SupportsFeature,
    .getMaxShaderVersion = Metal_GetMaxShaderVersion,
    .destroy = Metal_Destroy,
};

// ============================================================================
// Factory Implementation
// ============================================================================

ShaderMiddleware* CreateMetalMiddleware(id<MTLDevice> device) {
    if (!device) {
        return nullptr;
    }
    
    // Allocate middleware struct + platform data in one block
    size_t totalSize = sizeof(ShaderMiddleware) + sizeof(MetalMiddlewareData);
    void* mem = malloc(totalSize);
    if (!mem) {
        return nullptr;
    }
    
    // Initialize middleware header
    ShaderMiddleware* middleware = new (mem) ShaderMiddleware();
    middleware->vtable = &g_MetalVTable;
    middleware->platform = TargetBackend::Metal;
    middleware->platformName = "Metal";
    
    // Initialize platform data
    MetalMiddlewareData* data = middleware->GetPlatformData<MetalMiddlewareData>();
    new (data) MetalMiddlewareData();
    data->device = device;
    
    return middleware;
}

ShaderMiddleware* CreateMiddleware(TargetBackend platform) {
    switch (platform) {
        case TargetBackend::Metal:
            // Need to call CreateMetalMiddleware with a device
            return nullptr;  // Use CreateMetalMiddleware instead
        default:
            return nullptr;
    }
}

std::vector<TargetBackend> GetAvailableMiddlewarePlatforms() {
    return { TargetBackend::Metal };
}

// ============================================================================
// Public Helper Functions
// ============================================================================

id<MTLLibrary> CompileMSLSource(
    ShaderMiddleware* middleware,
    const std::string& source,
    const MetalMiddlewareConfig& config,
    std::string* outError
) {
    if (!middleware || middleware->platform != TargetBackend::Metal) {
        if (outError) *outError = "Invalid middleware";
        return nil;
    }
    
    MetalMiddlewareData* data = middleware->GetPlatformData<MetalMiddlewareData>();
    return CompileMSLInternal(data->device, source, config, outError);
}

MiddlewareCompilationResult CompileVariantMetal(
    ShaderMiddleware* middleware,
    const CompiledVariant& variant,
    const MetalMiddlewareConfig& config
) {
    MiddlewareCompilationResult result;
    result.success = false;
    
    if (!middleware || middleware->platform != TargetBackend::Metal) {
        result.errorMessage = "Invalid middleware";
        return result;
    }
    
    MetalMiddlewareData* data = middleware->GetPlatformData<MetalMiddlewareData>();
    
    if (!data->device) {
        result.errorMessage = "Metal device not initialized";
        return result;
    }
    
    // Compile vertex shader
    id<MTLLibrary> vertexLib = nil;
    if (!variant.vertexSpirv.empty()) {
        std::string vertexMSL = ConvertSPIRVToMSL(data, variant.vertexSpirv, 
                                                   ShaderStage::Vertex, &result.bindingRemaps);
        
        if (vertexMSL.empty()) {
            result.errorMessage = "Failed to convert vertex SPIR-V to MSL";
            return result;
        }
        
        if (config.preserveSourceCode) {
            result.vertexSource = vertexMSL;
        }
        
        std::string error;
        vertexLib = CompileMSLInternal(data->device, vertexMSL, config, &error);
        
        if (!vertexLib) {
            result.errorMessage = "Vertex shader compilation failed: " + error;
            return result;
        }
    }
    
    // Compile fragment shader
    id<MTLLibrary> fragmentLib = nil;
    if (!variant.fragmentSpirv.empty()) {
        std::string fragmentMSL = ConvertSPIRVToMSL(data, variant.fragmentSpirv,
                                                     ShaderStage::Fragment, nullptr);
        
        if (fragmentMSL.empty()) {
            result.errorMessage = "Failed to convert fragment SPIR-V to MSL";
            if (vertexLib) {
                vertexLib = nil;
            }
            return result;
        }
        
        if (config.preserveSourceCode) {
            result.fragmentSource = fragmentMSL;
        }
        
        std::string error;
        fragmentLib = CompileMSLInternal(data->device, fragmentMSL, config, &error);
        
        if (!fragmentLib) {
            result.errorMessage = "Fragment shader compilation failed: " + error;
            if (vertexLib) {
                vertexLib = nil;
            }
            return result;
        }
    }
    
    // Store handles
    result.handle.vertexShader = (__bridge_retained void*)vertexLib;
    result.handle.fragmentShader = (__bridge_retained void*)fragmentLib;
    result.handle.platform = TargetBackend::Metal;
    result.success = true;
    
    return result;
}

// ============================================================================
// Internal Helper Functions
// ============================================================================

static std::string ConvertSPIRVToMSL(
    MetalMiddlewareData* data,
    const std::vector<u32>& spirv,
    ShaderStage stage,
    std::vector<MiddlewareCompilationResult::BindingRemap>* outRemaps
) {
#if HAS_SPIRV_CROSS
    try {
        spirv_cross::CompilerMSL msl(spirv);
        
        // Configure MSL options
        spirv_cross::CompilerMSL::Options mslOptions;
        mslOptions.platform = spirv_cross::CompilerMSL::Options::macOS;
        mslOptions.msl_version = data->config.metalLanguageVersion;
        mslOptions.enable_decoration_binding = data->config.useExplicitBufferBindings;
        mslOptions.argument_buffers = data->config.argumentBufferTier > 0;
        
        // Set vertex attribute pulling options
        if (stage == ShaderStage::Vertex) {
            mslOptions.enable_decoration_binding = true;
        }
        
        msl.set_msl_options(mslOptions);
        
        // Configure resource bindings if needed
        if (data->config.useExplicitBufferBindings) {
            auto resources = msl.get_shader_resources();
            
            // Remap uniform buffers
            for (const auto& ubo : resources.uniform_buffers) {
                auto binding = msl.get_decoration(ubo.id, spv::DecorationBinding);
                auto set = msl.get_decoration(ubo.id, spv::DecorationDescriptorSet);
                
                spirv_cross::MSLResourceBinding resourceBinding;
                resourceBinding.stage = (stage == ShaderStage::Vertex) ? spv::ExecutionModelVertex :
                                       (stage == ShaderStage::Fragment) ? spv::ExecutionModelFragment :
                                       spv::ExecutionModelGLCompute;
                resourceBinding.desc_set = set;
                resourceBinding.binding = binding;
                resourceBinding.msl_buffer = binding;
                
                msl.add_msl_resource_binding(resourceBinding);
                
                if (outRemaps) {
                    MiddlewareCompilationResult::BindingRemap remap;
                    remap.originalSet = set;
                    remap.originalBinding = binding;
                    remap.newBinding = binding;
                    remap.name = ubo.name;
                    outRemaps->push_back(remap);
                }
            }
            
            // Remap storage buffers (for vertex pulling)
            for (const auto& ssbo : resources.storage_buffers) {
                auto binding = msl.get_decoration(ssbo.id, spv::DecorationBinding);
                auto set = msl.get_decoration(ssbo.id, spv::DecorationDescriptorSet);
                
                spirv_cross::MSLResourceBinding resourceBinding;
                resourceBinding.stage = (stage == ShaderStage::Vertex) ? spv::ExecutionModelVertex :
                                       (stage == ShaderStage::Fragment) ? spv::ExecutionModelFragment :
                                       spv::ExecutionModelGLCompute;
                resourceBinding.desc_set = set;
                resourceBinding.binding = binding;
                resourceBinding.msl_buffer = binding;
                
                msl.add_msl_resource_binding(resourceBinding);
                
                if (outRemaps) {
                    MiddlewareCompilationResult::BindingRemap remap;
                    remap.originalSet = set;
                    remap.originalBinding = binding;
                    remap.newBinding = binding;
                    remap.name = ssbo.name;
                    outRemaps->push_back(remap);
                }
            }
        }
        
        return msl.compile();
    }
    catch (const spirv_cross::CompilerError& e) {
        return "";
    }
#else
    (void)data;
    (void)outRemaps;
    
    // Fallback: Use command-line spirv-cross tool
    std::string tempSpvPath = "/tmp/bwsl_temp_shader.spv";
    std::string tempMslPath = "/tmp/bwsl_temp_shader.msl";
    
    FILE* spvFile = fopen(tempSpvPath.c_str(), "wb");
    if (!spvFile) {
        return "";
    }
    fwrite(spirv.data(), sizeof(u32), spirv.size(), spvFile);
    fclose(spvFile);
    
    // Run spirv-cross
    std::string cmd = "spirv-cross --msl --msl-version 20400 " + tempSpvPath + " -o " + tempMslPath + " 2>&1";
    int result = system(cmd.c_str());
    
    if (result != 0) {
        return "";
    }
    
    // Read MSL output
    std::ifstream mslFile(tempMslPath);
    if (!mslFile.is_open()) {
        return "";
    }
    
    std::string msl((std::istreambuf_iterator<char>(mslFile)),
                    std::istreambuf_iterator<char>());
    
    // Cleanup temp files
    remove(tempSpvPath.c_str());
    remove(tempMslPath.c_str());
    
    return msl;
#endif
}

static id<MTLLibrary> CompileMSLInternal(
    id<MTLDevice> device,
    const std::string& mslSource,
    const MetalMiddlewareConfig& config,
    std::string* outError
) {
    if (!device) {
        if (outError) *outError = "Metal device not initialized";
        return nil;
    }
    
    NSError* error = nil;
    MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
    options.fastMathEnabled = config.fastMathEnabled;
    
    // Set language version
    switch (config.metalLanguageVersion) {
        case 20400: options.languageVersion = MTLLanguageVersion2_4; break;
        case 20300: options.languageVersion = MTLLanguageVersion2_3; break;
        case 20200: options.languageVersion = MTLLanguageVersion2_2; break;
        case 20100: options.languageVersion = MTLLanguageVersion2_1; break;
        default: options.languageVersion = MTLLanguageVersion2_4; break;
    }
    
#ifdef DEBUG
    if (config.preserveInvariance || config.debugMode) {
        options.preserveInvariance = YES;
    }
#endif
    
    NSString* source = [NSString stringWithUTF8String:mslSource.c_str()];
    id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
    
    if (error && outError) {
        *outError = [[error localizedDescription] UTF8String];
    }
    
    return library;
}

} // namespace BWSL

#else // !__APPLE__

namespace BWSL {

ShaderMiddleware* CreateMiddleware(TargetBackend platform) {
    (void)platform;
    return nullptr;
}

std::vector<TargetBackend> GetAvailableMiddlewarePlatforms() {
    return {};
}

} // namespace BWSL

#endif // __APPLE__
