#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct main0_out
{
    float4 m_6 [[color(0)]];
};

fragment main0_out main0()
{
    main0_out out = {};
    float3 _9;
    if (0.5 > 0.5)
    {
        _9 = float3(1.0, 0.0, 0.0);
    }
    else
    {
        _9 = float3(0.0, 0.0, 1.0);
    }
    out.m_6 = float4(_9, 1.0);
    return out;
}

