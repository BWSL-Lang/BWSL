#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

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
    float _18 = 0.0;
    if (0.5 > 0.0)
    {
        float _24 = 1.0;
    }
    float _26 = 0.0;
    if (0.5 > 0.5)
    {
        float _31 = 1.0;
    }
    else
    {
        float _32 = -1.0;
    }
    float _34 = 0.0;
    if (0.5 < 0.25)
    {
        float _40 = 0.0;
    }
    else
    {
        if (0.5 < 0.5)
        {
            float _45 = 0.25;
        }
        else
        {
            if (0.5 < 0.75)
            {
                float _51 = 0.5;
            }
            else
            {
                float _52 = 1.0;
            }
        }
    }
    float _53 = 0.0;
    if (0.5 > 0.0)
    {
        if (0.5 < 1.0)
        {
        }
        else
        {
            float _63 = 1.0;
        }
    }
    else
    {
        float _64 = 0.0;
    }
    float _68 = 0.0;
    if (0.5 > 0.300000011920928955078125)
    {
        float _71 = 1.0;
    }
    float _74 = 0.0;
    if ((0.5 > 0.0) && (0.699999988079071044921875 > 0.0))
    {
    }
    float _82 = 0.0;
    if ((0.5 < 0.0) || (0.699999988079071044921875 > 0.5))
    {
        float _88 = 1.0;
    }
    float _91 = 0.0;
    if (!false)
    {
        float _95 = 1.0;
    }
    float _96 = 0.0;
    float _97 = 0.0;
    if (0.5 > 0.0)
    {
    }
    float _109 = 0.0;
    if (2 == 0)
    {
        float _115 = 0.0;
    }
    else
    {
        if (2 == 1)
        {
            float _121 = 0.25;
        }
        else
        {
            if (2 == 2)
            {
                float _126 = 0.5;
            }
            else
            {
                if (2 == 3)
                {
                    float _132 = 0.75;
                }
                else
                {
                    float _133 = 1.0;
                }
            }
        }
    }
    out.gl_Position = float4(in.a_position, 1.0);
    return out;
}

