package com.bwsl.plugin

import com.intellij.lexer.LexerBase
import com.intellij.psi.TokenType
import com.intellij.psi.tree.IElementType

class BwslLexer : LexerBase() {

    private var buffer: CharSequence = ""
    private var bufferEnd: Int = 0
    private var tokenStart: Int = 0
    private var tokenEnd: Int = 0
    private var currentToken: IElementType? = null

    companion object {
        // Structural block-level keywords — get their own highlight color.
        // attributes/resources/variants also appear in expressions; they keep this color there too.
        private val BLOCK_KEYWORDS: Set<String> = hashSetOf(
            "module", "pipeline",
            "pass", "vertex", "fragment", "compute", "compute_graph",
            "attributes", "resources", "variants",
            "struct", "enum", "eval",
            "node", "inputs", "outputs"
        )

        // Control-flow, declaration, and misc keywords.
        private val KEYWORDS: Set<String> = hashSetOf(
            "import", "using", "as",
            "use", "const", "shared", "constraint",
            "rules", "require", "conflict",
            "return", "if", "else", "for", "foreach", "while", "loop",
            "switch", "case", "default", "break", "skip", "continue",
            "discard", "in", "by", "until",
            "true", "false", "null", "self",
            "readonly", "readwrite", "writeonly",
            "vertex_function", "fragment_function", "compute_function", "pass_block",
            "flat", "noperspective", "compressed", "instance", "location"
        )

        private val TYPE_KEYWORDS: Set<String> = hashSetOf(
            // Scalar
            "bool", "int", "uint", "float", "double", "int64", "uint64",
            // Vector
            "int2", "int3", "int4",
            "uint2", "uint3", "uint4",
            "float2", "float3", "float4",
            "double2", "double3", "double4",
            "int64x2", "int64x3", "int64x4",
            "uint64x2", "uint64x3", "uint64x4",
            // Matrix
            "mat2", "mat3", "mat4",
            "dmat2", "dmat3", "dmat4",
            // Resource types
            "sampler",
            "texture2D", "texture3D", "textureCube", "texture2DArray",
            "image2D", "buffer", "cbuffer",
            "void"
        )
    }

    // ── LexerBase contract ──────────────────────────────────────────────────

    override fun start(buffer: CharSequence, startOffset: Int, endOffset: Int, initialState: Int) {
        this.buffer = buffer
        this.bufferEnd = endOffset
        this.tokenStart = startOffset
        this.tokenEnd = startOffset
        this.currentToken = null
        advance()
    }

    override fun getState(): Int = 0
    override fun getTokenType(): IElementType? = currentToken
    override fun getTokenStart(): Int = tokenStart
    override fun getTokenEnd(): Int = tokenEnd
    override fun getBufferSequence(): CharSequence = buffer
    override fun getBufferEnd(): Int = bufferEnd

    override fun advance() {
        tokenStart = tokenEnd
        if (tokenStart >= bufferEnd) {
            currentToken = null
            return
        }
        currentToken = readToken()
    }

    // ── Token dispatch ──────────────────────────────────────────────────────

    private fun readToken(): IElementType {
        val c = buffer[tokenEnd]

        if (c.isWhitespace()) return readWhitespace()

        // Line comment
        if (c == '/' && peek(1) == '*') return readBlockComment()
        if (c == '/' && peek(1) == '/') return readLineComment()

        if (c == '"') return readString()
        if (c.isDigit()) return readNumber()
        if (c.isLetter() || c == '_') return readWord()

        return readSymbol()
    }

    // ── Whitespace ──────────────────────────────────────────────────────────

    private fun readWhitespace(): IElementType {
        while (tokenEnd < bufferEnd && buffer[tokenEnd].isWhitespace()) tokenEnd++
        return TokenType.WHITE_SPACE
    }

    // ── Comments ────────────────────────────────────────────────────────────

