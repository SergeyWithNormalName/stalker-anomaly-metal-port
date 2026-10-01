#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "R4LocalShadowBatchValidation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
constexpr NSUInteger kRowAlignment = 256u;
constexpr double kClearDepth = 1.0 - 1.0e-7;
constexpr double kMinimumWorldDepth = 0.02;

struct Matrix4 { double v[16]{}; };
struct Vec4 { double x=0.0, y=0.0, z=0.0, w=0.0; };

struct Readback
{
    __strong id<MTLBuffer> buffer = nil;
    NSUInteger rowBytes = 0u;
};

struct BatchCapture
{
    R4LocalShadowBatchValidationDesc descriptor;
    std::uint32_t atlasWidth=0u, atlasHeight=0u;
    std::uint32_t width=0u, height=0u;
    Readback atlas, position, depth, stencil, accumulation;
};

NSUInteger aligned(NSUInteger bytes)
{
    return (bytes + kRowAlignment - 1u) & ~(kRowAlignment - 1u);
}

bool bufferSizeFits(NSUInteger rowBytes, std::uint32_t height)
{
    return rowBytes != 0u && height <= std::numeric_limits<NSUInteger>::max() / rowBytes;
}

Matrix4 asMatrix(const std::array<float, 16>& input)
{
    Matrix4 output;
    for (std::size_t index=0u; index<input.size(); ++index) output.v[index]=input[index];
    return output;
}

Vec4 multiply(const Matrix4& matrix, const Vec4& point)
{
    return {
        matrix.v[0]*point.x + matrix.v[4]*point.y + matrix.v[8]*point.z + matrix.v[12]*point.w,
        matrix.v[1]*point.x + matrix.v[5]*point.y + matrix.v[9]*point.z + matrix.v[13]*point.w,
        matrix.v[2]*point.x + matrix.v[6]*point.y + matrix.v[10]*point.z + matrix.v[14]*point.w,
        matrix.v[3]*point.x + matrix.v[7]*point.y + matrix.v[11]*point.z + matrix.v[15]*point.w,
    };
}

bool finiteMatrix(const std::array<float, 16>& matrix)
{
    return std::all_of(matrix.begin(), matrix.end(), [](float value) { return std::isfinite(value); });
}

double halfToDouble(std::uint16_t bits)
{
    const std::uint32_t sign=std::uint32_t(bits&0x8000u)<<16u;
    const std::uint32_t exponent=(bits>>10u)&31u;
    const std::uint32_t mantissa=bits&1023u;
    std::uint32_t raw=0u;
    if (exponent==0u)
    {
        if (mantissa==0u) raw=sign;
        else
        {
            std::uint32_t normalized=mantissa;
            int shift=0;
            while ((normalized&0x400u)==0u) { normalized<<=1u; ++shift; }
            normalized&=0x3ffu;
            raw=sign|(std::uint32_t(127-15-shift+1)<<23u)|(normalized<<13u);
        }
    }
    else if (exponent==31u) raw=sign|0x7f800000u|(mantissa<<13u);
    else raw=sign|((exponent+112u)<<23u)|(mantissa<<13u);
    float result=0.0f;
    std::memcpy(&result,&raw,sizeof(result));
    return result;
}

bool readDepth(const BatchCapture& capture, std::uint32_t x, std::uint32_t y, float& value)
{
    if (!capture.depth.buffer || x>=capture.width || y>=capture.height) return false;
    const auto* bytes=static_cast<const std::uint8_t*>(capture.depth.buffer.contents);
    std::memcpy(&value,bytes+std::size_t(y)*capture.depth.rowBytes+std::size_t(x)*sizeof(float),sizeof(value));
    return true;
}

bool readAtlas(const BatchCapture& capture, std::uint32_t x, std::uint32_t y, float& value)
{
    if (!capture.atlas.buffer || x>=capture.atlasWidth || y>=capture.atlasHeight) return false;
    const auto* bytes=static_cast<const std::uint8_t*>(capture.atlas.buffer.contents);
    std::memcpy(&value,bytes+std::size_t(y)*capture.atlas.rowBytes+std::size_t(x)*sizeof(float),sizeof(value));
    return true;
}

std::uint8_t readStencil(const BatchCapture& capture, std::uint32_t x, std::uint32_t y)
{
    const auto* bytes=static_cast<const std::uint8_t*>(capture.stencil.buffer.contents);
    return bytes[std::size_t(y)*capture.stencil.rowBytes+x];
}

