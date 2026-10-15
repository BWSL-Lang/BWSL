#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct main0_out
{
    float4 m_9 [[color(0)]];
};

struct main0_in
{
    float2 v_texcoord [[user(locn0)]];
};

fragment main0_out main0(main0_in in [[stage_in]], texture2d<float> t_albedo [[texture(2)]], texturecube<float> t_environment [[texture(3)]], texture3d<float> t_volume [[texture(4)]], texture2d_array<float> t_layers [[texture(5)]], sampler t_albedoSmplr [[sampler(0)]], sampler t_environmentSmplr [[sampler(1)]], sampler t_volumeSmplr [[sampler(2)]], sampler t_layersSmplr [[sampler(3)]])
{
    main0_out out = {};
    float3 _76 = float3(in.v_texcoord, 0.0);
    out.m_9 = ((((((float4(in.v_texcoord, fwidth(in.v_texcoord.x), 1.0) + t_albedo.sample(t_albedoSmplr, (in.v_texcoord * float2(2.0)))) + t_albedo.sample(t_albedoSmplr, (in.v_texcoord + float2(0.5)), level(-1.0))) + t_albedo.sample(t_albedoSmplr, abs((in.v_texcoord * float2(2.0)) - float2(1.0)), gradient2d(dfdx(in.v_texcoord), dfdy(in.v_texcoord)))) + t_environment.sample(t_environmentSmplr, reflect(fast::normalize(float3(0.0, 0.0, -1.0)), float3(0.0, 0.0, 1.0)))) + t_environment.sample(t_environmentSmplr, fast::normalize(float3((in.v_texcoord.x * 2.0) - 1.0, (in.v_texcoord.y * 2.0) - 1.0, 1.0)), level(2.0))) + t_volume.sample(t_volumeSmplr, float3(in.v_texcoord, 0.5))) + t_layers.sample(t_layersSmplr, _76.xy, uint(rint(_76.z)));
    return out;
}

