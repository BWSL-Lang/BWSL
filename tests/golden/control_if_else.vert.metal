#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct main0_out
{
    float4 gl_Position [[position]];
};

struct main0_in
{
    float3 m_6 [[attribute(0)]];
};

vertex main0_out main0(main0_in in [[stage_in]])
{
    main0_out out = {};
    float _37 = 0.5;
    float _29 = 0.0;
    float _12;
    if (_37 > 0.0)
    {
        float _30 = 1.0;
        _12 = _30;
    }
    else
    {
        _12 = _29;
    }
    float _65 = 0.0;
    float _13;
    if (_37 > 0.5)
    {
        float _31 = 1.0;
        _13 = _31;
    }
    else
    {
        _13 = -1.0;
    }
    float _71 = 0.0;
    float _14;
    if (_37 < 0.25)
    {
        float _33 = 0.0;
        _14 = _33;
    }
    else
    {
        float _15;
        if (_37 < 0.5)
        {
            float _34 = 0.25;
            _15 = _34;
        }
        else
        {
            float _16;
            if (_37 < 0.75)
            {
                float _35 = 0.5;
                _16 = _35;
            }
            else
            {
                float _36 = 1.0;
                _16 = _36;
            }
            _15 = _16;
        }
        _14 = _15;
    }
    float _86 = 0.0;
    float _18;
    if (_37 > 0.0)
    {
        float _17;
        if (_37 < 1.0)
        {
            _17 = _37;
        }
        else
        {
            float _38 = 1.0;
            _17 = _38;
        }
        _18 = _17;
    }
    else
    {
        float _39 = 0.0;
        _18 = _39;
    }
    float _40 = 0.0;
    float _19;
    if (_37 > 0.300000011920928955078125)
    {
        float _41 = 1.0;
        _19 = _41;
    }
    else
    {
        _19 = _40;
    }
    float _42 = 0.0;
    float _20;
    if ((_37 > 0.0) && (0.699999988079071044921875 > 0.0))
    {
        _20 = _37 * 0.699999988079071044921875;
    }
    else
    {
        _20 = _42;
    }
    float _44 = 0.0;
    float _21;
    if ((_37 < 0.0) || (0.699999988079071044921875 > 0.5))
    {
        float _45 = 1.0;
        _21 = _45;
    }
    else
    {
        _21 = _44;
    }
    float _46 = 0.0;
    float _22;
    if (!false)
    {
        float _47 = 1.0;
        _22 = _47;
    }
    else
    {
        _22 = _46;
    }
    float _48 = 0.0;
    float _50 = 0.0;
    float _23;
    float _24;
    if (_37 > 0.0)
    {
        float _51 = _37 * 2.0;
        _23 = _51 + 1.0;
        _24 = _51;
    }
    else
    {
        _23 = _48;
        _24 = _50;
    }
    float _128 = 0.0;
    float _25;
    if (2 == 0)
    {
        float _52 = 0.0;
        _25 = _52;
    }
    else
    {
        float _26;
        if (2 == 1)
        {
            float _53 = 0.25;
            _26 = _53;
        }
        else
        {
            float _27;
            if (2 == 2)
            {
                float _54 = 0.5;
                _27 = _54;
            }
            else
            {
                float _28;
                if (2 == 3)
                {
                    float _55 = 0.75;
                    _28 = _55;
                }
                else
                {
                    float _56 = 1.0;
                    _28 = _56;
                }
                _27 = _28;
            }
            _26 = _27;
        }
        _25 = _26;
    }
    out.gl_Position = float4(in.m_6, 1.0);
    return out;
}

