#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct _9
{
    packed_float3 _m0[1];
};

struct _13
{
    packed_float3 _m0[1];
};

struct main0_out
{
    float3 varying0 [[user(locn0)]];
    float4 gl_Position [[position]];
};

vertex main0_out main0(const device _9& _11 [[buffer(0)]], const device _13& _15 [[buffer(1)]], uint gl_VertexIndex [[vertex_id]])
{
    main0_out out = {};
    float3 _24 = float3(1.0, 0.0, 0.0);
    float3 _28 = float3(0.0, 1.0, 0.0);
    float3 _30 = float3(1.0);
    float3 _50 = float3(0.0);
    float3 _97 = float3(1.0, 0.0, 0.0);
    float2 _106 = float2(3.0, 4.0);
    float4 _116 = float4(1.0, 2.0, 3.0, 4.0);
    out.gl_Position = float4(_11._m0[gl_VertexIndex][0], _11._m0[gl_VertexIndex][1], _11._m0[gl_VertexIndex][2], 1.0);
    out.varying0 = float3(_15._m0[gl_VertexIndex]);
    return out;
}

