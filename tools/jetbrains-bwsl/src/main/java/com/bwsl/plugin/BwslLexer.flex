package com.bwsl.plugin;

import com.intellij.lexer.FlexLexer;
import com.intellij.psi.tree.IElementType;
import com.intellij.psi.TokenType;
import com.bwsl.plugin.BwslTokenTypes;

%%

%class _BwslLexer
%implements FlexLexer
%unicode
%function advance
%type IElementType

%eof{  return;
%eof}

///////////////////////////////////////////////////////////////////////////////
// Macros

LINE_TERM    = \r | \n | \r\n
WHITE_SPACE  = [ \t\f] | {LINE_TERM}
IDENT        = [a-zA-Z_][a-zA-Z0-9_]*

HEX_DIGIT    = [0-9a-fA-F]
BIN_DIGIT    = [01]
HEX_NUM      = 0 [xX] {HEX_DIGIT}+ [uU]?
BIN_NUM      = 0 [bB] {BIN_DIGIT}+ [uU]?
DEC_FRAC     = \. [0-9]+
DEC_EXP      = [eE] [+\-]? [0-9]+
DEC_NUM      = [0-9]+ {DEC_FRAC}? {DEC_EXP}? [fFuU]?
NUMBER_LIT   = {HEX_NUM} | {BIN_NUM} | {DEC_NUM}

STRING_LIT   = \" ( [^\"\\\r\n] | \\ [^\r\n] )* \"
LINE_CMT     = "//" [^\r\n]*
BLOCK_CMT    = "/*" ( [^*] | \*+ [^*/] )* \*+ "/"

%%