std::uint64_t hashPrimaryDepth(const BatchCapture& capture)
{
    std::uint64_t hash=14695981039346656037ull;
    const auto* bytes=static_cast<const std::uint8_t*>(capture.depth.buffer.contents);
    for (std::uint32_t y=0u;y<capture.height;++y)
    {
        const auto* row=bytes+std::size_t(y)*capture.depth.rowBytes;
        for (std::size_t byte=0u;byte<std::size_t(capture.width)*sizeof(float);++byte)
        { hash^=row[byte]; hash*=1099511628211ull; }
    }
    return hash;
}

std::uint64_t hashGeometryStencilBit(const BatchCapture& capture)
{
    std::uint64_t hash=14695981039346656037ull;
    for (std::uint32_t y=0u;y<capture.height;++y)
        for (std::uint32_t x=0u;x<capture.width;++x)
        { hash^=std::uint8_t(readStencil(capture,x,y)&1u); hash*=1099511628211ull; }
    return hash;
}

bool reconstructViewPosition(const BatchCapture& capture, const std::uint32_t x,
    const std::uint32_t y, double position[3], double packedXY[2])
{
    const auto* bytes=static_cast<const std::uint8_t*>(capture.position.buffer.contents)+
        std::size_t(y)*capture.position.rowBytes+std::size_t(x)*8u;
    std::uint16_t packed[4]{};
    std::memcpy(packed,bytes,sizeof(packed));
    packedXY[0]=halfToDouble(packed[0]);
    packedXY[1]=halfToDouble(packed[1]);
    const double z=halfToDouble(packed[2]);
    if (!std::isfinite(z)) return false;
    for (const float value:capture.descriptor.positionDecompressionParams)
        if (!std::isfinite(value)) return false;
    const auto& p=capture.descriptor.positionDecompressionParams;
    const double pixelX=double(x)+0.5;
    const double pixelY=double(y)+0.5;
    position[0]=z*(pixelX*p[2]-p[0]);
    position[1]=z*(pixelY*p[3]-p[1]);
    position[2]=z;
    return std::isfinite(position[0])&&std::isfinite(position[1])&&std::isfinite(position[2]);
}

std::uint64_t hashTile(const BatchCapture& capture,
    const xray::metal::R4LocalShadowAtlasRecord& record)
{
    std::uint64_t hash=14695981039346656037ull;
    const auto* bytes=static_cast<const std::uint8_t*>(capture.atlas.buffer.contents);
    for (std::uint32_t y=0u;y<record.atlasSize;++y)
    {
        const auto* row=bytes+std::size_t(record.atlasY+y)*capture.atlas.rowBytes+
            std::size_t(record.atlasX)*sizeof(float);
        for (std::size_t byte=0u;byte<std::size_t(record.atlasSize)*sizeof(float);++byte)
        {
            hash^=row[byte];
            hash*=1099511628211ull;
        }
    }
    return hash;
}

std::string artifactStem(const R4LocalShadowBatchValidationDesc& descriptor)
{
    std::ostringstream out;
    out << "local-shadow-frame-" << std::setw(8) << std::setfill('0') << descriptor.engineFrame
        << "-batch-" << std::setw(4) << std::setfill('0') << descriptor.batchId;
    return out.str();
}

std::string jsonEscape(const std::string& value)
{
    std::ostringstream out;
    for (const unsigned char character:value)
    {
        if (character=='"') out<<"\\\"";
        else if (character=='\\') out<<"\\\\";
        else if (character=='\n') out<<"\\n";
        else if (character=='\r') out<<"\\r";
        else if (character>=0x20u) out<<static_cast<char>(character);
    }
    return out.str();
}

struct TileMetrics
{
    std::uint64_t covered=0u, nonFinite=0u, hash=0u;
    std::uint64_t receiverCandidates=0u, receiverInLightFrustum=0u, shadowedReceivers=0u;
    double minimum=std::numeric_limits<double>::infinity();
    double maximum=-std::numeric_limits<double>::infinity();
};

struct Receiver
{
    std::uint32_t x=0u,y=0u;
    double viewPosition[3]{};
};

