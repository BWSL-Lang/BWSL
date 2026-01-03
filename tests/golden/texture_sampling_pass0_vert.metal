#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct _9
{
    packed_float3 _m0[1];
};

struct _14
{
    float2 _m0[1];
};

struct main0_out
{
    float2 varying0 [[user(locn0)]];
    float4 gl_Position [[position]];
};

vertex main0_out main0(const device _9& _11 [[buffer(0)]], const device _14& _16 [[buffer(1)]], uint gl_VertexIndex [[vertex_id]])
{
    main0_out out = {};
    out.gl_Position = float4(_11._m0[gl_VertexIndex][0], _11._m0[gl_VertexIndex][1], _11._m0[gl_VertexIndex][2], 1.0);
    out.varying0 = _16._m0[gl_VertexIndex];
    return out;
}