    private fun readLineComment(): IElementType {
        tokenEnd += 2
        while (tokenEnd < bufferEnd && buffer[tokenEnd] != '\n') tokenEnd++
        return BwslTokenTypes.LINE_COMMENT
    }

    private fun readBlockComment(): IElementType {
        tokenEnd += 2
        while (tokenEnd < bufferEnd) {
            if (buffer[tokenEnd] == '*' && peek(1) == '/') {
                tokenEnd += 2
                break
            }
            tokenEnd++
        }
        return BwslTokenTypes.BLOCK_COMMENT
    }

    // ── String literal ──────────────────────────────────────────────────────

    private fun readString(): IElementType {
        tokenEnd++ // opening "
        while (tokenEnd < bufferEnd && buffer[tokenEnd] != '"') {
            if (buffer[tokenEnd] == '\\') tokenEnd++ // skip escaped char
            if (tokenEnd < bufferEnd) tokenEnd++
        }
        if (tokenEnd < bufferEnd) tokenEnd++ // closing "
        return BwslTokenTypes.STRING
    }

    // ── Number literal ──────────────────────────────────────────────────────

    private fun readNumber(): IElementType {
        if (buffer[tokenEnd] == '0') {
            when (peek(1)) {
                'x', 'X' -> {
                    tokenEnd += 2
                    while (tokenEnd < bufferEnd && buffer[tokenEnd].isHexDigit()) tokenEnd++
                    consumeIntSuffix()
                    return BwslTokenTypes.NUMBER
                }
                'b', 'B' -> {
                    tokenEnd += 2
                    while (tokenEnd < bufferEnd && (buffer[tokenEnd] == '0' || buffer[tokenEnd] == '1')) tokenEnd++
                    consumeIntSuffix()
                    return BwslTokenTypes.NUMBER
                }
            }
        }

        // Decimal integer or float
        while (tokenEnd < bufferEnd && buffer[tokenEnd].isDigit()) tokenEnd++

        // Optional fractional part
        if (tokenEnd < bufferEnd && buffer[tokenEnd] == '.') {
            tokenEnd++
            while (tokenEnd < bufferEnd && buffer[tokenEnd].isDigit()) tokenEnd++
        }

        // Optional exponent
        if (tokenEnd < bufferEnd && (buffer[tokenEnd] == 'e' || buffer[tokenEnd] == 'E')) {
            tokenEnd++
            if (tokenEnd < bufferEnd && (buffer[tokenEnd] == '+' || buffer[tokenEnd] == '-')) tokenEnd++
            while (tokenEnd < bufferEnd && buffer[tokenEnd].isDigit()) tokenEnd++
        }

        // Optional suffix: f, F, u, U
        if (tokenEnd < bufferEnd) {
            when (buffer[tokenEnd]) {
                'f', 'F', 'u', 'U' -> tokenEnd++
            }
        }

        return BwslTokenTypes.NUMBER
    }

    private fun consumeIntSuffix() {
        if (tokenEnd < bufferEnd && (buffer[tokenEnd] == 'u' || buffer[tokenEnd] == 'U')) tokenEnd++
    }

    // ── Identifiers and keywords ────────────────────────────────────────────

    private fun readWord(): IElementType {
        while (tokenEnd < bufferEnd && (buffer[tokenEnd].isLetterOrDigit() || buffer[tokenEnd] == '_')) {
            tokenEnd++
        }
        val word = buffer.subSequence(tokenStart, tokenEnd).toString()
        return when {
            BLOCK_KEYWORDS.contains(word) -> BwslTokenTypes.BLOCK_KEYWORD
            TYPE_KEYWORDS.contains(word)  -> BwslTokenTypes.TYPE_KEYWORD
            KEYWORDS.contains(word)       -> BwslTokenTypes.KEYWORD
            isFollowedByColonColon()      -> BwslTokenTypes.FUNCTION_NAME
            else                          -> BwslTokenTypes.IDENTIFIER
        }
    }

    // ── Operators and punctuation ───────────────────────────────────────────