std::vector<TileMetrics> analyzeTiles(const BatchCapture& capture,
    std::uint64_t& nonFinitePrimaryDepth, std::uint64_t& validReceivers,
    std::uint64_t& cameraReprojectable, double& maxCameraPixelError)
{
    std::vector<Receiver> receivers;
    receivers.reserve(std::size_t(capture.width)*capture.height/2u);
    const Matrix4 view=asMatrix(capture.descriptor.view);
    const Matrix4 projection=asMatrix(capture.descriptor.projection);
    const Matrix4 inverseView=asMatrix(capture.descriptor.inverseView);
    for (std::uint32_t y=0u;y<capture.height;++y)
    {
        for (std::uint32_t x=0u;x<capture.width;++x)
        {
            float depth=1.0f;
            if (!readDepth(capture,x,y,depth) || !std::isfinite(depth))
            { ++nonFinitePrimaryDepth; continue; }
            if (depth<=kMinimumWorldDepth || depth>=kClearDepth) continue;
            double viewPosition[3]{}, packedXY[2]{};
            if (!reconstructViewPosition(capture,x,y,viewPosition,packedXY) ||
                viewPosition[2]<=kMinimumWorldDepth ||
                !std::isfinite(packedXY[0]) || !std::isfinite(packedXY[1]) ||
                (std::abs(packedXY[0])<=1.0e-8 && std::abs(packedXY[1])<=1.0e-8))
                continue;
            Receiver receiver;
            receiver.x=x; receiver.y=y;
            std::copy(viewPosition,viewPosition+3,receiver.viewPosition);
            receivers.push_back(receiver);
            ++validReceivers;

            const Vec4 world4=multiply(inverseView,{viewPosition[0],viewPosition[1],viewPosition[2],1.0});
            if (!std::isfinite(world4.w)||std::abs(world4.w)<1.0e-12) continue;
            const Vec4 cameraClip=multiply(projection,multiply(view,{world4.x/world4.w,
                world4.y/world4.w,world4.z/world4.w,1.0}));
            if (!std::isfinite(cameraClip.w)||cameraClip.w<=1.0e-12) continue;
            const double px=(cameraClip.x/cameraClip.w*0.5+0.5)*capture.width;
            const double py=(0.5-cameraClip.y/cameraClip.w*0.5)*capture.height;
            if (px>=0.0&&px<capture.width&&py>=0.0&&py<capture.height)
            {
                ++cameraReprojectable;
                maxCameraPixelError=std::max(maxCameraPixelError,
                    std::max(std::abs(px-(double(x)+0.5)),std::abs(py-(double(y)+0.5))));
            }
        }
    }

    std::vector<TileMetrics> result(capture.descriptor.records.size());
    for (std::size_t recordIndex=0u;recordIndex<capture.descriptor.records.size();++recordIndex)
    {
        const auto& record=capture.descriptor.records[recordIndex];
        TileMetrics& metrics=result[recordIndex];
        metrics.receiverCandidates=receivers.size();
        metrics.hash=hashTile(capture,record);
        for (std::uint32_t y=0u;y<record.atlasSize;++y)
            for (std::uint32_t x=0u;x<record.atlasSize;++x)
            {
                float depth=1.0f;
                if (!readAtlas(capture,record.atlasX+x,record.atlasY+y,depth)||!std::isfinite(depth))
                { ++metrics.nonFinite; continue; }
                metrics.minimum=std::min(metrics.minimum,double(depth));
                metrics.maximum=std::max(metrics.maximum,double(depth));
                if (depth>=0.0f&&depth<kClearDepth) ++metrics.covered;
            }

        const Matrix4 shadowMatrix=asMatrix(record.shadowMatrix);
        const Matrix4 lightView=asMatrix(record.lightView);
        const Matrix4 lightProjection=asMatrix(record.lightProjection);
        const Matrix4 deviceInverseView=asMatrix(record.deviceInverseView);
        for (const Receiver& receiver:receivers)
        {
            const Vec4 world=multiply(deviceInverseView,{receiver.viewPosition[0],
                receiver.viewPosition[1],receiver.viewPosition[2],1.0});
            const Vec4 lightCamera=multiply(lightView,world);
            const Vec4 lightClip=multiply(lightProjection,lightCamera);
            if (!std::isfinite(lightClip.w)||lightClip.w<=1.0e-12||
                lightClip.x < -lightClip.w||lightClip.x > lightClip.w||
                lightClip.y < -lightClip.w||lightClip.y > lightClip.w||
                lightClip.z < 0.0||lightClip.z > lightClip.w)
                continue;
            const Vec4 projected=multiply(shadowMatrix,{receiver.viewPosition[0],
                receiver.viewPosition[1],receiver.viewPosition[2],1.0});
            if (!std::isfinite(projected.w)||projected.w<=1.0e-12) continue;
            const double u=projected.x/projected.w;
            const double v=projected.y/projected.w;
            const double z=projected.z/projected.w;
            const double minU=double(record.atlasX+1u)/capture.atlasWidth;
            const double maxU=double(record.atlasX+record.atlasSize-1u)/capture.atlasWidth;
            const double minV=double(record.atlasY+1u)/capture.atlasHeight;
            const double maxV=double(record.atlasY+record.atlasSize-1u)/capture.atlasHeight;
            if (!std::isfinite(u)||!std::isfinite(v)||!std::isfinite(z)||
                u<minU||u>maxU||v<minV||v>maxV||z<0.0||z>1.0)
                continue;
            ++metrics.receiverInLightFrustum;
            // shadowMatrix is the source R4 sampling transform and already
            // includes this tile's +1 border and half-texel atlas offset.
            const std::uint32_t atlasX=std::min(capture.atlasWidth-1u,
                static_cast<std::uint32_t>(std::floor(u*capture.atlasWidth)));
            const std::uint32_t atlasY=std::min(capture.atlasHeight-1u,
                static_cast<std::uint32_t>(std::floor(v*capture.atlasHeight)));
            float casterDepth=1.0f;
            if (!readAtlas(capture,atlasX,atlasY,casterDepth)||!std::isfinite(casterDepth)) continue;
            if (z>double(casterDepth)+1.0e-4) ++metrics.shadowedReceivers;
        }
    }
    return result;
}

