#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <stdarg.h>

#include <readline/readline.h>
#include <readline/history.h>

#include "db.h"
#include "files.h"
#include "report.h"
#include "common.h"
#include "string.h"

typedef struct {
    Database *db;

    Files *fs;

    Mem_Clib  *mem_clib;
    Mem_Track *mem_root;

    String prog_name;
    String db_file_path;
    String query_file_path;

    struct {
        jmp_buf jmp;
        void (*print)(void);
    } error;
} Shell;

static void print_available_cli_flags (void) {
    printf(
        "Command line options:\n\n"
        "    -h           Print command line usage.\n"
        "    -d <path>    Database file path. Cannot be omitted.\n"
        "    -i <path>    If this flag is omitted, the shell starts.\n"
        "                 Otherwise, the input file will be run as a query.\n"
        "\n"
    );
}

static Noreturn error (Shell *sh, char *fmt, ...) {
    printf(ANSI_RED("ERROR: "));

    va_list va;
    va_start(va, fmt);
    vprintf(fmt, va);
    va_end(va);

    printf("\n");

    if (sh->error.print) {
        printf("\n");
        sh->error.print();
    }

    longjmp(sh->error.jmp, 1);
}

typedef struct {
    Shell *sh;
    char **tokens;
    char **current;
} Prompt_Lexer;

static Prompt_Lexer plex_new (Shell *sh, char **tokens) {
    return (Prompt_Lexer){
        .sh      = sh,
        .tokens  = tokens,
        .current = tokens,
    };
}

static char *plex_try_eat_token (Prompt_Lexer *lex) {
    return *lex->current ? *lex->current++ : NULL;
}

static char *plex_eat_token (Prompt_Lexer *lex, char *msg) {
    char *tok = plex_try_eat_token(lex);
    if (! tok) error(lex->sh, msg);
    return tok;
}

static void cli_parse (Shell *sh, int argc, char **argv) {
    Prompt_Lexer lex = plex_new(sh, argv);

    sh->prog_name = str(plex_try_eat_token(&lex));
    bool help_flag_set = false;

    while (1) {
        char *tok = plex_try_eat_token(&lex);

        if (! tok) {
            break;
        } else if (! strcmp(tok, "-h")) {
            help_flag_set = true;
        } else if (! strcmp(tok, "-i")) {
            sh->query_file_path = str(plex_eat_token(&lex, "Missing argument for '-i' flag."));
        } else if (! strcmp(tok, "-d")) {
            sh->db_file_path = str(plex_eat_token(&lex, "Missing argument for '-d' flag."));
        } else {
            error(sh, "Unknown command line argument: %s", tok);
        }
    }

    if (help_flag_set) print_available_cli_flags();

    if (! sh->db_file_path.data) {
        if (help_flag_set) longjmp(sh->error.jmp, 1);
        error(sh, "The '-d' flag is missing.");
    }
}

static void print_available_commands (void) {
    printf(
        "Available commands:\n\n"
        "    -h             Print available commands.\n"
        "    -run <path>    Run the file at <path> as a query.\n"
        "\n"
    );
}

static void run_query (Shell *sh, String query, Mem *mem) {
    DString report = ds_new(mem);
    db_run(sh->db, query, &report);
    ds_print(&report);
}

static void eval_command (Shell *sh, char *line, Mem_Arena *arena) {
    Prompt_Lexer lex = plex_new(sh, history_tokenize(line));

    if (setjmp(sh->error.jmp)) goto done;
    sh->error.print = print_available_commands;

    while (1) {
        char *tok = plex_try_eat_token(&lex);

        if (! tok) {
            break;
        } else if (! strcmp(tok, "-h")) {
            print_available_commands();
        } else if (! strcmp(tok, "-run")) {
            char *path = plex_eat_token(&lex, "Missing argument for '-run' command.");
            String query = fs_read_entire_file_p(sh->fs, str(path), (Mem*)arena);
            run_query(sh, query, (Mem*)arena);
        } else {
            error(sh, "The command '%s' is unknown.", tok);
        }
    }

    done: {
        char **cursor = lex.tokens;
        while (*cursor) free(*cursor++);
        free(lex.tokens);
    }
}

static char *get_prompt (Shell *sh) {
    DString ds = ds_new_cap((Mem*)sh->mem_root, 32);

    // We must tell readline to not count invisible escape sequences in the prompt.
    ds_add_fmt(&ds, "%c%s%c", RL_PROMPT_START_IGNORE, ANSI_START_BOLD_MAGENTA, RL_PROMPT_END_IGNORE);
    ds_add_cstr(&ds, "dbms >>> ");
    ds_add_fmt(&ds, "%c%s%c", RL_PROMPT_START_IGNORE, ANSI_END, RL_PROMPT_END_IGNORE);

    return ds_to_cstr(&ds);
}

static void save_line_to_history (Shell *sh, char *line) {
    // Readline doesn't seem to deduplicate consecutive
    // lines by default, so we do it manually. Also, the
    // functions current_history() and history_get() are
    // unable to reliably obtain the previous line, so
    // we access the entries array directly.
    if (history_length) {
        HIST_ENTRY *prev = history_list()[history_length - 1];
        if (strcmp(prev->line, line)) add_history(line);
    } else {
        add_history(line);
    }
}

static void start_shell (Shell *sh) {
    char *history_file_path = "/tmp/.mydb_shell_history";

    fs_create_file(sh->fs, str(history_file_path));
    read_history(history_file_path);

    printf("Type '-h' for help.\n");

    char *prompt = get_prompt(sh);
    Mem_Arena *arena = mem_arena_new((Mem*)sh->mem_root, 1*MB);

    while (1) {
        char *line = readline(prompt);
        if (! line) break;

        if (*line) {
            save_line_to_history(sh, line);

            if (*line == '-') {
                eval_command(sh, line, arena);
            } else {
                run_query(sh, str(line), (Mem*)arena);
            }
        }

        free(line);
        mem_arena_clear(arena);
    }

    write_history(history_file_path);
    printf("\n");
}

int main (int argc, char **argv) {
    Shell sh = {0};

    if (setjmp(sh.error.jmp)) goto done;
    sh.error.print = print_available_cli_flags;

    sh.mem_clib = mem_clib_new();
    sh.mem_root = mem_track_new((Mem*)sh.mem_clib);
    sh.fs       = fs_new((Mem*)sh.mem_root);

    cli_parse(&sh, argc, argv);

    db_init(&sh.db, sh.db_file_path, NULL);

    if (sh.query_file_path.data) {
        String query = fs_read_entire_file_p(sh.fs, sh.query_file_path, (Mem*)sh.mem_root);
        run_query(&sh, query, (Mem*)sh.mem_root);
    } else {
        start_shell(&sh);
    }

    done: {
        if (sh.db) db_close(sh.db);
        fs_destroy(sh.fs);
        mem_track_destroy(sh.mem_root);
        mem_clib_destroy(sh.mem_clib);
    }
}
