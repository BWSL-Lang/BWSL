#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

constant float _16 = {};

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
    float3 _31 = fast::normalize(float3(1.0));
    float3 _33 = float3(0.0, 0.0, 1.0);
    bool _19 = false;
    float _18 = dot(fast::normalize(in.v_normal), _31);
    float _12;
    bool _14;
    if (_18 < 0.0)
    {
        float _17 = 0.0;
        bool _20 = true;
        _12 = _17;
        _14 = _20;
    }
    else
    {
        _12 = _16;
        _14 = _19;
    }
    float _13;
    bool _15;
    if (_14)
    {
        _13 = _12;
        _15 = _14;
    }
    else
    {
        bool _21 = true;
        _13 = _18;
        _15 = _21;
    }
    float3 _56 = float3(0.039999999105930328369140625);
    bool _61 = false;
    float _62 = 1.0 - fast::max(dot(_33, fast::normalize(_31 + _33)), 0.0);
    float _64 = _62 * _62;
    float3 _73 = _56 + ((float3(1.0) - _56) * float3((_64 * _64) * _62));
    bool _75 = true;
    out.m_9 = float4((float3(_13) * (float3(1.0) - _73)) + _73, 1.0);
    return out;
}

