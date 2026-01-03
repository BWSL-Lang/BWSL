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

struct _9
{
    packed_float3 _m0[1];
};

constant int _35 = {};

struct main0_out
{
    float4 gl_Position [[position]];
};

vertex main0_out main0(const device _9& _11 [[buffer(0)]], uint gl_VertexIndex [[vertex_id]])
{
    main0_out out = {};
    spvUnsafeArray<float, 4> _49;
    _49[0] = 1.0;
    _49[1] = 2.0;
    _49[2] = 3.0;
    _49[3] = 4.0;
    spvUnsafeArray<int, 4> _53;
    _53[0] = 10;
    _53[1] = 20;
    _53[2] = 30;
    _53[3] = 40;
    spvUnsafeArray<float3, 3> _58;
    _58[0] = float3(1.0, 0.0, 0.0);
    _58[1] = float3(0.0, 1.0, 0.0);
    _58[2] = float3(0.0, 0.0, 1.0);
    _49[0] *= 2.0;
    _53[1] += 5;
    float _27 = 0.0;
    int _29 = 0;
    float _28;
    float _17 = _27;
    int _18 = _29;
    for (; _18 < 4; _17 = _28, _18++)
    {
        _28 = _17 + _49[_18];
    }
    float _32 = 0.0;
    int _33 = 0;
    float _19;
    int _23;
    float _20 = _32;
    int _21 = _33;
    int _22;
    for (; _21 < 3; _20 = _19, _21++, _22 = _23)
    {
        int _37 = 0;
        _19 = _20;
        _23 = _37;
        float _31;
        for (; _23 < 3; _19 = _31, _23++)
        {
            _31 = _19 + dot(_58[_21], _58[_23]);
        }
    }
    spvUnsafeArray<float, 4> _62;
    _62[0] = 1.0;
    _62[1] = 2.0;
    _62[2] = 3.0;
    _62[3] = 4.0;
    bool _172 = false;
    float _39 = 0.0;
    int _41 = 0;
    float _40;
    float _24 = _39;
    int _25 = _41;
    for (; (_25 < 4) && (!_172); _24 = _40, _25++)
    {
        _40 = _24 + _62[_25];
    }
    bool _187 = true;
    float _43 = 0.0;
    float _26;
    if (2 < 4)
    {
        _26 = _49[2];
    }
    else
    {
        _26 = _43;
    }
    spvUnsafeArray<float4x4, 2> _67;
    _67[0] = float4x4(float4(1.0), float4(0.0), float4(0.0), float4(0.0));
    _67[1] = float4x4(float4(2.0), float4(0.0), float4(0.0), float4(0.0));
    out.gl_Position = float4(_11._m0[gl_VertexIndex][0], _11._m0[gl_VertexIndex][1], _11._m0[gl_VertexIndex][2], 1.0);
    return out;
}

