#version 300 es
precision highp float;
precision highp int;

layout(std140) uniform ub_params {
    vec4 shift;   // hue shift (degrees), saturation scale
} params;

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
// Linear sRGB -> XYZ, by row.
const vec3 RGB_TO_X = vec3(0.41239079926595948, 0.35758433938387796, 0.18048078840183429);
const vec3 RGB_TO_Y = vec3(0.21263900587151036, 0.71516867876775593, 0.072192315360733715);
const vec3 RGB_TO_Z = vec3(0.019330818715591851, 0.11919477979462599, 0.95053215224966058);

float fromLinear(float c) {
    return c <= 0.0031308 ? 12.92 * c : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

float toLinear(float c) {
    return c > 0.04045 ? pow((c + 0.055) / 1.055, 2.4) : c / 12.92;
}

float yToL(float y) {
    return y <= EPSILON ? y * KAPPA : 116.0 * pow(y, 1.0 / 3.0) - 16.0;
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

vec3 rgbToXyz(vec3 rgb) {
    vec3 lin = vec3(toLinear(rgb.r), toLinear(rgb.g), toLinear(rgb.b));
    return vec3(dot(RGB_TO_X, lin), dot(RGB_TO_Y, lin), dot(RGB_TO_Z, lin));
}

vec3 xyzToLuv(vec3 xyz) {
    float l = yToL(xyz.y);
    if (l == 0.0) {
        return vec3(0.0);
    }
    float div = 1.0 / (xyz.x + 15.0 * xyz.y + 3.0 * xyz.z);
    return vec3(l, 13.0 * l * (4.0 * xyz.x * div - REF_U), 13.0 * l * (9.0 * xyz.y * div - REF_V));
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

vec3 luvToLch(vec3 luv) {
    float c = length(luv.yz);
    float h = 0.0;
    if (c > 1e-4) {
        h = degrees(atan(luv.z, luv.y));
        if (h < 0.0) {
            h += 360.0;
        }
    }
    return vec3(luv.x, c, h);
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

vec3 lchToHsluv(vec3 lch) {
    if (lch.x > 99.9999) {
        return vec3(lch.z, 0.0, 100.0);
    }
    if (lch.x < 0.00001) {
        return vec3(lch.z, 0.0, 0.0);
    }
    return vec3(lch.z, lch.y / maxChromaForLH(lch.x, lch.z) * 100.0, lch.x);
}

vec3 hsluvToRgb(vec3 hsl) {
    return xyzToRgb(luvToXyz(lchToLuv(hsluvToLch(hsl))));
}

vec3 rgbToHsluv(vec3 rgb) {
    return lchToHsluv(luvToLch(xyzToLuv(rgbToXyz(rgb))));
}

// Lightness at which a hue's most saturated color is reached, sampled every
// 5 degrees.
float maxLightnessForHue(float hue) {
    float lightness[72] = float[72](
        54.5, 54.0, 53.5, 56.0, 60.2, 63.8,
        67.0, 69.9, 72.5, 75.1, 77.5, 80.0,
        82.4, 84.9, 87.5, 90.2, 93.2, 96.5,
        96.2, 95.2, 94.2, 93.1, 92.0, 90.9,
        89.7, 88.5, 87.9, 88.3, 88.7, 89.0,
        89.2, 89.5, 89.7, 89.9, 90.1, 90.4,
        90.6, 90.8, 91.0, 89.9, 87.8, 85.7,
        83.6, 81.5, 79.3, 77.0, 74.5, 71.8,
        68.7, 65.1, 60.8, 55.2, 47.7, 35.6,
        36.7, 41.0, 44.6, 47.9, 50.8, 53.6,
        56.3, 58.9, 59.9, 59.2, 58.5, 57.9,
        57.3, 56.8, 56.3, 55.8, 55.4, 54.9);
    float index = mod(hue, 360.0) / 5.0;
    int i0 = int(index);
    int i1 = (i0 + 1) % 72;
    return mix(lightness[i0], lightness[i1], fract(index));
}

void main() {
    vec2 uv = v_uv;
    vec3 hsl = vec3(uv.x * 360.0, 20.0 + 75.0 * uv.y, 10.0 + 80.0 * fract(uv.x * 7.0 + uv.y * 3.0));
    vec3 rgb = hsluvToRgb(hsl);

    vec3 back = rgbToHsluv(rgb);
    back.x = mod(back.x + params.shift.x, 360.0);
    back.y = clamp(back.y * params.shift.y, 0.0, 100.0);
    vec3 shifted = hsluvToRgb(back);

    float maxL = maxLightnessForHue(back.x);
    fragColor = vec4(clamp(mix(rgb, shifted, 0.5), 0.0, 1.0), maxL / 100.0);
}
