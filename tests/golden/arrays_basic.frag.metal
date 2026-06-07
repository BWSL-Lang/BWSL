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
    float4 m_6 [[color(0)]];
};

fragment main0_out main0()
{
    main0_out out = {};
    spvUnsafeArray<float, 4> _20;
    _20[0] = 0.100000001490116119384765625;
    _20[1] = 0.20000000298023223876953125;
    _20[2] = 0.300000011920928955078125;
    _20[3] = 0.4000000059604644775390625;
    spvUnsafeArray<float3, 4> _25;
    _25[0] = float3(1.0, 0.0, 0.0);
    _25[1] = float3(0.0, 1.0, 0.0);
    _25[2] = float3(0.0, 0.0, 1.0);
    _25[3] = float3(1.0);
    int _13 = 0;
    float3 _9;
    _9 = float3(0.0);
    float3 _12;
    for (int _10 = _13; _10 < 4; _9 = _12, _10++)
    {
        _12 = _9 + (_25[_10] * float3(_20[_10]));
    }
    out.m_6 = float4(_9, 1.0);
    return out;
}

