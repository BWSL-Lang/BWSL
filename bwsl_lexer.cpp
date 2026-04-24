
#include "bwsl_lexer.h"
#include <cctype>

namespace BWSL {

Lexer::Lexer(const std::string& source, TokenStream& stream)
    : source(source), stream(stream) {
    // Build line table for fast source location lookups
    lineTable.Build(this->source.data(), static_cast<u32>(this->source.length()));
}

auto is_digit = [](unsigned c){ return unsigned(c - '0') < 10; };

void Lexer::Tokenize() {
    while (!IsAtEnd()) {
        NextToken();
    }
    // Push EOF token
    stream.Push(current, static_cast<uint8_t>(TokenType::EOF_TOKEN));
}

TokenRef Lexer::NextToken() {
    SkipWhitespace();

    if (IsAtEnd()) {
        return stream.Push(current, static_cast<uint8_t>(TokenType::EOF_TOKEN));
    }

    uint32_t startPos = current;
    char c = Advance();

    // Single character tokens
    switch (c) {
        case '{': return stream.Push(startPos, static_cast<uint8_t>(TokenType::LEFT_BRACE));
        case '}': return stream.Push(startPos, static_cast<uint8_t>(TokenType::RIGHT_BRACE));
        case '(': return stream.Push(startPos, static_cast<uint8_t>(TokenType::LEFT_PAREN));
        case ')': return stream.Push(startPos, static_cast<uint8_t>(TokenType::RIGHT_PAREN));
        case '[': return stream.Push(startPos, static_cast<uint8_t>(TokenType::LEFT_BRACKET));
        case ']': return stream.Push(startPos, static_cast<uint8_t>(TokenType::RIGHT_BRACKET));
        case ';': return stream.Push(startPos, static_cast<uint8_t>(TokenType::SEMICOLON));
        case ',': return stream.Push(startPos, static_cast<uint8_t>(TokenType::COMMA));
        case '.':
            if (Match('.')) {
                if (Match('=')) return stream.Push(startPos, static_cast<uint8_t>(TokenType::DOT_DOT_EQUAL));
                return stream.Push(startPos, static_cast<uint8_t>(TokenType::DOT_DOT));
            }
            return stream.Push(startPos, static_cast<uint8_t>(TokenType::DOT));
        case '?': return stream.Push(startPos, static_cast<uint8_t>(TokenType::QUESTION));
        case '@': return ScanDecorator();
        case ':':
            if (Match(':')) return stream.Push(startPos, static_cast<uint8_t>(TokenType::DOUBLE_COLON));
            return stream.Push(startPos, static_cast<uint8_t>(TokenType::COLON));
        case '-':
            if (Match('>')) return stream.Push(startPos, static_cast<uint8_t>(TokenType::ARROW));
            if (Match('-')) return stream.Push(startPos, static_cast<uint8_t>(TokenType::DECREMENT));
            if (Match('=')) return stream.Push(startPos, static_cast<uint8_t>(TokenType::MINUS_ASSIGN));
            return stream.Push(startPos, static_cast<uint8_t>(TokenType::MINUS));
        case '+':
            if (Match('+')) return stream.Push(startPos, static_cast<uint8_t>(TokenType::INCREMENT));
            if (Match('=')) return stream.Push(startPos, static_cast<uint8_t>(TokenType::PLUS_ASSIGN));
            return stream.Push(startPos, static_cast<uint8_t>(TokenType::PLUS));
        case '*':
            if (Match('=')) return stream.Push(startPos, static_cast<uint8_t>(TokenType::MULTIPLY_ASSIGN));
            return stream.Push(startPos, static_cast<uint8_t>(TokenType::MULTIPLY));
        case '/':
            if (Match('/')) {
                SkipComment();
                return NextToken();
            }
            if (Match('=')) return stream.Push(startPos, static_cast<uint8_t>(TokenType::DIVIDE_ASSIGN));
            return stream.Push(startPos, static_cast<uint8_t>(TokenType::DIVIDE));
        case '%':
            if (Match('=')) return stream.Push(startPos, static_cast<uint8_t>(TokenType::MODULO_ASSIGN));
            return stream.Push(startPos, static_cast<uint8_t>(TokenType::MODULO));
        case '=':
            if (Match('=')) return stream.Push(startPos, static_cast<uint8_t>(TokenType::EQUALS));
            return stream.Push(startPos, static_cast<uint8_t>(TokenType::ASSIGN));
        case '!':
            if (Match('=')) return stream.Push(startPos, static_cast<uint8_t>(TokenType::NOT_EQUALS));
            return stream.Push(startPos, static_cast<uint8_t>(TokenType::NOT));
        case '<':
            if (Match('<')) {
                if (Match('=')) return stream.Push(startPos, static_cast<uint8_t>(TokenType::LEFT_SHIFT_ASSIGN));
                return stream.Push(startPos, static_cast<uint8_t>(TokenType::LEFT_SHIFT));
            }
            if (Match('=')) return stream.Push(startPos, static_cast<uint8_t>(TokenType::LESS_EQUAL));
            return stream.Push(startPos, static_cast<uint8_t>(TokenType::LESS));
        case '>':
            if (Match('>')) {
                if (Match('=')) return stream.Push(startPos, static_cast<uint8_t>(TokenType::RIGHT_SHIFT_ASSIGN));
                return stream.Push(startPos, static_cast<uint8_t>(TokenType::RIGHT_SHIFT));
            }
            if (Match('=')) return stream.Push(startPos, static_cast<uint8_t>(TokenType::GREATER_EQUAL));
            return stream.Push(startPos, static_cast<uint8_t>(TokenType::GREATER));
        case '&':
            if (Match('&')) return stream.Push(startPos, static_cast<uint8_t>(TokenType::AND));
            if (Match('=')) return stream.Push(startPos, static_cast<uint8_t>(TokenType::BITWISE_AND_ASSIGN));
            return stream.Push(startPos, static_cast<uint8_t>(TokenType::BITWISE_AND));
        case '|':
            if (Match('|')) return stream.Push(startPos, static_cast<uint8_t>(TokenType::OR));
            if (Match('=')) return stream.Push(startPos, static_cast<uint8_t>(TokenType::BITWISE_OR_ASSIGN));
            return stream.Push(startPos, static_cast<uint8_t>(TokenType::BITWISE_OR));
        case '^':
            if (Match('=')) return stream.Push(startPos, static_cast<uint8_t>(TokenType::BITWISE_XOR_ASSIGN));
            return stream.Push(startPos, static_cast<uint8_t>(TokenType::BITWISE_XOR));
        case '~':
            return stream.Push(startPos, static_cast<uint8_t>(TokenType::BITWISE_NOT));
        case '"': return ScanString();
        // Note: T, U, V for generics are handled as regular identifiers
        // and converted to their token types via FastKeywordLookup
    }

    if (is_digit(c)) {
        current--;
        column--;
        return ScanNumber();
    }

    if (IsIdentStart[static_cast<unsigned char>(c)]) {
        current--;
        column--;
        return ScanIdentifier();
    }

    // Unexpected character - return Error token
    return stream.Push(startPos, static_cast<uint8_t>(TokenType::ERROR_TOKEN));
}

void Lexer::SkipWhitespace() {
    while (true) {
        char c = Peek();
        switch (c) {
            case ' ':
            case '\r':
            case '\t':
                Advance();
                break;
            case '\n':
                line++;
                column = 0;
                Advance();
                break;
            case '/':
                if (PeekNext() == '/') {
                    Advance(); 
                    Advance();
                    SkipComment();
                }
                else  if (PeekNext() == '*') {
                    Advance(); 
                    Advance();
                    SkipMultiLineComment();
                }
                else {
                    return;
                }
                break;
            default:
                return;
        }
    }
}

void Lexer::SkipComment() {
    while (Peek() != '\n' && !IsAtEnd()) {
        Advance();
    }
}

void Lexer::SkipMultiLineComment() {
    unsigned depth = 1;
    while (!IsAtEnd() && depth) {
        char c = Peek();
        if (c == '\n') { line++; column = 0; Advance(); }
        else if (c == '/' && PeekNext() == '*') { Advance(); Advance(); ++depth; }
        else if (c == '*' && PeekNext() == '/') { Advance(); Advance(); --depth; }
        else { Advance(); }
    }    

}

TokenRef Lexer::ScanString() {
    u32 startPos = current;  // Start of string content (after quote)

    while (Peek() != '"' && !IsAtEnd()) {
        if (Peek() == '\n') {
            line++;
            column = 0;
        }
        if (Peek() == '\\') {
            Advance();
            if (!IsAtEnd()) Advance(); // Skip escaped character
        } else {
            Advance();
        }
    }

    if (IsAtEnd()) {
        // Unterminated string - return error token
        return stream.Push(current, static_cast<uint8_t>(TokenType::ERROR_TOKEN));
    }

    Advance(); // Closing quote

    u16 len = static_cast<u16>(current - startPos - 1);  // Exclude closing quote
    return stream.PushWithLength(startPos, static_cast<uint8_t>(TokenType::STRING), len);
}

TokenRef Lexer::ScanNumber() {
    u32 startPos = current;

    auto is_hex_digit = [](unsigned c){
        unsigned d = unsigned(c - '0');
        if (d < 10) return true;
        unsigned lo = unsigned(c | 0x20); // fold to lowercase
        return lo >= 'a' && lo <= 'f';
    };

    // Hex integer: 0x...
    if (Peek() == '0' && (PeekNext() == 'x' || PeekNext() == 'X')) {
        Advance(); // '0'
        Advance(); // 'x'/'X'
        // At least one hex digit (could error-token if none?)
        while (is_hex_digit(static_cast<unsigned>(Peek()))) {
            Advance();
        }
        // Optional unsigned suffix
        if (Peek() == 'u' || Peek() == 'U') {
            Advance();
        }
        u16 len = static_cast<u16>(current - startPos);
        return stream.PushWithLength(startPos, static_cast<uint8_t>(TokenType::NUMBER), len);
    }

    // Binary integer: 0b...
    if (Peek() == '0' && (PeekNext() == 'b' || PeekNext() == 'B')) {
        Advance(); // '0'
        Advance(); // 'b'/'B'
        // At least one binary digit
        while (Peek() == '0' || Peek() == '1') {
            Advance();
        }
        // Optional unsigned suffix
        if (Peek() == 'u' || Peek() == 'U') {
            Advance();
        }
        u16 len = static_cast<u16>(current - startPos);
        return stream.PushWithLength(startPos, static_cast<uint8_t>(TokenType::NUMBER), len);
    }

    // Decimal: int / float / scientific
    // Integer part
    while (is_digit(static_cast<unsigned>(Peek()))) {
        Advance();
    }

    // Fractional part. Accepts `10.5` and also trailing-dot literals
    // like `10.` (C / GLSL both accept these). Two ambiguities to keep
    // in mind:
    //   - Member access: `v.x` — dot followed by identifier start.
    //   - Range operator: `0..10` — dot followed by another dot. The
    //     token here must be an integer `0` so the parser sees
    //     NUMBER(0) DOT_DOT NUMBER(10); if we eat the first dot we
    //     break all for-each loops.
    bool hasFraction = false;
    if (Peek() == '.') {
        char after = PeekNext();
        bool isMemberAccess = (after == '_') || (after >= 'a' && after <= 'z') ||
                              (after >= 'A' && after <= 'Z');
        bool isRangeOp = (after == '.');
        if (!isMemberAccess && !isRangeOp) {
            hasFraction = true;
            Advance(); // '.'
            while (is_digit(static_cast<unsigned>(Peek()))) {
                Advance();
            }
        }
    }

    // Scientific notation: e/E [+-]? digits
    if (Peek() == 'e' || Peek() == 'E') {
        // Only treat as exponent if followed by an optional sign and at least one digit
        char c1 = PeekNext();
        char c2 = (current + 2 < source.length()) ? source[current + 2] : '\0';
        bool validExp = is_digit(static_cast<unsigned>(c1)) ||
                        ((c1 == '+' || c1 == '-') && is_digit(static_cast<unsigned>(c2)));
        if (validExp) {
            Advance(); // 'e'/'E'
            if (Peek() == '+' || Peek() == '-') Advance();
            while (is_digit(static_cast<unsigned>(Peek()))) {
                Advance();
            }
            hasFraction = true; // scientific implies float
        }
    }

    // Float suffix
    if (Peek() == 'f' || Peek() == 'F') {
        Advance();
    } else if (!hasFraction) {
        // Optional unsigned suffix for plain decimal integers
        if (Peek() == 'u' || Peek() == 'U') {
            Advance();
        }
    }

    u16 len = static_cast<u16>(current - startPos);
    return stream.PushWithLength(startPos, static_cast<uint8_t>(TokenType::NUMBER), len);
}


TokenRef Lexer::ScanIdentifier() {
    uint32_t startPos = current;

    while (current < source.length() && char_table[static_cast<unsigned char>(source[current])]) {
        current++;
        column++;
    }

    size_t len = current - startPos;

    // Direct memory comparison for keywords
    TokenType type = FastKeywordLookup(&source[startPos], len);

    return stream.PushWithLength(startPos, static_cast<uint8_t>(type), static_cast<u16>(len));
}

TokenRef Lexer::ScanDecorator() {
    u32 startPos = current;
    while (IsIdentCont[(unsigned char)Peek()]) Advance();
    u16 len = static_cast<u16>(current - startPos);
    return stream.PushWithLength(startPos, static_cast<uint8_t>(TokenType::AT), len);
}

char Lexer::Advance() {
    column++;
    return source[current++];
}

char Lexer::Peek() const {
    if (IsAtEnd()) return '\0';
    return source[current];
}

char Lexer::PeekNext() const {
    if (current + 1 >= source.length()) return '\0';
    return source[current + 1];
}

bool Lexer::Match(char expected) {
    if (IsAtEnd()) return false;
    if (source[current] != expected) return false;
    current++;
    column++;
    return true;
}

TokenType Lexer::FastKeywordLookup(const char* str, size_t len) {
    switch(len) {
        case 2:
            if (memcmp(str, "if", 2) == 0) return TokenType::IF;
            if (memcmp(str, "is", 2) == 0) return TokenType::IS;
            if (memcmp(str, "as", 2) == 0) return TokenType::AS;
            if (memcmp(str, "it", 2) == 0) return TokenType::IT;
            if (memcmp(str, "in", 2) == 0) return TokenType::IN;
            if (memcmp(str, "by", 2) == 0) return TokenType::BY;
            break;
            
        case 3:
            if (memcmp(str, "for", 3) == 0) return TokenType::FOR;
            if (memcmp(str, "int", 3) == 0) return TokenType::INT;
            if (memcmp(str, "use", 3) == 0) return TokenType::USE;
            if (memcmp(str, "out", 3) == 0) return TokenType::OUT;
            break;
            
        case 4:
            if (memcmp(str, "pass", 4) == 0) return TokenType::PASS;
            if (memcmp(str, "node", 4) == 0) return TokenType::NODE;
            if (memcmp(str, "true", 4) == 0) return TokenType::TRUE;
            if (memcmp(str, "bool", 4) == 0) return TokenType::BOOL;
            if (memcmp(str, "mat2", 4) == 0) return TokenType::MAT2;
            if (memcmp(str, "mat3", 4) == 0) return TokenType::MAT3;
            if (memcmp(str, "mat4", 4) == 0) return TokenType::MAT4;
            if (memcmp(str, "int2", 4) == 0) return TokenType::INT2;
            if (memcmp(str, "int3", 4) == 0) return TokenType::INT3;
            if (memcmp(str, "int4", 4) == 0) return TokenType::INT4;
            if (memcmp(str, "uint", 4) == 0) return TokenType::UINT;
            if (memcmp(str, "else", 4) == 0) return TokenType::ELSE;
            if (memcmp(str, "null", 4) == 0) return TokenType::NULL_TOKEN;
            if (memcmp(str, "slot", 4) == 0) return TokenType::SLOT;
            if (memcmp(str, "enum", 4) == 0) return TokenType::ENUM;
            if (memcmp(str, "self", 4) == 0) return TokenType::SELF;
            if (memcmp(str, "loop", 4) == 0) return TokenType::LOOP;
            if (memcmp(str, "skip", 4) == 0) return TokenType::SKIP;
            if (memcmp(str, "eval", 4) == 0) return TokenType::EVAL;
            if (memcmp(str, "case", 4) == 0) return TokenType::CASE;
            break;
            
        case 5:
            if (memcmp(str, "float", 5) == 0) return TokenType::FLOAT;
            if (memcmp(str, "false", 5) == 0) return TokenType::FALSE;
            if (memcmp(str, "const", 5) == 0) return TokenType::CONST;
            if (memcmp(str, "uint2", 5) == 0) return TokenType::UINT2;
            if (memcmp(str, "uint3", 5) == 0) return TokenType::UINT3;
            if (memcmp(str, "uint4", 5) == 0) return TokenType::UINT4;
            if (memcmp(str, "where", 5) == 0) return TokenType::WHERE;
            if (memcmp(str, "until", 5) == 0) return TokenType::UNTIL;
            if (memcmp(str, "range", 5) == 0) return TokenType::RANGE;
            if (memcmp(str, "break", 5) == 0) return TokenType::BREAK;
            if (memcmp(str, "rules", 5) == 0) return TokenType::RULES;
            break;
            
        case 6:
            if (memcmp(str, "vertex", 6) == 0) return TokenType::VERTEX;
            if (memcmp(str, "shader", 6) == 0) return TokenType::SHADER;
            if (memcmp(str, "import", 6) == 0) return TokenType::IMPORT;
            if (memcmp(str, "return", 6) == 0) return TokenType::RETURN;
            if (memcmp(str, "shared", 6) == 0) return TokenType::SHARED;
            if (memcmp(str, "inputs", 6) == 0) return TokenType::INPUTS;
            if (memcmp(str, "float2", 6) == 0) return TokenType::FLOAT2;
            if (memcmp(str, "float3", 6) == 0) return TokenType::FLOAT3;
            if (memcmp(str, "float4", 6) == 0) return TokenType::FLOAT4;
            if (memcmp(str, "module", 6) == 0) return TokenType::MODULE;
            if (memcmp(str, "struct", 6) == 0) return TokenType::STRUCT;
            if (memcmp(str, "buffer", 6) == 0) return TokenType::BUFFER;
            if (memcmp(str, "extend", 6) == 0) return TokenType::EXTEND;
            if (memcmp(str, "switch", 6) == 0) return TokenType::SWITCH;
            break;
            
        case 7:
            if (memcmp(str, "cbuffer", 7) == 0) return TokenType::CBUFFER;
            if (memcmp(str, "sampler", 7) == 0) return TokenType::SAMPLER;
            if (memcmp(str, "compute", 7) == 0) return TokenType::COMPUTE;
            if (memcmp(str, "outputs", 7) == 0) return TokenType::OUTPUTS;
            if (memcmp(str, "extends", 7) == 0) return TokenType::EXTENDS;
            if (memcmp(str, "foreach", 7) == 0) return TokenType::FOREACH;
            if (memcmp(str, "default", 7) == 0) return TokenType::DEFAULT;
            if (memcmp(str, "discard", 7) == 0) return TokenType::DISCARD;
            if (memcmp(str, "require", 7) == 0) return TokenType::REQUIRE;
            break;
            
        case 8:
            if (memcmp(str, "pipeline", 8) == 0) return TokenType::PIPELINE;
            if (memcmp(str, "fragment", 8) == 0) return TokenType::FRAGMENT;
            if (memcmp(str, "readonly", 8) == 0) return TokenType::READONLY;
            if (memcmp(str, "variants", 8) == 0) return TokenType::VARIANTS;
            if (memcmp(str, "conflict", 8) == 0) return TokenType::CONFLICT;
            break;
            
        case 9:
            if (memcmp(str, "texture2D", 9) == 0) return TokenType::TEXTURE2D;
            if (memcmp(str, "texture3D", 9) == 0) return TokenType::TEXTURE3D;
            if (memcmp(str, "resources", 9) == 0) return TokenType::RESOURCES;
            if (memcmp(str, "readwrite", 9) == 0) return TokenType::READWRITE;
            if (memcmp(str, "writeonly", 9) == 0) return TokenType::WRITEONLY;
            break;
            
        case 10:
            if (memcmp(str, "attributes", 10) == 0) return TokenType::ATTRIBUTES;
            if (memcmp(str, "pass_block", 10) == 0) return TokenType::PASS_BLOCK;
            if (memcmp(str, "constraint", 10) == 0) return TokenType::CONSTRAINT;
            break;
            
        case 11:
            if (memcmp(str, "textureCube", 11) == 0) return TokenType::TEXTURECUBE;
            break;
            
        case 13:
            if (memcmp(str, "compute_graph", 13) == 0) return TokenType::COMPUTE_GRAPH;
            break;

        case 14:
            if (memcmp(str, "texture2DArray", 14) == 0) return TokenType::TEXTURE2DARRAY;
            break;
            
        case 15:
            if (memcmp(str, "vertex_function", 15) == 0) return TokenType::VERTEX_FUNCTION;
            break;
            
        case 16:
            if (memcmp(str, "compute_function", 16) == 0) return TokenType::COMPUTE_FUNCTION;
            break;
            
        case 17:
            if (memcmp(str, "fragment_function", 17) == 0) return TokenType::FRAGMENT_FUNCTION;
            break;
    }
    
    return TokenType::IDENTIFIER;
}

std::string Lexer::GetLine(size_t lineNum) const {
    if (lineNum == 0 || lineTable.lineStarts.empty()) return "";

    size_t lineIndex = lineNum - 1;  // Convert 1-based to 0-based
    if (lineIndex >= lineTable.lineStarts.size()) return "";

    u32 lineStart = lineTable.lineStarts[lineIndex];
    u32 lineEnd;

    if (lineIndex + 1 < lineTable.lineStarts.size()) {
        lineEnd = lineTable.lineStarts[lineIndex + 1];
    } else {
        lineEnd = static_cast<u32>(source.length());
    }

    // Trim trailing newline and carriage return
    while (lineEnd > lineStart && (source[lineEnd - 1] == '\n' || source[lineEnd - 1] == '\r')) {
        lineEnd--;
    }

    if (lineStart >= source.length()) return "";

    return source.substr(lineStart, lineEnd - lineStart);
}

} // namespace BWSL
