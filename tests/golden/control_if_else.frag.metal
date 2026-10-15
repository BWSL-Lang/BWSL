#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct main0_out
{
    float4 m_8 [[color(0)]];
};

fragment main0_out main0(float4 gl_FragCoord [[position]])
{
    main0_out out = {};
    float3 _11;
    if (gl_FragCoord.x > 0.5)
    {
        _11 = float3(1.0, 0.0, 0.0);
    }
    else
    {
        _11 = float3(0.0, 0.0, 1.0);
    }
    out.m_8 = float4(_11, 1.0);
    return out;
}

