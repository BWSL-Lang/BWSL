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
    spvUnsafeArray<float, 4> _46;
    _46[0] = 1.0;
    _46[1] = 2.0;
    _46[2] = 3.0;
    _46[3] = 4.0;
    spvUnsafeArray<int, 4> _50;
    _50[0] = 10;
    _50[1] = 20;
    _50[2] = 30;
    _50[3] = 40;
    spvUnsafeArray<float3, 3> _55;
    _55[0] = float3(1.0, 0.0, 0.0);
    _55[1] = float3(0.0, 1.0, 0.0);
    _55[2] = float3(0.0, 0.0, 1.0);
    _46[0] *= 2.0;
    _50[1] += 5;
    float _21 = 0.0;
    int _23 = 0;
    float _22;
    float _12 = _21;
    int _13 = _23;
    for (; _13 < 4; _12 = _22, _13++)
    {
        _22 = _12 + _46[_13];
    }
    float _26 = 0.0;
    int _27 = 0;
    float _37 = _26;
    for (int _16 = _27; _16 < 3; _16++)
    {
        int _29 = 0;
        float _25;
        for (int _17 = _29; _17 < 3; _37 = _25, _17++)
        {
            _25 = _37 + dot(_55[_16], _55[_17]);
        }
    }
    spvUnsafeArray<float, 4> _59;
    _59[0] = 1.0;
    _59[1] = 2.0;
    _59[2] = 3.0;
    _59[3] = 4.0;
    uint _31 = 0u;
    spvUnsafeArray<float, 4> _62;
    for (uint _18 = _31; _18 < 4u; _18++)
    {
        _62[_18] = _59[_18];
    }
    float _33 = 0.0;
    int _35 = 0;
    float _34;
    float _19 = _33;
    int _20 = _35;
    for (; _20 < 4; _19 = _34, _20++)
    {
        _34 = _19 + _62[_20];
    }
    int _173 = 2;
    spvUnsafeArray<float4x4, 2> _67;
    _67[0] = float4x4(float4(1.0, 0.0, 0.0, 0.0), float4(0.0, 1.0, 0.0, 0.0), float4(0.0, 0.0, 1.0, 0.0), float4(0.0, 0.0, 0.0, 1.0));
    _67[1] = float4x4(float4(2.0, 0.0, 0.0, 0.0), float4(0.0, 2.0, 0.0, 0.0), float4(0.0, 0.0, 2.0, 0.0), float4(0.0, 0.0, 0.0, 2.0));
    out.gl_Position = float4(in.a_position, 1.0);
    return out;
}

