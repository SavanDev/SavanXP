/* Web Viewer's table grid and column widths (webengine_layout.h): CSS 2.1
 * 17.2 (the table's parts), 17.5 (the grid of rows and columns, cells that
 * span) and 17.5.2.2 (automatic table layout). HTML's table model places
 * the cells, rows spanning down skipping the slots taken from above.
 *
 * 17.5.2.2 leaves the distribution of width open; this one does what
 * browsers do in the common cases: columns start at their minimum, then
 * percentage columns grow to their share, columns with a width to it, the
 * other columns towards their preferred width, and what is left goes to the
 * columns without a width, in proportion to their preferred widths. Written
 * from the specification; NetSurf's table layout is not its model (the
 * clean-room rule). */

#include "webengine_layout.h"

#include <stdlib.h>
#include <string.h>

static int fixed_round(css_fixed value) {
    return value >= 0 ? (int)((value + 512) >> 10) : -(int)((-value + 512) >> 10);
}

static int px(const struct webengine_len *length) {
    return length->kind == WEBENGINE_LEN_PX ? fixed_round(length->value) : 0;
}

static bool grow(void **data, size_t *capacity, size_t count, size_t size) {
    if (count < *capacity) {
        return true;
    }
    size_t grown = *capacity == 0 ? 8 : *capacity * 2;
    while (grown <= count) {
        grown *= 2;
    }
    void *bigger = realloc(*data, grown * size);
    if (bigger == NULL) {
        return false;
    }
    *data = bigger;
    *capacity = grown;
    return true;
}

void webengine_table_free(struct webengine_table *table) {
    free(table->cells);
    free(table->columns);
    free(table->rows);
    free(table->captions);
    free(table->row_first);
    free(table->scratch);
    memset(table, 0, sizeof(*table));
}

struct builder {
    struct webengine_table *table;
    size_t column_capacity;
    size_t cell_capacity;
    size_t row_capacity;
    size_t caption_capacity;
    /* Per column, the first row no cell from above takes. */
    int *busy;
    size_t busy_capacity;
    bool failed;
};

static bool ensure_columns(struct builder *b, size_t count) {
    struct webengine_table *table = b->table;
    if (count <= table->column_count) {
        return true;
    }
    if (!grow((void **)&table->columns, &b->column_capacity, count - 1, sizeof(*table->columns)) ||
        !grow((void **)&b->busy, &b->busy_capacity, count - 1, sizeof(*b->busy))) {
        b->failed = true;
        return false;
    }
    for (size_t i = table->column_count; i < count; i++) {
        memset(&table->columns[i], 0, sizeof(table->columns[i]));
        table->columns[i].fixed = -1;
        table->columns[i].percent = -1;
        b->busy[i] = 0;
    }
    table->column_count = count;
    return true;
}

/* A column's specified width: pixels make it fixed, a percentage a share
 * of the table. */
static void column_width(struct webengine_table_column *column, const struct webengine_box *box, int edges) {
    const struct webengine_len *width = &box->style->width;
    if (width->kind == WEBENGINE_LEN_PX) {
        int size = fixed_round(width->value);
        if (box->style->box_sizing != CSS_BOX_SIZING_BORDER_BOX) {
            size += edges;
        }
        column->fixed = size > column->fixed ? size : column->fixed;
    } else if (width->kind == WEBENGINE_LEN_PERCENT && width->value > column->percent) {
        column->percent = width->value;
    }
}

static void add_columns(struct builder *b, const struct webengine_box *box, size_t *next) {
    if (!ensure_columns(b, *next + (size_t)box->col_span)) {
        return;
    }
    for (int i = 0; i < box->col_span; i++) {
        column_width(&b->table->columns[*next + (size_t)i], box, 0);
    }
    *next += (size_t)box->col_span;
}

