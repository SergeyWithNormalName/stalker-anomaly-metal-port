#pragma once
#include "IR4Pipeline.h"
#include "IR4Resources.h"
#include "R4DdsTexture.h"
#include "../xrRender/MetalLensFlareCapture.h"
#include <filesystem>
#include <memory>
#include <string>
class IR4Backend;
class MetalR4LensFlareAdapter final {
public:
 struct Options { std::filesystem::path shaderDirectory; xray::metal::R4DdsBytesProvider ddsBytesProvider; };
 bool Configure(IR4Backend&, const Options&, std::string&);
 bool StageFrame(IR4Resources&, xray::metal::lensflare::Frame, std::string&);
 bool DrawIntoActiveForward(IR4Backend&, const std::shared_ptr<IR4Buffer>& staticGlobals, std::string&);
 bool IsReady() const { return ready; }
private: struct Impl; std::unique_ptr<Impl> impl; bool ready=false;
};
