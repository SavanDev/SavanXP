/* Web Viewer's painting order (webengine_box.h): CSS 2.1 Appendix E for what
 * the engine lays out. The root is a layer, and so is each float and each
 * positioned box. A layer paints its own background and borders, the
 * positioned layers inside it with a negative z-index, the backgrounds and
 * borders of its other blocks in tree order, its floats, its line boxes and
 * markers, then its positioned layers with z-index auto or 0 in tree order
 * and those with a positive one, lowest first. Each layer inside is painted
 * whole where it comes. Every positioned box is treated as a stacking
 * context of its own, z-index auto included. Each item carries the clip
 * of overflow (11.1.1): a box whose overflow is not visible clips what is
 * inside it to its padding box, and an absolute box escapes the clips
 * between it and its containing block. Written from the specification;
 * NetSurf's redraw is not its model (the clean-room rule).
 *
 * The layers wait on an explicit stack of tasks, so nesting does not
 * recurse. */

#include "webengine_layout.h"

#include <stdlib.h>

enum task_kind { TASK_LAYER, TASK_SELF, TASK_BACKGROUNDS, TASK_LINES };

struct task {
    uint8_t kind;
    /* Inside a position: fixed layer. */
    bool fixed;
    const struct webengine_box *box;
};

/* A layer inside the one being expanded, with where it goes. */
struct nested {
    const struct webengine_box *box;
    /* 0: negative z-index, 1: a float, 2: auto or 0, 3: positive. */
    int group;
    int32_t z;
    size_t order;
};

struct order {
    struct webengine_tree *tree;
    struct task *tasks;
    size_t count;
    size_t capacity;
    struct nested *nested;
    size_t nested_count;
    size_t nested_capacity;
    bool fixed;
    bool failed;
};

static bool is_layer(const struct webengine_box *box) {
    return webengine_box_floats(box) || webengine_box_positioned(box);
}

/* What clips the painting of `box` itself: the clip of the box it is
 * inside, its containing block for an absolute box, nothing for a fixed one
 * or one in the viewport. */
static struct webengine_clip outer_clip(const struct webengine_box *box) {
    const struct webengine_box *from = box->parent;
    if (webengine_box_absolute(box)) {
        from = box->style->position == CSS_POSITION_FIXED ? NULL : box->containing;
    }
    return from != NULL ? from->clip : (struct webengine_clip){ 0 };
}

static struct webengine_clip intersect(struct webengine_clip a, struct webengine_clip b) {
    if (!a.on) {
        return b;
    }
    if (!b.on) {
        return a;
    }
    int left = a.x > b.x ? a.x : b.x;
    int top = a.y > b.y ? a.y : b.y;
    int right = a.x + a.width < b.x + b.width ? a.x + a.width : b.x + b.width;
    int bottom = a.y + a.height < b.y + b.height ? a.y + a.height : b.y + b.height;
    return (struct webengine_clip){ true, left, top, right > left ? right - left : 0,
                                    bottom > top ? bottom - top : 0 };
}

/* Every box's clip, parents and containing blocks first: tree order. */
static void set_clips(struct webengine_box *root) {
    struct webengine_box *box = root;
    while (box != NULL) {
        box->clip = outer_clip(box);
        if (box->type == WEBENGINE_BOX_BLOCK && box->style->overflow != CSS_OVERFLOW_VISIBLE) {
            struct webengine_clip padding = {
                true, box->x + box->border[WEBENGINE_LEFT], box->y + box->border[WEBENGINE_TOP],
                box->width - box->border[WEBENGINE_LEFT] - box->border[WEBENGINE_RIGHT],
                box->height - box->border[WEBENGINE_TOP] - box->border[WEBENGINE_BOTTOM] };
            box->clip = intersect(box->clip, padding);
        }
        if (box->first != NULL) {
            box = box->first;
            continue;
        }
        while (box != NULL && box->next == NULL) {
            box = box->parent;
        }
        if (box != NULL) {
            box = box->next;
        }
    }
}

static void emit(struct order *order, uint8_t kind, const struct webengine_box *box) {
    struct webengine_tree *tree = order->tree;
    if (tree->paint_count == tree->paint_capacity) {
        size_t capacity = tree->paint_capacity == 0 ? 64 : tree->paint_capacity * 2;
        struct webengine_paint *paint = realloc(tree->paint, capacity * sizeof(*paint));
        if (paint == NULL) {
            order->failed = true;
            return;
        }
        tree->paint = paint;
        tree->paint_capacity = capacity;
    }
    /* Lines are inside their block; a block and a marker are painted by
     * what they are in. */
    struct webengine_clip clip = kind == WEBENGINE_PAINT_LINES ? box->clip
        : kind == WEBENGINE_PAINT_MARKER ? outer_clip(box->parent) : outer_clip(box);
    tree->paint[tree->paint_count++] = (struct webengine_paint){ kind, order->fixed, clip, box };
}

