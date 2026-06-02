package com.bwsl.plugin

import com.intellij.psi.tree.IElementType

class BwslTokenType(debugName: String) : IElementType(debugName, BwslLanguage)

object BwslTokenTypes {
    @JvmField val KEYWORD      = BwslTokenType("KEYWORD")
    @JvmField val TYPE_KEYWORD = BwslTokenType("TYPE_KEYWORD")
    @JvmField val IDENTIFIER   = BwslTokenType("IDENTIFIER")
    @JvmField val NUMBER       = BwslTokenType("NUMBER")
    @JvmField val STRING       = BwslTokenType("STRING")
    @JvmField val LINE_COMMENT = BwslTokenType("LINE_COMMENT")
    @JvmField val BLOCK_COMMENT = BwslTokenType("BLOCK_COMMENT")
    @JvmField val OPERATOR     = BwslTokenType("OPERATOR")
    @JvmField val PUNCTUATION  = BwslTokenType("PUNCTUATION")
    @JvmField val BAD_CHARACTER = BwslTokenType("BAD_CHARACTER")
}
