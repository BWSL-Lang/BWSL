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
    float _36 = in.a_position.x;
    float _28 = 0.0;
    bool _61 = _36 > 0.0;
    float _12;
    if (_61)
    {
        float _29 = 1.0;
        _12 = _29;
    }
    else
    {
        _12 = _28;
    }
    float _13;
    if (_36 > 0.5)
    {
        float _30 = 1.0;
        _13 = _30;
    }
    else
    {
        float _31 = -1.0;
        _13 = _31;
    }
    float _14;
    if (_36 < 0.25)
    {
        float _32 = 0.0;
        _14 = _32;
    }
    else
    {
        float _15;
        if (_36 < 0.5)
        {
            float _33 = 0.25;
            _15 = _33;
        }
        else
        {
            float _16;
            if (_36 < 0.75)
            {
                float _34 = 0.5;
                _16 = _34;
            }
            else
            {
                float _35 = 1.0;
                _16 = _35;
            }
            _15 = _16;
        }
        _14 = _15;
    }
    float _18;
    if (_61)
    {
        float _17;
        if (_36 < 1.0)
        {
            _17 = _36;
        }
        else
        {
            float _37 = 1.0;
            _17 = _37;
        }
        _18 = _17;
    }
    else
    {
        float _38 = 0.0;
        _18 = _38;
    }
    float _39 = 0.0;
    float _19;
    if (_36 > 0.300000011920928955078125)
    {
        float _40 = 1.0;
        _19 = _40;
    }
    else
    {
        _19 = _39;
    }
    float _41 = 0.0;
    float _20;
    if (_61 && (in.a_position.y > 0.0))
    {
        _20 = _36 * in.a_position.y;
    }
    else
    {
        _20 = _41;
    }
    float _43 = 0.0;
    float _21;
    if ((_36 < 0.0) || (in.a_position.y > 0.5))
    {
        float _44 = 1.0;
        _21 = _44;
    }
    else
    {
        _21 = _43;
    }
    float _45 = 0.0;
    float _22;
    if (!(in.a_position.z > 1.0))
    {
        float _46 = 1.0;
        _22 = _46;
    }
    else
    {
        _22 = _45;
    }
    float _47 = 0.0;
    float _23;
    if (_61)
    {
        _23 = (_36 * 2.0) + 1.0;
    }
    else
    {
        _23 = _47;
    }
    int _126 = int(in.a_position.z);
    float _24;
    if (_126 == 0)
    {
        float _49 = 0.0;
        _24 = _49;
    }
    else
    {
        float _25;
        if (_126 == 1)
        {
            float _50 = 0.25;
            _25 = _50;
        }
        else
        {
            float _26;
            if (_126 == 2)
            {
                float _51 = 0.5;
                _26 = _51;
            }
            else
            {
                float _27;
                if (_126 == 3)
                {
                    float _52 = 0.75;
                    _27 = _52;
                }
                else
                {
                    float _53 = 1.0;
                    _27 = _53;
                }
                _26 = _27;
            }
            _25 = _26;
        }
        _24 = _25;
    }
    out.gl_Position = float4(in.a_position, 1.0 + ((((((((((_12 + _13) + _14) + _18) + _19) + _20) + _21) + _22) + _23) + _24) * 0.001000000047497451305389404296875));
    return out;
}

