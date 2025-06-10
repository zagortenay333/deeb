#pragma once

#include <setjmp.h>

#include "plan.h"
#include "string.h"
#include "common.h"

// X(uppercase, camelcase, lowercase)
//
// To add a new keyword just add a new entry into this
// list. After that whenever the lexer encounters this
// keyword it will emit a token with tag TOKEN_##upp.
#define X_KEYWORDS\
    X(ON, On, on)\
    X(OR, Or, or)\
    X(BY, By, by)\
    X(IS, Is, is)\
    X(AS, As, as)\
    X(RUN, Run, run)\
    X(KEY, Key, key)\
    X(SET, Set, set)\
    X(INT, Int, int)\
    X(NOT, Not, not)\
    X(AND, And, and)\
    X(ASC, Asc, asc)\
    X(DESC, Desc, desc)\
    X(CHAR, Char, char)\
    X(NULL, Null, null)\
    X(INTO, Into, into)\
    X(DROP, Drop, drop)\
    X(TEXT, Text, text)\
    X(FROM, From, from)\
    X(JOIN, Join, join)\
    X(BOOL, Bool, bool)\
    X(TRUE, True, true)\
    X(FALSE, False, false)\
    X(CROSS, Cross, cross)\
    X(INNER, Inner, inner)\
    X(TABLE, Table, table)\
    X(GROUP, Group, group)\
    X(ORDER, Order, order)\
    X(WHERE, Where, where)\
    X(LIMIT, Limit, limit)\
    X(UPDATE, Update, update)\
    X(OFFSET, Offset, offset)\
    X(HAVING, Having, having)\
    X(CREATE, Create, create)\
    X(INSERT, Insert, insert)\
    X(DELETE, Delete, delete)\
    X(SELECT, Select, select)\
    X(EXPLAIN, Explain, explain)\
    X(PRIMARY, Primary, primary)

// X(tag_value, tag, name)
//
// This is a list of non-keyword tokens. The tag_value
// matches the ASCII encoding so that we can use chars
// like '!' instead of TOKEN_EXCLAMATION.
#define X_TOKENS\
    X(0,    TOKEN_EOF, "EOF")\
    X('!',  TOKEN_EXCLAMATION, "!")\
    X('"',  TOKEN_DOUBLE_QUOTE, "\"")\
    X('#',  TOKEN_HASH, "#")\
    X('$',  TOKEN_DOLLAR, "$")\
    X('%',  TOKEN_PERCENT, "%")\
    X('&',  TOKEN_AMPERSAND, "&")\
    X('\'', TOKEN_SINGLE_QUOTE, "'")\
    X('(',  TOKEN_OPEN_PAREN, "(")\
    X(')',  TOKEN_CLOSED_PAREN, ")")\
    X('*',  TOKEN_ASTERISK, "*")\
    X('+',  TOKEN_PLUS, "+")\
    X(',',  TOKEN_COMMA, ",")\
    X('-',  TOKEN_MINUS, "-")\
    X('.',  TOKEN_DOT, ".")\
    X('/',  TOKEN_SLASH, "/")\
    X(':',  TOKEN_COLON, ":")\
    X(';',  TOKEN_SEMICOLON, ";")\
    X('<',  TOKEN_LESS, "<")\
    X('=',  TOKEN_EQUAL, "=")\
    X('>',  TOKEN_GREATER, ">")\
    X('?',  TOKEN_QUESTION_MARK, "?")\
    X('@',  TOKEN_AT, "@")\
    X('[',  TOKEN_OPEN_BRACKET, "[")\
    X('\\', TOKEN_BACKSLASH, "\\")\
    X(']',  TOKEN_CLOSED_BRACKET, "]")\
    X('^',  TOKEN_CARET, "^")\
    X('_',  TOKEN_UNDERSCORE, "_")\
    X('`',  TOKEN_BACKTICK, "`")\
    X('{',  TOKEN_OPEN_BRACE, "{")\
    X('|',  TOKEN_VBAR, "|")\
    X('}',  TOKEN_CLOSED_BRACE, "}")\
    X('~',  TOKEN_TILDE, "~")\
    X(256,  TOKEN_NOT_EQUAL, "!=")\
    X(257,  TOKEN_LESS_EQUAL, "<=")\
    X(258,  TOKEN_GREATER_EQUAL, ">=")\
    X(259,  TOKEN_IDENT, "identifier")\
    X(260,  TOKEN_LITERAL_INT, "literal int")\
    X(261,  TOKEN_LITERAL_STRING, "literal string")

typedef enum {
    #define X(tag_value, tag, ...) tag = tag_value,
    X_TOKENS
    #undef X

    #define X(upp, ...) TOKEN_##upp,
    X_KEYWORDS
    #undef X

    TOKEN_TAG_MAX_
} Token_Tag;

typedef struct {
    Token_Tag tag;

    String txt; // View into Lexer.txt.
    Source src;

    union {
        // For TOKEN_LITERAL_INT.
        s64 val;

        // For TOKEN_LITERAL_STRING.
        //
        // If the string has no escape sequences, then this
        // is a view into Lexer.txt with quotes excluded.
        //
        // If the string has escapes, then this is a view
        // into an escaped string allocated by Lexer.mem.
        // That string will not be freed by lexer_free().
        String str;
    };
} Token;

typedef struct Lexer Lexer;

// IMPORTANT: Instead of a giant array of tokens, we maintain a
// small ring buffer of size MAX_TOKEN_LOOKAHEAD. This means that
// a token returned by the lexer will eventually get overwritten.
//
// Make sure to keep this number greater than 1 and a power of 2.
#define MAX_TOKEN_LOOKAHEAD 8

Lexer *lex_new                 (Mem *, String, DString *report, jmp_buf *error_handler);
void   lex_free                (Lexer *);
Token *lex_peek_token          (Lexer *);
Token *lex_peek_the_token      (Lexer *, Token_Tag);
Token *lex_peek_nth_token      (Lexer *, u32 nth); // 1-indexed
Token *lex_try_peek_token      (Lexer *, Token_Tag);
Token *lex_try_peek_nth_token  (Lexer *, u32 nth, Token_Tag); // 1-indexed
Token *lex_eat_token           (Lexer *);
Token *lex_eat_the_token       (Lexer *, Token_Tag);
Token *lex_try_eat_token       (Lexer *, Token_Tag);
char  *lex_token_to_str        (Token_Tag);
u32    lex_get_prev_end_line   (Lexer *);
u32    lex_get_prev_end_offset (Lexer *);
