package com.bwsl.plugin

enum class BwslOutputFormat(val displayName: String, val flag: String?) {
    SPIRV_ONLY("SPIR-V", null),
    ALL("All formats", "-all"),
    METAL("Metal", "-metal"),
    HLSL("HLSL", "-hlsl"),
    GLSL("GLSL 450", "-glsl"),
    GLES("GLSL ES / WebGL", "-gles"),
}
