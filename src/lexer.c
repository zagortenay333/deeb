#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "lexer.h"
#include "report.h"

struct Lexer {
    Mem *mem;
    jmp_buf *error_handler;
    DString *report;

    char *eof;
    String txt;

    char *cursor; // Current position.
    u32 line; // Current line number.

    u32 end_line; // Last line number of most recently eaten token.
    u32 end_offset; // Offset of last byte of most recently eaten token.

    Token ring[MAX_TOKEN_LOOKAHEAD];
    u32   ring_count;
    u32   ring_cursor;
};

#define is_decimal_digit(C) (C >= '0' && C <= '9')
#define is_binary_digit(C)  (C == '0' || C == '1')
#define is_octal_digit(C)   (C >= '0' && C <= '7')
#define is_hex_digit(C)     ((C >= '0' && C <= '9') || (C >= 'a' && C <= 'f') || (C >= 'A' && C <= 'F'))
#define is_whitespace(C)    (C == ' ' || C == '\t' || C == '\r' || C == '\n')
#define is_alpha(C)         ((C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || C == '_' || (unsigned char)C > 127)

static char *tag_to_str [TOKEN_TAG_MAX_] = {
    #define X(_, tag, name) [tag] = name,
    X_TOKENS
    #undef X

    #define X(upp, mid, low) [TOKEN_##upp] = #low,
    X_KEYWORDS
    #undef X
};

static Noreturn error (Lexer *lex, Source src, char *fmt, ...) {
    report_fmt_va(lex->report, REPORT_ERROR, fmt);
    report_source(lex->report, lex->txt, src);
    longjmp(*lex->error_handler, 1);
}

// This is 0-indexed.
static int peek_nth_char (Lexer *lex, u32 lookahead) {
    u32 left_in_buf = (u32)(lex->eof - lex->cursor);
    return (lookahead < left_in_buf) ? lex->cursor[lookahead] : -1;
}

static int peek_char (Lexer *lex) {
    return (lex->cursor < lex->eof) ? *lex->cursor : 0;
}

static int eat_char (Lexer *lex) {
    if (lex->cursor == lex->eof) return 0;
    int result = *lex->cursor++;
    if (result == '\n') lex->line++;
    return result;
}

static u32 eat_char_greedy (Lexer *lex) {
    if (lex->cursor == lex->eof) return 0;
    u32 n_eaten = 1;
    int C = eat_char(lex);
    while (peek_char(lex) == C) { eat_char(lex); n_eaten++; }
    return n_eaten;
}

static void eat_multi_line_comment (Lexer *lex) {
    Source start = {
        .offset     = lex->cursor - lex->txt.data,
        .length     = 1,
        .first_line = lex->line,
        .last_line  = lex->line,
    };

    eat_char(lex); // Eat the '/'.

    u32 n_open_comments = 1;
    u32 n_asterisks = eat_char_greedy(lex);

    while (n_open_comments) {
        switch (peek_char(lex)) {
        case '*':
            if (eat_char_greedy(lex) == n_asterisks && eat_char(lex) == '/') n_open_comments--;
            break;
        case '/':
            eat_char(lex);
            if (peek_char(lex) == '*' && eat_char_greedy(lex) == n_asterisks) n_open_comments++;
            break;
        case 0:
            error(lex, start, "Unterminated comment. " ANSI_CYAN("(Note that asterisks must match: /* */ /** **/ ..."));
            return;
        default:
            eat_char(lex);
            break;
        }
    }
}

static void eat_single_line_comment (Lexer *lex) {
    while (1) {
        int C = eat_char(lex);
        if (C == '\n' || C == 0) break;
    }
}

static void eat_whitespace_and_comments (Lexer *lex) {
    while (1) {
        int C = peek_char(lex);

        if (is_whitespace(C)) {
            eat_char(lex);
        } else if (C == '-') {
            if (peek_nth_char(lex, 1) == '-') eat_single_line_comment(lex);
            else break;
        } else if (C == '/') {
            if (peek_nth_char(lex, 1) == '*') eat_multi_line_comment(lex);
            else break;
        } else {
            break;
        }
    }
}

static String escape_string (Lexer *lex, String str) {
    DString ds     = ds_new_cap(lex->mem, str.count);
    char *cursor   = str.data;
    char *prev_pos = cursor;

    while (1) {
        switch (*cursor++) {
        case '\\': {
            u32 length = (u32)(cursor - prev_pos - 1);
            if (length) ds_add_str(&ds, (String){ prev_pos, length });

            prev_pos = cursor;

            switch (*cursor++) {
            case 'n':  ds_add_byte(&ds, '\n'); prev_pos++; break;
            case '"':  ds_add_byte(&ds, '"'); prev_pos++; break;
            case '\n': while (is_whitespace(*cursor)) { cursor++; } prev_pos = cursor; break;
            }
        } break;

        case '"': {
            u32 len = (u32)(cursor - prev_pos - 1);
            if (len) ds_add_str(&ds, (String){ .data=prev_pos, .count=len, });
            goto brk;
        }
        }
    } brk:

    return ds_to_str(&ds);
}

