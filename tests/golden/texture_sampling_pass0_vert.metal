#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct main0_out
{
    float2 varying0 [[user(locn0)]];
    float4 gl_Position [[position]];
};

struct main0_in
{
    float3 m_6 [[attribute(0)]];
    float2 m_9 [[attribute(1)]];
};

vertex main0_out main0(main0_in in [[stage_in]])
{
    main0_out out = {};
    out.gl_Position = float4(in.m_6, 1.0);
    out.varying0 = in.m_9;
    return out;
}

