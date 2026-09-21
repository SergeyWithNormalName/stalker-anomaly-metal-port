#pragma once

#include <filesystem>
#include <string>

namespace xray::metal
{
enum class R4StaticForwardFamily
{
    Unsupported,
    Lightmapped,
    VertexColor,
    Wallmark,
    Lightplane,
    Water,
};

R4StaticForwardFamily ClassifyR4StaticForwardFamily(const std::string& shaderName);
bool ValidateR4StaticForwardMscBundle(
    const std::filesystem::path& shaderDirectory, std::string& error);
bool RunR4StaticForwardFoundationSelfTest(std::string& error);
} // namespace xray::metal
