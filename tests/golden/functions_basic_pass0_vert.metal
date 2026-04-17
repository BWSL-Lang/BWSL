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
    float _38 = 2.0 + 3.0;
    float3 _56 = float3(1.0, 0.5, 0.25);
    float _74 = 1.0 + 2.0;
    out.gl_Position = float4(in.m_6, 1.0);
    out.varying0 = in.m_7;
    return out;
}

