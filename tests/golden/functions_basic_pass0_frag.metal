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
    float3 varying0 [[user(locn0)]];
};

fragment main0_out main0(main0_in in [[stage_in]])
{
    main0_out out = {};
    float3 _28 = fast::normalize(float3(1.0));
    float3 _30 = float3(0.0, 0.0, 1.0);
    bool _19 = false;
    float _18 = dot(fast::normalize(in.varying0), _28);
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
    float3 _51 = float3(0.039999999105930328369140625);
    bool _54 = false;
    float _55 = 1.0 - fast::max(dot(_30, fast::normalize(_28 + _30)), 0.0);
    float _57 = _55 * _55;
    float3 _66 = _51 + ((float3(1.0) - _51) * float3((_57 * _57) * _55));
    bool _68 = true;
    out.m_9 = float4((float3(_13) * (float3(1.0) - _66)) + _66, 1.0);
    return out;
}

