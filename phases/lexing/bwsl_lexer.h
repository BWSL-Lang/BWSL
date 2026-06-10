#pragma once
#include <array>
#include <string>
#include <cstring>
#include "bwsl_token_stream.h"
#include "bwsl_ast_common.h"
#include "bwsl_utils.h"

namespace BWSL {

// Doc comment blocks (`/// ...` lines or `/** ... */`) captured while
// skipping comments. Stored as raw source spans - no text is materialized
// during lexing; cleaning happens once per documented declaration via
// CleanDocCommentText. Parallel arrays indexed together, arena-backed,
// sorted by tokenIndex. Consecutive `///` lines merge into one span.
struct DocCommentStream {
    ArenaArray<u32> rawStarts;     // Source offset of the block start (markers included for `///`)
    ArenaArray<u32> rawEnds;       // Source offset one past the block end
    ArenaArray<u32> tokenIndices;  // Index of the token each block immediately precedes
    ArenaArray<u32> lines;         // 1-based source line where each block starts
    ArenaArray<u8> isLineComment;  // 1 for `///` blocks, 0 for `/** */` interiors

    u32 Count() const { return tokenIndices.count; }

    // Index of the first block recorded for tokenIndex, or -1. Blocks of
    // different comment styles preceding the same token are stored as
    // adjacent entries with equal tokenIndices.
    s32 FindFirst(u32 tokenIndex) const {
        u32 lo = 0, hi = tokenIndices.count;
        while (lo < hi) {
            u32 mid = lo + (hi - lo) / 2;
            if (tokenIndices[mid] < tokenIndex) lo = mid + 1;
            else hi = mid;
        }
        if (lo < tokenIndices.count && tokenIndices[lo] == tokenIndex) {
            return (s32)lo;
        }
        return -1;
    }
};

// Strips doc comment markers from the raw span [begin, end): per line,
// leading whitespace, a `///` or `*` gutter plus one following space, and
// trailing whitespace; blank lines at either end are dropped. Writes the
// cleaned text to `out`, which must hold at least (end - begin) bytes, and
// returns the cleaned length (not NUL-terminated).
u32 CleanDocCommentText(const char* begin, const char* end, char* out);

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

    // Doc comment spans captured during tokenization
    const DocCommentStream& GetDocComments() const { return docComments; }

private:
    static constexpr auto IsIdentStart = [] {
        std::array<bool, 256> table = {};
        for (unsigned c = 'a'; c <= 'z'; ++c) table[c] = true;
        for (unsigned c = 'A'; c <= 'Z'; ++c) table[c] = true;
        table[static_cast<unsigned>('_')] = true;
        return table;
    }();

    static constexpr auto IsIdentCont = [] {
        std::array<bool, 256> table = IsIdentStart;
        for (unsigned c = '0'; c <= '9'; ++c) table[c] = true;
        return table;
    }();

    static constexpr auto char_table = IsIdentCont;

    std::string source;
    TokenStream& stream;  // Reference to TokenStream to populate
    LineTable lineTable;  // Precomputed line offsets for fast lookup
    DocCommentStream docComments;  // Arena-backed (TokenStream's arena)
    u32 current = 0;  // Changed to uint32_t to match token offset
    u32 line = 1;     // These can be uint32_t too
    u32 column = 1;

    TokenType FastKeywordLookup(const char* str, size_t len);

    char Advance();
    char Peek() const;
    char PeekNext() const;
    bool Match(char expected);
    void SkipWhitespace();
    void SkipComment();
    void SkipMultiLineComment();
    void RecordDocSpan(u32 rawStart, u32 rawEnd, u32 startLine, bool lineComment);

    TokenRef ScanString();
    TokenRef ScanNumber();
    TokenRef ScanIdentifier();
    TokenRef ScanDecorator();
};

} // namespace BWSL
