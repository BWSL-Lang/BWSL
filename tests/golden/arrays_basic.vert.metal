#pragma clang diagnostic ignored "-Wmissing-prototypes"
#pragma clang diagnostic ignored "-Wmissing-braces"

#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

template<typename T, size_t Num>
struct spvUnsafeArray
{
    T elements[Num ? Num : 1];
    
    thread T& operator [] (size_t pos) thread
    {
        return elements[pos];
    }
    constexpr const thread T& operator [] (size_t pos) const thread
    {
        return elements[pos];
    }
    
    device T& operator [] (size_t pos) device
    {
        return elements[pos];
    }
    constexpr const device T& operator [] (size_t pos) const device
    {
        return elements[pos];
    }
    
    constexpr const constant T& operator [] (size_t pos) const constant
    {
        return elements[pos];
    }
    
    threadgroup T& operator [] (size_t pos) threadgroup
    {
        return elements[pos];
    }
    constexpr const threadgroup T& operator [] (size_t pos) const threadgroup
    {
        return elements[pos];
    }
};

constant int _30 = {};

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
    spvUnsafeArray<float, 4> _45;
    _45[0] = 1.0;
    _45[1] = 2.0;
    _45[2] = 3.0;
    _45[3] = 4.0;
    spvUnsafeArray<int, 4> _49;
    _49[0] = 10;
    _49[1] = 20;
    _49[2] = 30;
    _49[3] = 40;
    spvUnsafeArray<float3, 3> _54;
    _54[0] = float3(1.0, 0.0, 0.0);
    _54[1] = float3(0.0, 1.0, 0.0);
    _54[2] = float3(0.0, 0.0, 1.0);
    _45[0] *= 2.0;
    _49[1] += 5;
    float _22 = 0.0;
    int _24 = 0;
    float _23;
    float _12 = _22;
    int _13 = _24;
    for (; _13 < 4; _12 = _23, _13++)
    {
        _23 = _12 + _45[_13];
    }
    float _27 = 0.0;
    int _28 = 0;
    float _14;
    int _18;
    float _15 = _27;
    int _16 = _28;
    int _17;
    for (; _16 < 3; _15 = _14, _16++, _17 = _18)
    {
        int _32 = 0;
        _14 = _15;
        _18 = _32;
        float _26;
        for (; _18 < 3; _14 = _26, _18++)
        {
            _26 = _14 + dot(_54[_16], _54[_18]);
        }
    }
    spvUnsafeArray<float, 4> _58;
    _58[0] = 1.0;
    _58[1] = 2.0;
    _58[2] = 3.0;
    _58[3] = 4.0;
    bool _168 = false;
    float _34 = 0.0;
    int _36 = 0;
    float _35;
    float _19 = _34;
    int _20 = _36;
    for (; (_20 < 4) && (!_168); _19 = _35, _20++)
    {
        _35 = _19 + _58[_20];
    }
    bool _183 = true;
    float _38 = 0.0;
    float _21;
    if (2 < 4)
    {
        _21 = _45[2];
    }
    else
    {
        _21 = _38;
    }
    spvUnsafeArray<float4x4, 2> _63;
    _63[0] = float4x4(float4(1.0), float4(0.0), float4(0.0), float4(0.0));
    _63[1] = float4x4(float4(2.0), float4(0.0), float4(0.0), float4(0.0));
    out.gl_Position = float4(in.m_6, 1.0);
    return out;
}

