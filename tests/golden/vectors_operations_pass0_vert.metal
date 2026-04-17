#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct main0_out
{
    float3 varying0 [[user(locn0)]];
    float4 gl_Position [[position]];
};

struct main0_in
{
    float3 m_6 [[attribute(0)]];
    float3 m_7 [[attribute(1)]];
};

vertex main0_out main0(main0_in in [[stage_in]])
{
    main0_out out = {};
    float3 _16 = float3(1.0, 0.0, 0.0);
    float3 _20 = float3(0.0, 1.0, 0.0);
    float3 _22 = float3(1.0);
    float3 _42 = float3(0.0);
    float3 _89 = float3(1.0, 0.0, 0.0);
    float2 _98 = float2(3.0, 4.0);
    float4 _108 = float4(1.0, 2.0, 3.0, 4.0);
    out.gl_Position = float4(in.m_6, 1.0);
    out.varying0 = in.m_7;
    return out;
}

