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
    float _42 = 0.5;
    float _34 = 0.0;
    float _17;
    if (_42 > 0.0)
    {
        float _35 = 1.0;
        _17 = _35;
    }
    else
    {
        _17 = _34;
    }
    float _70 = 0.0;
    float _18;
    if (_42 > 0.5)
    {
        float _36 = 1.0;
        _18 = _36;
    }
    else
    {
        _18 = -1.0;
    }
    float _76 = 0.0;
    float _19;
    if (_42 < 0.25)
    {
        float _38 = 0.0;
        _19 = _38;
    }
    else
    {
        float _20;
        if (_42 < 0.5)
        {
            float _39 = 0.25;
            _20 = _39;
        }
        else
        {
            float _21;
            if (_42 < 0.75)
            {
                float _40 = 0.5;
                _21 = _40;
            }
            else
            {
                float _41 = 1.0;
                _21 = _41;
            }
            _20 = _21;
        }
        _19 = _20;
    }
    float _91 = 0.0;
    float _23;
    if (_42 > 0.0)
    {
        float _22;
        if (_42 < 1.0)
        {
            _22 = _42;
        }
        else
        {
            float _43 = 1.0;
            _22 = _43;
        }
        _23 = _22;
    }
    else
    {
        float _44 = 0.0;
        _23 = _44;
    }
    float _45 = 0.0;
    float _24;
    if (_42 > 0.300000011920928955078125)
    {
        float _46 = 1.0;
        _24 = _46;
    }
    else
    {
        _24 = _45;
    }
    float _47 = 0.0;
    float _25;
    if ((_42 > 0.0) && (0.699999988079071044921875 > 0.0))
    {
        _25 = _42 * 0.699999988079071044921875;
    }
    else
    {
        _25 = _47;
    }
    float _49 = 0.0;
    float _26;
    if ((_42 < 0.0) || (0.699999988079071044921875 > 0.5))
    {
        float _50 = 1.0;
        _26 = _50;
    }
    else
    {
        _26 = _49;
    }
    float _51 = 0.0;
    float _27;
    if (!false)
    {
        float _52 = 1.0;
        _27 = _52;
    }
    else
    {
        _27 = _51;
    }
    float _53 = 0.0;
    float _55 = 0.0;
    float _28;
    float _29;
    if (_42 > 0.0)
    {
        float _56 = _42 * 2.0;
        _28 = _56 + 1.0;
        _29 = _56;
    }
    else
    {
        _28 = _53;
        _29 = _55;
    }
    float _133 = 0.0;
    float _30;
    if (2 == 0)
    {
        float _57 = 0.0;
        _30 = _57;
    }
    else
    {
        float _31;
        if (2 == 1)
        {
            float _58 = 0.25;
            _31 = _58;
        }
        else
        {
            float _32;
            if (2 == 2)
            {
                float _59 = 0.5;
                _32 = _59;
            }
            else
            {
                float _33;
                if (2 == 3)
                {
                    float _60 = 0.75;
                    _33 = _60;
                }
                else
                {
                    float _61 = 1.0;
                    _33 = _61;
                }
                _32 = _33;
            }
            _31 = _32;
        }
        _30 = _31;
    }
    out.gl_Position = float4(_11._m0[gl_VertexIndex][0], _11._m0[gl_VertexIndex][1], _11._m0[gl_VertexIndex][2], 1.0);
    return out;
}