static void make_string_token (Lexer *lex, Token *token) {
    token->tag = TOKEN_LITERAL_STRING;

    bool escaped = false;

    while (1) {
        token->txt.count++;

        switch (eat_char(lex)) {
        case '"':
            goto brk;
        case '\\':
            escaped = true;
            eat_char(lex);
            token->txt.count++;
            break;
        case 0:
            token->src.length    = 1;
            token->src.last_line = token->src.first_line;
            error(lex, token->src, "Unterminated string literal.");
        }
    } brk:

    ASSERT(token->txt.count > 1);

    String str = { .data = (token->txt.data + 1), .count = (token->txt.count - 2) };
    token->str = escaped ? escape_string(lex, str) : str;
}

static void make_ident_token (Lexer *lex, Token *token) {
    while (1) {
        int C = peek_char(lex);
        if (!is_alpha(C) && !is_decimal_digit(C)) break;
        eat_char(lex);
        token->txt.count++;
    }

    if (false);
    #define X(upp, mid, low)\
        else if (str_match(token->txt, str(#upp))) token->tag = TOKEN_##upp;\
        else if (str_match(token->txt, str(#mid))) token->tag = TOKEN_##upp;\
        else if (str_match(token->txt, str(#low))) token->tag = TOKEN_##upp;
    X_KEYWORDS
    #undef X
    else token->tag = TOKEN_IDENT;
}

static bool str_to_s64 (char *str, s64 *out, int base) {
    errno = 0;
    char *endptr = NULL;
    long result = strtol(str, &endptr, base);
    if (errno != 0 || endptr == str) return false;
    *out = (s64)result;
    return true;
}

static Noreturn error_invalid_int_literal (Lexer *lex, Token *token) {
    token->src.last_line = lex->line;
    token->src.length = token->txt.count;
    error(lex, token->src, "Invalid number literal.");
}

static void make_int_token_binary (Lexer *lex, Token *token) {
    token->tag = TOKEN_LITERAL_INT;
    eat_char(lex); // Eat the 'b' in '0b'.
    token->txt.count++;

    enum { max_digit_count = 64 };
    char c_str[max_digit_count + 1];
    u32 c_str_cursor = 0;

    while (1) {
        int C = peek_char(lex);
        if (C == '_') { token->txt.count += eat_char_greedy(lex); C = peek_char(lex); }
        if (! is_binary_digit(C)) break;
        token->txt.count++;
        if (c_str_cursor == max_digit_count) error_invalid_int_literal(lex, token);
        c_str[c_str_cursor++] = (char)eat_char(lex);
    }

    c_str[c_str_cursor] = 0;
    if (! str_to_s64(c_str, &token->val, 2)) error_invalid_int_literal(lex, token);
}

static void make_int_token_octal (Lexer *lex, Token *token) {
    token->tag = TOKEN_LITERAL_INT;
    eat_char(lex); // Eat the 'o' in '0o'.
    token->txt.count++;

    enum { max_digit_count = 22 };
    char c_str[max_digit_count + 1];
    u32 c_str_cursor = 0;

    while (1) {
        int C = peek_char(lex);
        if (C == '_') { token->txt.count += eat_char_greedy(lex); C = peek_char(lex); }
        if (! is_octal_digit(C)) break;
        token->txt.count++;
        if (c_str_cursor == max_digit_count) error_invalid_int_literal(lex, token);
        c_str[c_str_cursor++] = (char)eat_char(lex);
    }

    c_str[c_str_cursor] = 0;
    if (! str_to_s64(c_str, &token->val, 8)) error_invalid_int_literal(lex, token);
}

static void make_int_token_hex (Lexer *lex, Token *token) {
    token->tag = TOKEN_LITERAL_INT;
    eat_char(lex); // Eat the 'x' in '0x'.
    token->txt.count++;

    enum { max_digit_count = 17 };
    char c_str[max_digit_count + 1];
    u32 c_str_cursor = 0;

    while (1) {
        int C = peek_char(lex);
        if (C == '_') { token->txt.count += eat_char_greedy(lex); C = peek_char(lex); }
        if (! is_hex_digit(C)) break;
        token->txt.count++;
        if (c_str_cursor == max_digit_count) error_invalid_int_literal(lex, token);
        c_str[c_str_cursor++] = (char)eat_char(lex);
    }

    c_str[c_str_cursor] = 0;
    if (! str_to_s64(c_str, &token->val, 16)) error_invalid_int_literal(lex, token);
}

static void make_int_token_decimal (Lexer *lex, Token *token) {
    token->tag = TOKEN_LITERAL_INT;

    enum { max_digit_count = 20 };
    char c_str[max_digit_count + 1];
    u32 c_str_cursor = 0;

    char first_char = *token->txt.data;

    if (first_char == '0' && peek_char(lex) == '0') {
        token->txt.count += eat_char_greedy(lex);
    } else {
        c_str[c_str_cursor++] = first_char;
    }

    while (1) {
        int C = peek_char(lex);
        if (C == '_') { token->txt.count += eat_char_greedy(lex); C = peek_char(lex); }
        if (! is_decimal_digit(C)) break;
        if (c_str_cursor == max_digit_count) error_invalid_int_literal(lex, token);
        c_str[c_str_cursor++] = (char)eat_char(lex);
        token->txt.count++;
    }

    c_str[c_str_cursor] = 0;
    if (! str_to_s64(c_str, &token->val, 10)) error_invalid_int_literal(lex, token);
}

static void make_token (Lexer *lex) {
    eat_whitespace_and_comments(lex);

    u32 idx               = (lex->ring_cursor + lex->ring_count++) & (MAX_TOKEN_LOOKAHEAD - 1);
    Token *token          = &lex->ring[idx];
    token->txt.data       = lex->cursor;
    token->txt.count      = 1;
    token->tag            = (Token_Tag)eat_char(lex);
    token->src.offset     = token->txt.data - lex->txt.data;
    token->src.first_line = lex->line;

    #define extend_to(new_tag) do{\
        eat_char(lex);\
        token->txt.count++;\
        token->tag = new_tag;\
    }while(0)

    switch (token->tag) {
    case '"':
        make_string_token(lex, token);
        break;
    case '!':
        if (peek_char(lex) == '=') extend_to(TOKEN_NOT_EQUAL);
        break;
    default:
        if (is_decimal_digit(token->tag)) {
            switch (peek_char(lex)) {
            case 'x': make_int_token_hex(lex, token); break;
            case 'o': make_int_token_octal(lex, token); break;
            case 'b': make_int_token_binary(lex, token); break;
            default:  make_int_token_decimal(lex, token); break;
            }
        } else if (is_alpha(token->tag)) {
            make_ident_token(lex, token);
        }
        break;
    }

    token->src.last_line = lex->line;
    token->src.length = token->txt.count;

    #undef extend_to
}

// The lifetime of @txt must be greater than that of
// the lexer. The lexer will not modify nor free @txt.
Lexer *lex_new (Mem *mem, String txt, DString *report, jmp_buf *error_handler) {
    Lexer *lex = MEM_ALLOC(mem, sizeof(Lexer));

    *lex = (Lexer){
        .txt           = txt,
        .cursor        = txt.data,
        .eof           = txt.data + txt.count,
        .line          = 1,
        .end_line      = 1,
        .mem           = mem,
        .report        = report,
        .error_handler = error_handler,
    };

    return lex;
}

// Escaped strings will not be freed!
void lex_free (Lexer *lex) {
    MEM_FREE(lex->mem, lex, sizeof(Lexer));
}

Token *lex_peek_nth_token (Lexer *lex, u32 n) {
    ASSERT(n > 0 && n <= MAX_TOKEN_LOOKAHEAD);
    while (lex->ring_count < n) make_token(lex);
    u32 idx = (lex->ring_cursor + n - 1) & (MAX_TOKEN_LOOKAHEAD - 1);
    return &lex->ring[idx];
}

Token *lex_peek_token (Lexer *lex) {
    return lex_peek_nth_token(lex, 1);
}

Token *lex_peek_the_token (Lexer *lex, Token_Tag tag) {
    Token *token = lex_peek_nth_token(lex, 1);
    if (token->tag != tag) error(lex, token->src, "Expected '%s'.", tag_to_str[tag]);
    return token;
}

Token *lex_try_peek_token (Lexer *lex, Token_Tag tag) {
    Token *token = lex_peek_token(lex);
    return (token->tag == tag) ? token : NULL;
}

Token *lex_try_peek_nth_token (Lexer *lex, u32 nth, Token_Tag tag) {
    Token *token = lex_peek_nth_token(lex, nth);
    return (token->tag == tag) ? token : NULL;
}

Token *lex_eat_token (Lexer *lex) {
    Token *token     = lex_peek_token(lex);
    lex->ring_count -= 1;
    lex->ring_cursor = (lex->ring_cursor + 1) & (MAX_TOKEN_LOOKAHEAD - 1);
    lex->end_line    = token->src.last_line;
    lex->end_offset  = token->src.offset + token->src.length;
    return token;
}

Token *lex_try_eat_token (Lexer *lex, Token_Tag tag) {
    return (lex_peek_token(lex)->tag == tag) ? lex_eat_token(lex) : NULL;
}

Token *lex_eat_the_token (Lexer *lex, Token_Tag tag) {
    Token *token = lex_eat_token(lex);
    if (token->tag != tag) error(lex, token->src, "Expected '%s'.", tag_to_str[tag]);
    return token;
}

char *lex_token_to_str (Token_Tag tag) {
    return tag_to_str[tag];
}

u32 lex_get_prev_end_line (Lexer *lex) {
    return lex->end_line;
}

u32 lex_get_prev_end_offset (Lexer *lex) {
    return lex->end_offset;
}
