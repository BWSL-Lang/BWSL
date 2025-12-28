#pragma once
#include <string>
#include <cstring>
#include "bwsl_token_stream.h"
#include "bwsl_utils.h"

namespace BWSL {

class Lexer {
public:
    // Initialize lexer with source and TokenStream to populate
    Lexer(const std::string& source, TokenStream& stream);

    // Tokenize next token and push to stream, returns TokenRef
    TokenRef NextToken();

    // Tokenize entire source into stream
    void Tokenize();

    bool IsAtEnd() const { return current >= source.length(); }

    // Provide base pointer for token resolution
    const char* GetSourceBase() const { return source.data(); }

    // Fast O(log n) source location lookup
    SourceLocation GetSourceLocation(u32 offset) const { return lineTable.Get(offset); }

    // Access line table directly if needed
    const LineTable& GetLineTable() const { return lineTable; }

    // For convenient token handling (via TokenRef)
    std::string_view GetTokenValue(TokenRef token) const {
        return stream.GetValue(token);
    }

    // For error reporting
    std::string GetLine(size_t lineNum) const;
    
    static constexpr bool IsIdentStart[256] = {
        ['a' ... 'z'] = true,
        ['A' ... 'Z'] = true,
        ['_'] = true,
    };
    
    static constexpr bool IsIdentCont[256] = {
        ['a' ... 'z'] = true,
        ['A' ... 'Z'] = true,
        ['0' ... '9'] = true,
        ['_'] = true,
    };
    
private:
    std::string source;
    TokenStream& stream;  // Reference to TokenStream to populate
    LineTable lineTable;  // Precomputed line offsets for fast lookup
    u32 current = 0;  // Changed to uint32_t to match token offset
    u32 line = 1;     // These can be uint32_t too
    u32 column = 1;

    // Lookup tables for character classification
    static constexpr bool char_table[256] = {
        ['0' ... '9'] = true,
        ['A' ... 'Z'] = true,
        ['a' ... 'z'] = true,
        ['_'] = true
    };

    TokenType FastKeywordLookup(const char* str, size_t len);

    char Advance();
    char Peek() const;
    char PeekNext() const;
    bool Match(char expected);
    void SkipWhitespace();
    void SkipComment();
    void SkipMultiLineComment();

    TokenRef ScanString();
    TokenRef ScanNumber();
    TokenRef ScanIdentifier();
    TokenRef ScanDecorator();
};

} // namespace BWSL