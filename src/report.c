#include <stdio.h>

#include "array.h"
#include "report.h"

static char *get_line_start (char *cursor, char *stop_at) {
    while (1) {
        if (cursor == stop_at) break;
        if (*--cursor == '\n') { cursor++; break; }
    }

    return cursor;
}

static char *get_line_end (char *cursor, char *stop_at) {
    while (cursor != stop_at && *cursor++ != '\n');
    return cursor;
}

typedef struct {
    char *eof;    // txt.data + txt.count
    String txt;   // The text that contains the source.
    String src;   // View into @txt of source.
    u32 L0, L1;   // First and last line numbers of source. 1-indexed.
    u32 col;      // Start column of source. 0-indexed.
    String line;  // View into @txt of current line.
    u32 line_num; // Current line number.

    #define INDENTATION          4
    #define TOP_PADDING          2
    #define BOTTOM_PADDING       2
    #define MAX_SOURCE_DISTANCE  10
    #define TEXT_NORMAL_COLOR    ANSI_START_CYAN
    #define TEXT_HIGHLIGHT_COLOR ANSI_START_RED
} Source_Printer;

static Source_Printer sp_new (String txt, Source src) {
    ASSERT(src.offset < txt.count);

    Source_Printer sp = {0};
    sp.txt            = txt,
    sp.eof            = txt.data + txt.count,
    sp.src.data       = txt.data + src.offset,
    sp.src.count      = src.length,
    sp.L0             = src.first_line,
    sp.L1             = src.last_line,
    sp.line_num       = src.first_line,
    sp.line.data      = get_line_start(sp.src.data, txt.data);
    sp.line.count     = get_line_end(sp.src.data, sp.eof) - sp.line.data;
    sp.col            = sp.src.data - sp.line.data;

    return sp;
}

static bool sp_next_line (Source_Printer *sp) {
    char *start = sp->line.data + sp->line.count;
    if (start == sp->eof) return false;

    sp->line.data  = start;
    sp->line.count = get_line_end(start, sp->eof) - start;
    sp->line_num++;
    return true;
}

static bool sp_prev_line (Source_Printer *sp) {
    char *cursor = sp->line.data;
    if (cursor == sp->txt.data) return false;

    cursor = get_line_start(cursor - 1, sp->txt.data);
    sp->line.count = sp->line.data - cursor;
    sp->line.data  = cursor;
    sp->line_num--;
    return true;
}

static void sp_print_line_header (Source_Printer *sp, DString *ds, u32 left_margin) {
    ds_add_fmt(ds, TEXT_NORMAL_COLOR "%*i | " ANSI_END, left_margin, sp->line_num);
}

static void sp_print_normal_text (DString *ds, char *data, u32 len) {
    ds_add_fmt(ds, TEXT_NORMAL_COLOR "%.*s" ANSI_END, (int)len, data);
}

static void sp_print_highlighted_text (DString *ds, char *data, u32 len) {
    ds_add_fmt(ds, TEXT_HIGHLIGHT_COLOR "%.*s" ANSI_END, (int)len, data);
}