static void push(struct order *order, uint8_t kind, bool fixed, const struct webengine_box *box) {
    if (order->count == order->capacity) {
        size_t capacity = order->capacity == 0 ? 32 : order->capacity * 2;
        struct task *tasks = realloc(order->tasks, capacity * sizeof(*tasks));
        if (tasks == NULL) {
            order->failed = true;
            return;
        }
        order->tasks = tasks;
        order->capacity = capacity;
    }
    order->tasks[order->count++] = (struct task){ kind, fixed, box };
}

/* The box after `box` in tree order within `layer`, into its children when
 * `descend`. */
static const struct webengine_box *next_in(const struct webengine_box *layer, const struct webengine_box *box,
                                           bool descend) {
    if (descend && box->first != NULL) {
        return box->first;
    }
    while (box != layer && box->next == NULL) {
        box = box->parent;
    }
    return box == layer ? NULL : box->next;
}

static void add_nested(struct order *order, const struct webengine_box *box) {
    if (order->nested_count == order->nested_capacity) {
        size_t capacity = order->nested_capacity == 0 ? 16 : order->nested_capacity * 2;
        struct nested *nested = realloc(order->nested, capacity * sizeof(*nested));
        if (nested == NULL) {
            order->failed = true;
            return;
        }
        order->nested = nested;
        order->nested_capacity = capacity;
    }
    const struct webengine_box_style *style = box->style;
    int group = !webengine_box_positioned(box) ? 1 : style->z_auto || style->z_index == 0 ? 2
        : style->z_index < 0 ? 0 : 3;
    order->nested[order->nested_count] = (struct nested){ box, group, style->z_auto ? 0 : style->z_index,
                                                          order->nested_count };
    order->nested_count++;
}

static int compare_nested(const void *a, const void *b) {
    const struct nested *left = a;
    const struct nested *right = b;
    if (left->group != right->group) {
        return left->group < right->group ? -1 : 1;
    }
    if ((left->group == 0 || left->group == 3) && left->z != right->z) {
        return left->z < right->z ? -1 : 1;
    }
    return left->order < right->order ? -1 : left->order > right->order;
}

/* Does one task: a layer becomes its parts, the others emit what the
 * layer's own boxes paint. */
static void run(struct order *order, struct task task) {
    const struct webengine_box *layer = task.box;
    order->fixed = task.fixed;
    if (task.kind == TASK_LAYER) {
        order->nested_count = 0;
        for (const struct webengine_box *box = layer; box != NULL && !order->failed;) {
            bool nested = box != layer && is_layer(box);
            if (nested) {
                add_nested(order, box);
            }
            box = next_in(layer, box, !nested);
        }
        if (order->failed) {
            return;
        }
        qsort(order->nested, order->nested_count, sizeof(*order->nested), compare_nested);
        /* The stack gives tasks back last first: push the parts backwards,
         * the backgrounds after the negative layers and the lines after the
         * floats. */
        size_t i = order->nested_count;
        for (; i > 0 && order->nested[i - 1].group >= 2; i--) {
            const struct webengine_box *box = order->nested[i - 1].box;
            push(order, TASK_LAYER, task.fixed || box->style->position == CSS_POSITION_FIXED, box);
        }
        push(order, TASK_LINES, task.fixed, layer);
        for (; i > 0 && order->nested[i - 1].group == 1; i--) {
            push(order, TASK_LAYER, task.fixed, order->nested[i - 1].box);
        }
        push(order, TASK_BACKGROUNDS, task.fixed, layer);
        for (; i > 0; i--) {
            const struct webengine_box *box = order->nested[i - 1].box;
            push(order, TASK_LAYER, task.fixed || box->style->position == CSS_POSITION_FIXED, box);
        }
        push(order, TASK_SELF, task.fixed, layer);
        return;
    }
    if (task.kind == TASK_SELF) {
        emit(order, WEBENGINE_PAINT_BLOCK, layer);
        return;
    }
    for (const struct webengine_box *box = layer; box != NULL && !order->failed;) {
        bool nested = box != layer && is_layer(box);
        if (!nested && box->type == WEBENGINE_BOX_BLOCK) {
            if (task.kind == TASK_BACKGROUNDS) {
                if (box != layer) {
                    emit(order, WEBENGINE_PAINT_BLOCK, box);
                }
            } else {
                if (box->marker != NULL) {
                    emit(order, WEBENGINE_PAINT_MARKER, box->marker);
                }
                if (box->inline_content && box->line_count > 0) {
                    emit(order, WEBENGINE_PAINT_LINES, box);
                }
            }
        }
        box = next_in(layer, box, !nested);
    }
}

int webengine_paint_order(struct webengine_tree *tree) {
    struct order order = { .tree = tree };
    tree->paint_count = 0;
    if (tree->root == NULL) {
        return 0;
    }
    set_clips(tree->root);
    push(&order, TASK_LAYER, false, tree->root);
    while (order.count > 0 && !order.failed) {
        struct task task = order.tasks[--order.count];
        run(&order, task);
    }
    free(order.tasks);
    free(order.nested);
    return order.failed ? -1 : 0;
}
