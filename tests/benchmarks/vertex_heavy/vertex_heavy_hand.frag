#version 300 es
precision highp float;

in vec3 v_shade;
layout(location = 0) out vec4 fragColor;

void main() {
    fragColor = vec4(v_shade, 1.0);
}
