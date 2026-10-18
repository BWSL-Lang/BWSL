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
    spvUnsafeArray<float, 4> _34;
    _34[0] = 1.0;
    _34[1] = 2.0;
    _34[2] = 3.0;
    _34[3] = 4.0;
    spvUnsafeArray<int, 4> _39;
    _39[0] = 10;
    _39[1] = 20;
    _39[2] = 30;
    _39[3] = 40;
    spvUnsafeArray<float3, 3> _44;
    _44[0] = float3(1.0, 0.0, 0.0);
    _44[1] = float3(0.0, 1.0, 0.0);
    _44[2] = float3(0.0, 0.0, 1.0);
    _34[0] *= 2.0;
    _39[1] += 5;
    int _17 = 0;
    for (int _12 = _17; _12 < 4; _12++)
    {
    }
    int _19 = 0;
    for (int _13 = _19; _13 < 3; _13++)
    {
        int _21 = 0;
        for (int _14 = _21; _14 < 3; _14++)
        {
        }
    }
    spvUnsafeArray<float, 4> _48;
    _48[0] = 1.0;
    _48[1] = 2.0;
    _48[2] = 3.0;
    _48[3] = 4.0;
    uint _23 = 0u;
    spvUnsafeArray<float, 4> _51;
    for (uint _15 = _23; _15 < 4u; _15++)
    {
        _51[_15] = _48[_15];
    }
    int _25 = 0;
    for (int _16 = _25; _16 < 4; _16++)
    {
    }
    int _157 = 2;
    spvUnsafeArray<float4x4, 2> _56;
    _56[0] = float4x4(float4(1.0, 0.0, 0.0, 0.0), float4(0.0, 1.0, 0.0, 0.0), float4(0.0, 0.0, 1.0, 0.0), float4(0.0, 0.0, 0.0, 1.0));
    _56[1] = float4x4(float4(2.0, 0.0, 0.0, 0.0), float4(0.0, 2.0, 0.0, 0.0), float4(0.0, 0.0, 2.0, 0.0), float4(0.0, 0.0, 0.0, 2.0));
    out.gl_Position = float4(in.a_position, 1.0);
    return out;
}

