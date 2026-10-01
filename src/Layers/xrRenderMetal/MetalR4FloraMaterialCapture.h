#pragma once

// Engine-only archive reader. Keep this out of portable Metal adapter and
// probe headers; they consume only R4FloraBlenderMaterial values.
#include "../xrRenderPC_R4/stdafx.h"
#include "../xrRender/Blender_tree.h"
#include "../xrRender/blenders/Blender_CLSID.h"
#include "R4FloraBlenderMaterial.h"

#include <string>
#include <unordered_set>
#include <vector>

inline bool LoadR4FloraBlenderMaterials(const char* shaderLibrary,
    std::vector<R4FloraBlenderMaterial>& materials, std::string& error)
{
    materials.clear();
    if (!shaderLibrary || !shaderLibrary[0])
    {
        error = "flora blender capture received an empty shaders.xr path";
        return false;
    }

    IReader* library = FS.r_open(shaderLibrary);
    if (!library)
    {
        error = "cannot open the archive-winning shaders.xr library for flora materials";
        return false;
    }
    IReader* blenderChunks = library->open_chunk(2u);
    if (!blenderChunks)
    {
        FS.r_close(library);
        error = "shaders.xr has no blender chunk for flora materials";
        return false;
    }
    const auto closeLibrary = [&]()
    {
        blenderChunks->close();
        FS.r_close(library);
    };

    std::unordered_set<std::string> uniqueNames;
    for (u32 chunkId = 0u;; ++chunkId)
    {
        IReader* chunk = blenderChunks->open_chunk(chunkId);
        if (!chunk)
            break;
        if (chunk->elapsed() < sizeof(CBlender_DESC))
        {
            chunk->close();
            closeLibrary();
            error = "shaders.xr contains a truncated flora blender record";
            return false;
        }

        CBlender_DESC description;
        chunk->r(&description, sizeof(description));
        if (description.CLS == B_TREE)
        {
            // CBlender_Tree::Load reads Alpha-blend for v0/v1 and Object LOD
            // only for v1. Its constructor initializes the v0 Object LOD value
            // to false, matching the native Windows reader.
            if (description.version > 1u)
            {
                chunk->close();
                closeLibrary();
                error = "shaders.xr flora B_TREE version is newer than the native reader";
                return false;
            }
            CBlender_Tree blender;
            chunk->seek(0u);
            blender.Load(*chunk, description.version);
            const std::string shaderName(description.cName);
            if (shaderName.empty())
            {
                chunk->close();
                closeLibrary();
                error = "shaders.xr contains a flora B_TREE record with an empty shader name";
                return false;
            }
            if (!uniqueNames.emplace(shaderName).second)
            {
                chunk->close();
                closeLibrary();
                error = "shaders.xr contains duplicate B_TREE material records for " + shaderName;
                return false;
            }
            R4FloraBlenderMaterial material;
            material.shaderName = shaderName;
            material.alphaBlend = blender.oBlend.value != FALSE;
            material.objectLod = blender.oNotAnTree.value != FALSE;
            materials.emplace_back(std::move(material));
        }
        chunk->close();
    }

    closeLibrary();
    if (materials.empty())
    {
        error = "shaders.xr contains no B_TREE flora material records";
        return false;
    }
    return true;
}
