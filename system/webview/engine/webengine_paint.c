/* Web Viewer's painting order (webengine_box.h): CSS 2.1 Appendix E for what
 * the engine lays out. Within a layer, the backgrounds and borders of its
 * blocks come first, in tree order, then the layers inside it, then its line
 * boxes and markers. The root is a layer, and so is each float, painted
 * whole where Appendix E puts floats: over the blocks around it, under
 * their text. Written from the specification; NetSurf's redraw is not its
 * model (the clean-room rule).
 *
 * The layers wait on an explicit stack of tasks, so nesting does not
 * recurse. */

#include "webengine_layout.h"

#include <stdlib.h>

enum task_kind { TASK_LAYER, TASK_BACKGROUNDS, TASK_LINES };

struct task {
    uint8_t kind;
    const struct webengine_box *box;
};

struct order {
    struct webengine_tree *tree;
    struct task *tasks;
    size_t count;
    size_t capacity;
    bool failed;
};

static bool is_layer(const struct webengine_box *box) {
    return webengine_box_floats(box);
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
    tree->paint[tree->paint_count++] = (struct webengine_paint){ kind, box };
}

static void push(struct order *order, uint8_t kind, const struct webengine_box *box) {
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
    order->tasks[order->count++] = (struct task){ kind, box };
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

/* Does one task: a layer becomes its three parts, the others emit what the
 * layer's own boxes paint. */
static void run(struct order *order, struct task task) {
    const struct webengine_box *layer = task.box;
    if (task.kind == TASK_LAYER) {
        push(order, TASK_LINES, layer);
        size_t first = order->count;
        for (const struct webengine_box *box = layer; box != NULL && !order->failed;) {
            bool nested = box != layer && is_layer(box);
            if (nested) {
                push(order, TASK_LAYER, box);
            }
            box = next_in(layer, box, !nested);
        }
        /* The stack gives them back last first: reverse them. */
        for (size_t i = first, j = order->count; !order->failed && i + 1 < j; i++, j--) {
            struct task swap = order->tasks[i];
            order->tasks[i] = order->tasks[j - 1];
            order->tasks[j - 1] = swap;
        }
        push(order, TASK_BACKGROUNDS, layer);
        return;
    }
    for (const struct webengine_box *box = layer; box != NULL && !order->failed;) {
        bool nested = box != layer && is_layer(box);
        if (!nested && box->type == WEBENGINE_BOX_BLOCK) {
            if (task.kind == TASK_BACKGROUNDS) {
                emit(order, WEBENGINE_PAINT_BLOCK, box);
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
    push(&order, TASK_LAYER, tree->root);
    while (order.count > 0 && !order.failed) {
        struct task task = order.tasks[--order.count];
        run(&order, task);
    }
    free(order.tasks);
    return order.failed ? -1 : 0;
}
