package com.bwsl.plugin

import com.intellij.openapi.editor.colors.TextAttributesKey
import com.intellij.openapi.fileTypes.SyntaxHighlighter
import com.intellij.openapi.options.colors.AttributesDescriptor
import com.intellij.openapi.options.colors.ColorDescriptor
import com.intellij.openapi.options.colors.ColorSettingsPage
import javax.swing.Icon

class BwslColorSettingsPage : ColorSettingsPage {

    private val descriptors = arrayOf(
        AttributesDescriptor("Control flow keyword",  BwslSyntaxHighlighter.KEYWORD),
        AttributesDescriptor("Type keyword",          BwslSyntaxHighlighter.TYPE_KEYWORD),
        AttributesDescriptor("Number",                BwslSyntaxHighlighter.NUMBER),
        AttributesDescriptor("String",                BwslSyntaxHighlighter.STRING),
        AttributesDescriptor("Line comment",          BwslSyntaxHighlighter.LINE_COMMENT),
        AttributesDescriptor("Block comment",         BwslSyntaxHighlighter.BLOCK_COMMENT),
        AttributesDescriptor("Operator / punctuation", BwslSyntaxHighlighter.OPERATOR),
        AttributesDescriptor("Identifier",            BwslSyntaxHighlighter.IDENTIFIER),
        AttributesDescriptor("Bad character",         BwslSyntaxHighlighter.BAD_CHARACTER),
    )

    override fun getDisplayName(): String = "BWSL"
    override fun getIcon(): Icon? = null
    override fun getHighlighter(): SyntaxHighlighter = BwslSyntaxHighlighter()
    override fun getAttributeDescriptors(): Array<AttributesDescriptor> = descriptors
    override fun getColorDescriptors(): Array<ColorDescriptor> = ColorDescriptor.EMPTY_ARRAY
    override fun getAdditionalHighlightingTagToDescriptorMap(): Map<String, TextAttributesKey>? = null

    override fun getDemoText(): String = """
// BWSL Shader Language demo
/* Block comment */

module MathUtils {
    const float PI = 3.14159f;
    const uint MAX_SAMPLES = 64u;

    dot :: (float3 a, float3 b) -> float {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    enum BlendMode {
        None,
        Additive(float weight),
        Multiply,
    }
}

pipeline PBR {
    import MathUtils as M

    resources {
        albedo    : texture2D
        normalMap : texture2D
        samp      : sampler
        ubo       : cbuffer<float4>
    }

    attributes {
        position  : float4
        uv        : float2
        normal    : float3
        @instance transform : mat4
    }

    variants {
        useNormalMap : bool = true
        rules {
            require useNormalMap -> true;
        }
    }

    pass "Main" {
        use attributes { position, uv, normal }
        use resources  { albedo, normalMap?, samp }

        outputs {
            color : float4 @location(0)
        }

        vertex {
            float4 worldPos = transform * float4(position.xyz, 1.0f);
            return worldPos;
        }

        fragment {
            @flat float3 n = normalize(normal);
            float4 col     = albedo.sample(samp, uv);
            if (col.a < 0.01f) { discard; }
            color = float4(n * 0.5f + 0.5f, 1.0f) * col;
        }
    }
}
""".trimIndent()
}
