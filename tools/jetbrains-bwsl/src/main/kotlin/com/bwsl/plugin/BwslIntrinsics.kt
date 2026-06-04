package com.bwsl.plugin

enum class IntrinsicFlag { FRAGMENT_ONLY, VERTEX_ONLY, TEXTURE_OP, WAVE_OP, ATOMIC_OP }

data class IntrinsicParam(val type: String, val name: String)

data class IntrinsicFunction(
    val name: String,
    val returnType: String,
    val params: List<IntrinsicParam>,
    val minParams: Int = params.size,
    val maxParams: Int = params.size,
    val flags: Set<IntrinsicFlag> = emptySet(),
    val description: String = ""
)

private fun p(type: String, name: String) = IntrinsicParam(type, name)
private fun fn(
    name: String, ret: String, vararg params: IntrinsicParam,
    min: Int = params.size, max: Int = params.size,
    flags: Set<IntrinsicFlag> = emptySet(), desc: String = ""
) = IntrinsicFunction(name, ret, params.toList(), min, max, flags, desc)

object BwslIntrinsics {

    val ALL: List<IntrinsicFunction> = listOf(

        // ── Math ────────────────────────────────────────────────────────────
        fn("lerp",       "floatN", p("floatN","a"), p("floatN","b"), p("scalar","t"),           desc = "Linear interpolation a*(1-t) + b*t"),
        fn("smoothstep", "floatN", p("scalar","edge0"), p("scalar","edge1"), p("floatN","x"),   desc = "Smooth Hermite interpolation"),
        fn("saturate",   "floatN", p("floatN","x"),                                             desc = "Clamp to [0, 1]"),
        fn("fract",      "floatN", p("floatN","x"),                                             desc = "Fractional part of x"),
        fn("step",       "floatN", p("floatN","edge"), p("floatN","x"),                         desc = "0 if x < edge, else 1"),
        fn("clamp",      "numeric",p("numeric","x"), p("numeric","min"), p("numeric","max"),    desc = "Clamp x to [min, max]"),
        fn("sign",       "numeric",p("numeric","x"),                                            desc = "Sign of x: -1, 0, or 1"),
        fn("abs",        "numeric",p("numeric","x"),                                            desc = "Absolute value"),
        fn("min",        "numeric",p("numeric","a"), p("numeric","b"),   min = 2, max = 255,    desc = "Minimum of two or more values"),
        fn("max",        "numeric",p("numeric","a"), p("numeric","b"),   min = 2, max = 255,    desc = "Maximum of two or more values"),
        fn("floor",      "floatN", p("floatN","x"),                                             desc = "Largest integer <= x"),
        fn("ceil",       "floatN", p("floatN","x"),                                             desc = "Smallest integer >= x"),
        fn("round",      "floatN", p("floatN","x"),                                             desc = "Round to nearest even integer"),
        fn("trunc",      "floatN", p("floatN","x"),                                             desc = "Truncate toward zero"),
        fn("mod",        "floatN", p("floatN","x"), p("floatN","y"),                            desc = "Modulo x - y*floor(x/y)"),
        fn("fmod",       "floatN", p("floatN","x"), p("floatN","y"),                            desc = "Floating-point remainder x - y*trunc(x/y)"),
        fn("fma",        "floatN", p("floatN","a"), p("floatN","b"), p("floatN","c"),           desc = "Fused multiply-add: a*b + c"),
        fn("pow",        "floatN", p("floatN","x"), p("floatN","y"),                            desc = "x raised to the power y"),
        fn("sqrt",       "floatN", p("floatN","x"),                                             desc = "Square root"),
        fn("rsqrt",      "floatN", p("floatN","x"),                                             desc = "Reciprocal square root"),
        fn("rcp",        "floatN", p("floatN","x"),                                             desc = "Reciprocal 1/x"),
        fn("exp",        "floatN", p("floatN","x"),                                             desc = "e^x"),
        fn("exp2",       "floatN", p("floatN","x"),                                             desc = "2^x"),
        fn("log",        "floatN", p("floatN","x"),                                             desc = "Natural logarithm"),
        fn("log2",       "floatN", p("floatN","x"),                                             desc = "Base-2 logarithm"),
        fn("log10",      "floatN", p("floatN","x"),                                             desc = "Base-10 logarithm"),
        fn("frexp",      "T",      p("float","x"),                                              desc = "Split into significand and exponent"),
        fn("ldexp",      "float",  p("float","x"), p("int","exp"),                              desc = "x * 2^exp"),
        fn("modf",       "T",      p("float","x"),                                              desc = "Split into integer and fractional parts"),

        // ── Trigonometry ────────────────────────────────────────────────────
        fn("sin",     "floatN", p("floatN","x"),                                                desc = "Sine (radians)"),
        fn("cos",     "floatN", p("floatN","x"),                                                desc = "Cosine (radians)"),
        fn("tan",     "floatN", p("floatN","x"),                                                desc = "Tangent (radians)"),
        fn("asin",    "floatN", p("floatN","x"),                                                desc = "Arc sine"),
        fn("acos",    "floatN", p("floatN","x"),                                                desc = "Arc cosine"),
        fn("atan",    "floatN", p("floatN","x"),                                                desc = "Arc tangent"),
        fn("atan2",   "floatN", p("floatN","y"), p("floatN","x"),                              desc = "Arc tangent of y/x"),
        fn("sincos",  "void",   p("float","x"), p("float","outSin"), p("float","outCos"),       desc = "Compute sin and cos simultaneously"),
        fn("sinh",    "floatN", p("floatN","x"),                                                desc = "Hyperbolic sine"),
        fn("cosh",    "floatN", p("floatN","x"),                                                desc = "Hyperbolic cosine"),
        fn("tanh",    "floatN", p("floatN","x"),                                                desc = "Hyperbolic tangent"),
        fn("degrees", "floatN", p("floatN","x"),                                                desc = "Radians to degrees"),
        fn("radians", "floatN", p("floatN","x"),                                                desc = "Degrees to radians"),

        // ── Vector ──────────────────────────────────────────────────────────
        fn("dot",         "float",   p("floatVecN","a"), p("floatVecN","b"),                    desc = "Dot product"),
        fn("cross",       "float3",  p("float3","a"), p("float3","b"),                          desc = "Cross product"),
        fn("normalize",   "floatVecN", p("floatVecN","v"),                                      desc = "Normalize to unit length"),
        fn("length",      "float",   p("floatVecN","v"),                                        desc = "Euclidean length"),
        fn("distance",    "float",   p("floatVecN","a"), p("floatVecN","b"),                    desc = "Distance between two points"),
        fn("reflect",     "floatVecN", p("floatVecN","i"), p("floatVecN","n"),                  desc = "Reflection of i about normal n"),
        fn("refract",     "floatVecN", p("floatVecN","i"), p("floatVecN","n"), p("float","eta"),desc = "Refraction vector"),
        fn("faceforward", "floatVecN", p("floatVecN","n"), p("floatVecN","i"), p("floatVecN","nRef"), desc = "Return n facing same side as i"),

        // ── Matrix ──────────────────────────────────────────────────────────
        fn("transpose",   "matN",  p("matN","m"),                                               desc = "Matrix transpose"),
        fn("determinant", "float", p("matN","m"),                                               desc = "Matrix determinant"),
        fn("inverse",     "matN",  p("matN","m"),                                               desc = "Matrix inverse"),

        // ── Derivatives (fragment only) ─────────────────────────────────────
        fn("ddx",          "floatN", p("floatN","x"), flags = setOf(IntrinsicFlag.FRAGMENT_ONLY), desc = "Partial derivative in x"),
        fn("ddy",          "floatN", p("floatN","x"), flags = setOf(IntrinsicFlag.FRAGMENT_ONLY), desc = "Partial derivative in y"),
        fn("ddx_fine",     "floatN", p("floatN","x"), flags = setOf(IntrinsicFlag.FRAGMENT_ONLY), desc = "Fine partial derivative in x"),
        fn("ddy_fine",     "floatN", p("floatN","x"), flags = setOf(IntrinsicFlag.FRAGMENT_ONLY), desc = "Fine partial derivative in y"),
        fn("ddx_coarse",   "floatN", p("floatN","x"), flags = setOf(IntrinsicFlag.FRAGMENT_ONLY), desc = "Coarse partial derivative in x"),
        fn("ddy_coarse",   "floatN", p("floatN","x"), flags = setOf(IntrinsicFlag.FRAGMENT_ONLY), desc = "Coarse partial derivative in y"),
        fn("fwidth",       "floatN", p("floatN","x"), flags = setOf(IntrinsicFlag.FRAGMENT_ONLY), desc = "abs(ddx(x)) + abs(ddy(x))"),
        fn("fwidth_fine",  "floatN", p("floatN","x"), flags = setOf(IntrinsicFlag.FRAGMENT_ONLY), desc = "Fine fwidth"),
        fn("fwidth_coarse","floatN", p("floatN","x"), flags = setOf(IntrinsicFlag.FRAGMENT_ONLY), desc = "Coarse fwidth"),

        // ── Texture ─────────────────────────────────────────────────────────
        fn("sample",             "float4", p("texture","tex"), p("sampler","s"), p("floatN","uv"),                                    min = 2, max = 6, flags = setOf(IntrinsicFlag.TEXTURE_OP), desc = "Sample texture"),
        fn("sample_lod",         "float4", p("texture","tex"), p("sampler","s"), p("floatN","uv"), p("float","lod"),                  min = 3, max = 6, flags = setOf(IntrinsicFlag.TEXTURE_OP), desc = "Sample texture at explicit LOD"),
        fn("sample_grad",        "float4", p("texture","tex"), p("sampler","s"), p("floatN","uv"), p("floatN","ddx"), p("floatN","ddy"), min = 4, max = 7, flags = setOf(IntrinsicFlag.TEXTURE_OP), desc = "Sample texture with explicit gradients"),
        fn("sample_bias",        "float4", p("texture","tex"), p("sampler","s"), p("floatN","uv"), p("float","bias"),                 min = 3, max = 5, flags = setOf(IntrinsicFlag.TEXTURE_OP, IntrinsicFlag.FRAGMENT_ONLY), desc = "Sample texture with LOD bias"),
        fn("sample_cmp",         "float4", p("texture","tex"), p("sampler","s"), p("floatN","uv"), p("float","cmpVal"),               min = 3, max = 5, flags = setOf(IntrinsicFlag.TEXTURE_OP), desc = "Sample texture with depth comparison"),
        fn("sample_offset",      "float4", p("texture","tex"), p("sampler","s"), p("floatN","uv"), p("int2","offset"),                min = 3, max = 4, flags = setOf(IntrinsicFlag.TEXTURE_OP), desc = "Sample texture with texel offset"),
        fn("sample_lod_offset",  "float4", p("texture","tex"), p("sampler","s"), p("floatN","uv"), p("float","lod"), p("int2","offset"), min = 4, max = 5, flags = setOf(IntrinsicFlag.TEXTURE_OP), desc = "Sample texture at LOD with offset"),
        fn("sample_bias_offset", "float4", p("texture","tex"), p("sampler","s"), p("floatN","uv"), p("float","bias"), p("int2","offset"), min = 4, max = 5, flags = setOf(IntrinsicFlag.TEXTURE_OP, IntrinsicFlag.FRAGMENT_ONLY), desc = "Sample texture with bias and offset"),
        fn("gather",             "float4", p("texture","tex"), p("sampler","s"), p("floatN","uv"),                                    min = 3, max = 4, flags = setOf(IntrinsicFlag.TEXTURE_OP), desc = "Gather four texels"),
        fn("gather_offset",      "float4", p("texture","tex"), p("sampler","s"), p("floatN","uv"), p("int2","offset"),                min = 4, max = 5, flags = setOf(IntrinsicFlag.TEXTURE_OP), desc = "Gather four texels with offset"),
        fn("load",               "float4", p("texture","tex"), p("int2","coord"), p("int","mip"),                                     min = 3, max = 4, flags = setOf(IntrinsicFlag.TEXTURE_OP), desc = "Load texel at integer coordinates"),
        fn("load_offset",        "float4", p("texture","tex"), p("int2","coord"), p("int","mip"), p("int2","offset"),                 min = 4, max = 5, flags = setOf(IntrinsicFlag.TEXTURE_OP), desc = "Load texel with offset"),
        fn("store",              "void",   p("texture","tex"), p("int2","coord"), p("float4","value"),                                flags = setOf(IntrinsicFlag.TEXTURE_OP), desc = "Write texel to image"),
        fn("texture_size",       "int2",   p("texture","tex"), p("int","mip"),                                                        min = 1, max = 2, flags = setOf(IntrinsicFlag.TEXTURE_OP), desc = "Texture dimensions at mip level"),
        fn("texture_levels",     "int",    p("texture","tex"),                                                                        flags = setOf(IntrinsicFlag.TEXTURE_OP), desc = "Number of mip levels"),

        // ── Synchronization ─────────────────────────────────────────────────
        fn("barrier",        "void", desc = "Full execution and memory barrier"),
        fn("memoryBarrier",  "void", desc = "Memory barrier"),
        fn("storageBarrier", "void", desc = "Storage memory barrier"),

        // ── Wave / subgroup ─────────────────────────────────────────────────
        fn("wave_sum",        "floatN",  p("floatN","x"),                    flags = setOf(IntrinsicFlag.WAVE_OP), desc = "Sum across wave"),
        fn("wave_product",    "floatN",  p("floatN","x"),                    flags = setOf(IntrinsicFlag.WAVE_OP), desc = "Product across wave"),
        fn("wave_min",        "numeric", p("numeric","x"),                   flags = setOf(IntrinsicFlag.WAVE_OP), desc = "Minimum across wave"),
        fn("wave_max",        "numeric", p("numeric","x"),                   flags = setOf(IntrinsicFlag.WAVE_OP), desc = "Maximum across wave"),
        fn("wave_all",        "bool",    p("bool","x"),                      flags = setOf(IntrinsicFlag.WAVE_OP), desc = "True if all lanes are true"),
        fn("wave_any",        "bool",    p("bool","x"),                      flags = setOf(IntrinsicFlag.WAVE_OP), desc = "True if any lane is true"),
        fn("wave_broadcast",  "numeric", p("numeric","x"), p("int","laneId"),flags = setOf(IntrinsicFlag.WAVE_OP), desc = "Broadcast value from lane"),
        fn("wave_read_first", "numeric", p("numeric","x"),                   flags = setOf(IntrinsicFlag.WAVE_OP), desc = "Read value from first active lane"),

        // ── Atomics ─────────────────────────────────────────────────────────
        fn("atomic_add",         "int", p("T","ptr"), p("int","value"),                          flags = setOf(IntrinsicFlag.ATOMIC_OP), desc = "Atomic add, returns old value"),
        fn("atomic_min",         "int", p("T","ptr"), p("int","value"),                          flags = setOf(IntrinsicFlag.ATOMIC_OP), desc = "Atomic min, returns old value"),
        fn("atomic_max",         "int", p("T","ptr"), p("int","value"),                          flags = setOf(IntrinsicFlag.ATOMIC_OP), desc = "Atomic max, returns old value"),
        fn("atomic_and",         "int", p("T","ptr"), p("int","value"),                          flags = setOf(IntrinsicFlag.ATOMIC_OP), desc = "Atomic AND, returns old value"),
        fn("atomic_or",          "int", p("T","ptr"), p("int","value"),                          flags = setOf(IntrinsicFlag.ATOMIC_OP), desc = "Atomic OR, returns old value"),
        fn("atomic_xor",         "int", p("T","ptr"), p("int","value"),                          flags = setOf(IntrinsicFlag.ATOMIC_OP), desc = "Atomic XOR, returns old value"),
        fn("atomic_exchange",    "int", p("T","ptr"), p("int","value"),                          flags = setOf(IntrinsicFlag.ATOMIC_OP), desc = "Atomic exchange, returns old value"),
        fn("atomic_cmp_exchange","int", p("T","ptr"), p("int","cmp"), p("int","value"),          flags = setOf(IntrinsicFlag.ATOMIC_OP), desc = "Atomic compare-exchange"),

        // ── Bit operations ───────────────────────────────────────────────────
        fn("count_bits",       "int",      p("int|uint","x"),                                   desc = "Count set bits (popcount)"),
        fn("reverse_bits",     "int|uint", p("int|uint","x"),                                   desc = "Reverse bit order"),
        fn("first_bit_low",    "int",      p("int|uint","x"),                                   desc = "Index of lowest set bit"),
        fn("first_bit_high",   "int",      p("int|uint","x"),                                   desc = "Index of highest set bit"),
        fn("bitfield_extract", "int|uint", p("int|uint","x"), p("int","offset"), p("int","count"), desc = "Extract bit range"),
        fn("bitfield_insert",  "int|uint", p("int|uint","base"), p("int|uint","insert"), p("int","offset"), p("int","count"), desc = "Insert bit range"),
        fn("pack_unorm2x16",   "uint",  p("float2","v"), desc = "Pack float2 to 2x uint16 unorm"),
        fn("unpack_unorm2x16", "float2",p("uint","x"),   desc = "Unpack 2x uint16 unorm to float2"),
        fn("pack_unorm4x8",    "uint",  p("float4","v"), desc = "Pack float4 to 4x uint8 unorm"),
        fn("unpack_unorm4x8",  "float4",p("uint","x"),   desc = "Unpack 4x uint8 unorm to float4"),
        fn("pack_snorm2x16",   "uint",  p("float2","v"), desc = "Pack float2 to 2x int16 snorm"),
        fn("unpack_snorm2x16", "float2",p("uint","x"),   desc = "Unpack 2x int16 snorm to float2"),
        fn("pack_snorm4x8",    "uint",  p("float4","v"), desc = "Pack float4 to 4x int8 snorm"),
        fn("unpack_snorm4x8",  "float4",p("uint","x"),   desc = "Unpack 4x int8 snorm to float4"),
        fn("pack_half2x16",    "uint",  p("float2","v"), desc = "Pack float2 to 2x float16"),
        fn("unpack_half2x16",  "float2",p("uint","x"),   desc = "Unpack 2x float16 to float2"),
        fn("f32tof16",         "uint",  p("float","x"),  desc = "Convert float32 to float16 bits"),
        fn("f16tof32",         "float", p("uint","x"),   desc = "Convert float16 bits to float32"),
        fn("asfloat",          "floatN",p("int|uint","x"), desc = "Reinterpret bits as float"),
        fn("asint",            "intN",  p("float|uint","x"), desc = "Reinterpret bits as int"),
        fn("asuint",           "uintN", p("float|int","x"),  desc = "Reinterpret bits as uint"),

        // ── Control flow ────────────────────────────────────────────────────
        fn("select", "numeric", p("numeric","falseVal"), p("numeric","trueVal"), p("bool","cond"), desc = "Component-wise ternary select"),

        // ── Boolean reductions ───────────────────────────────────────────────
        fn("any", "bool", p("boolVecN","v"), desc = "True if any component is true"),
        fn("all", "bool", p("boolVecN","v"), desc = "True if all components are true"),

        // ── Float classification ─────────────────────────────────────────────
        fn("isnan",    "bool|boolN", p("floatN","x"), desc = "True if x is NaN"),
        fn("isinf",    "bool|boolN", p("floatN","x"), desc = "True if x is infinity"),
        fn("isfinite", "bool|boolN", p("floatN","x"), desc = "True if x is finite"),
        fn("isnormal", "bool|boolN", p("floatN","x"), desc = "True if x is normal"),
    )

    val NAMES: Set<String> = ALL.map { it.name }.toHashSet()
    val BY_NAME: Map<String, List<IntrinsicFunction>> = ALL.groupBy { it.name }
}
