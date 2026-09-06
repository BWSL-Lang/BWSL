#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cmath>
#include <cstdio>

// Render one pixel with independently bound nearest/linear samplers. A
// collapsed sampler identity returns zero instead of the expected -0.25.
int main(int argc, const char** argv) {
    @autoreleasepool {
        if (argc != 3) return 2;
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) { puts("SKIP: no Metal device"); return 77; }
        NSError* error = nil;
        NSString* vertexSource = [NSString stringWithContentsOfFile:@(argv[1]) encoding:NSUTF8StringEncoding error:&error];
        NSString* fragmentSource = [NSString stringWithContentsOfFile:@(argv[2]) encoding:NSUTF8StringEncoding error:&error];
        id<MTLLibrary> vertexLibrary = [device newLibraryWithSource:vertexSource options:nil error:&error];
        if (!vertexLibrary) { fprintf(stderr, "%s\n", error.description.UTF8String); return 1; }
        id<MTLLibrary> fragmentLibrary = [device newLibraryWithSource:fragmentSource options:nil error:&error];
        if (!fragmentLibrary) { fprintf(stderr, "%s\n", error.description.UTF8String); return 1; }
        MTLRenderPipelineDescriptor* pipelineDesc = [MTLRenderPipelineDescriptor new];
        pipelineDesc.vertexFunction = [vertexLibrary newFunctionWithName:@"main0"];
        pipelineDesc.fragmentFunction = [fragmentLibrary newFunctionWithName:@"main0"];
        pipelineDesc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
        id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:pipelineDesc error:&error];
        if (!pipeline) { fprintf(stderr, "%s\n", error.description.UTF8String); return 1; }
        MTLTextureDescriptor* inputDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:2 height:1 mipmapped:NO];
        inputDesc.usage = MTLTextureUsageShaderRead;
        inputDesc.storageMode = MTLStorageModeShared;
        id<MTLTexture> input = [device newTextureWithDescriptor:inputDesc];
        const unsigned char pixels[] = {0,0,0,255, 255,255,255,255};
        [input replaceRegion:MTLRegionMake2D(0,0,2,1) mipmapLevel:0 withBytes:pixels bytesPerRow:8];
        MTLSamplerDescriptor* samplerDesc = [MTLSamplerDescriptor new];
        samplerDesc.sAddressMode = MTLSamplerAddressModeClampToEdge;
        samplerDesc.tAddressMode = MTLSamplerAddressModeClampToEdge;
        samplerDesc.minFilter = samplerDesc.magFilter = MTLSamplerMinMagFilterNearest;
        id<MTLSamplerState> nearest = [device newSamplerStateWithDescriptor:samplerDesc];
        samplerDesc.minFilter = samplerDesc.magFilter = MTLSamplerMinMagFilterLinear;
        id<MTLSamplerState> linear = [device newSamplerStateWithDescriptor:samplerDesc];
        MTLTextureDescriptor* outputDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:1 height:1 mipmapped:NO];
        outputDesc.usage = MTLTextureUsageRenderTarget;
        outputDesc.storageMode = MTLStorageModeShared;
        id<MTLTexture> output = [device newTextureWithDescriptor:outputDesc];
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = output;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
        [encoder setRenderPipelineState:pipeline];
        [encoder setFragmentTexture:input atIndex:0];
        [encoder setFragmentSamplerState:nearest atIndex:0];
        [encoder setFragmentSamplerState:linear atIndex:1];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding];
        [command commit];
        [command waitUntilCompleted];
        if (command.error) { fprintf(stderr, "%s\n", command.error.description.UTF8String); return 1; }
        float result[4] = {};
        [output getBytes:result bytesPerRow:sizeof(result) fromRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0];
        printf("Metal sampler result: %.8f %.8f %.8f %.8f\n", result[0],result[1],result[2],result[3]);
        for (int i = 0; i < 3; ++i) if (std::fabs(result[i] + 0.25f) > 0.002f) return 1;
        return std::fabs(result[3]) < 0.00001f ? 0 : 1;
    }
}