/* Places the cells of rows [first, first + count), a row group, in the grid. */
static void place_rows(struct builder *b, struct webengine_box *group, struct webengine_box *first, size_t count) {
    struct webengine_table *table = b->table;
    int group_start = (int)table->row_count;
    int group_end = group_start + (int)count;
    struct webengine_box *row = first;
    for (size_t r = 0; r < count && !b->failed; r++, row = row->next) {
        while (row->table_role != WEBENGINE_TABLE_ROW) {
            row = row->next;
        }
        if (!grow((void **)&table->rows, &b->row_capacity, table->row_count, sizeof(*table->rows))) {
            b->failed = true;
            return;
        }
        int index = (int)table->row_count++;
        table->rows[index] = (struct webengine_table_row){ .box = row, .group = group };
        size_t column = 0;
        for (struct webengine_box *cell = row->first; cell != NULL && !b->failed; cell = cell->next) {
            if (cell->type != WEBENGINE_BOX_BLOCK || cell->table_role != WEBENGINE_TABLE_CELL) {
                continue;
            }
            while (column < table->column_count && b->busy[column] > index) {
                column++;
            }
            int rows = cell->row_span == 0 || cell->row_span > group_end - index ? group_end - index : cell->row_span;
            if (!ensure_columns(b, column + (size_t)cell->col_span) ||
                !grow((void **)&table->cells, &b->cell_capacity, table->cell_count, sizeof(*table->cells))) {
                b->failed = true;
                return;
            }
            for (int i = 0; i < cell->col_span; i++) {
                b->busy[column + (size_t)i] = index + rows;
            }
            table->spans = table->spans || rows > 1;
            table->cells[table->cell_count++] = (struct webengine_table_cell){
                .box = cell, .row = index, .column = (int)column, .rows = rows, .columns = cell->col_span };
            column += (size_t)cell->col_span;
        }
    }
}

static size_t count_rows(const struct webengine_box *group) {
    size_t count = 0;
    for (const struct webengine_box *row = group->first; row != NULL; row = row->next) {
        count += row->type == WEBENGINE_BOX_BLOCK && row->table_role == WEBENGINE_TABLE_ROW;
    }
    return count;
}

/* The rows of a run of rows directly in the table, from `row` on: one row
 * group of their own. The box after them. */
static struct webengine_box *place_run(struct builder *b, struct webengine_box *row) {
    size_t count = 0;
    struct webengine_box *end = row;
    while (end != NULL && end->table_role == WEBENGINE_TABLE_ROW) {
        count++;
        end = end->next;
    }
    place_rows(b, NULL, row, count);
    return end;
}

int webengine_table_grid(struct webengine_table *table, struct webengine_box *box) {
    struct builder b = { .table = table };
    memset(table, 0, sizeof(*table));
    table->box = box;
    table->spacing[0] = box->style->border_spacing[0];
    table->spacing[1] = box->style->border_spacing[1];
    struct webengine_box *header = NULL;
    struct webengine_box *footer = NULL;
    size_t next_column = 0;
    for (struct webengine_box *child = box->first; child != NULL && !b.failed; child = child->next) {
        switch (child->table_role) {
        case WEBENGINE_TABLE_CAPTION:
            if (!grow((void **)&table->captions, &b.caption_capacity, table->caption_count, sizeof(*table->captions))) {
                b.failed = true;
                break;
            }
            table->captions[table->caption_count++] = child;
            break;
        case WEBENGINE_TABLE_COLUMN:
            add_columns(&b, child, &next_column);
            break;
        case WEBENGINE_TABLE_COLUMN_GROUP: {
            bool any = false;
            for (struct webengine_box *column = child->first; column != NULL; column = column->next) {
                if (column->table_role == WEBENGINE_TABLE_COLUMN) {
                    size_t start = next_column;
                    add_columns(&b, column, &next_column);
                    if (column->style->width.kind == WEBENGINE_LEN_AUTO && !b.failed) {
                        for (size_t i = start; i < next_column; i++) {
                            column_width(&table->columns[i], child, 0);
                        }
                    }
                    any = true;
                }
            }
            if (!any) {
                add_columns(&b, child, &next_column);
            }
            break;
        }
        case WEBENGINE_TABLE_HEADER_GROUP:
            header = header == NULL ? child : header;
            break;
        case WEBENGINE_TABLE_FOOTER_GROUP:
            footer = footer == NULL ? child : footer;
            break;
        default:
            break;
        }
    }
    /* The first header group goes first and the first footer group last
     * (17.2); the others stay where they are, as bodies. */
    if (header != NULL && !b.failed) {
        place_rows(&b, header, header->first, count_rows(header));
    }
    for (struct webengine_box *child = box->first; child != NULL && !b.failed;) {
        if (child->table_role == WEBENGINE_TABLE_ROW) {
            child = place_run(&b, child);
            continue;
        }
        if ((child->table_role == WEBENGINE_TABLE_ROW_GROUP || child->table_role == WEBENGINE_TABLE_HEADER_GROUP ||
             child->table_role == WEBENGINE_TABLE_FOOTER_GROUP) && child != header && child != footer) {
            place_rows(&b, child, child->first, count_rows(child));
        }
        child = child->next;
    }
    if (footer != NULL && !b.failed) {
        place_rows(&b, footer, footer->first, count_rows(footer));
    }
    if (!b.failed && table->row_count > 0) {
        /* Where each row's cells start, for moving a row. */
        table->row_first = malloc((table->row_count + 1) * sizeof(*table->row_first));
        if (table->row_first == NULL) {
            b.failed = true;
        } else {
            size_t cell = 0;
            for (size_t r = 0; r <= table->row_count; r++) {
                while (cell < table->cell_count && table->cells[cell].row < (int)r) {
                    cell++;
                }
                table->row_first[r] = cell;
            }
        }
    }
    free(b.busy);
    if (b.failed) {
        webengine_table_free(table);
        return -1;
    }
    return 0;
}

