package com.bwsl.plugin

import com.intellij.lexer.Lexer
import com.intellij.openapi.editor.DefaultLanguageHighlighterColors
import com.intellij.openapi.editor.HighlighterColors
import com.intellij.openapi.editor.colors.TextAttributesKey
import com.intellij.openapi.editor.colors.TextAttributesKey.createTextAttributesKey
import com.intellij.openapi.fileTypes.SyntaxHighlighterBase
import com.intellij.psi.TokenType
import com.intellij.psi.tree.IElementType

class BwslSyntaxHighlighter : SyntaxHighlighterBase() {

    companion object {
        @JvmField
        val KEYWORD = createTextAttributesKey("BWSL_KEYWORD", DefaultLanguageHighlighterColors.KEYWORD)
        @JvmField
        val TYPE_KEYWORD = createTextAttributesKey("BWSL_TYPE_KEYWORD", DefaultLanguageHighlighterColors.KEYWORD)
        @JvmField
        val NUMBER = createTextAttributesKey("BWSL_NUMBER", DefaultLanguageHighlighterColors.NUMBER)
        @JvmField
        val STRING = createTextAttributesKey("BWSL_STRING", DefaultLanguageHighlighterColors.STRING)
        @JvmField
        val LINE_COMMENT = createTextAttributesKey("BWSL_LINE_COMMENT", DefaultLanguageHighlighterColors.LINE_COMMENT)
        @JvmField
        val BLOCK_COMMENT = createTextAttributesKey("BWSL_BLOCK_COMMENT", DefaultLanguageHighlighterColors.BLOCK_COMMENT)
        @JvmField
        val OPERATOR = createTextAttributesKey("BWSL_OPERATOR", DefaultLanguageHighlighterColors.OPERATION_SIGN)
        @JvmField
        val IDENTIFIER = createTextAttributesKey("BWSL_IDENTIFIER", DefaultLanguageHighlighterColors.IDENTIFIER)
        @JvmField
        val BAD_CHARACTER = createTextAttributesKey("BWSL_BAD_CHARACTER", HighlighterColors.BAD_CHARACTER)

        private val KEYWORD_KEYS       = arrayOf(KEYWORD)
        private val TYPE_KEYWORD_KEYS  = arrayOf(TYPE_KEYWORD)
        private val NUMBER_KEYS        = arrayOf(NUMBER)
        private val STRING_KEYS        = arrayOf(STRING)
        private val LINE_COMMENT_KEYS  = arrayOf(LINE_COMMENT)
        private val BLOCK_COMMENT_KEYS = arrayOf(BLOCK_COMMENT)
        private val OPERATOR_KEYS      = arrayOf(OPERATOR)
        private val IDENTIFIER_KEYS    = arrayOf(IDENTIFIER)
        private val BAD_CHARACTER_KEYS = arrayOf(BAD_CHARACTER)
        private val EMPTY              = emptyArray<TextAttributesKey>()
    }

    override fun getHighlightingLexer(): Lexer = BwslLexer()

    override fun getTokenHighlights(tokenType: IElementType): Array<TextAttributesKey> = when (tokenType) {
        BwslTokenTypes.KEYWORD       -> KEYWORD_KEYS
        BwslTokenTypes.TYPE_KEYWORD  -> TYPE_KEYWORD_KEYS
        BwslTokenTypes.NUMBER        -> NUMBER_KEYS
        BwslTokenTypes.STRING        -> STRING_KEYS
        BwslTokenTypes.LINE_COMMENT  -> LINE_COMMENT_KEYS
        BwslTokenTypes.BLOCK_COMMENT -> BLOCK_COMMENT_KEYS
        BwslTokenTypes.OPERATOR      -> OPERATOR_KEYS
        BwslTokenTypes.PUNCTUATION   -> OPERATOR_KEYS
        BwslTokenTypes.IDENTIFIER    -> IDENTIFIER_KEYS
        BwslTokenTypes.BAD_CHARACTER -> BAD_CHARACTER_KEYS
        TokenType.BAD_CHARACTER      -> BAD_CHARACTER_KEYS
        else                         -> EMPTY
    }
}
