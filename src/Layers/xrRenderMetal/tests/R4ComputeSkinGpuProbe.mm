#include "../R4ComputeSkinFoundation.h"
#include "../R4ComputeSkinMetal.h"
#include "../MetalR4DynamicWorldAdapter.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>

#ifndef R4_COMPUTE_SKIN_SOURCE_PATH
#define R4_COMPUTE_SKIN_SOURCE_PATH "src/Layers/xrRenderMetal/R4ComputeSkin.metal"
#endif

namespace
{
using Clock=std::chrono::steady_clock;
using Bones=std::vector<std::array<float,16>>;
constexpr std::size_t GuardBytes=64;

std::string nativeError(NSError* error)
{ return error?std::string(error.localizedDescription.UTF8String):"unknown Metal error"; }

class SkinGpu
{
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    id<MTLComputePipelineState> pipeline;
    id<MTLBuffer> input, bones, output, readback, trace;
    std::uint32_t vertices=0, influences=0;
    bool diagnostics=false;
public:
    bool initialize(std::string& error,bool traceDiagnostics=false)
    {
        diagnostics=traceDiagnostics;
        device=MTLCreateSystemDefaultDevice();
        if (!device) { error="Metal GPU unavailable"; return false; }
        queue=[device newCommandQueue];
        NSError* failure=nil;
        NSString* source=[NSString stringWithContentsOfFile:@R4_COMPUTE_SKIN_SOURCE_PATH
            encoding:NSUTF8StringEncoding error:&failure];
        if (!queue || !source) { error=nativeError(failure); return false; }
        MTLCompileOptions* options=xray::metal::R4ComputeSkinCompileOptions(diagnostics);
        id<MTLLibrary> library=[device newLibraryWithSource:source options:options error:&failure];
        id<MTLFunction> function=library?[library newFunctionWithName:@"r4_compute_skin"]:nil;
        if (!function) { error=nativeError(failure); return false; }
        pipeline=[device newComputePipelineStateWithFunction:function error:&failure];
        if (!pipeline) { error=nativeError(failure); return false; }
        std::printf("COMPUTE_SKIN_DEVICE name=\"%s\" math=safe_precise input_stride=88 output_stride=36\n",
            device.name.UTF8String);
        return true;
    }

