#include <metal_stdlib>
using namespace metal;

struct SkinInput
{
    float position[3], normal[3], tangent[3], binormal[3], texcoord[2];
    uint bones[4];
    float weights[4];
};
struct SkinOutput
{
    float position[4];
    uchar normal[4], tangent[4], binormal[4];
    float texcoord[2];
};
struct SkinParams { uint vertices, influences; };
static_assert(sizeof(SkinInput)==88, "compute skin input stride");
static_assert(sizeof(SkinOutput)==36, "existing SKIN_0 output stride");

float3 load3(device const float* values) { return float3(values[0],values[1],values[2]); }
float3 transform3(device const float* matrix, float3 value, bool point)
{
    return float3(
        value.x*matrix[0]+value.y*matrix[4]+value.z*matrix[8]+(point?matrix[12]:0.0f),
        value.x*matrix[1]+value.y*matrix[5]+value.z*matrix[9]+(point?matrix[13]:0.0f),
        value.x*matrix[2]+value.y*matrix[6]+value.z*matrix[10]+(point?matrix[14]:0.0f));
}
float3 normalizedBasis(float3 value)
{
    // Match the compiled ARM64 CPU reference: y*y, FMA(x*x,+), FMA(z*z,+).
    const float length2=fma(value.z,value.z,fma(value.x,value.x,value.y*value.y));
    return length2>1.e-12f?value*(1.0f/precise::sqrt(length2)):float3(0,1,0);
}
uchar packComponent(float value)
{
    return uchar(clamp(floor((clamp(value,-1.0f,1.0f)+1.0f)*127.5f),0.0f,255.0f));
}

kernel void r4_compute_skin(device const SkinInput* input [[buffer(0)]],
    device const float* bones [[buffer(1)]], device SkinOutput* output [[buffer(2)]],
    constant SkinParams& params [[buffer(3)]],
#if R4_SKIN_DIAGNOSTIC_TRACE
    device float* trace [[buffer(4)]],
#endif
    uint vertexIndex [[thread_position_in_grid]])
{
    if (vertexIndex>=params.vertices) return;
    device const SkinInput& source=input[vertexIndex];
    float3 p=float3(0), n=float3(0), t=float3(0), b=float3(0);
    for (uint influence=0;influence<params.influences;++influence)
    {
        device const float* matrix=bones+16*source.bones[influence];
        const float weight=source.weights[influence];
        p+=transform3(matrix,load3(source.position),true)*weight;
        n+=transform3(matrix,load3(source.normal),false)*weight;
        t+=transform3(matrix,load3(source.tangent),false)*weight;
        b+=transform3(matrix,load3(source.binormal),false)*weight;
    }
#if R4_SKIN_DIAGNOSTIC_TRACE
    const float3 beforeNormal=n;
    const float normalLength2=n.x*n.x+n.y*n.y+n.z*n.z;
#endif
    n=normalizedBasis(n); t=normalizedBasis(t); b=normalizedBasis(b);
#if R4_SKIN_DIAGNOSTIC_TRACE
    const uint base=vertexIndex*7;
    trace[base]=beforeNormal.x; trace[base+1]=beforeNormal.y; trace[base+2]=beforeNormal.z;
    trace[base+3]=n.x; trace[base+4]=n.y; trace[base+5]=n.z; trace[base+6]=normalLength2;
#endif
    device SkinOutput& destination=output[vertexIndex];
    destination.position[0]=p.x; destination.position[1]=p.y;
    destination.position[2]=p.z; destination.position[3]=1;
    // MSC SKIN_0 keeps the legacy D3DCOLOR .zyx swizzle.
    destination.normal[0]=packComponent(n.z); destination.normal[1]=packComponent(n.y);
    destination.normal[2]=packComponent(n.x); destination.normal[3]=0;
    destination.tangent[0]=packComponent(t.z); destination.tangent[1]=packComponent(t.y);
    destination.tangent[2]=packComponent(t.x); destination.tangent[3]=0;
    destination.binormal[0]=packComponent(b.z); destination.binormal[1]=packComponent(b.y);
    destination.binormal[2]=packComponent(b.x); destination.binormal[3]=0;
    destination.texcoord[0]=source.texcoord[0]; destination.texcoord[1]=source.texcoord[1];
}
