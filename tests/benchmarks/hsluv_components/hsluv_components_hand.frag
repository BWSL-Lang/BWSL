#version 300 es
precision highp float;
precision highp int;

in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

// HSLuv (https://www.hsluv.org), written out per channel.

const float REF_U = 0.19783000664283681;
const float REF_V = 0.468319994938791;
const float KAPPA = 903.2962962962963;
const float EPSILON = 0.0088564516790356308;

// XYZ -> linear sRGB, by row.
const vec3 XYZ_TO_R = vec3(3.2409699419045214, -1.5373831775700935, -0.49861076029300328);
const vec3 XYZ_TO_G = vec3(-0.96924363628087983, 1.8759675015077207, 0.041555057407175613);
const vec3 XYZ_TO_B = vec3(0.055630079696993609, -0.20397695888897657, 1.0569715142428786);

float fromLinear(float c) {
    return c <= 0.0031308 ? 12.92 * c : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

float lToY(float l) {
    return l <= 8.0 ? l / KAPPA : pow((l + 16.0) / 116.0, 3.0);
}

// Distance along hue h to the edge of the sRGB gamut at lightness l.
float maxChromaForLH(float l, float h) {
    float hrad = radians(h);
    float sinH = sin(hrad);
    float cosH = cos(hrad);
    float sub1 = pow(l + 16.0, 3.0) / 1560896.0;
    float sub2 = sub1 > EPSILON ? sub1 : l / KAPPA;

    // Each sRGB channel bounds the gamut with two lines (channel = 0 and
    // channel = 1); xyzToRgb's columns give one component per channel.
    vec3 m1 = vec3(XYZ_TO_R.x, XYZ_TO_G.x, XYZ_TO_B.x);
    vec3 m2 = vec3(XYZ_TO_R.y, XYZ_TO_G.y, XYZ_TO_B.y);
    vec3 m3 = vec3(XYZ_TO_R.z, XYZ_TO_G.z, XYZ_TO_B.z);
    vec3 top1 = (284517.0 * m1 - 94839.0 * m3) * sub2;
    vec3 top2 = (838422.0 * m3 + 769860.0 * m2 + 731718.0 * m1) * l * sub2;
    vec3 bottom = (632260.0 * m3 - 126452.0 * m2) * sub2;

    float result = 1000.0;
    for (int t = 0; t < 2; t++) {
        float tf = float(t);
        vec3 b = bottom + 126452.0 * tf;
        vec3 slope = top1 / b;
        vec3 intercept = (top2 - 769860.0 * tf * l) / b;
        vec3 len = intercept / (sinH - slope * cosH);
        for (int c = 0; c < 3; c++) {
            if (len[c] >= 0.0) {
                result = min(result, len[c]);
            }
        }
    }
    return result;
}

vec3 xyzToRgb(vec3 xyz) {
    return vec3(fromLinear(dot(XYZ_TO_R, xyz)),
                fromLinear(dot(XYZ_TO_G, xyz)),
                fromLinear(dot(XYZ_TO_B, xyz)));
}

vec3 luvToXyz(vec3 luv) {
    if (luv.x == 0.0) {
        return vec3(0.0);
    }
    float u = luv.y / (13.0 * luv.x) + REF_U;
    float v = luv.z / (13.0 * luv.x) + REF_V;
    float y = lToY(luv.x);
    float x = 9.0 * u * y / (4.0 * v);
    float z = (9.0 * y - 15.0 * v * y - v * x) / (3.0 * v);
    return vec3(x, y, z);
}

vec3 lchToLuv(vec3 lch) {
    float hrad = radians(lch.z);
    return vec3(lch.x, cos(hrad) * lch.y, sin(hrad) * lch.y);
}

vec3 hsluvToLch(vec3 hsl) {
    if (hsl.z > 99.9999) {
        return vec3(100.0, 0.0, hsl.x);
    }
    if (hsl.z < 0.00001) {
        return vec3(0.0, 0.0, hsl.x);
    }
    return vec3(hsl.z, maxChromaForLH(hsl.z, hsl.x) / 100.0 * hsl.y, hsl.x);
}

vec3 hsluvToRgb(vec3 hsl) {
    return xyzToRgb(luvToXyz(lchToLuv(hsluvToLch(hsl))));
}

void main() {
    vec2 uv = v_uv;
    vec3 hueSweep = hsluvToRgb(vec3(uv.x * 360.0, 100.0, 50.0));
    vec3 saturationSweep = hsluvToRgb(vec3(180.0, uv.y * 100.0, 50.0));
    vec3 lightnessSweep = hsluvToRgb(vec3(10.0, 100.0, 5.0 + 90.0 * uv.x));
    fragColor = vec4(clamp(hueSweep * 0.5 + saturationSweep * 0.3 + lightnessSweep * 0.2, 0.0, 1.0), 1.0);
}
