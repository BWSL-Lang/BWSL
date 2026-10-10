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
    float _39 = 0.5;
    float _31 = 0.0;
    float _12;
    if (_39 > 0.0)
    {
        float _32 = 1.0;
        _12 = _32;
    }
    else
    {
        _12 = _31;
    }
    float _74 = 0.0;
    float _13;
    if (_39 > 0.5)
    {
        float _33 = 1.0;
        _13 = _33;
    }
    else
    {
        float _34 = -1.0;
        _13 = _34;
    }
    float _80 = 0.0;
    float _14;
    if (_39 < 0.25)
    {
        float _35 = 0.0;
        _14 = _35;
    }
    else
    {
        float _15;
        if (_39 < 0.5)
        {
            float _36 = 0.25;
            _15 = _36;
        }
        else
        {
            float _16;
            if (_39 < 0.75)
            {
                float _37 = 0.5;
                _16 = _37;
            }
            else
            {
                float _38 = 1.0;
                _16 = _38;
            }
            _15 = _16;
        }
        _14 = _15;
    }
    float _95 = 0.0;
    float _18;
    if (_39 > 0.0)
    {
        float _17;
        if (_39 < 1.0)
        {
            _17 = _39;
        }
        else
        {
            float _40 = 1.0;
            _17 = _40;
        }
        _18 = _17;
    }
    else
    {
        float _41 = 0.0;
        _18 = _41;
    }
    float _42 = 0.0;
    float _19;
    if (_39 > 0.300000011920928955078125)
    {
        float _43 = 1.0;
        _19 = _43;
    }
    else
    {
        _19 = _42;
    }
    float _44 = 0.0;
    bool _46 = _39 > 0.0;
    bool _21;
    if (_46)
    {
        _21 = 0.699999988079071044921875 > 0.0;
    }
    else
    {
        _21 = _46;
    }
    float _20;
    if (_21)
    {
        _20 = _39 * 0.699999988079071044921875;
    }
    else
    {
        _20 = _44;
    }
    float _48 = 0.0;
    bool _50 = _39 < 0.0;
    bool _23;
    if (_50)
    {
        _23 = _50;
    }
    else
    {
        _23 = 0.699999988079071044921875 > 0.5;
    }
    float _22;
    if (_23)
    {
        float _49 = 1.0;
        _22 = _49;
    }
    else
    {
        _22 = _48;
    }
    float _52 = 0.0;
    float _24;
    if (!false)
    {
        float _53 = 1.0;
        _24 = _53;
    }
    else
    {
        _24 = _52;
    }
    float _54 = 0.0;
    float _56 = 0.0;
    float _25;
    float _26;
    if (_39 > 0.0)
    {
        float _57 = _39 * 2.0;
        _25 = _57 + 1.0;
        _26 = _57;
    }
    else
    {
        _25 = _54;
        _26 = _56;
    }
    float _137 = 0.0;
    float _27;
    if (2 == 0)
    {
        float _58 = 0.0;
        _27 = _58;
    }
    else
    {
        float _28;
        if (2 == 1)
        {
            float _59 = 0.25;
            _28 = _59;
        }
        else
        {
            float _29;
            if (2 == 2)
            {
                float _60 = 0.5;
                _29 = _60;
            }
            else
            {
                float _30;
                if (2 == 3)
                {
                    float _61 = 0.75;
                    _30 = _61;
                }
                else
                {
                    float _62 = 1.0;
                    _30 = _62;
                }
                _29 = _30;
            }
            _28 = _29;
        }
        _27 = _28;
    }
    out.gl_Position = float4(in.a_position, 1.0);
    return out;
}

