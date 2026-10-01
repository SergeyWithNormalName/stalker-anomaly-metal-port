#include "../../xrCore/xrCore.h"
#include "MetalR4EngineFs.h"
#include "MetalR4UiVideo.h"

// No UI or engine filesystem is exercised by this offscreen probe. Fail
// closed if such a path is reached; the light fixture provides its cookie.
void printLuaStack() {}
bool ResolveMetalR4EngineUiTextureFrame(const std::string&, std::string&, std::string& error)
{ error = "rollover probe has no engine UI"; return false; }
bool ResolveMetalR4EngineTextureDdsPath(const std::filesystem::path&, std::filesystem::path&, std::string& error)
{ error = "rollover probe has no engine filesystem"; return false; }
bool CreateMetalR4EngineXrfsProvider(xray::metal::R4DdsBytesProvider&, std::string& error)
{ error = "rollover probe has no engine filesystem provider"; return false; }
bool OpenMetalR4UiVideo(const std::string&, std::unique_ptr<IR4UiVideoPlayback>&, std::string& error)
{ error = "rollover probe has no UI video"; return false; }
bool GetMetalR4UiVideoDimensions(const std::string&, std::uint32_t&, std::uint32_t&, std::string& error)
{ error = "rollover probe has no UI video dimensions"; return false; }
#include <typeinfo>
// Provide the interface RTTI locally as well, so the linker need not pull
// the engine video implementation just for its weak typeinfo definition.
const std::type_info& R4RolloverProbeVideoTypeInfo()
{
    return typeid(IR4UiVideoPlayback);
}

// Materialize the engine allocator delete locally; otherwise its weak inline
// definition can pull the engine UI video object from the static archive.
void (*volatile R4RolloverProbeDelete)(void*) = &operator delete;
void InitializeR4RolloverProbeRuntime()
{
    Memory._initialize(FALSE);
    xrLogger::InitLog();
}
void ShutdownR4RolloverProbeRuntime()
{
    xrLogger::CloseLog();
}
