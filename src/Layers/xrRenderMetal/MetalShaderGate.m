#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#define IR_PRIVATE_IMPLEMENTATION
#include <metal_irconverter_runtime/metal_irconverter_runtime.h>

#include <stdio.h>

static id<MTLFunction> loadSingleFunction(
    id<MTLDevice> device,
    NSString* path,
    NSError** outError)
{
    NSURL* url = [NSURL fileURLWithPath:path];
    id<MTLLibrary> library = [device newLibraryWithURL:url error:outError];
    if (!library)
        return nil;

    if (library.functionNames.count != 1)
    {
        if (outError)
        {
            *outError = [NSError errorWithDomain:@"XRayMetalShaderGate"
                                             code:1
                                         userInfo:@{
                                             NSLocalizedDescriptionKey:
                                                 [NSString stringWithFormat:
                                                     @"Expected one function in %@, found %lu",
                                                     path,
                                                     (unsigned long)library.functionNames.count]
                                         }];
        }
        return nil;
    }

    // MSC owns the generated function name. Querying the library keeps this
    // gate valid across converter releases instead of assuming HLSL's `main`.
    return [library newFunctionWithName:library.functionNames.firstObject];
}

static BOOL createShadowPipeline(
    id<MTLDevice> device,
    id<MTLFunction> vertexFunction,
    NSError** outError)
{
    MTLVertexDescriptor* vertexDescriptor = [[MTLVertexDescriptor alloc] init];
    vertexDescriptor.attributes[kIRStageInAttributeStartIndex].format = MTLVertexFormatFloat4;
    vertexDescriptor.attributes[kIRStageInAttributeStartIndex].offset = 0;
    vertexDescriptor.attributes[kIRStageInAttributeStartIndex].bufferIndex = kIRVertexBufferBindPoint;
    vertexDescriptor.layouts[kIRVertexBufferBindPoint].stride = 16;
    vertexDescriptor.layouts[kIRVertexBufferBindPoint].stepFunction = MTLVertexStepFunctionPerVertex;

    MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.label = @"R4 shadow_direct_tree representative PSO";
    descriptor.vertexFunction = vertexFunction;
    descriptor.vertexDescriptor = vertexDescriptor;
    descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    descriptor.rasterSampleCount = 1;

    id<MTLRenderPipelineState> pipeline =
        [device newRenderPipelineStateWithDescriptor:descriptor error:outError];
    return pipeline != nil;
}

static BOOL createSkinnedGBufferPipeline(
    id<MTLDevice> device,
    id<MTLFunction> vertexFunction,
    id<MTLFunction> fragmentFunction,
    NSError** outError)
{
    // Exact one-weight layout from FSkinned.cpp::dwDecl_01W (36 bytes).
    MTLVertexDescriptor* vertexDescriptor = [[MTLVertexDescriptor alloc] init];
    const NSUInteger attributeBase = kIRStageInAttributeStartIndex;
    const NSUInteger bufferIndex = kIRVertexBufferBindPoint;
    vertexDescriptor.attributes[attributeBase + 0].format = MTLVertexFormatFloat4;
    vertexDescriptor.attributes[attributeBase + 0].offset = 0;
    vertexDescriptor.attributes[attributeBase + 0].bufferIndex = bufferIndex;
    vertexDescriptor.attributes[attributeBase + 1].format = MTLVertexFormatUChar4Normalized;
    vertexDescriptor.attributes[attributeBase + 1].offset = 16;
    vertexDescriptor.attributes[attributeBase + 1].bufferIndex = bufferIndex;
    vertexDescriptor.attributes[attributeBase + 2].format = MTLVertexFormatUChar4Normalized;
    vertexDescriptor.attributes[attributeBase + 2].offset = 20;
    vertexDescriptor.attributes[attributeBase + 2].bufferIndex = bufferIndex;
    vertexDescriptor.attributes[attributeBase + 3].format = MTLVertexFormatUChar4Normalized;
    vertexDescriptor.attributes[attributeBase + 3].offset = 24;
    vertexDescriptor.attributes[attributeBase + 3].bufferIndex = bufferIndex;
    vertexDescriptor.attributes[attributeBase + 4].format = MTLVertexFormatFloat2;
    vertexDescriptor.attributes[attributeBase + 4].offset = 28;
    vertexDescriptor.attributes[attributeBase + 4].bufferIndex = bufferIndex;
    vertexDescriptor.layouts[bufferIndex].stride = 36;
    vertexDescriptor.layouts[bufferIndex].stepFunction = MTLVertexStepFunctionPerVertex;

    MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.label = @"R4 deffer_model_bump SKIN_0 G-buffer representative PSO";
    descriptor.vertexFunction = vertexFunction;
    descriptor.fragmentFunction = fragmentFunction;
    descriptor.vertexDescriptor = vertexDescriptor;
    // Exact non-MSAA scene MRT classes from r4_rendertarget.cpp. The albedo
    // slot uses the non-HDR, non-albedo_wo A8R8G8B8 branch for this smoke.
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
    descriptor.colorAttachments[1].pixelFormat = MTLPixelFormatRGBA8Unorm;
    descriptor.colorAttachments[2].pixelFormat = MTLPixelFormatRGBA8Unorm;
    descriptor.colorAttachments[3].pixelFormat = MTLPixelFormatRGBA16Float;
    descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    descriptor.rasterSampleCount = 1;

    id<MTLRenderPipelineState> pipeline =
        [device newRenderPipelineStateWithDescriptor:descriptor error:outError];
    return pipeline != nil;
}

