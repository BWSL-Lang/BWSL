#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct main0_out
{
    float4 m_9 [[color(0)]];
};

struct main0_in
{
    float3 v_normal [[user(locn0)]];
};

fragment main0_out main0(main0_in in [[stage_in]])
{
    main0_out out = {};
    float _14 = dot(fast::normalize(in.v_normal), float3(0.57735025882720947265625));
    float _12;
    if (_14 < 0.0)
    {
        float _13 = 0.0;
        _12 = _13;
    }
    else
    {
        _12 = _14;
    }
    out.m_9 = float4((float3(_12) * float3(0.9599831104278564453125)) + float3(0.0400168634951114654541015625), 1.0);
    return out;
}