    private fun readSymbol(): IElementType {
        val c = buffer[tokenEnd++]
        return when (c) {
            '{', '}', '(', ')', '[', ']', ';', ',', '?' -> BwslTokenTypes.PUNCTUATION

            ':' -> { if (peek(0) == ':') tokenEnd++; BwslTokenTypes.OPERATOR }

            '.' -> {
                if (peek(0) == '.') {
                    tokenEnd++
                    if (peek(0) == '=') tokenEnd++
                }
                BwslTokenTypes.OPERATOR
            }

            '-' -> {
                when (peek(0)) {
                    '>' -> { tokenEnd++; BwslTokenTypes.OPERATOR }
                    '-' -> { tokenEnd++; BwslTokenTypes.OPERATOR }
                    '=' -> { tokenEnd++; BwslTokenTypes.OPERATOR }
                    else -> BwslTokenTypes.OPERATOR
                }
            }

            '+' -> { if (peek(0) == '+' || peek(0) == '=') tokenEnd++; BwslTokenTypes.OPERATOR }
            '*' -> { if (peek(0) == '=') tokenEnd++; BwslTokenTypes.OPERATOR }
            '/' -> { if (peek(0) == '=') tokenEnd++; BwslTokenTypes.OPERATOR }
            '%' -> { if (peek(0) == '=') tokenEnd++; BwslTokenTypes.OPERATOR }
            '~' -> BwslTokenTypes.OPERATOR
            '@' -> {
                // Consume @identifier as one decorator token (e.g. @flat, @location).
                if (tokenEnd < bufferEnd && (buffer[tokenEnd].isLetter() || buffer[tokenEnd] == '_')) {
                    while (tokenEnd < bufferEnd && (buffer[tokenEnd].isLetterOrDigit() || buffer[tokenEnd] == '_')) {
                        tokenEnd++
                    }
                    BwslTokenTypes.DECORATOR
                } else {
                    BwslTokenTypes.OPERATOR
                }
            }

            '!' -> { if (peek(0) == '=') tokenEnd++; BwslTokenTypes.OPERATOR }
            '=' -> { if (peek(0) == '=') tokenEnd++; BwslTokenTypes.OPERATOR }

            '&' -> { if (peek(0) == '&' || peek(0) == '=') tokenEnd++; BwslTokenTypes.OPERATOR }
            '|' -> { if (peek(0) == '|' || peek(0) == '=') tokenEnd++; BwslTokenTypes.OPERATOR }
            '^' -> { if (peek(0) == '=') tokenEnd++; BwslTokenTypes.OPERATOR }

            '<' -> {
                when (peek(0)) {
                    '<' -> {
                        tokenEnd++
                        if (peek(0) == '=') tokenEnd++
                    }
                    '=' -> tokenEnd++
                }
                BwslTokenTypes.OPERATOR
            }

            '>' -> {
                when (peek(0)) {
                    '>' -> {
                        tokenEnd++
                        if (peek(0) == '=') tokenEnd++
                    }
                    '=' -> tokenEnd++
                }
                BwslTokenTypes.OPERATOR
            }

            else -> BwslTokenTypes.BAD_CHARACTER
        }
    }

    // ── Helpers ─────────────────────────────────────────────────────────────

    // Returns true when the identifier just consumed is a function/method name,
    // i.e. it is followed (ignoring whitespace) by '::'.
    private fun isFollowedByColonColon(): Boolean {
        var i = tokenEnd
        while (i < bufferEnd && buffer[i].isWhitespace()) i++
        return i + 1 < bufferEnd && buffer[i] == ':' && buffer[i + 1] == ':'
    }

    private fun peek(offset: Int): Char {
        val idx = tokenEnd + offset
        return if (idx < bufferEnd) buffer[idx] else ' '
    }

    private fun Char.isHexDigit(): Boolean =
        isDigit() || this in 'a'..'f' || this in 'A'..'F'
}
