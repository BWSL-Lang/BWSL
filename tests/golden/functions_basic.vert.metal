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
    float _48 = 2.0 + 3.0;
    float3 _74 = float3(1.0, 0.5, 0.25);
    float _92 = 1.0 + 2.0;
    out.gl_Position = float4(in.a_position, 1.0);
    out.v_normal = in.a_normal;
    return out;
}

