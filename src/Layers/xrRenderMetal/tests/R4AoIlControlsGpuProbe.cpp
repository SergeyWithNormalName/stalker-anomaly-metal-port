#include "IR4Backend.h"
#include "MetalR4SsfxAoAdapter.h"
#include <SDL.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
void InitializeR4RolloverProbeRuntime();
void ShutdownR4RolloverProbeRuntime();
namespace {
constexpr std::array<float,16> Identity={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
struct Step { bool ao,il,aoAvailable,ilAvailable,aoReuse,ilReuse,discard,cut,resize,invalidate; float aoScale=1,ilScale=1; };
bool run(const std::filesystem::path& shaders,const std::filesystem::path& artifacts,std::string& error)
{
    if(SDL_InitSubSystem(SDL_INIT_VIDEO)!=0){error=SDL_GetError();return false;}
    auto* window=SDL_CreateWindow("AO IL controls probe",SDL_WINDOWPOS_UNDEFINED,SDL_WINDOWPOS_UNDEFINED,64,32,SDL_WINDOW_HIDDEN|SDL_WINDOW_METAL);
    if(!window){error=SDL_GetError();return false;}
    auto backend=CreateMetalR4Backend(); bool passed=false;
    do {
        if(!backend->Initialize(window,64,32,error))break;
        MetalR4SsfxAoAdapter adapter; MetalR4SsfxAoAdapter::Options options; options.shaderDirectory=shaders;
        if(!adapter.Prepare(*backend,options,error)||!adapter.IsReady())break;
        std::vector<Step> steps={
            {1,1,1,1,0,0,0,0,0,0},{1,1,1,1,1,1,0,0,0,0},
            {1,0,1,1,1,0,0,0,0,0},{1,0,1,1,1,0,0,0,0,0},
            {1,1,1,1,1,0,0,0,0,0},{0,1,1,1,0,1,0,0,0,0},
            {1,1,1,1,0,1,0,0,0,0},{0,0,1,1,0,0,0,0,0,0},
            {1,1,1,1,0,0,0,0,0,0},{1,0,1,1,1,0,1,0,0,0},
            {1,1,1,1,1,1,0,0,0,0},{1,1,0,1,0,1,0,0,0,0},
            {1,1,1,0,0,0,0,0,0,0},{1,1,1,1,0,0,0,0,0,0,2,1},
            {1,1,1,1,1,0,0,0,0,0,2,4},{1,1,1,1,0,0,0,1,0,0,2,4},
            {1,1,1,1,0,0,0,0,1,0,2,4},{1,1,1,1,0,0,0,0,0,1,2,4}
        };
        std::shared_ptr<IR4Texture> position,motion,accumulator;
        std::shared_ptr<IR4Buffer> globals; std::uint32_t previousFrame=0,width=64,height=32;
        auto prepare=[&](){
            auto texture=[&](const char* label){R4Texture2DDesc desc;desc.width=width;desc.height=height;desc.format=R4PixelFormat::RGBA16Float;desc.storage=R4ResourceStorage::Private;desc.usage=R4TextureUsage::ShaderRead|R4TextureUsage::RenderTarget;desc.label=label;return backend->Resources().CreateTexture2D(desc,error);};
            position=texture("AO IL synthetic packed position");motion=texture("AO IL synthetic zero motion");accumulator=texture("AO IL synthetic accumulator");
            std::array<unsigned char,528> bytes{};for(unsigned i=0;i<4;++i)std::memcpy(bytes.data()+i*64,Identity.data(),64);
            const std::array<float,4> decompression={1,-1,2.0f/width,-2.0f/height},resolution={float(width),float(height),1.0f/width,1.0f/height};
            std::memcpy(bytes.data()+400,decompression.data(),16);std::memcpy(bytes.data()+416,resolution.data(),16);
            R4BufferDesc desc;desc.size=bytes.size();desc.storage=R4ResourceStorage::Shared;desc.usage=R4BufferUsage::Uniform;desc.label="AO IL static globals";
            globals=backend->Resources().CreateBuffer(desc,bytes.data(),bytes.size(),error);
            return position&&motion&&accumulator&&globals&&adapter.PrepareRenderTargets(*backend,width,height,error);
        };
        if(!prepare())break;
        bool failed=false;
        for(unsigned index=0;index<steps.size();++index){const auto& step=steps[index];const unsigned frame=index+1;
            if(step.resize){width=32;height=16;if(!prepare()){failed=true;break;}}
            if(step.invalidate)adapter.InvalidateHistory();
            if(backend->BeginFrame(error)!=R4FrameAcquireResult::Ready){failed=true;break;}
            auto clear=[&](const std::shared_ptr<IR4Texture>& texture,R4ClearColor color){R4RenderPassDesc pass;pass.useDrawable=false;pass.label="AO IL fixture input clear";pass.colorAttachments.push_back({texture,R4LoadAction::Clear,R4StoreAction::Store,color});return backend->BeginRenderPass(pass,error)&&backend->EndRenderPass(error);};
            if(!clear(position,{0,0,5,1})||!clear(motion,{0,0,0,0})||!clear(accumulator,{.4,.2,.1,0})){failed=true;break;}
            MetalR4SsfxAoAdapter::FrameInputs input;input.position=position;input.motionVectors=motion;input.accumulator=accumulator;input.staticGlobals=globals;
            input.view=input.previousView=input.projection=input.previousProjection=Identity;
            input.environment.valid=true;input.environment.engineFrame=frame;
            input.environment.ssfxAoAvailable=step.aoAvailable;input.environment.ssfxIlAvailable=step.ilAvailable;
            input.environment.ssfxAo={step.aoScale,step.ao?5.f:0.f,1,1.9f};input.environment.ssfxIl={step.ilScale,step.il?.8f:0.f,1,5};
            input.environment.ssfxAoSetup1={150,1,.5f,1};input.environment.ssfxIlSetup1={150,1,1,0};
            input.previousValid=previousFrame>0;input.previousFrame=previousFrame;input.cameraCut=step.cut;
            if(!adapter.Draw(*backend,input,error)){failed=true;break;}
            const auto& t=adapter.LastTelemetry();const bool enabledAo=step.ao&&step.aoAvailable,enabledIl=step.il&&step.ilAvailable;
            if(t.aoEnabled!=enabledAo||t.ilEnabled!=enabledIl||t.passes!=(enabledAo?5u:0u)||t.indirectLightPasses!=(enabledIl?5u:0u)||t.aoHistory!=step.aoReuse||t.ilHistory!=step.ilReuse){error="AO IL gate/history mismatch at step "+std::to_string(frame);failed=true;break;}
            std::printf("AO_IL_STEP frame=%u enabled=%d,%d reuse=%d,%d passes=%u,%u discard=%d extent=%ux%u\n",frame,t.aoEnabled,t.ilEnabled,t.aoHistory,t.ilHistory,t.passes,t.indirectLightPasses,step.discard,width,height);
            if(step.discard){adapter.DiscardUnsubmittedFrame();backend->DiscardFrame();continue;}
            auto witness=[&](const std::shared_ptr<IR4Texture>& texture,const char* label,bool disabled){R4TemporalTextureValidationDesc desc;desc.textures={texture};desc.labels={label};desc.engineFrame=frame;desc.previousFrame=previousFrame;desc.phase=std::string("controls_")+label;desc.outputDirectory=artifacts.string();desc.expectFinite=true;desc.expectAlphaOne=disabled;desc.expectRgbZero=disabled;return backend->ValidateTemporalTextures(desc,error);};
            if(!witness(adapter.Output(),"ao",!enabledAo)||!witness(adapter.IndirectLightOutput(),"il",!enabledIl)||!backend->SubmitFrame(error)){failed=true;break;}
            adapter.CommitSubmittedFrame();previousFrame=frame;
        }
        if(failed){backend->DiscardFrame();break;}passed=true;
    }while(false);
    backend->Shutdown();if(backend->HasCommandBufferError())passed=false;backend.reset();SDL_DestroyWindow(window);SDL_QuitSubSystem(SDL_INIT_VIDEO);return passed;
}
}
int main(int argc,char** argv){if(argc!=4||std::string(argv[1])!="--gpu"){std::fprintf(stderr,"usage: AoIlControlsGpuProbe --gpu <shader-dir> <artifact-dir>\n");return 2;}InitializeR4RolloverProbeRuntime();std::string error;const bool passed=run(argv[2],argv[3],error);std::fprintf(passed?stdout:stderr,"AO IL controls encoding %s %s\n",passed?"PASS":"FAIL",error.c_str());ShutdownR4RolloverProbeRuntime();return passed?0:1;}