bool writeBytes(const std::filesystem::path& path, const Readback& source,
    std::uint32_t width, std::uint32_t height, std::size_t bytesPerPixel,
    std::string& error)
{
    std::ofstream output(path,std::ios::binary|std::ios::trunc);
    if (!output) { error="cannot open validation artifact: "+path.string(); return false; }
    const auto* bytes=static_cast<const std::uint8_t*>(source.buffer.contents);
    const std::size_t rowBytes=std::size_t(width)*bytesPerPixel;
    for (std::uint32_t y=0u;y<height;++y)
        output.write(reinterpret_cast<const char*>(bytes+std::size_t(y)*source.rowBytes),
            static_cast<std::streamsize>(rowBytes));
    if (!output) { error="failed writing validation artifact: "+path.string(); return false; }
    return true;
}

bool writeReport(const BatchCapture& capture, const bool gpuSuccess,
    const std::string& gpuError, std::string& error)
{
    const auto& descriptor=capture.descriptor;
    const std::filesystem::path directory(descriptor.outputDirectory);
    std::error_code filesystemError;
    std::filesystem::create_directories(directory,filesystemError);
    if (filesystemError) { error="cannot create local-shadow validation directory: "+filesystemError.message(); return false; }
    const std::string stem=artifactStem(descriptor);
    std::uint64_t nonFiniteDepth=0u, validReceivers=0u, cameraReprojectable=0u;
    double maxCameraPixelError=0.0;
    const std::vector<TileMetrics> tiles=gpuSuccess?
        analyzeTiles(capture,nonFiniteDepth,validReceivers,cameraReprojectable,maxCameraPixelError):
        std::vector<TileMetrics>(descriptor.records.size());
    std::uint64_t accumulationFiniteChannels=0u, accumulationNonFiniteChannels=0u;
    if (gpuSuccess)
    {
        const auto* bytes=static_cast<const std::uint8_t*>(capture.accumulation.buffer.contents);
        for (std::uint32_t y=0u;y<capture.height;++y)
            for (std::size_t channel=0u;channel<std::size_t(capture.width)*4u;++channel)
            {
                std::uint16_t half=0u;
                std::memcpy(&half,bytes+std::size_t(y)*capture.accumulation.rowBytes+channel*2u,2u);
                if (std::isfinite(halfToDouble(half))) ++accumulationFiniteChannels;
                else ++accumulationNonFiniteChannels;
            }
    }
    const bool anyCovered=std::any_of(tiles.begin(),tiles.end(),[](const TileMetrics& tile) {
        return tile.covered>0u;
    });
    const bool allRendered=std::all_of(descriptor.records.begin(),descriptor.records.end(),
        [](const auto& record) { return record.renderedDepth; });
    const bool allTileDataFinite=std::all_of(tiles.begin(),tiles.end(),
        [](const TileMetrics& tile) { return tile.nonFinite==0u; });
    const std::uint64_t primaryDepthHash=gpuSuccess?hashPrimaryDepth(capture):0u;
    const std::uint64_t geometryStencilHash=gpuSuccess?hashGeometryStencilBit(capture):0u;
    std::string artifactError;
    bool artifactsWritten=gpuSuccess;
    if (gpuSuccess)
    {
        artifactsWritten=writeBytes(directory/(stem+".r32"),capture.atlas,
            capture.atlasWidth,capture.atlasHeight,4u,artifactError);
        std::string accumulationError;
        if (!writeBytes(directory/(stem+".rgba16f"),capture.accumulation,
                capture.width,capture.height,8u,accumulationError))
        {
            artifactsWritten=false;
            if (artifactError.empty()) artifactError=accumulationError;
        }
    }
    const bool pass=gpuSuccess&&artifactsWritten&&allRendered&&allTileDataFinite&&anyCovered&&
        nonFiniteDepth==0u&&accumulationNonFiniteChannels==0u;

    std::ofstream output(directory/(stem+".json"),std::ios::binary|std::ios::trunc);
    if (!output) { error="cannot write local-shadow validation report"; return false; }
    const Matrix4 inverseView=asMatrix(descriptor.inverseView);
    Vec4 forward=multiply(inverseView,{0.0,0.0,1.0,0.0});
    const double forwardLength=std::sqrt(forward.x*forward.x+forward.y*forward.y+forward.z*forward.z);
    if (std::isfinite(forwardLength)&&forwardLength>1.0e-12)
    { forward.x/=forwardLength; forward.y/=forwardLength; forward.z/=forwardLength; }
    output<<std::setprecision(9)
        <<"{\n  \"schema\": \"xray.r4.local-shadow-batch-validation.v1\",\n"
        <<"  \"frame\": "<<descriptor.engineFrame<<",\n  \"batch\": "<<descriptor.batchId<<",\n"
        <<"  \"pass\": "<<(pass?"true":"false")<<",\n"
        <<"  \"gpu_completed\": "<<(gpuSuccess?"true":"false")<<",\n"
        <<"  \"gpu_error\": \""<<jsonEscape(gpuError)<<"\",\n"
        <<"  \"artifacts_written\": "<<(artifactsWritten?"true":"false")<<",\n"
        <<"  \"artifact_error\": \""<<jsonEscape(artifactError)<<"\",\n"
        <<"  \"atlas_width\": "<<capture.atlasWidth<<",\n  \"atlas_height\": "<<capture.atlasHeight<<",\n"
        <<"  \"camera_width\": "<<capture.width<<",\n  \"camera_height\": "<<capture.height<<",\n"
        <<"  \"camera_eye\": ["<<descriptor.inverseView[12]<<", "<<descriptor.inverseView[13]<<", "<<descriptor.inverseView[14]<<"],\n"
        <<"  \"camera_forward\": ["<<forward.x<<", "<<forward.y<<", "<<forward.z<<"],\n"
        <<"  \"view\": [";
    for (std::size_t index=0u;index<descriptor.view.size();++index)
        output<<(index?", ":"")<<descriptor.view[index];
    output<<"],\n  \"projection\": [";
    for (std::size_t index=0u;index<descriptor.projection.size();++index)
        output<<(index?", ":"")<<descriptor.projection[index];
    output<<"],\n  \"inverse_view\": [";
    for (std::size_t index=0u;index<descriptor.inverseView.size();++index)
        output<<(index?", ":"")<<descriptor.inverseView[index];
    output<<"],\n"
        <<"  \"receiver_position_pixels\": "<<validReceivers<<",\n"
        <<"  \"nonfinite_primary_depth_pixels\": "<<nonFiniteDepth<<",\n"
        <<"  \"accumulation_finite_channels\": "<<accumulationFiniteChannels<<",\n"
        <<"  \"accumulation_nonfinite_channels\": "<<accumulationNonFiniteChannels<<",\n"
        <<"  \"camera_reprojectable_pixels\": "<<cameraReprojectable<<",\n"
        <<"  \"camera_max_reprojection_error_pixels\": "<<maxCameraPixelError<<",\n"
        <<"  \"depth_hash_fnv1a64\": \""<<std::hex<<std::setw(16)<<std::setfill('0')<<primaryDepthHash
        <<std::dec<<std::setfill(' ')<<"\",\n"
        <<"  \"geometry_stencil_bit_hash_fnv1a64\": \""<<std::hex<<std::setw(16)<<std::setfill('0')<<geometryStencilHash
        <<std::dec<<std::setfill(' ')<<"\",\n"
        <<"  \"atlas_file\": \""<<stem<<".r32\",\n  \"accumulator_file\": \""<<stem<<".rgba16f\",\n"
        <<"  \"records\": [\n";
    for (std::size_t index=0u;index<descriptor.records.size();++index)
    {
        const auto& record=descriptor.records[index];
        const TileMetrics& metrics=tiles[index];
        output<<"    {\"light_id\": "<<record.lightIdentity
            <<", \"parent_id\": "<<descriptor.parentIds[index]
            <<", \"face\": "<<record.face<<", \"tile\": "<<index
            <<", \"atlas_rect\": ["<<record.atlasX<<", "<<record.atlasY<<", "<<record.atlasSize<<"]"
            <<", \"rendered_depth\": "<<(record.renderedDepth?"true":"false")
            <<", \"tile_hash_fnv1a64\": \""<<std::hex<<std::setw(16)<<std::setfill('0')<<metrics.hash
            <<std::dec<<std::setfill(' ')<<"\", \"covered_pixels\": "<<metrics.covered
            <<", \"nonfinite_pixels\": "<<metrics.nonFinite
            <<", \"minimum_depth\": "<<(std::isfinite(metrics.minimum)?metrics.minimum:-1.0)
            <<", \"maximum_depth\": "<<(std::isfinite(metrics.maximum)?metrics.maximum:-1.0)
            <<", \"primary_receiver_candidates\": "<<metrics.receiverCandidates
            <<", \"receivers_in_light_frustum\": "<<metrics.receiverInLightFrustum
            <<", \"shadowed_receiver_pixels\": "<<metrics.shadowedReceivers<<"}"
            <<(index+1u==descriptor.records.size()?"\n":",\n");
    }
    output<<"  ]\n}\n";
    if (!output) { error="failed writing local-shadow validation report"; return false; }
    return true;
}