/* The horizontal border and padding of a cell, in pixels. */
static int cell_edges(const struct webengine_box *cell) {
    const struct webengine_box_style *style = cell->style;
    int left = px(&style->padding[WEBENGINE_LEFT]);
    int right = px(&style->padding[WEBENGINE_RIGHT]);
    return style->border_width[WEBENGINE_LEFT] + style->border_width[WEBENGINE_RIGHT] + (left > 0 ? left : 0) +
        (right > 0 ? right : 0);
}

/* Spreads `amount` over columns [first, first + count) in proportion to
 * their `weights`, evenly when those are all zero; `only` picks the columns
 * that may take any (all when none of them may). */
static void spread(struct webengine_table *table, size_t first, size_t count, int amount, int *target,
                   const int *weights, bool (*only)(const struct webengine_table_column *)) {
    bool any = false;
    for (size_t i = first; i < first + count && only != NULL; i++) {
        any = any || only(&table->columns[i]);
    }
    int64_t total = 0;
    size_t takers = 0;
    for (size_t i = first; i < first + count; i++) {
        if (!any || only(&table->columns[i])) {
            total += weights[i];
            takers++;
        }
    }
    if (takers == 0 || amount <= 0) {
        return;
    }
    int64_t sum = 0;
    int given = 0;
    size_t seen = 0;
    for (size_t i = first; i < first + count; i++) {
        if (any && !only(&table->columns[i])) {
            continue;
        }
        seen++;
        sum += total > 0 ? weights[i] : 1;
        int upto = (int)(((int64_t)amount * sum) / (total > 0 ? total : (int64_t)takers));
        if (seen == takers) {
            upto = amount;
        }
        target[i] += upto - given;
        given = upto;
    }
}

static bool is_auto(const struct webengine_table_column *column) {
    return column->fixed < 0 && column->percent < 0;
}

static bool is_fixed(const struct webengine_table_column *column) {
    return column->fixed >= 0 && column->percent < 0;
}

static bool is_percent(const struct webengine_table_column *column) {
    return column->percent >= 0;
}

/* Scratch arrays the size of the columns. */
static int *scratch(const struct webengine_table *table, size_t which) {
    return table->scratch + which * table->column_count;
}

