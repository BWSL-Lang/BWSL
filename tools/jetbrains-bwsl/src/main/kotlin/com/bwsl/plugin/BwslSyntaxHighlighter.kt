package com.bwsl.plugin

import com.intellij.lexer.Lexer
import com.intellij.openapi.editor.DefaultLanguageHighlighterColors
import com.intellij.openapi.editor.HighlighterColors
import com.intellij.openapi.editor.colors.TextAttributesKey
import com.intellij.openapi.editor.colors.TextAttributesKey.createTextAttributesKey
import com.intellij.openapi.fileTypes.SyntaxHighlighterBase
import com.intellij.psi.TokenType
import com.intellij.psi.tree.IElementType
import com.intellij.psi.tree.TokenSet

class BwslSyntaxHighlighter : SyntaxHighlighterBase() {

    companion object {
        @JvmField val DECORATOR    = createTextAttributesKey("BWSL_DECORATOR",    DefaultLanguageHighlighterColors.METADATA)
        @JvmField val BLOCK_KEYWORD= createTextAttributesKey("BWSL_BLOCK_KEYWORD",DefaultLanguageHighlighterColors.FUNCTION_DECLARATION)
        @JvmField val FUNCTION_NAME= createTextAttributesKey("BWSL_FUNCTION_NAME",DefaultLanguageHighlighterColors.FUNCTION_DECLARATION)
        @JvmField val KEYWORD      = createTextAttributesKey("BWSL_KEYWORD",      DefaultLanguageHighlighterColors.KEYWORD)
        @JvmField val TYPE_KEYWORD = createTextAttributesKey("BWSL_TYPE_KEYWORD", DefaultLanguageHighlighterColors.CLASS_REFERENCE)
        @JvmField val NUMBER       = createTextAttributesKey("BWSL_NUMBER",       DefaultLanguageHighlighterColors.NUMBER)
        @JvmField val STRING       = createTextAttributesKey("BWSL_STRING",       DefaultLanguageHighlighterColors.STRING)
        @JvmField val LINE_COMMENT = createTextAttributesKey("BWSL_LINE_COMMENT", DefaultLanguageHighlighterColors.LINE_COMMENT)
        @JvmField val BLOCK_COMMENT= createTextAttributesKey("BWSL_BLOCK_COMMENT",DefaultLanguageHighlighterColors.BLOCK_COMMENT)
        @JvmField val OPERATOR     = createTextAttributesKey("BWSL_OPERATOR",     DefaultLanguageHighlighterColors.OPERATION_SIGN)
        @JvmField val INTRINSIC    = createTextAttributesKey("BWSL_INTRINSIC",    DefaultLanguageHighlighterColors.GLOBAL_VARIABLE)
        @JvmField val IDENTIFIER   = createTextAttributesKey("BWSL_IDENTIFIER",   DefaultLanguageHighlighterColors.IDENTIFIER)
        @JvmField val BAD_CHARACTER= createTextAttributesKey("BWSL_BAD_CHARACTER",HighlighterColors.BAD_CHARACTER)

        // pipeline, pass, vertex, fragment, attributes, resources, …
        private val BLOCK_KW_SET = TokenSet.create(
            BwslTokenTypes.KW_MODULE, BwslTokenTypes.KW_PIPELINE,
            BwslTokenTypes.KW_PASS, BwslTokenTypes.KW_VERTEX, BwslTokenTypes.KW_FRAGMENT,
            BwslTokenTypes.KW_COMPUTE, BwslTokenTypes.KW_COMPUTE_GRAPH,
            BwslTokenTypes.KW_ATTRIBUTES, BwslTokenTypes.KW_RESOURCES, BwslTokenTypes.KW_VARIANTS,
            BwslTokenTypes.KW_STRUCT, BwslTokenTypes.KW_ENUM, BwslTokenTypes.KW_EVAL,
            BwslTokenTypes.KW_NODE, BwslTokenTypes.KW_INPUTS, BwslTokenTypes.KW_OUTPUTS
        )

        // if, return, for, import, const, …
        private val KW_SET = TokenSet.create(
            BwslTokenTypes.KW_IMPORT, BwslTokenTypes.KW_USING, BwslTokenTypes.KW_AS,
            BwslTokenTypes.KW_USE, BwslTokenTypes.KW_CONST, BwslTokenTypes.KW_SHARED,
            BwslTokenTypes.KW_CONSTRAINT, BwslTokenTypes.KW_RULES,
            BwslTokenTypes.KW_REQUIRE, BwslTokenTypes.KW_CONFLICT,
            BwslTokenTypes.KW_RETURN, BwslTokenTypes.KW_IF, BwslTokenTypes.KW_ELSE,
            BwslTokenTypes.KW_FOR, BwslTokenTypes.KW_FOREACH, BwslTokenTypes.KW_WHILE,
            BwslTokenTypes.KW_LOOP, BwslTokenTypes.KW_SWITCH, BwslTokenTypes.KW_CASE,
            BwslTokenTypes.KW_DEFAULT, BwslTokenTypes.KW_BREAK, BwslTokenTypes.KW_SKIP,
            BwslTokenTypes.KW_CONTINUE, BwslTokenTypes.KW_DISCARD,
            BwslTokenTypes.KW_IN, BwslTokenTypes.KW_BY, BwslTokenTypes.KW_UNTIL,
            BwslTokenTypes.KW_NULL, BwslTokenTypes.KW_TRUE, BwslTokenTypes.KW_FALSE, BwslTokenTypes.KW_SELF,
            BwslTokenTypes.KW_READONLY, BwslTokenTypes.KW_READWRITE, BwslTokenTypes.KW_WRITEONLY,
            BwslTokenTypes.KW_VERTEX_FUNCTION, BwslTokenTypes.KW_FRAGMENT_FUNCTION,
            BwslTokenTypes.KW_COMPUTE_FUNCTION, BwslTokenTypes.KW_PASS_BLOCK
        )

        // float3, mat4, texture2D, cbuffer, …
        private val TYPE_KW_SET = TokenSet.create(
            BwslTokenTypes.KW_BOOL,
            BwslTokenTypes.KW_INT,   BwslTokenTypes.KW_UINT,   BwslTokenTypes.KW_FLOAT,
            BwslTokenTypes.KW_DOUBLE,BwslTokenTypes.KW_INT64,  BwslTokenTypes.KW_UINT64,
            BwslTokenTypes.KW_INT2,  BwslTokenTypes.KW_INT3,   BwslTokenTypes.KW_INT4,
            BwslTokenTypes.KW_UINT2, BwslTokenTypes.KW_UINT3,  BwslTokenTypes.KW_UINT4,
            BwslTokenTypes.KW_FLOAT2,BwslTokenTypes.KW_FLOAT3, BwslTokenTypes.KW_FLOAT4,
            BwslTokenTypes.KW_DOUBLE2,BwslTokenTypes.KW_DOUBLE3,BwslTokenTypes.KW_DOUBLE4,
            BwslTokenTypes.KW_INT64X2,BwslTokenTypes.KW_INT64X3,BwslTokenTypes.KW_INT64X4,
            BwslTokenTypes.KW_UINT64X2,BwslTokenTypes.KW_UINT64X3,BwslTokenTypes.KW_UINT64X4,
            BwslTokenTypes.KW_MAT2, BwslTokenTypes.KW_MAT3,  BwslTokenTypes.KW_MAT4,
            BwslTokenTypes.KW_DMAT2,BwslTokenTypes.KW_DMAT3, BwslTokenTypes.KW_DMAT4,
            BwslTokenTypes.KW_SAMPLER,
            BwslTokenTypes.KW_TEXTURE2D, BwslTokenTypes.KW_TEXTURE3D,
            BwslTokenTypes.KW_TEXTURECUBE, BwslTokenTypes.KW_TEXTURE2DARRAY,
            BwslTokenTypes.KW_IMAGE2D, BwslTokenTypes.KW_BUFFER, BwslTokenTypes.KW_CBUFFER,
            BwslTokenTypes.KW_VOID
        )

        private val DECORATOR_KEYS     = arrayOf(DECORATOR)
        private val BLOCK_KEYWORD_KEYS = arrayOf(BLOCK_KEYWORD)
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

    override fun getHighlightingLexer(): Lexer = BwslLexerAdapter()

    override fun getTokenHighlights(tokenType: IElementType): Array<TextAttributesKey> = when {
        tokenType == BwslTokenTypes.DECORATOR      -> DECORATOR_KEYS
        BLOCK_KW_SET.contains(tokenType)      -> BLOCK_KEYWORD_KEYS
        KW_SET.contains(tokenType)            -> KEYWORD_KEYS
        TYPE_KW_SET.contains(tokenType)       -> TYPE_KEYWORD_KEYS
        tokenType == BwslTokenTypes.NUMBER_LIT     -> NUMBER_KEYS
        tokenType == BwslTokenTypes.STRING_LIT     -> STRING_KEYS
        tokenType == BwslTokenTypes.LINE_COMMENT   -> LINE_COMMENT_KEYS
        tokenType == BwslTokenTypes.BLOCK_COMMENT  -> BLOCK_COMMENT_KEYS
        tokenType == BwslTokenTypes.IDENTIFIER     -> IDENTIFIER_KEYS
        tokenType == TokenType.BAD_CHARACTER  -> BAD_CHARACTER_KEYS
        isOperator(tokenType)                 -> OPERATOR_KEYS
        else                                  -> EMPTY
    }

    private fun isOperator(t: IElementType) = t == BwslTokenTypes.PLUS || t == BwslTokenTypes.MINUS
        || t == BwslTokenTypes.STAR || t == BwslTokenTypes.SLASH || t == BwslTokenTypes.PERCENT
        || t == BwslTokenTypes.AMP || t == BwslTokenTypes.PIPE || t == BwslTokenTypes.CARET
        || t == BwslTokenTypes.TILDE || t == BwslTokenTypes.BANG
        || t == BwslTokenTypes.EQ || t == BwslTokenTypes.EQEQ || t == BwslTokenTypes.NEQ
        || t == BwslTokenTypes.LT || t == BwslTokenTypes.GT || t == BwslTokenTypes.LE || t == BwslTokenTypes.GE
        || t == BwslTokenTypes.AND || t == BwslTokenTypes.OR
        || t == BwslTokenTypes.LSHIFT || t == BwslTokenTypes.RSHIFT
        || t == BwslTokenTypes.LSHIFTEQ || t == BwslTokenTypes.RSHIFTEQ
        || t == BwslTokenTypes.PLUSEQ || t == BwslTokenTypes.MINUSEQ || t == BwslTokenTypes.STAREQ
        || t == BwslTokenTypes.SLASHEQ || t == BwslTokenTypes.PERCENTEQ
        || t == BwslTokenTypes.AMPEQ || t == BwslTokenTypes.PIPEEQ || t == BwslTokenTypes.CARETEQ
        || t == BwslTokenTypes.PLUSPLUS || t == BwslTokenTypes.MINUSMINUS
        || t == BwslTokenTypes.ARROW || t == BwslTokenTypes.COLONCOLON || t == BwslTokenTypes.COLON
        || t == BwslTokenTypes.DOT || t == BwslTokenTypes.DOTDOT || t == BwslTokenTypes.DOTDOTEQ
        || t == BwslTokenTypes.LBRACE || t == BwslTokenTypes.RBRACE
        || t == BwslTokenTypes.LPAREN || t == BwslTokenTypes.RPAREN
        || t == BwslTokenTypes.LBRACKET || t == BwslTokenTypes.RBRACKET
        || t == BwslTokenTypes.SEMI || t == BwslTokenTypes.COMMA || t == BwslTokenTypes.QUESTION
}
