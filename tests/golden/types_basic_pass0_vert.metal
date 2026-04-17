#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct main0_out
{
    float4 gl_Position [[position]];
};

struct main0_in
{
    float3 m_6 [[attribute(0)]];
};

vertex main0_out main0(main0_in in [[stage_in]])
{
    main0_out out = {};
    float2 _46 = float2(1.0, 2.0);
    out.gl_Position = float4(in.m_6, 1.0);
    return out;
}

