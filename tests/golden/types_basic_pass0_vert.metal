#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct _9
{
    packed_float3 _m0[1];
};

struct main0_out
{
    float4 gl_Position [[position]];
};

vertex main0_out main0(const device _9& _11 [[buffer(0)]], uint gl_VertexIndex [[vertex_id]])
{
    main0_out out = {};
    float2 _50 = float2(1.0, 2.0);
    out.gl_Position = float4(_11._m0[gl_VertexIndex][0], _11._m0[gl_VertexIndex][1], _11._m0[gl_VertexIndex][2], 1.0);
    return out;
}