int main(int argc, char** argv)
{
    @autoreleasepool
    {
        if (argc < 2)
        {
            fprintf(stderr, "usage: AnomalyMetalShaderGate shader.metallib [...]\n");
            return 2;
        }

        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device)
        {
            fprintf(stderr, "METAL_SHADER_GATE_ERROR no Metal device\n");
            return 1;
        }

        id<MTLFunction> shadowFunction = nil;
        id<MTLFunction> skinnedModelFunction = nil;
        id<MTLFunction> gBufferFragmentFunction = nil;
        NSUInteger loadedFunctions = 0;

        for (int index = 1; index < argc; ++index)
        {
            NSString* path = [NSString stringWithUTF8String:argv[index]];
            NSError* error = nil;
            id<MTLFunction> function = loadSingleFunction(device, path, &error);
            if (!function)
            {
                fprintf(stderr, "METAL_SHADER_GATE_ERROR load path=\"%s\" error=\"%s\"\n",
                    argv[index], error.localizedDescription.UTF8String);
                return 1;
            }

            ++loadedFunctions;
            printf("METAL_FUNCTION_READY path=\"%s\" function=\"%s\" type=%ld\n",
                argv[index], function.name.UTF8String, (long)function.functionType);

            if ([path.lastPathComponent isEqualToString:@"shadow_direct_tree.vs.metallib"])
                shadowFunction = function;
            else if ([path.lastPathComponent isEqualToString:@"deffer_model_bump.vs.metallib"])
                skinnedModelFunction = function;
            else if ([path.lastPathComponent isEqualToString:@"deffer_base_bump.ps.metallib"])
                gBufferFragmentFunction = function;
        }

        if (!shadowFunction || !skinnedModelFunction || !gBufferFragmentFunction)
        {
            fprintf(stderr,
                "METAL_SHADER_GATE_ERROR missing required shadow or G-buffer metallib\n");
            return 1;
        }

        NSError* pipelineError = nil;
        if (!createShadowPipeline(device, shadowFunction, &pipelineError))
        {
            fprintf(stderr, "METAL_SHADER_GATE_ERROR shadow PSO error=\"%s\"\n",
                pipelineError.localizedDescription.UTF8String);
            return 1;
        }

        printf("METAL_SHADOW_PSO_READY depth=Depth32Float vertex_attribute=%llu vertex_buffer=%llu\n",
            (unsigned long long)kIRStageInAttributeStartIndex,
            (unsigned long long)kIRVertexBufferBindPoint);

        pipelineError = nil;
        if (!createSkinnedGBufferPipeline(
                device, skinnedModelFunction, gBufferFragmentFunction, &pipelineError))
        {
            fprintf(stderr, "METAL_SHADER_GATE_ERROR G-buffer PSO error=\"%s\"\n",
                pipelineError.localizedDescription.UTF8String);
            return 1;
        }
        printf("METAL_GBUFFER_PSO_READY layout=dwDecl_01W stride=36 render_targets=4 "
               "formats=RGBA16Float,RGBA8Unorm,RGBA8Unorm,RGBA16Float depth=Depth32Float "
               "vertex_attribute_base=%llu vertex_buffer=%llu\n",
            (unsigned long long)kIRStageInAttributeStartIndex,
            (unsigned long long)kIRVertexBufferBindPoint);
        printf("METAL_SHADER_GATE_COMPLETE device=\"%s\" functions=%lu\n",
            device.name.UTF8String, (unsigned long)loadedFunctions);
        return 0;
    }
}