    bool prepare(const std::vector<xray::metal::R4ComputeSkinVertex>& source,
        const Bones& transforms, std::uint32_t influenceCount, std::string& error)
    {
        if (source.empty() || source.size()>UINT32_MAX || transforms.empty() ||
            influenceCount<1 || influenceCount>4)
        { error="invalid compute probe job"; return false; }
        for (const auto& matrix:transforms)
            for (const float value:matrix)
                if (!std::isfinite(value)) { error="nonfinite compute probe pose"; return false; }
        for (const auto& vertex:source)
            for (std::uint32_t i=0;i<influenceCount;++i)
                if (vertex.bones[i]>=transforms.size())
                { error="uncaptured compute probe bone"; return false; }
        vertices=static_cast<std::uint32_t>(source.size()); influences=influenceCount;
        input=[device newBufferWithBytes:source.data() length:source.size()*sizeof(source.front())
            options:MTLResourceStorageModeShared];
        bones=[device newBufferWithBytes:transforms.data() length:transforms.size()*sizeof(transforms.front())
            options:MTLResourceStorageModeShared];
        const std::size_t bytes=std::size_t(vertices)*sizeof(R4DynamicWorldSkinnedVertex)+2*GuardBytes;
        output=[device newBufferWithLength:bytes options:MTLResourceStorageModePrivate];
        readback=[device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
        if (diagnostics) trace=[device newBufferWithLength:std::size_t(vertices)*7*sizeof(float)
            options:MTLResourceStorageModeShared];
        if (!input || !bones || !output || !readback || (diagnostics && !trace))
        { error="compute probe buffer allocation failed"; return false; }
        return true;
    }

    // Blocking completion belongs only to this offscreen oracle. Production
    // integration must encode into its acquired frame and defer readback.
    bool run(std::vector<R4DynamicWorldSkinnedVertex>& result, double& gpuMs,
        double& encodeMs, std::string& error, std::uint32_t repeats=1)
    {
        const auto start=Clock::now();
        id<MTLCommandBuffer> command=[queue commandBuffer];
        id<MTLBlitCommandEncoder> clear=[command blitCommandEncoder];
        if (!command || !clear) { error="compute probe command creation failed"; return false; }
        [clear fillBuffer:output range:NSMakeRange(0,output.length) value:0xa5];
        [clear endEncoding];
        id<MTLComputeCommandEncoder> compute=[command computeCommandEncoder];
        if (!compute) { error="compute probe encoder creation failed"; return false; }
        const std::array<std::uint32_t,2> params{vertices,influences};
        [compute setComputePipelineState:pipeline];
        [compute setBuffer:input offset:0 atIndex:0];
        [compute setBuffer:bones offset:0 atIndex:1];
        [compute setBuffer:output offset:GuardBytes atIndex:2];
        [compute setBytes:params.data() length:sizeof(params) atIndex:3];
        if (diagnostics) [compute setBuffer:trace offset:0 atIndex:4];
        const NSUInteger threads=std::min<NSUInteger>(256,pipeline.maxTotalThreadsPerThreadgroup);
        for (std::uint32_t repeat=0;repeat<repeats;++repeat)
            [compute dispatchThreads:MTLSizeMake(vertices,1,1)
                threadsPerThreadgroup:MTLSizeMake(threads,1,1)];
        [compute endEncoding];
        id<MTLBlitCommandEncoder> blit=[command blitCommandEncoder];
        if (!blit) { error="compute probe readback encoder creation failed"; return false; }
        [blit copyFromBuffer:output sourceOffset:0 toBuffer:readback destinationOffset:0 size:output.length];
        [blit endEncoding];
        [command commit];
        encodeMs=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
        [command waitUntilCompleted];
        if (command.status!=MTLCommandBufferStatusCompleted)
        { error=nativeError(command.error); return false; }
        gpuMs=(command.GPUEndTime-command.GPUStartTime)*1000;
        if (!std::isfinite(gpuMs) || gpuMs<=0) { error="compute probe GPU timestamp unavailable"; return false; }
        const auto* data=static_cast<const std::uint8_t*>(readback.contents);
        for (std::size_t i=0;i<GuardBytes;++i)
            if (data[i]!=0xa5 || data[readback.length-GuardBytes+i]!=0xa5)
            { error="compute skin output wrote outside the 36-byte vertex span"; return false; }
        result.resize(vertices);
        std::memcpy(result.data(),data+GuardBytes,result.size()*sizeof(result.front()));
        return true;
    }

    const float* traceData() const
    { return diagnostics?static_cast<const float*>(trace.contents):nullptr; }
};

R4EngineWorldGeometry fixture(std::uint32_t influences, std::uint32_t count, bool signedWeights,
    bool variedBasis, bool badBasis=false, bool zeroBasis=false)
{
    R4EngineWorldGeometry source;
    source.skinned=true; source.vertexFormat=influences;
    source.vertexStride=std::array<std::uint32_t,4>{60,64,70,76}[influences-1];
    source.vertexCount=count; source.indexCount=3;
    source.indexData=std::make_shared<const std::vector<std::uint16_t>>(
        std::initializer_list<std::uint16_t>{0,1,2});
    auto bytes=std::make_shared<std::vector<std::uint8_t>>(std::size_t(count)*source.vertexStride);
    const std::size_t p=influences==1?0:influences*2;
    const std::size_t uv=influences==1?48:influences==2?56:influences==3?62:68;
    const std::size_t w=influences==2?52:influences==3?54:56;
    for (std::size_t vertex=0;vertex<count;++vertex)
    {
        auto* raw=bytes->data()+vertex*source.vertexStride;
        const float position[3]={float(vertex%17)*.15f,float(vertex%13)*-.2f,float(vertex%11)*.07f};
        const float normal[3]={variedBasis?.31f:0,zeroBasis?0.0f:1.0f,variedBasis?-.17f:0};
        float tangent[3]={zeroBasis?0.0f:1.0f,variedBasis?.11f:0,variedBasis?.23f:0};
        const float binormal[3]={variedBasis?-.09f:0,variedBasis?.19f:0,zeroBasis?0.0f:1.0f};
        if (badBasis && vertex==0) tangent[0]=std::numeric_limits<float>::quiet_NaN();
        const float texcoord[2]={float(vertex%7)/7,float(vertex%5)/5};
        std::memcpy(raw+p,position,12); std::memcpy(raw+p+12,normal,12);
        std::memcpy(raw+p+24,tangent,12); std::memcpy(raw+p+36,binormal,12);
        std::memcpy(raw+uv,texcoord,8);
        if (influences==1)
        {
            const std::uint32_t bone=1;
            std::memcpy(raw+56,&bone,4);
        }
        else
        {
            const std::uint16_t ids[4]={0,1,2,3};
            const float weights[3]={signedWeights?1.25f:.2f,.3f,.1f};
            std::memcpy(raw,ids,influences*2);
            std::memcpy(raw+w,weights,(influences-1)*4);
        }
    }
    source.vertexBytes=bytes;
    return source;
}

R4EngineWorldGeometry stressFixture(std::uint32_t influences)
{
    auto source=fixture(influences,8193,false,false);
    auto bytes=std::make_shared<std::vector<std::uint8_t>>(*source.vertexBytes);
    const std::size_t p=influences==1?0:influences*2;
    const std::size_t w=influences==2?52:influences==3?54:56;
    std::uint32_t state=0x730ad8efu+influences;
    const auto random=[&]() {
        state=state*1664525u+1013904223u;
        return float(state>>8u)/float(0x01000000u)*2-1;
    };
    for (std::size_t vertex=0;vertex<source.vertexCount;++vertex)
    {
        auto* raw=bytes->data()+vertex*source.vertexStride;
        float n[3]={random(),random(),random()};
        if (vertex%2==0)
        {
            // Deliberately sit near a byte-packing floor boundary. A one-byte
            // CPU/GPU difference must fail and remain visible in the receipt.
            n[0]=float(vertex%254+1)/127.5f-1;
            n[1]=std::sqrt(std::max(0.0f,1-n[0]*n[0])); n[2]=0;
        }
        const float t[3]={random(),random(),random()}, b[3]={random(),random(),random()};
        std::memcpy(raw+p+12,n,12); std::memcpy(raw+p+24,t,12); std::memcpy(raw+p+36,b,12);
        if (influences>1)
        {
            const float weights[3]={random()*.75f+.5f,random()*.75f+.5f,random()*.75f+.5f};
            std::memcpy(raw+w,weights,(influences-1)*4);
        }
    }
    source.vertexBytes=bytes;
    return source;
}

bool loadFixture(const std::string& path,R4EngineWorldGeometry& source,std::uint32_t& boneCount,
    std::string& error)
{
    std::ifstream stream(path,std::ios::binary|std::ios::ate);
    const auto length=stream?stream.tellg():std::streampos(-1);
    if (length<28 || length>16*1024*1024)
    { error="raw installed fixture is missing or outside the 16 MiB limit"; return false; }
    std::vector<std::uint8_t> data(static_cast<std::size_t>(length));
    stream.seekg(0); stream.read(reinterpret_cast<char*>(data.data()),data.size());
    if (!stream || std::memcmp(data.data(),"R4SKIN01",8))
    { error="raw installed fixture has an invalid header"; return false; }
    std::array<std::uint32_t,5> fields{};
    std::memcpy(fields.data(),data.data()+8,sizeof(fields));
    source.skinned=true; source.vertexFormat=fields[0]; source.vertexStride=fields[1];
    source.vertexCount=fields[2]; source.indexCount=fields[3]; boneCount=fields[4];
    const std::uint64_t vertexBytes=std::uint64_t(source.vertexCount)*source.vertexStride;
    const std::uint64_t indexBytes=std::uint64_t(source.indexCount)*2;
    if (boneCount==0 || boneCount>256 || 28+vertexBytes+indexBytes!=data.size())
    { error="raw installed fixture has inconsistent spans/palette count"; return false; }
    source.vertexBytes=std::make_shared<const std::vector<std::uint8_t>>(data.begin()+28,data.begin()+28+vertexBytes);
    auto indices=std::make_shared<std::vector<std::uint16_t>>(source.indexCount);
    std::memcpy(indices->data(),data.data()+28+vertexBytes,indexBytes);
    source.indexData=indices;
    return true;
}

bool compare(const char* name, const std::vector<R4DynamicWorldSkinnedVertex>& cpu,
    const std::vector<R4DynamicWorldSkinnedVertex>& gpu,
    const std::vector<xray::metal::R4ComputeSkinVertex>* input=nullptr,
    const Bones* transforms=nullptr,std::uint32_t influences=0,const float* trace=nullptr)
{
    if (cpu.size()!=gpu.size())
    { std::fprintf(stderr,"COMPUTE_SKIN_ORACLE case=%s count_mismatch=%zu/%zu\n",name,cpu.size(),gpu.size()); return false; }
    std::size_t exactMismatch=0, uvMismatch=0, alphaMismatch=0;
    unsigned normalDelta=0,tangentDelta=0,binormalDelta=0;
    std::size_t normalMismatchVertices=0,normalMismatchComponents=0,firstNormal=cpu.size();
    double absError=0,relativeError=0;
    bool finite=true;
    for (std::size_t vertex=0;vertex<cpu.size();++vertex)
    {
        const auto& a=cpu[vertex]; const auto& b=gpu[vertex];
        exactMismatch+=std::memcmp(&a,&b,sizeof(a))!=0;
        uvMismatch+=std::memcmp(a.texcoord,b.texcoord,sizeof(a.texcoord))!=0;
        alphaMismatch+=b.normal[3]!=0 || b.tangent[3]!=0 || b.binormal[3]!=0 || b.position[3]!=1;
        bool normalMismatch=false;
        for (unsigned axis=0;axis<3;++axis)
        {
            const double difference=std::abs(double(a.position[axis])-b.position[axis]);
            finite&=std::isfinite(b.position[axis]);
            absError=std::max(absError,difference);
            relativeError=std::max(relativeError,difference/std::max(1.0,std::abs(double(a.position[axis]))));
            normalDelta=std::max(normalDelta,unsigned(std::abs(int(a.normal[axis])-b.normal[axis])));
            if (a.normal[axis]!=b.normal[axis]) { normalMismatch=true; ++normalMismatchComponents; }
            tangentDelta=std::max(tangentDelta,unsigned(std::abs(int(a.tangent[axis])-b.tangent[axis])));
            binormalDelta=std::max(binormalDelta,unsigned(std::abs(int(a.binormal[axis])-b.binormal[axis])));
        }
        if (normalMismatch) { ++normalMismatchVertices; firstNormal=std::min(firstNormal,vertex); }
    }
    const bool ok=finite && uvMismatch==0 && alphaMismatch==0 &&
        normalDelta==0 && tangentDelta==0 && binormalDelta==0 && relativeError<=1.e-6;
    std::printf("COMPUTE_SKIN_ORACLE case=%s vertices=%zu exact_mismatch=%zu position_abs=%.9g position_relative=%.9g normal_byte_delta=%u normal_mismatch_vertices=%zu normal_mismatch_components=%zu tangent_byte_delta=%u binormal_byte_delta=%u uv_mismatch=%zu alpha_mismatch=%zu accepted=%u\n",
        name,cpu.size(),exactMismatch,absError,relativeError,normalDelta,
        normalMismatchVertices,normalMismatchComponents,tangentDelta,binormalDelta,
        uvMismatch,alphaMismatch,unsigned(ok));
    if (firstNormal<cpu.size() && input && transforms)
    {
        const auto& v=(*input)[firstNormal];
        float pre[3]{};
        for (std::uint32_t influence=0;influence<influences;++influence)
            for (unsigned axis=0;axis<3;++axis)
            {
                const auto& m=(*transforms)[v.bones[influence]];
                const float transformed=std::fma(v.normal[2],m[axis+8],
                    std::fma(v.normal[0],m[axis],v.normal[1]*m[axis+4]));
                volatile float weighted=transformed*v.weights[influence];
                pre[axis]+=weighted;
            }
        const float length2=std::fma(pre[2],pre[2],std::fma(pre[0],pre[0],pre[1]*pre[1]));
        const float reciprocal=length2>1.e-12f?1/std::sqrt(length2):0;
        std::printf("COMPUTE_SKIN_NORMAL_FIRST case=%s vertex=%zu raw=%.9g,%.9g,%.9g weights=%.9g,%.9g,%.9g,%.9g bones=%u,%u,%u,%u cpu_packed=%u,%u,%u gpu_packed=%u,%u,%u cpu_instruction_model_pre=%.9g,%.9g,%.9g cpu_instruction_model_length2=%.9g cpu_instruction_model_normalized=%.9g,%.9g,%.9g\n",
            name,firstNormal,v.normal[0],v.normal[1],v.normal[2],v.weights[0],v.weights[1],v.weights[2],v.weights[3],
            v.bones[0],v.bones[1],v.bones[2],v.bones[3],cpu[firstNormal].normal[0],cpu[firstNormal].normal[1],cpu[firstNormal].normal[2],
            gpu[firstNormal].normal[0],gpu[firstNormal].normal[1],gpu[firstNormal].normal[2],pre[0],pre[1],pre[2],length2,
            pre[0]*reciprocal,pre[1]*reciprocal,pre[2]*reciprocal);
        if (trace)
        {
            const auto* values=trace+firstNormal*7;
            std::printf("COMPUTE_SKIN_NORMAL_GPU case=%s vertex=%zu pre=%.9g,%.9g,%.9g normalized=%.9g,%.9g,%.9g length2=%.9g\n",
                name,firstNormal,values[0],values[1],values[2],values[3],values[4],values[5],values[6]);
        }
        for (std::uint32_t influence=0;influence<influences;++influence)
        {
            const auto& m=(*transforms)[v.bones[influence]];
            std::printf("COMPUTE_SKIN_NORMAL_BONE case=%s vertex=%zu bone=%u matrix=",name,firstNormal,v.bones[influence]);
            for (unsigned i=0;i<16;++i) std::printf("%s%.9g",i?",":"",m[i]);
            std::printf("\n");
        }
    }
    return ok;
}
} // namespace

int main(int argc,char** argv)
{
    const bool timing=argc==2 && std::string(argv[1])=="--timing";
    const bool installed=argc==3 && std::string(argv[1])=="--fixture";
    const bool diagnose=argc==2 && std::string(argv[1])=="--diagnose";
    if (!installed && (argc!=2 || (!timing && !diagnose && std::string(argv[1])!="--self-test")))
    { std::fprintf(stderr,"usage: R4ComputeSkinGpuProbe --self-test|--diagnose|--timing|--fixture FILE\n"); return 2; }
    @autoreleasepool
    {
        std::string error;
        SkinGpu gpu;
        if (!gpu.initialize(error,diagnose)) { std::fprintf(stderr,"%s\n",error.c_str()); return 1; }
        auto bones=std::make_shared<Bones>(4);
        for (auto& matrix:*bones) matrix={1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        (*bones)[1][12]=2; (*bones)[1][13]=3; (*bones)[1][14]=4;
        (*bones)[2]={0,1,0,0, -1,0,0,0, 0,0,1,0, 6,0,2,1};
        (*bones)[3]={1,.1f,0,0, .2f,1,0,0, 0,0,1.1f,0, -2,5,0,1};
        bool ok=true;
        if (installed)
        {
            R4EngineWorldGeometry source;
            std::uint32_t boneCount=0;
            if (!loadFixture(argv[2],source,boneCount,error))
            { std::fprintf(stderr,"%s\n",error.c_str()); return 1; }
            bones->resize(boneCount);
            for (std::size_t bone=0;bone<bones->size();++bone)
            {
                const float angle=float(bone)*.013f;
                const float c=std::cos(angle),s=std::sin(angle);
                (*bones)[bone]={c,s,0,0, -s,c,0,0, 0,0,1,0, float(bone%7)*.04f,0,0,1};
            }
            std::vector<xray::metal::R4ComputeSkinVertex> canonical;
            std::uint32_t count=0;
            const auto admission=xray::metal::PrepareR4ComputeSkinVertices(source,bones->size(),canonical,count,error);
            std::vector<R4DynamicWorldSkinnedVertex> cpu,result;
            std::string cpuError;
            const bool cpuReady=RepackR4DynamicWorldSkinnedVertices(source,bones,cpu,cpuError);
            if (admission!=xray::metal::R4ComputeSkinAdmission::Ready)
            {
                std::printf("COMPUTE_SKIN_INSTALLED_FALLBACK file=\"%s\" vertices=%u gpu_admission=%u cpu_ready=%u reason=\"%s\" cpu_reason=\"%s\"\n",
                    argv[2],source.vertexCount,unsigned(admission),unsigned(cpuReady),error.c_str(),cpuError.c_str());
                return cpuReady?0:1;
            }
            if (!cpuReady || !gpu.prepare(canonical,*bones,count,error))
            { std::fprintf(stderr,"%s %s\n",cpuError.c_str(),error.c_str()); return 1; }
            double gpuMs=0,encodeMs=0;
            if (!gpu.run(result,gpuMs,encodeMs,error))
            { std::fprintf(stderr,"%s\n",error.c_str()); return 1; }
            return compare(argv[2],cpu,result)?0:1;
        }
        for (std::uint32_t influences=1;influences<=4;++influences)
        {
            for (unsigned variant=0;variant<(timing?1u:4u);++variant)
            {
                auto source=fixture(influences,timing?131073:257,variant==1,variant==2,false,variant==3);
                if (variant==3) source.vertexFormat=std::array<std::uint32_t,4>{
                    0x12071980u,0x240e3300u,0x481c6600u,0x5a237f80u}[influences-1];
                std::vector<xray::metal::R4ComputeSkinVertex> canonical;
                std::uint32_t count=0;
                if (xray::metal::PrepareR4ComputeSkinVertices(source,bones->size(),canonical,count,error)!=
                        xray::metal::R4ComputeSkinAdmission::Ready || !gpu.prepare(canonical,*bones,count,error))
                { std::fprintf(stderr,"%s\n",error.c_str()); return 1; }
                std::vector<R4DynamicWorldSkinnedVertex> cpu,result;
                const auto cpuStart=Clock::now();
                if (!RepackR4DynamicWorldSkinnedVertices(source,bones,cpu,error))
                { std::fprintf(stderr,"CPU reference: %s\n",error.c_str()); return 1; }
                const double cpuMs=std::chrono::duration<double,std::milli>(Clock::now()-cpuStart).count();
                double gpuMs=0,encodeMs=0;
                if (!gpu.run(result,gpuMs,encodeMs,error))
                { std::fprintf(stderr,"%s\n",error.c_str()); return 1; }
                const std::string name=std::to_string(influences)+"w_"+std::to_string(variant);
                ok=compare(name.c_str(),cpu,result) && ok;
                if (timing)
                {
                    std::vector<double> samples,encodes;
                    for (unsigned repeat=0;repeat<12;++repeat)
                    {
                        if (!gpu.run(result,gpuMs,encodeMs,error))
                        { std::fprintf(stderr,"%s\n",error.c_str()); return 1; }
                        if (repeat>=2) { samples.push_back(gpuMs); encodes.push_back(encodeMs); }
                    }
                    std::sort(samples.begin(),samples.end()); std::sort(encodes.begin(),encodes.end());
                    std::printf("COMPUTE_SKIN_SCREEN weights=%u vertices=%u cpu_repack_one_sample_ms=%.6f gpu_command_elapsed_including_clear_readback_p50_ms=%.6f cpu_encode_p50_ms=%.6f samples=%zu\n",
                        influences,source.vertexCount,cpuMs,samples[samples.size()/2],encodes[encodes.size()/2],samples.size());
                }
            }
            auto repair=fixture(influences,3,false,false,true);
            std::vector<xray::metal::R4ComputeSkinVertex> canonical;
            std::uint32_t count=0;
            ok &= xray::metal::PrepareR4ComputeSkinVertices(repair,bones->size(),canonical,count,error)==
                xray::metal::R4ComputeSkinAdmission::CpuFallback && canonical.empty();
            std::vector<R4DynamicWorldSkinnedVertex> repairedCpu;
            ok &= RepackR4DynamicWorldSkinnedVertices(repair,bones,repairedCpu,error);
            repair.vertexStride=59;
            ok &= xray::metal::PrepareR4ComputeSkinVertices(repair,bones->size(),canonical,count,error)==
                xray::metal::R4ComputeSkinAdmission::Invalid;
            auto invalidBone=fixture(influences,3,false,false);
            ok &= xray::metal::PrepareR4ComputeSkinVertices(invalidBone,1,canonical,count,error)==
                xray::metal::R4ComputeSkinAdmission::Invalid;
            auto invalidUv=fixture(influences,3,false,false);
            auto invalidBytes=std::make_shared<std::vector<std::uint8_t>>(*invalidUv.vertexBytes);
            const std::size_t uvOffset=influences==1?48:influences==2?56:influences==3?62:68;
            const float nan=std::numeric_limits<float>::quiet_NaN();
            std::memcpy(invalidBytes->data()+uvOffset,&nan,4);
            invalidUv.vertexBytes=invalidBytes;
            ok &= xray::metal::PrepareR4ComputeSkinVertices(invalidUv,bones->size(),canonical,count,error)==
                xray::metal::R4ComputeSkinAdmission::Invalid;
            if (!timing)
            {
                auto source=stressFixture(influences);
                for (unsigned palette=0;palette<2;++palette)
                {
                    auto stressBones=std::make_shared<Bones>(*bones);
                    if (palette==0)
                        for (auto& matrix:*stressBones) matrix={1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
                    std::vector<R4DynamicWorldSkinnedVertex> cpu,result;
                    if (xray::metal::PrepareR4ComputeSkinVertices(source,stressBones->size(),canonical,count,error)!=
                            xray::metal::R4ComputeSkinAdmission::Ready ||
                        !RepackR4DynamicWorldSkinnedVertices(source,stressBones,cpu,error) ||
                        !gpu.prepare(canonical,*stressBones,count,error))
                    { std::fprintf(stderr,"%s\n",error.c_str()); return 1; }
                    double gpuMs=0,encodeMs=0;
                    if (!gpu.run(result,gpuMs,encodeMs,error))
                    { std::fprintf(stderr,"%s\n",error.c_str()); return 1; }
                    const auto name=std::to_string(influences)+"w_stress_"+std::to_string(palette);
                    ok=compare(name.c_str(),cpu,result,&canonical,stressBones.get(),count,gpu.traceData()) && ok;
                }
            }
        }
        return ok?0:1;
    }
}

void printLuaStack() {}