// @top_padding:
// Number of lines above the first line of the source to print.
// If set to -1, the first line of the source will be printed
// without indentation and only the part including the source.
//
// @bottom_padding:
// Number of lines below the last line of the source to print.
// If set to -1, then the last line will be printed only up to
// the column that includes the source.
//
// @left_margin:
// Minimum width of the left margin of each line. Printed line
// numbers will fit into this margin if they can.
static void sp_print (Source_Printer *sp, DString *ds, s32 top_padding, s32 bottom_padding, u32 left_margin) {
    String first_line = sp->line; // So we can move back to first line quickly.

    if (top_padding >= 0) {
        if (top_padding > 0) {
            for (s32 i = 0; i < top_padding; ++i) if (! sp_prev_line(sp)) break;

            while (sp->line_num < sp->L0) {
                sp_print_line_header(sp, ds, left_margin);
                sp_print_normal_text(ds, sp->line.data, sp->line.count);
                sp_next_line(sp);
            }
        }

        sp_print_line_header(sp, ds, left_margin);
        sp_print_normal_text(ds, sp->line.data, sp->col);
    }

    char *last_byte = sp->src.data + sp->src.count;

    if (sp->L0 == sp->L1) {
        sp_print_highlighted_text(ds, sp->line.data + sp->col, sp->src.count);
    } else {
        sp_print_highlighted_text(ds, sp->line.data + sp->col, sp->line.count - sp->col);
        sp_next_line(sp);

        while (sp->line_num < sp->L1) {
            sp_print_line_header(sp, ds, left_margin);
            sp_print_highlighted_text(ds, sp->line.data, sp->line.count);
            sp_next_line(sp);
        }

        sp_print_line_header(sp, ds, left_margin);
        sp_print_highlighted_text(ds, sp->line.data, last_byte - sp->line.data);
    }

    if (bottom_padding >= 0) {
        sp_print_normal_text(ds, last_byte, sp->line.count - (last_byte - sp->line.data));

        if (bottom_padding > 0) {
            for (s32 i = 0; i < bottom_padding; ++i) {
                if (! sp_next_line(sp)) break;
                sp_print_line_header(sp, ds, left_margin);
                sp_print_normal_text(ds, sp->line.data, sp->line.count);
            }
        }
    }

    sp->line = first_line;
    sp->line_num = sp->L0;
}

void report_sources (DString *ds, String str, Source src1, Source src2) {
    if (! ds) return;
    if (src2.offset < src1.offset) swap(src1, src2);

    Source_Printer sp1 = sp_new(str, src1);
    Source_Printer sp2 = sp_new(str, src2);
    u32 left_margin    = INDENTATION + digit_count(MAX(sp1.L1, sp2.L1) + BOTTOM_PADDING);

    ds_add_byte(ds, '\n');

    if (src1.offset + src1.length > src2.offset) {
        // Overlaping.
        sp_print(&sp1, ds, TOP_PADDING, BOTTOM_PADDING, left_margin);
        ds_add_byte(ds, '\n');
        sp_print(&sp2, ds, TOP_PADDING, BOTTOM_PADDING, left_margin);
    } else if (src2.first_line - src1.last_line > MAX_SOURCE_DISTANCE) {
        // Far apart.
        sp_print(&sp1, ds, TOP_PADDING, 0, left_margin);
        ds_add_fmt(ds, TEXT_NORMAL_COLOR "%*s.\n" ANSI_END, left_margin - 1, "");
        ds_add_fmt(ds, TEXT_NORMAL_COLOR "%*s.\n" ANSI_END, left_margin - 1, "");
        ds_add_fmt(ds, TEXT_NORMAL_COLOR "%*s.\n" ANSI_END, left_margin - 1, "");
        sp_print(&sp2, ds, 0, BOTTOM_PADDING, left_margin);
    } else if (src1.last_line < src2.first_line) {
        // Near.
        sp_print(&sp1, ds, TOP_PADDING, 0, left_margin);
        sp_print(&sp2, ds, src2.first_line - src1.last_line - 1, BOTTOM_PADDING, left_margin);
    } else {
        // Sharing bottom and top line.
        sp_print(&sp1, ds, TOP_PADDING, -1, left_margin);
        u32 src1_end_column = (sp1.src.data + sp1.src.count) - sp2.line.data;
        sp_print_normal_text(ds, sp2.line.data + src1_end_column, sp2.col - src1_end_column);
        sp_print(&sp2, ds, -1, BOTTOM_PADDING, left_margin);
    }

    ds_add_byte(ds, '\n');
}

void report_source (DString *ds, String str, Source src) {
    if (! ds) return;
    Source_Printer sp = sp_new(str, src);
    ds_add_byte(ds, '\n');
    sp_print(&sp, ds, TOP_PADDING, BOTTOM_PADDING, INDENTATION + digit_count(sp.L1));
    ds_add_byte(ds, '\n');
}
