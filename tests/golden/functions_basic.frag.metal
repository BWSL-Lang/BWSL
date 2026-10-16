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
    float3 _24 = fast::normalize(float3(1.0));
    float _14 = dot(fast::normalize(in.v_normal), _24);
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
    float _50 = 1.0 - fast::max(dot(float3(0.0, 0.0, 1.0), fast::normalize(_24 + float3(0.0, 0.0, 1.0))), 0.0);
    float _52 = _50 * _50;
    float3 _61 = float3(0.039999999105930328369140625) + (float3(0.959999978542327880859375) * float3((_52 * _52) * _50));
    out.m_9 = float4((float3(_12) * (float3(1.0) - _61)) + _61, 1.0);
    return out;
}

