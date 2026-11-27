/*
 * undo.c -- The RichEdit control's undo and redo: every change is a
 * replacement at one position, kept as what it took out and what it put
 * in. Typing grows one record until something else happens, as on Windows
 * XP, where "ab cd" typed in one go is undone at once. Undoing selects what
 * comes back; redoing leaves the caret after what goes in again.
 */
#include "msftedit_private.h"

static void free_record(undo_record *u) {
    rich_free(u->removed);
    rich_free(u->inserted);
    u->removed = 0;
    u->inserted = 0;
}

void rich_undo_clear(undo_stack *s) {
    for (int i = 0; i < s->count; ++i) {
        free_record(&s->records[i]);
    }
    s->count = 0;
}

static WCHAR *copy_text(const WCHAR *text, int length) {
    WCHAR *out = rich_alloc((u32)(length + 1) * sizeof(WCHAR));
    if (out != 0) {
        for (int i = 0; i < length; ++i) {
            out[i] = text[i];
        }
        out[length] = 0;
    }
    return out;
}

/* Room for one more record, dropping the oldest past the limit. */
static undo_record *push(rich *r, undo_stack *s) {
    if (r->undo_limit <= 0) {
        return 0;
    }
    if (s->count >= r->undo_limit) {
        free_record(&s->records[0]);
        for (int i = 1; i < s->count; ++i) {
            s->records[i - 1] = s->records[i];
        }
        --s->count;
    }
    if (s->count == s->capacity) {
        const int capacity = s->capacity == 0 ? 16 : s->capacity * 2;
        undo_record *grown = rich_alloc((u32)capacity * sizeof(undo_record));
        if (grown == 0) {
            return 0;
        }
        for (int i = 0; i < s->count; ++i) {
            grown[i] = s->records[i];
        }
        rich_free(s->records);
        s->records = grown;
        s->capacity = capacity;
    }
    undo_record *u = &s->records[s->count++];
    u->removed = 0;
    u->inserted = 0;
    return u;
}

void rich_undo_push(rich *r, int kind, int cp, const WCHAR *removed, int removed_length, const WCHAR *inserted,
                    int inserted_length) {
    r->typing_open = 0;
    rich_undo_clear(&r->redo);
    undo_record *u = push(r, &r->undo);
    if (u == 0) {
        return;
    }
    u->kind = kind;
    u->group = r->undo_group;
    u->cp = cp;
    u->removed = copy_text(removed, removed_length);
    u->removed_length = removed_length;
    u->inserted = copy_text(inserted, inserted_length);
    u->inserted_length = inserted_length;
}

/* A character typed at cp over `removed` (the selection it replaced). */
void rich_undo_typing(rich *r, int cp, const WCHAR *removed, int removed_length, WCHAR c) {
    if (r->typing_open && r->undo.count > 0 && removed_length == 0) {
        undo_record *top = &r->undo.records[r->undo.count - 1];
        if (top->kind == UNDO_TYPING && top->cp + top->inserted_length == cp) {
            WCHAR *grown = rich_alloc((u32)(top->inserted_length + 2) * sizeof(WCHAR));
            if (grown != 0) {
                for (int i = 0; i < top->inserted_length; ++i) {
                    grown[i] = top->inserted[i];
                }
                grown[top->inserted_length] = c;
                grown[top->inserted_length + 1] = 0;
                rich_free(top->inserted);
                top->inserted = grown;
                ++top->inserted_length;
                rich_undo_clear(&r->redo);
                return;
            }
        }
    }
    rich_undo_push(r, UNDO_TYPING, cp, removed, removed_length, &c, 1);
    r->typing_open = r->undo.count > 0;
}

/* Undoes (or redoes) the newest record. Returns 0 when there is none. */
static int undo_one(rich *r, int redo) {
    undo_stack *from = redo ? &r->redo : &r->undo;
    undo_stack *to = redo ? &r->undo : &r->redo;
    r->typing_open = 0;
    if (from->count == 0) {
        return 0;
    }
    undo_record u = from->records[--from->count];
    if (redo) {
        rich_splice(r, u.cp, u.removed_length, u.inserted, u.inserted_length);
        r->anchor = r->caret = u.cp + u.inserted_length;
    } else {
        rich_splice(r, u.cp, u.inserted_length, u.removed, u.removed_length);
        r->anchor = u.kind == UNDO_BACKSPACE ? u.cp + u.removed_length : u.cp;
        r->caret = u.cp + u.removed_length;
    }
    if (to->count == to->capacity) {
        const int capacity = to->capacity == 0 ? 16 : to->capacity * 2;
        undo_record *grown = rich_alloc((u32)capacity * sizeof(undo_record));
        if (grown == 0) {
            free_record(&u);
            return 1;
        }
        for (int i = 0; i < to->count; ++i) {
            grown[i] = to->records[i];
        }
        rich_free(to->records);
        to->records = grown;
        to->capacity = capacity;
    }
    to->records[to->count++] = u;
    return 1;
}

/* Undoes (or redoes) the newest action: one record, or every record of its
 * group. Returns 0 when there is none. */
int rich_undo(rich *r, int redo) {
    undo_stack *from = redo ? &r->redo : &r->undo;
    if (from->count == 0) {
        return 0;
    }
    const int group = from->records[from->count - 1].group;
    undo_one(r, redo);
    while (group != 0 && from->count > 0 && from->records[from->count - 1].group == group) {
        undo_one(r, redo);
    }
    return 1;
}
