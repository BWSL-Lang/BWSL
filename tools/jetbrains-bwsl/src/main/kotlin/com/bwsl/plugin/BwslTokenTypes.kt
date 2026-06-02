package com.bwsl.plugin

import com.intellij.psi.tree.IElementType

class BwslTokenType(debugName: String) : IElementType(debugName, BwslLanguage)

object BwslTokenTypes {
    @JvmField val BLOCK_KEYWORD = BwslTokenType("BLOCK_KEYWORD")   // pipeline, pass, vertex, …
    @JvmField val KEYWORD       = BwslTokenType("KEYWORD")         // if, return, import, …
    @JvmField val TYPE_KEYWORD  = BwslTokenType("TYPE_KEYWORD")    // float3, mat4, texture2D, …
    @JvmField val DECORATOR     = BwslTokenType("DECORATOR")
    @JvmField val FUNCTION_NAME = BwslTokenType("FUNCTION_NAME")
    @JvmField val IDENTIFIER    = BwslTokenType("IDENTIFIER")
    @JvmField val NUMBER       = BwslTokenType("NUMBER")
    @JvmField val STRING       = BwslTokenType("STRING")
    @JvmField val LINE_COMMENT = BwslTokenType("LINE_COMMENT")
    @JvmField val BLOCK_COMMENT = BwslTokenType("BLOCK_COMMENT")
    @JvmField val OPERATOR     = BwslTokenType("OPERATOR")
    @JvmField val PUNCTUATION  = BwslTokenType("PUNCTUATION")
    @JvmField val BAD_CHARACTER = BwslTokenType("BAD_CHARACTER")
}