<YYINITIAL> {

  // Whitespace & comments (must be first)
  {WHITE_SPACE}   { return TokenType.WHITE_SPACE; }
  {LINE_CMT}      { return BwslTokenTypes.LINE_COMMENT; }
  {BLOCK_CMT}     { return BwslTokenTypes.BLOCK_COMMENT; }

  // String literal
  {STRING_LIT}    { return BwslTokenTypes.STRING_LIT; }

  // Decorator: @identifier consumed as one token
  "@" {IDENT}     { return BwslTokenTypes.DECORATOR; }

  // Number (before identifiers so "0x..." isn't split)
  {NUMBER_LIT}    { return BwslTokenTypes.NUMBER_LIT; }

  // ── Structural keywords ────────────────────────────────────────────────────
  // Longer keywords that share a prefix must come first (e.g. compute_graph > compute,
  // texture2DArray > texture2D, int64x2 > int64 > int2 > int).

  "compute_graph"     { return BwslTokenTypes.KW_COMPUTE_GRAPH; }
  "compute"           { return BwslTokenTypes.KW_COMPUTE; }
  "module"            { return BwslTokenTypes.KW_MODULE; }
  "pipeline"          { return BwslTokenTypes.KW_PIPELINE; }
  "pass"              { return BwslTokenTypes.KW_PASS; }
  "vertex"            { return BwslTokenTypes.KW_VERTEX; }
  "fragment"          { return BwslTokenTypes.KW_FRAGMENT; }
  "attributes"        { return BwslTokenTypes.KW_ATTRIBUTES; }
  "resources"         { return BwslTokenTypes.KW_RESOURCES; }
  "variants"          { return BwslTokenTypes.KW_VARIANTS; }
  "struct"            { return BwslTokenTypes.KW_STRUCT; }
  "enum"              { return BwslTokenTypes.KW_ENUM; }
  "eval"              { return BwslTokenTypes.KW_EVAL; }
  "node"              { return BwslTokenTypes.KW_NODE; }
  "inputs"            { return BwslTokenTypes.KW_INPUTS; }
  "outputs"           { return BwslTokenTypes.KW_OUTPUTS; }

  // ── Control / declaration keywords ────────────────────────────────────────
  "import"            { return BwslTokenTypes.KW_IMPORT; }
  "using"             { return BwslTokenTypes.KW_USING; }
  "as"                { return BwslTokenTypes.KW_AS; }
  "use"               { return BwslTokenTypes.KW_USE; }
  "const"             { return BwslTokenTypes.KW_CONST; }
  "shared"            { return BwslTokenTypes.KW_SHARED; }
  "constraint"        { return BwslTokenTypes.KW_CONSTRAINT; }
  "rules"             { return BwslTokenTypes.KW_RULES; }
  "require"           { return BwslTokenTypes.KW_REQUIRE; }
  "conflict"          { return BwslTokenTypes.KW_CONFLICT; }
  "return"            { return BwslTokenTypes.KW_RETURN; }
  "if"                { return BwslTokenTypes.KW_IF; }
  "else"              { return BwslTokenTypes.KW_ELSE; }
  "foreach"           { return BwslTokenTypes.KW_FOREACH; }
  "for"               { return BwslTokenTypes.KW_FOR; }
  "while"             { return BwslTokenTypes.KW_WHILE; }
  "loop"              { return BwslTokenTypes.KW_LOOP; }
  "switch"            { return BwslTokenTypes.KW_SWITCH; }
  "case"              { return BwslTokenTypes.KW_CASE; }
  "default"           { return BwslTokenTypes.KW_DEFAULT; }
  "break"             { return BwslTokenTypes.KW_BREAK; }
  "skip"              { return BwslTokenTypes.KW_SKIP; }
  "continue"          { return BwslTokenTypes.KW_CONTINUE; }
  "discard"           { return BwslTokenTypes.KW_DISCARD; }
  "in"                { return BwslTokenTypes.KW_IN; }
  "by"                { return BwslTokenTypes.KW_BY; }
  "until"             { return BwslTokenTypes.KW_UNTIL; }
  "null"              { return BwslTokenTypes.KW_NULL; }
  "true"              { return BwslTokenTypes.KW_TRUE; }
  "false"             { return BwslTokenTypes.KW_FALSE; }
  "self"              { return BwslTokenTypes.KW_SELF; }
  "readonly"          { return BwslTokenTypes.KW_READONLY; }
  "readwrite"         { return BwslTokenTypes.KW_READWRITE; }
  "writeonly"         { return BwslTokenTypes.KW_WRITEONLY; }
  "vertex_function"   { return BwslTokenTypes.KW_VERTEX_FUNCTION; }
  "fragment_function" { return BwslTokenTypes.KW_FRAGMENT_FUNCTION; }
  "compute_function"  { return BwslTokenTypes.KW_COMPUTE_FUNCTION; }
  "pass_block"        { return BwslTokenTypes.KW_PASS_BLOCK; }

  // ── Type keywords (longer prefixes first) ─────────────────────────────────
  "bool"              { return BwslTokenTypes.KW_BOOL; }
  "uint64x2"          { return BwslTokenTypes.KW_UINT64X2; }
  "uint64x3"          { return BwslTokenTypes.KW_UINT64X3; }
  "uint64x4"          { return BwslTokenTypes.KW_UINT64X4; }
  "uint64"            { return BwslTokenTypes.KW_UINT64; }
  "uint2"             { return BwslTokenTypes.KW_UINT2; }
  "uint3"             { return BwslTokenTypes.KW_UINT3; }
  "uint4"             { return BwslTokenTypes.KW_UINT4; }
  "uint"              { return BwslTokenTypes.KW_UINT; }
  "int64x2"           { return BwslTokenTypes.KW_INT64X2; }
  "int64x3"           { return BwslTokenTypes.KW_INT64X3; }
  "int64x4"           { return BwslTokenTypes.KW_INT64X4; }
  "int64"             { return BwslTokenTypes.KW_INT64; }
  "int2"              { return BwslTokenTypes.KW_INT2; }
  "int3"              { return BwslTokenTypes.KW_INT3; }
  "int4"              { return BwslTokenTypes.KW_INT4; }
  "int"               { return BwslTokenTypes.KW_INT; }
  "float2"            { return BwslTokenTypes.KW_FLOAT2; }
  "float3"            { return BwslTokenTypes.KW_FLOAT3; }
  "float4"            { return BwslTokenTypes.KW_FLOAT4; }
  "float"             { return BwslTokenTypes.KW_FLOAT; }
  "double2"           { return BwslTokenTypes.KW_DOUBLE2; }
  "double3"           { return BwslTokenTypes.KW_DOUBLE3; }
  "double4"           { return BwslTokenTypes.KW_DOUBLE4; }
  "double"            { return BwslTokenTypes.KW_DOUBLE; }
  "dmat2"             { return BwslTokenTypes.KW_DMAT2; }
  "dmat3"             { return BwslTokenTypes.KW_DMAT3; }
  "dmat4"             { return BwslTokenTypes.KW_DMAT4; }
  "mat2"              { return BwslTokenTypes.KW_MAT2; }
  "mat3"              { return BwslTokenTypes.KW_MAT3; }
  "mat4"              { return BwslTokenTypes.KW_MAT4; }
  "sampler"           { return BwslTokenTypes.KW_SAMPLER; }
  "texture2DArray"    { return BwslTokenTypes.KW_TEXTURE2DARRAY; }
  "texture2D"         { return BwslTokenTypes.KW_TEXTURE2D; }
  "texture3D"         { return BwslTokenTypes.KW_TEXTURE3D; }
  "textureCube"       { return BwslTokenTypes.KW_TEXTURECUBE; }
  "image2D"           { return BwslTokenTypes.KW_IMAGE2D; }
  "cbuffer"           { return BwslTokenTypes.KW_CBUFFER; }
  "buffer"            { return BwslTokenTypes.KW_BUFFER; }
  "void"              { return BwslTokenTypes.KW_VOID; }

  // ── Identifiers (after all keywords) ──────────────────────────────────────
  {IDENT}             { return BwslTokenTypes.IDENTIFIER; }

  // ── Operators (longest match first) ───────────────────────────────────────
  "..="               { return BwslTokenTypes.DOTDOTEQ; }
  ".."                { return BwslTokenTypes.DOTDOT; }
  "."                 { return BwslTokenTypes.DOT; }
  "::"                { return BwslTokenTypes.COLONCOLON; }
  ":"                 { return BwslTokenTypes.COLON; }
  "->"                { return BwslTokenTypes.ARROW; }
  "<<="               { return BwslTokenTypes.LSHIFTEQ; }
  ">>="               { return BwslTokenTypes.RSHIFTEQ; }
  "<<"                { return BwslTokenTypes.LSHIFT; }
  ">>"                { return BwslTokenTypes.RSHIFT; }
  "<="                { return BwslTokenTypes.LE; }
  ">="                { return BwslTokenTypes.GE; }
  "=="                { return BwslTokenTypes.EQEQ; }
  "!="                { return BwslTokenTypes.NEQ; }
  "++"                { return BwslTokenTypes.PLUSPLUS; }
  "--"                { return BwslTokenTypes.MINUSMINUS; }
  "+="                { return BwslTokenTypes.PLUSEQ; }
  "-="                { return BwslTokenTypes.MINUSEQ; }
  "*="                { return BwslTokenTypes.STAREQ; }
  "/="                { return BwslTokenTypes.SLASHEQ; }
  "%="                { return BwslTokenTypes.PERCENTEQ; }
  "&="                { return BwslTokenTypes.AMPEQ; }
  "|="                { return BwslTokenTypes.PIPEEQ; }
  "^="                { return BwslTokenTypes.CARETEQ; }
  "&&"                { return BwslTokenTypes.AND; }
  "||"                { return BwslTokenTypes.OR; }
  "<"                 { return BwslTokenTypes.LT; }
  ">"                 { return BwslTokenTypes.GT; }
  "="                 { return BwslTokenTypes.EQ; }
  "+"                 { return BwslTokenTypes.PLUS; }
  "-"                 { return BwslTokenTypes.MINUS; }
  "*"                 { return BwslTokenTypes.STAR; }
  "/"                 { return BwslTokenTypes.SLASH; }
  "%"                 { return BwslTokenTypes.PERCENT; }
  "&"                 { return BwslTokenTypes.AMP; }
  "|"                 { return BwslTokenTypes.PIPE; }
  "^"                 { return BwslTokenTypes.CARET; }
  "~"                 { return BwslTokenTypes.TILDE; }
  "!"                 { return BwslTokenTypes.BANG; }
  "{"                 { return BwslTokenTypes.LBRACE; }
  "}"                 { return BwslTokenTypes.RBRACE; }
  "("                 { return BwslTokenTypes.LPAREN; }
  ")"                 { return BwslTokenTypes.RPAREN; }
  "["                 { return BwslTokenTypes.LBRACKET; }
  "]"                 { return BwslTokenTypes.RBRACKET; }
  ";"                 { return BwslTokenTypes.SEMI; }
  ","                 { return BwslTokenTypes.COMMA; }
  "?"                 { return BwslTokenTypes.QUESTION; }

  // Fallback — truly unrecognised character
  [^]                 { return TokenType.BAD_CHARACTER; }
}