NSString* localizedError(id<MTLCommandBuffer> commandBuffer)
{
    NSError* error=commandBuffer.error;
    return error ? error.localizedDescription : @"Metal command buffer failed without an NSError";
}
} // namespace

bool EncodeMetalR4LocalShadowBatchValidation(void* commandBufferHandle,
    void* atlasHandle, void* positionHandle, void* depthStencilHandle,
    void* accumulationHandle, const R4LocalShadowBatchValidationDesc& descriptor,
    std::string& error)
{
    id<MTLCommandBuffer> commandBuffer=(__bridge id<MTLCommandBuffer>)commandBufferHandle;
    id<MTLTexture> atlas=(__bridge id<MTLTexture>)atlasHandle;
    id<MTLTexture> position=(__bridge id<MTLTexture>)positionHandle;
    id<MTLTexture> depthStencil=(__bridge id<MTLTexture>)depthStencilHandle;
    id<MTLTexture> accumulation=(__bridge id<MTLTexture>)accumulationHandle;
    if (!commandBuffer||!atlas||!position||!depthStencil||!accumulation||
        descriptor.outputDirectory.empty()||descriptor.records.empty()||
        descriptor.records.size()!=descriptor.parentIds.size())
    {
        error="local-shadow batch validation needs command buffer, four textures, output directory, and aligned records/parents";
        return false;
    }
    if (atlas.pixelFormat!=MTLPixelFormatR32Float||position.pixelFormat!=MTLPixelFormatRGBA16Float||
        accumulation.pixelFormat!=MTLPixelFormatRGBA16Float||
        depthStencil.pixelFormat!=MTLPixelFormatDepth32Float_Stencil8||atlas.sampleCount!=1u||
        position.sampleCount!=1u||accumulation.sampleCount!=1u||depthStencil.sampleCount!=1u||
        atlas.width==0u||atlas.height==0u||position.width==0u||position.height==0u||
        position.width!=depthStencil.width||position.height!=depthStencil.height||
        position.width!=accumulation.width||position.height!=accumulation.height)
    {
        error="local-shadow batch validation requires single-sample R32 atlas, matching RGBA16F G-buffer/accumulator, and Depth32Float_Stencil8";
        return false;
    }
    if (atlas.width>std::numeric_limits<std::uint32_t>::max()||
        atlas.height>std::numeric_limits<std::uint32_t>::max()||
        position.width>std::numeric_limits<std::uint32_t>::max()||
        position.height>std::numeric_limits<std::uint32_t>::max()||
        !finiteMatrix(descriptor.view)||!finiteMatrix(descriptor.projection)||
        !finiteMatrix(descriptor.inverseView))
    {
        error="local-shadow batch validation dimensions or camera transforms are invalid";
        return false;
    }
    for (const float value:descriptor.positionDecompressionParams)
        if (!std::isfinite(value))
        { error="local-shadow batch validation has non-finite position decompression parameters"; return false; }
    for (const auto& record:descriptor.records)
    {
        if (!xray::metal::ValidateR4LocalShadowAtlasRecord(record,descriptor.engineFrame,error))
            return false;
        if (record.smapId!=descriptor.batchId||record.atlasExtent!=atlas.width||
            record.atlasExtent!=atlas.height||record.atlasSize>atlas.width||
            record.atlasSize>atlas.height||record.atlasX>atlas.width-record.atlasSize||
            record.atlasY>atlas.height-record.atlasSize)
        {
            error="local-shadow batch record does not match the supplied atlas extent or batch";
            return false;
        }
    }
    std::map<std::uint64_t,std::vector<std::size_t>> parentRecords;
    std::set<std::uint64_t> uniqueLightIds;
    for (std::size_t index=0u;index<descriptor.records.size();++index)
    {
        const auto& record=descriptor.records[index];
        if (!uniqueLightIds.insert(record.lightIdentity).second)
        { error="local-shadow validation batch repeats a light identity"; return false; }
        parentRecords[descriptor.parentIds[index]].push_back(index);
    }
    for (std::size_t left=0u;left<descriptor.records.size();++left)
        for (std::size_t right=left+1u;right<descriptor.records.size();++right)
        {
            const auto& a=descriptor.records[left];
            const auto& b=descriptor.records[right];
            const bool overlap=a.atlasX<b.atlasX+b.atlasSize&&b.atlasX<a.atlasX+a.atlasSize&&
                a.atlasY<b.atlasY+b.atlasSize&&b.atlasY<a.atlasY+a.atlasSize;
            if (overlap)
            { error="local-shadow batch records overlap atlas tiles"; return false; }
        }
    for (const auto& [parentId,indices]:parentRecords)
    {
        (void)parentId;
        if (indices.size()==1u) continue;
        if (indices.size()!=6u)
        { error="local-shadow validation batch split a repeated parent-light group"; return false; }
        std::array<bool,6> seenFaces{};
        for (const std::size_t index:indices)
        {
            const std::uint32_t face=descriptor.records[index].face;
            if (face>=seenFaces.size()||seenFaces[face])
            { error="local-shadow validation parent group has an invalid or duplicate face"; return false; }
            seenFaces[face]=true;
        }
    }

    auto capture=std::make_shared<BatchCapture>();
    capture->descriptor=descriptor;
    capture->atlasWidth=static_cast<std::uint32_t>(atlas.width);
    capture->atlasHeight=static_cast<std::uint32_t>(atlas.height);
    capture->width=static_cast<std::uint32_t>(position.width);
    capture->height=static_cast<std::uint32_t>(position.height);
    capture->atlas.rowBytes=aligned(atlas.width*sizeof(float));
    capture->position.rowBytes=aligned(position.width*8u);
    capture->depth.rowBytes=aligned(depthStencil.width*sizeof(float));
    capture->stencil.rowBytes=aligned(depthStencil.width);
    capture->accumulation.rowBytes=aligned(accumulation.width*8u);
    if (!bufferSizeFits(capture->atlas.rowBytes,capture->atlasHeight)||
        !bufferSizeFits(capture->position.rowBytes,capture->height)||
        !bufferSizeFits(capture->depth.rowBytes,capture->height)||
        !bufferSizeFits(capture->stencil.rowBytes,capture->height)||
        !bufferSizeFits(capture->accumulation.rowBytes,capture->height))
    { error="local-shadow batch readback buffer size overflows NSUInteger"; return false; }

    id<MTLDevice> device=commandBuffer.device;
    capture->atlas.buffer=[device newBufferWithLength:capture->atlas.rowBytes*capture->atlasHeight options:MTLResourceStorageModeShared];
    capture->position.buffer=[device newBufferWithLength:capture->position.rowBytes*capture->height options:MTLResourceStorageModeShared];
    capture->depth.buffer=[device newBufferWithLength:capture->depth.rowBytes*capture->height options:MTLResourceStorageModeShared];
    capture->stencil.buffer=[device newBufferWithLength:capture->stencil.rowBytes*capture->height options:MTLResourceStorageModeShared];
    capture->accumulation.buffer=[device newBufferWithLength:capture->accumulation.rowBytes*capture->height options:MTLResourceStorageModeShared];
    if (!capture->atlas.buffer||!capture->position.buffer||!capture->depth.buffer||
        !capture->stencil.buffer||!capture->accumulation.buffer)
    { error="Metal failed to allocate local-shadow batch readback buffers"; return false; }

    id<MTLBlitCommandEncoder> blit=[commandBuffer blitCommandEncoder];
    if (!blit) { error="Metal failed to create local-shadow batch readback blit encoder"; return false; }
    [blit copyFromTexture:atlas sourceSlice:0u sourceLevel:0u sourceOrigin:MTLOriginMake(0u,0u,0u)
        sourceSize:MTLSizeMake(atlas.width,atlas.height,1u) toBuffer:capture->atlas.buffer
        destinationOffset:0u destinationBytesPerRow:capture->atlas.rowBytes
        destinationBytesPerImage:capture->atlas.rowBytes*capture->atlasHeight options:MTLBlitOptionNone];
    [blit copyFromTexture:position sourceSlice:0u sourceLevel:0u sourceOrigin:MTLOriginMake(0u,0u,0u)
        sourceSize:MTLSizeMake(position.width,position.height,1u) toBuffer:capture->position.buffer
        destinationOffset:0u destinationBytesPerRow:capture->position.rowBytes
        destinationBytesPerImage:capture->position.rowBytes*capture->height options:MTLBlitOptionNone];
    [blit copyFromTexture:depthStencil sourceSlice:0u sourceLevel:0u sourceOrigin:MTLOriginMake(0u,0u,0u)
        sourceSize:MTLSizeMake(depthStencil.width,depthStencil.height,1u) toBuffer:capture->depth.buffer
        destinationOffset:0u destinationBytesPerRow:capture->depth.rowBytes
        destinationBytesPerImage:capture->depth.rowBytes*capture->height options:MTLBlitOptionDepthFromDepthStencil];
    [blit copyFromTexture:depthStencil sourceSlice:0u sourceLevel:0u sourceOrigin:MTLOriginMake(0u,0u,0u)
        sourceSize:MTLSizeMake(depthStencil.width,depthStencil.height,1u) toBuffer:capture->stencil.buffer
        destinationOffset:0u destinationBytesPerRow:capture->stencil.rowBytes
        destinationBytesPerImage:capture->stencil.rowBytes*capture->height options:MTLBlitOptionStencilFromDepthStencil];
    [blit copyFromTexture:accumulation sourceSlice:0u sourceLevel:0u sourceOrigin:MTLOriginMake(0u,0u,0u)
        sourceSize:MTLSizeMake(accumulation.width,accumulation.height,1u) toBuffer:capture->accumulation.buffer
        destinationOffset:0u destinationBytesPerRow:capture->accumulation.rowBytes
        destinationBytesPerImage:capture->accumulation.rowBytes*capture->height options:MTLBlitOptionNone];
    [blit endEncoding];

    [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> completedBuffer) {
        const bool success=completedBuffer.status==MTLCommandBufferStatusCompleted;
        std::string reportError;
        const std::string gpuError=success?std::string():
            std::string(localizedError(completedBuffer).UTF8String ?: "Metal command buffer failed");
        if (!writeReport(*capture,success,gpuError,reportError))
            NSLog(@"[Metal] local-shadow batch validation artifact failure: %s",reportError.c_str());
    }];
    error.clear();
    return true;
}
