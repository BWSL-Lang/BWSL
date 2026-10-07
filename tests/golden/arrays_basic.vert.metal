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

constant int _31 = {};

struct main0_out
{
    float4 gl_Position [[position]];
};

struct main0_in
{
    float3 a_position [[attribute(0)]];
};

vertex main0_out main0(main0_in in [[stage_in]])
{
    main0_out out = {};
    spvUnsafeArray<float, 4> _50;
    _50[0] = 1.0;
    _50[1] = 2.0;
    _50[2] = 3.0;
    _50[3] = 4.0;
    spvUnsafeArray<int, 4> _54;
    _54[0] = 10;
    _54[1] = 20;
    _54[2] = 30;
    _54[3] = 40;
    spvUnsafeArray<float3, 3> _59;
    _59[0] = float3(1.0, 0.0, 0.0);
    _59[1] = float3(0.0, 1.0, 0.0);
    _59[2] = float3(0.0, 0.0, 1.0);
    _50[0] *= 2.0;
    _54[1] += 5;
    float _23 = 0.0;
    int _25 = 0;
    float _24;
    float _12 = _23;
    int _13 = _25;
    for (; _13 < 4; _12 = _24, _13++)
    {
        _24 = _12 + _50[_13];
    }
    float _28 = 0.0;
    int _29 = 0;
    float _14;
    int _18;
    float _15 = _28;
    int _16 = _29;
    int _17;
    for (; _16 < 3; _15 = _14, _16++, _17 = _18)
    {
        int _33 = 0;
        _14 = _15;
        _18 = _33;
        float _27;
        for (; _18 < 3; _14 = _27, _18++)
        {
            _27 = _14 + dot(_59[_16], _59[_18]);
        }
    }
    spvUnsafeArray<float, 4> _63;
    _63[0] = 1.0;
    _63[1] = 2.0;
    _63[2] = 3.0;
    _63[3] = 4.0;
    uint _35 = 0u;
    spvUnsafeArray<float, 4> _66;
    for (uint _19 = _35; _19 < 4u; _19++)
    {
        _66[_19] = _63[_19];
    }
    bool _185 = false;
    float _37 = 0.0;
    int _39 = 0;
    float _38;
    float _20 = _37;
    int _21 = _39;
    for (; (_21 < 4) && (!_185); _20 = _38, _21++)
    {
        _38 = _20 + _66[_21];
    }
    bool _200 = true;
    float _41 = 0.0;
    float _22;
    if (2 < 4)
    {
        _22 = _50[2];
    }
    else
    {
        _22 = _41;
    }
    spvUnsafeArray<float4x4, 2> _71;
    _71[0] = float4x4(float4(1.0, 0.0, 0.0, 0.0), float4(0.0, 1.0, 0.0, 0.0), float4(0.0, 0.0, 1.0, 0.0), float4(0.0, 0.0, 0.0, 1.0));
    _71[1] = float4x4(float4(2.0, 0.0, 0.0, 0.0), float4(0.0, 2.0, 0.0, 0.0), float4(0.0, 0.0, 2.0, 0.0), float4(0.0, 0.0, 0.0, 2.0));
    out.gl_Position = float4(in.a_position, 1.0);
    return out;
}

