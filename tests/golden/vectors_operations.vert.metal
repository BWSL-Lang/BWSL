#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct main0_out
{
    float3 v_normal [[user(locn0)]];
    float4 gl_Position [[position]];
};

struct main0_in
{
    float3 a_position [[attribute(0)]];
    float3 a_normal [[attribute(1)]];
};

vertex main0_out main0(main0_in in [[stage_in]])
{
    main0_out out = {};
    float3 _19 = float3(1.0, 0.0, 0.0);
    float3 _23 = float3(0.0, 1.0, 0.0);
    float3 _25 = float3(1.0);
    float3 _45 = float3(0.0);
    float3 _92 = float3(1.0, 0.0, 0.0);
    float2 _101 = float2(3.0, 4.0);
    float4 _111 = float4(1.0, 2.0, 3.0, 4.0);
    out.gl_Position = float4(in.a_position, 1.0);
    out.v_normal = in.a_normal;
    return out;
}