int webengine_table_columns(struct webengine_table *table) {
    size_t n = table->column_count;
    free(table->scratch);
    table->scratch = calloc(n * 3 + 1, sizeof(int));
    if (table->scratch == NULL) {
        return -1;
    }
    for (size_t i = 0; i < n; i++) {
        table->columns[i].min = 0;
        table->columns[i].max = 0;
    }
    /* Cells that span one column, then the wider ones, narrowest first. */
    int widest = 1;
    for (size_t c = 0; c < table->cell_count; c++) {
        widest = table->cells[c].columns > widest ? table->cells[c].columns : widest;
    }
    for (int span = 1; span <= widest; span++) {
        for (size_t c = 0; c < table->cell_count; c++) {
            const struct webengine_table_cell *cell = &table->cells[c];
            if (cell->columns != span) {
                continue;
            }
            int edges = cell_edges(cell->box);
            int min = cell->box->min_content + edges;
            int max = cell->box->max_content + edges;
            max = max > min ? max : min;
            if (span == 1) {
                struct webengine_table_column *column = &table->columns[cell->column];
                column_width(column, cell->box, edges);
                column->min = min > column->min ? min : column->min;
                column->max = max > column->max ? max : column->max;
                continue;
            }
            size_t first = (size_t)cell->column;
            size_t count = (size_t)span;
            int have_min = table->spacing[0] * (span - 1);
            int have_max = have_min;
            int *weights = scratch(table, 0);
            int *grown = scratch(table, 1);
            for (size_t i = first; i < first + count; i++) {
                have_min += table->columns[i].min;
                have_max += table->columns[i].max;
                grown[i] = 0;
            }
            if (min > have_min) {
                /* What the cell needs beyond its columns' minimums: up to
                 * their preferred widths first, in proportion to the room
                 * each has, then the rest to those without a width. */
                int room = have_max - have_min;
                int need = min - have_min;
                for (size_t i = first; i < first + count; i++) {
                    weights[i] = table->columns[i].max - table->columns[i].min;
                }
                spread(table, first, count, need < room ? need : room, grown, weights, NULL);
                for (size_t i = first; i < first + count; i++) {
                    table->columns[i].min += grown[i];
                    weights[i] = table->columns[i].max;
                    grown[i] = 0;
                }
                if (need > room) {
                    spread(table, first, count, need - room, grown, weights, is_auto);
                    for (size_t i = first; i < first + count; i++) {
                        table->columns[i].min += grown[i];
                        grown[i] = 0;
                    }
                }
                have_max = table->spacing[0] * (span - 1);
                for (size_t i = first; i < first + count; i++) {
                    if (table->columns[i].max < table->columns[i].min) {
                        table->columns[i].max = table->columns[i].min;
                    }
                    have_max += table->columns[i].max;
                }
            }
            for (size_t i = first; i < first + count; i++) {
                weights[i] = table->columns[i].max;
            }
            spread(table, first, count, max - have_max, grown, weights, is_auto);
            for (size_t i = first; i < first + count; i++) {
                table->columns[i].max += grown[i];
            }
        }
        if (span == 1) {
            /* A width replaces the preferred width, never below the
             * minimum, before the cells that span take theirs. */
            for (size_t i = 0; i < n; i++) {
                struct webengine_table_column *column = &table->columns[i];
                if (column->fixed >= 0) {
                    column->max = column->fixed > column->min ? column->fixed : column->min;
                }
            }
        }
    }
    table->min = 0;
    table->max = 0;
    for (size_t i = 0; i < n; i++) {
        struct webengine_table_column *column = &table->columns[i];
        table->min += column->min;
        table->max += column->max;
    }
    if (n > 0) {
        table->min += table->spacing[0] * (int)(n + 1);
        table->max += table->spacing[0] * (int)(n + 1);
    }
    return 0;
}

void webengine_table_widths(struct webengine_table *table, int width) {
    size_t n = table->column_count;
    if (n == 0) {
        return;
    }
    int room = width - table->spacing[0] * (int)(n + 1);
    int *target = scratch(table, 1);
    int *weights = scratch(table, 0);
    int *gap = scratch(table, 2);
    int left = room;
    for (size_t i = 0; i < n; i++) {
        table->columns[i].width = table->columns[i].min;
        left -= table->columns[i].min;
    }
    /* Percentages, widths, then preferred widths, each as far as it goes. */
    for (int pass = 0; pass < 3 && left > 0; pass++) {
        int wanted = 0;
        for (size_t i = 0; i < n; i++) {
            const struct webengine_table_column *column = &table->columns[i];
            int goal = column->width;
            if (pass == 0 && is_percent(column)) {
                goal = fixed_round((css_fixed)(((int64_t)column->percent * (room > 0 ? room : 0)) / 100));
            } else if (pass == 1 && is_fixed(column)) {
                goal = column->max;
            } else if (pass == 2 && is_auto(column)) {
                goal = column->max;
            }
            gap[i] = goal > column->width ? goal - column->width : 0;
            wanted += gap[i];
        }
        if (wanted == 0) {
            continue;
        }
        for (size_t i = 0; i < n; i++) {
            target[i] = 0;
        }
        int given = wanted <= left ? wanted : left;
        spread(table, 0, n, given, target, gap, NULL);
        for (size_t i = 0; i < n; i++) {
            table->columns[i].width += target[i];
            left -= target[i];
        }
    }
    if (left > 0) {
        /* The rest: to the columns without a width, by their preferred
         * widths; when every column has one, to those in pixels, then to
         * the percentages. */
        bool any_auto = false;
        bool any_fixed = false;
        for (size_t i = 0; i < n; i++) {
            any_auto = any_auto || is_auto(&table->columns[i]);
            any_fixed = any_fixed || is_fixed(&table->columns[i]);
            weights[i] = table->columns[i].max;
            target[i] = 0;
        }
        spread(table, 0, n, left, target, weights, any_auto ? is_auto : any_fixed ? is_fixed : is_percent);
        for (size_t i = 0; i < n; i++) {
            table->columns[i].width += target[i];
        }
    }
    int x = table->spacing[0];
    for (size_t i = 0; i < n; i++) {
        table->columns[i].x = x;
        x += table->columns[i].width + table->spacing[0];
    }
}
