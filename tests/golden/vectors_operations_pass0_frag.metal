#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct main0_out
{
    float4 m_9 [[color(0)]];
};

struct main0_in
{
    float3 varying0 [[user(locn0)]];
};

fragment main0_out main0(main0_in in [[stage_in]])
{
    main0_out out = {};
    out.m_9 = float4(float3(fast::max(dot(fast::normalize(in.varying0), fast::normalize(float3(1.0))), 0.0)), 1.0);
    return out;
}

