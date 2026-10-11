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
    float _24 = 0.0;
    if (0.5 > 0.0)
    {
        float _30 = 1.0;
    }
    float _32 = 0.0;
    if (0.5 > 0.5)
    {
        float _37 = 1.0;
    }
    else
    {
        float _38 = -1.0;
    }
    float _40 = 0.0;
    if (0.5 < 0.25)
    {
        float _46 = 0.0;
    }
    else
    {
        if (0.5 < 0.5)
        {
            float _51 = 0.25;
        }
        else
        {
            if (0.5 < 0.75)
            {
                float _57 = 0.5;
            }
            else
            {
                float _58 = 1.0;
            }
        }
    }
    float _59 = 0.0;
    if (0.5 > 0.0)
    {
        if (0.5 < 1.0)
        {
        }
        else
        {
            float _69 = 1.0;
        }
    }
    else
    {
        float _70 = 0.0;
    }
    float _74 = 0.0;
    if (0.5 > 0.300000011920928955078125)
    {
        float _77 = 1.0;
    }
    float _80 = 0.0;
    bool _14 = 0.5 > 0.0;
    bool _12;
    if (_14)
    {
        _12 = 0.699999988079071044921875 > 0.0;
    }
    else
    {
        _12 = _14;
    }
    if (_12)
    {
    }
    float _88 = 0.0;
    bool _16 = 0.5 < 0.0;
    bool _13;
    if (_16)
    {
        _13 = _16;
    }
    else
    {
        _13 = 0.699999988079071044921875 > 0.5;
    }
    if (_13)
    {
        float _94 = 1.0;
    }
    float _97 = 0.0;
    if (!false)
    {
        float _101 = 1.0;
    }
    float _102 = 0.0;
    float _103 = 0.0;
    if (0.5 > 0.0)
    {
    }
    float _115 = 0.0;
    if (2 == 0)
    {
        float _121 = 0.0;
    }
    else
    {
        if (2 == 1)
        {
            float _127 = 0.25;
        }
        else
        {
            if (2 == 2)
            {
                float _132 = 0.5;
            }
            else
            {
                if (2 == 3)
                {
                    float _138 = 0.75;
                }
                else
                {
                    float _139 = 1.0;
                }
            }
        }
    }
    out.gl_Position = float4(in.a_position, 1.0);
    return out;
}

