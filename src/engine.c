// TODO: We need to document this module. We need to write
// a description of the architecture as well as write some
// comments throughout the module.

#include <string.h>

#include "pager.h"
#include "report.h"
#include "engine.h"

// The minimum outdegree of our btree is 2; therefore, the
// btree reaches maximum height when it is a balanced full
// binary tree. The height of such a tree is obtained with
// the formula ceil(log2(n + 1)) where n is the number of
// nodes. In our case n = 2^32 - 1 because the Page_Id is
// a u32 and the pager reserves one page for itself.
#define MAX_BTREE_HEIGHT 32

// Node header byte layout:
//   flags:               2
//   cell_count:          2
//   cell_area:           2
//   cell_area_logical:   2
//   rightmost_child:     4
#define NODE_HEADER_SIZE 12

typedef struct {
    #define F_NODE_IS_LEAF FLAG(0)
    #define F_NODE_IS_FREE FLAG(1)

    u16 flags;
    u16 cell_count;
    u16 cell_area;
    u16 cell_area_logical;
    Page_Id rightmost_child;

    Page_Ref *page;
} Node;

struct BCursor {
    #define F_CURSOR_SKIP_NEXT           FLAG(0)
    #define F_CURSOR_DELETE_NODE_ON_EXIT FLAG(1)

    u16 flags;
    BTree *tree;

    u8    path_len;
    u16   path_cells[MAX_BTREE_HEIGHT];
    Node *path_nodes[MAX_BTREE_HEIGHT];
};

struct BTree {
    BType *type;
    Page_Id root;
    BEngine *engine;
};

struct BEngine {
    Mem *mem;
    Mem_Arena *key_saver;
    Files *fs;
    Pager *pager;
    u16 page_size; // Doesn't include header.
    u16 full_page_size; // Includes header.
    u8 *scratch_page;
};

static u16 cursor_idx (BCursor *);
static bool node_is_leaf (Node *);
static bool node_is_inner (Node *);
static void split_node (BCursor *);
static Node *cursor_node (BCursor *);
static void cursor_remove (BCursor *);
static u8 *node_get_cell (Node *, u16);
static bool cursor_goto_next_node (BCursor *);
static BType *get_btype_for_table (Type_Table *);
static void node_ensure_cell_space (BCursor *, u16);
static void node_add_cell_pointer (Node *, u16, u16);

#define KEY(PTR) ((Key){ PTR })
#define VAL(PTR) ((Val){ PTR })

// Cell iterators work syntactically just like for loops:
//
//     cell_iter (node) do_something();
//     cell_iter (node) { do_something(); }
//
// - Parameters are evaled only once.
// - The "from" parameter is inclusive.
// - Both "break" and "continue" work as expected.
//
// - Inside the loop the following variables are defined:
//
//       CELL_ARRAY:   Array_View_u16  array of cell offsets
//       CELL_NODE:    Node*           node we're looping over
//       CELL:         u8*             current cell
//       CELL_IDX:     u16             current index in CELL_ARRAY
//       CELL_IDX_PTR: u16*            pointer to current entry in CELL_ARRAY
//
// - The body of the cell_iter loop is nested inside an
//   "array_iter" loop which also defines some variables
//   like ARRAY_IDX. These should not be used.
#define _cell_iter(node, iter)\
    for (bool _(ONCE)=1; _(ONCE);)\
    for (u8 *CELL; (void)CELL, _(ONCE);)\
    for (u16 CELL_IDX; _(ONCE);)\
    for (DEF(Node*, CELL_NODE, node); _(ONCE);)\
    for (Array_View_u16 CELL_ARRAY = { .count = CELL_NODE->cell_count, .data = (char*)(CELL_NODE->page->buf + NODE_HEADER_SIZE) }; _(ONCE);)\
    for (; _(ONCE); _(ONCE)=0)\
    iter\
        if (CELL_IDX = (u16)ARRAY_IDX,\
            CELL     = node_get_cell(node, CELL_IDX),\
            false);\
        else

#define cell_iter(node)                    _cell_iter(node, array_iter_ptr(CELL_IDX_PTR, CELL_ARRAY))
#define cell_iter_from(node, from)         _cell_iter(node, array_iter_ptr_from(CELL_IDX_PTR, CELL_ARRAY, from))
#define cell_iter_reverse(node)            _cell_iter(node, array_iter_ptr_reverse(CELL_IDX_PTR, CELL_ARRAY))
#define cell_iter_from_reverse(node, from) _cell_iter(node, array_iter_ptr_reverse_from(CELL_IDX_PTR, CELL_ARRAY, from))

static Page_Id cell_get_child (u8 *cell) {
    return read_u32_le(cell);
}

static Key cell_get_key (u8 *cell, Node *node) {
    return node_is_inner(node) ? KEY(cell + 4) : KEY(cell);
}

static Val cell_get_val (BTree *tree, u8 *cell, Node *node) {
    ASSERT(node_is_leaf(node));
    return VAL(cell + tree->type->sizeof_key(KEY(cell)));
}

static u16 cell_get_size (BTree *tree, u8 *cell, Node *node) {
    Key key      = cell_get_key(cell, node);
    u16 key_size = tree->type->sizeof_key(key);

    return node_is_inner(node) ?
           (u16)(key_size + 4) :
           (u16)(key_size + tree->type->sizeof_val(VAL(cell + key_size)));
}

static bool node_is_leaf (Node *node) {
    return node->flags & F_NODE_IS_LEAF;
}

static bool node_is_inner (Node *node) {
    return !(node->flags & F_NODE_IS_LEAF);
}

static void node_serialize_header (Node *node) {
    u8 *buf = node->page->buf;
    write_u16_le(buf + 0, node->flags);
    write_u16_le(buf + 2, node->cell_count);
    write_u16_le(buf + 4, node->cell_area);
    write_u16_le(buf + 6, node->cell_area_logical);
    write_u32_le(buf + 8, node->rightmost_child);
}

static void node_deserialize_header (Node *node, Page_Ref *page) {
    u8 *buf                 = page->buf;
    node->page              = page;
    node->flags             = read_u16_le(buf + 0);
    node->cell_count        = read_u16_le(buf + 2);
    node->cell_area         = read_u16_le(buf + 4);
    node->cell_area_logical = read_u16_le(buf + 6);
    node->rightmost_child   = read_u32_le(buf + 8);
}

static void node_reset (BEngine *engine, Node *node) {
    node->flags             = 0;
    node->cell_count        = 0;
    node->cell_area         = engine->full_page_size;
    node->cell_area_logical = engine->full_page_size;
    node->rightmost_child   = 0;
}

static Node *node_from_page (BEngine *engine, Page_Ref *page) {
    Node *node = page->user_buf;
    ASSERT(! (node->flags & F_NODE_IS_FREE));
    if (! node->page) node_deserialize_header(node, page);
    return node;
}

static Node *node_from_page_id (BEngine *engine, Page_Id page_id) {
    Page_Ref *page = pager_get_page_mutable(engine->pager, page_id);
    ASSERT(page);
    return node_from_page(engine, page);
}

static Node *node_new (BEngine *engine, u16 flags) {
    Page_Ref *page = pager_alloc_page(engine->pager);
    Node *node = page->user_buf;
    node_reset(engine, node);
    node->page = page;
    node->flags = flags;
    node_serialize_header(node);
    return node;
}

static void node_unref (BEngine *engine, Node *node) {
    if (pager_is_page_mutable(engine->pager, node->page)) node_serialize_header(node);
    pager_unref_page(engine->pager, node->page);
}

static void node_delete (BEngine *engine, Node *node) {
    node->flags |= F_NODE_IS_FREE;
    ASSERT(pager_is_page_mutable(engine->pager, node->page));
    bool success = pager_delete_page(engine->pager, node->page);
    ASSERT(success);
}

static Node *node_get_child (BEngine *engine, Node *node, u16 idx) {
    ASSERT(idx <= node->cell_count);
    ASSERT(node_is_inner(node));

    if (idx < node->cell_count) {
        Page_Id page = cell_get_child(node_get_cell(node, idx));
        return node_from_page_id(engine, page);
    }

    return node_from_page_id(engine, node->rightmost_child);
}

static void node_copy (BEngine *engine, Node *to, Node *from) {
    Page_Ref *page = to->page;
    *to = *from;
    to->page = page;
    memcpy(to->page->buf, from->page->buf, engine->full_page_size);
}

static u8 *node_get_cell_idx_ptr (Node *node, u16 idx) {
    return &node->page->buf[NODE_HEADER_SIZE + 2*idx];
}

static u8 *node_get_cell (Node *node, u16 idx) {
    ASSERT(idx < node->cell_count);
    u8 *idx_ptr = node_get_cell_idx_ptr(node, idx);
    return node->page->buf + read_u16_le(idx_ptr);
}

static u16 node_get_free_space (Node *node) {
    return node->cell_area - NODE_HEADER_SIZE - 2*node->cell_count;
}

static u16 node_get_logical_free_space (Node *node) {
    return node->cell_area_logical - NODE_HEADER_SIZE - 2*node->cell_count;
}

static bool node_can_fit_cell (Node *node, u16 cell_size) {
    return (cell_size + 2) <= node_get_logical_free_space(node);
}

#if defined(RELEASE_BUILD)
    #define CHECK(...)
#else
    // TODO: We should add many more integrity checks here.
    static void CHECK (BTree *tree, Node *node) {
        u16 offset = tree->engine->full_page_size;
        cell_iter (node) offset -= cell_get_size(tree, CELL, node);
        ASSERT(offset == node->cell_area_logical);
    }
#endif

// TODO: If someone holds a pointer to a cell inside the node,
// then we cannot defragment it since it would invalidate the
// pointer. So we have to either implement some ref counting
// or "be careful"...
static void node_defragment (BTree *tree, Node *node) {
    if (node->cell_count == 0) return;

    BEngine *engine = tree->engine;
    u16 offset = engine->full_page_size;

    cell_iter (node) {
        u16 cell_size = cell_get_size(tree, CELL, node);
        offset -= cell_size;
        memcpy(engine->scratch_page + offset, CELL, cell_size);
        write_u16_le((u8*)CELL_IDX_PTR, offset);
    }

    memcpy(node->page->buf + offset, engine->scratch_page + offset, engine->full_page_size - offset);
    ASSERT(offset == node->cell_area_logical);
    node->cell_area = offset;

    CHECK(tree, node);
}

static void node_shrink_cell (Node *node, u8 *cell, u16 by) {
    (void)cell;
    node->cell_area_logical += by;
}

// TODO: Add a cell free list.
static u8 *node_alloc_cell (BTree *tree, Node *node, u16 size) {
    ASSERT(node_can_fit_cell(node, size));

    if ((size + 2) > node_get_free_space(node)) node_defragment(tree, node);

    node->cell_area -= size;
    node->cell_area_logical -= size;
    u8 *result = node->page->buf + node->cell_area;

    return result;
}

static u8 *node_add_cell (BTree *tree, Node *node, u16 idx, u16 size) {
    u8 *cell = node_alloc_cell(tree, node, size);
    node_add_cell_pointer(node, idx, (u16)(cell - node->page->buf));
    return cell;
}

static void node_free_cell (Node *node, u8 *cell, u16 size) {
    node->cell_area_logical += size;
    if (cell == (node->page->buf + node->cell_area)) node->cell_area += size;
}

static void node_delete_cell (BTree *tree, Node *node, u16 idx) {
    u8 *cell = node_get_cell(node, idx);
    node_free_cell(node, cell, cell_get_size(tree, cell, node));

    u8 *idx_ptr = node_get_cell_idx_ptr(node, idx);
    memmove(idx_ptr, idx_ptr+2, 2*(node->cell_count - idx - 1));
    node->cell_count--;

    CHECK(tree, node);
}

static void node_add_cell_pointer (Node *node, u16 idx, u16 offset) {
    u8 *idx_ptr = node_get_cell_idx_ptr(node, idx);
    memmove(idx_ptr+2, idx_ptr, 2*(node->cell_count - idx));
    write_u16_le(idx_ptr, offset);
    node->cell_count++;
}

static void node_add_inner_cell (BTree *tree, Node *node, u16 idx, Key key, Page_Id child) {
    ASSERT(node_is_inner(node));

    u16 key_size = tree->type->sizeof_key(key);
    u8 *new_cell = node_alloc_cell(tree, node, 4 + key_size);

    write_u32_le(new_cell, child);
    memcpy(new_cell + 4, key.ptr, key_size);

    node_add_cell_pointer(node, idx, (u16)(new_cell - node->page->buf));

    CHECK(tree, node);
}

static void copy_key_into_inner_cell (BCursor *cursor, Key key) {
    BTree *tree       = cursor->tree;
    Node *node        = cursor_node(cursor);
    u8 *cell          = node_get_cell(node, cursor_idx(cursor));
    u16 cell_size     = cell_get_size(tree, cell, node);
    u16 key_size      = tree->type->sizeof_key(key);
    u16 new_cell_size = 4 + key_size;

    ASSERT(node_is_inner(node));

    if (cell_size < new_cell_size) {
        Page_Id child = cell_get_child(cell);
        node_delete_cell(tree, node, cursor_idx(cursor));
        node_ensure_cell_space(cursor, new_cell_size);
        node_add_inner_cell(tree, cursor_node(cursor), cursor_idx(cursor), key, child);
    } else {
        Key to = cell_get_key(cell, node);
        memcpy(to.ptr, key.ptr, key_size);
        if (cell_size > new_cell_size) node->cell_area_logical += (cell_size - new_cell_size);
        CHECK(cursor->tree, node);
    }
}

static void node_move_cells_left (BTree *tree, Node *left, Node *right, u16 n) {
    ASSERT(n <= right->cell_count);

    if (n == 0) return;

    u8 *left_idx_array = node_get_cell_idx_ptr(left, left->cell_count);

    cell_iter (right) {
        if (CELL_IDX == n) break;

        u16 cell_size = cell_get_size(tree, CELL, right);
        u8 *left_cell = node_alloc_cell(tree, left, cell_size);

        memcpy(left_cell, CELL, cell_size);
        write_u16_le(left_idx_array, (u16)(left_cell - left->page->buf));
        left_idx_array += 2;

        node_free_cell(right, CELL, cell_size);
    }

    u8 *right_idx_array = node_get_cell_idx_ptr(right, 0);
    memmove(right_idx_array, &right_idx_array[2*n], 2*(right->cell_count - n));

    left->cell_count  += n;
    right->cell_count -= n;

    CHECK(tree, left);
    CHECK(tree, right);
}

static void node_move_cells_right (BTree *tree, Node *left, Node *right, u16 n) {
    ASSERT(n <= left->cell_count);

    if (n == 0) return;

    u16 old_right_cell_count = right->cell_count;
    u8 *right_idx = node_get_cell_idx_ptr(right, right->cell_count);

    cell_iter_from (left, (left->cell_count - n)) {
        u16 cell_size  = cell_get_size(tree, CELL, left);
        u8 *right_cell = node_alloc_cell(tree, right, cell_size);

        memcpy(right_cell, CELL, cell_size);
        write_u16_le(right_idx, (u16)(right_cell - right->page->buf));
        right->cell_count++;
        right_idx += 2;

        node_free_cell(left, CELL, cell_size);
    }

    left->cell_count -= n;

    {
        // Since we are moving cells from left to right we must prepend n cell
        // offsets to the right node's cell array, and we would like to avoid
        // performing n memmove() calls.
        //
        // The natural approach of doing a single memmove() on the array ahead
        // of time is awkward to implement since in any given iteration of the
        // above loop we might make a call to node_defragment() which needs to
        // loop over that array.
        //
        // We take a middle approach. We will *append* instead of prepending
        // and then swap two halfs of the array. This will work because the
        // node_defragment() function doesn't care about the order of cells.
        //
        // We use the new space in the left node as a temporary swap buffer.
        u8 *L = node_get_cell_idx_ptr(left, left->cell_count);
        u8 *R = node_get_cell_idx_ptr(right, 0);

        memcpy(L, &R[2*old_right_cell_count], 2*n);
        memmove(&R[2*n], R, 2*old_right_cell_count);
        memcpy(R, L, 2*n);
    }

    CHECK(tree, left);
    CHECK(tree, right);
}

// This function assumes that the left node has enough room.
// The cursor must be pointing at the parent of the two nodes.
//
// TODO: We should add an optimization to the various cell rotate functions
// whereby they check whether the cells are all of a fixed size and perform
// closed-form calculations instead of loops.
static void node_rotate_cells_left (BCursor *cursor, Node *left, Node *right, u16 n) {
    ASSERT(n);
    ASSERT(n < right->cell_count);

    BTree *tree = cursor->tree;

    if (node_is_inner(left)) {
        Node *parent    = cursor_node(cursor);
        u8 *parent_cell = node_get_cell(parent, cursor_idx(cursor));
        Key parent_key = cell_get_key(parent_cell, parent);

        node_add_inner_cell(tree, left, left->cell_count, parent_key, left->rightmost_child);
        node_move_cells_left(tree, left, right, n - 1);

        u8 *child_cell = node_get_cell(right, 0);
        Key child_key = cell_get_key(child_cell, right);

        copy_key_into_inner_cell(cursor, child_key);
        left->rightmost_child = cell_get_child(child_cell);
        node_delete_cell(tree, right, 0);
    } else {
        node_move_cells_left(tree, left, right, n);
        u8 *child_cell = node_get_cell(left, left->cell_count - 1);
        Key child_key = cell_get_key(child_cell, left);
        copy_key_into_inner_cell(cursor, child_key);
    }

    CHECK(tree, left);
    CHECK(tree, right);
}

// This function assumes that the right node has enough room.
// The cursor must be pointing at the parent of the two nodes.
static void node_rotate_cells_right (BCursor *cursor, Node *left, Node *right, u16 n) {
    ASSERT(n);
    ASSERT(n < left->cell_count);

    BTree *tree = cursor->tree;

    if (node_is_inner(left)) {
        Node *parent    = cursor_node(cursor);
        u8 *parent_cell = node_get_cell(parent, cursor_idx(cursor));
        Key parent_key = cell_get_key(parent_cell, parent);

        node_add_inner_cell(tree, right, 0, parent_key, left->rightmost_child);
        node_move_cells_right(tree, left, right, n - 1);

        u8 *child_cell = node_get_cell(left, left->cell_count - 1);
        Key child_key = cell_get_key(child_cell, left);

        copy_key_into_inner_cell(cursor, child_key);
        left->rightmost_child = cell_get_child(child_cell);
        node_delete_cell(tree, left, left->cell_count - 1);
    } else {
        node_move_cells_right(tree, left, right, n);
        u8 *child_cell = node_get_cell(left, (left->cell_count ? left->cell_count - 1 : 0));
        Key child_key = cell_get_key(child_cell, left);
        copy_key_into_inner_cell(cursor, child_key);
    }

    CHECK(tree, left);
    CHECK(tree, right);
}

// The cursor must be pointing at the parent of the two nodes.
static bool node_try_rotate_bytes_left (BCursor *cursor, Node *left, Node *right, u16 min_bytes_to_rotate, u16 min_bytes_to_remain, u16 min_cells_to_remain) {
    ASSERT(min_bytes_to_rotate);

    u16 left_free_space  = node_get_logical_free_space(left);
    u16 right_free_space = node_get_logical_free_space(right);

    u16 cells_to_rotate = 0;
    u16 bytes_to_rotate = 0;

    cell_iter (right) {
        cells_to_rotate++;
        bytes_to_rotate += 2 + cell_get_size(cursor->tree, CELL, right);
        if (bytes_to_rotate >= min_bytes_to_rotate) break;
    }

    if (bytes_to_rotate > left_free_space) return false;
    if (bytes_to_rotate < min_bytes_to_rotate) return false;
    if ((right->cell_count - cells_to_rotate) < min_cells_to_remain) return false;

    u16 bytes_to_remain = cursor->tree->engine->page_size - right_free_space - bytes_to_rotate;
    if (bytes_to_remain < min_bytes_to_remain) return false;

    node_rotate_cells_left(cursor, left, right, cells_to_rotate);
    return true;
}

// The cursor must be pointing at the parent of the two nodes.
static bool node_try_rotate_bytes_right (BCursor *cursor, Node *left, Node *right, u16 min_bytes_to_rotate, u16 min_bytes_to_remain, u16 min_cells_to_remain) {
    ASSERT(min_bytes_to_rotate);

    u16 left_free_space  = node_get_logical_free_space(left);
    u16 right_free_space = node_get_logical_free_space(right);

    u16 cells_to_rotate = 0;
    u16 bytes_to_rotate = 0;

    cell_iter_reverse (left) {
        cells_to_rotate++;
        bytes_to_rotate += 2 + cell_get_size(cursor->tree, CELL, left);
        if (bytes_to_rotate >= min_bytes_to_rotate) break;
    }

    if (bytes_to_rotate > right_free_space) return false;
    if (bytes_to_rotate < min_bytes_to_rotate) return false;
    if ((left->cell_count - cells_to_rotate) < min_cells_to_remain) return false;

    u16 bytes_to_remain = cursor->tree->engine->page_size - left_free_space - bytes_to_rotate;
    if (bytes_to_remain < min_bytes_to_remain) return false;

    node_rotate_cells_right(cursor, left, right, cells_to_rotate);
    return true;
}

static Node *cursor_node (BCursor *cursor) {
    return cursor->path_len ? cursor->path_nodes[cursor->path_len - 1] : NULL;
}

static void cursor_prev_cell (BCursor *cursor) {
    ASSERT(cursor->path_len > 0);
    --cursor->path_cells[cursor->path_len - 1];
}

static void cursor_next_cell (BCursor *cursor) {
    ++cursor->path_cells[cursor->path_len - 1];
}

static u16 cursor_idx (BCursor *cursor) {
    ASSERT(cursor->path_len);
    return cursor->path_cells[cursor->path_len - 1];
}

static Node *cursor_try_get_left_sibling (BCursor *cursor) {
    if (cursor->path_len < 2) return NULL;
    Node *parent = cursor->path_nodes[cursor->path_len - 2];
    u16 cell_idx = cursor->path_cells[cursor->path_len - 2];
    return (cell_idx == 0) ? NULL : node_get_child(cursor->tree->engine, parent, cell_idx - 1);
}

static Node *cursor_try_get_right_sibling (BCursor *cursor) {
    if (cursor->path_len < 2) return NULL;
    Node *parent = cursor->path_nodes[cursor->path_len - 2];
    u16 cell_idx = cursor->path_cells[cursor->path_len - 2];
    return (cell_idx == parent->cell_count) ? NULL : node_get_child(cursor->tree->engine, parent, cell_idx + 1);
}

static Node *cursor_pop_get (BCursor *cursor, u16 *out_idx) {
    ASSERT(cursor->path_len);
    u8 idx = --cursor->path_len;
    *out_idx = cursor->path_cells[idx];
    return cursor->path_nodes[idx];
}

static void cursor_pop (BCursor *cursor) {
    if (cursor->path_len) cursor->path_len--;
}

static void cursor_pop_unref (BCursor *cursor) {
    if (cursor->path_len) {
        node_unref(cursor->tree->engine, cursor_node(cursor));
        cursor_pop(cursor);
    }
}

void bcursor_reset (BCursor *cursor) {
    cursor->flags = 0;
    while (cursor->path_len) cursor_pop_unref(cursor);
}

static void cursor_push (BCursor *cursor, Node *node, u16 cell_idx) {
    ASSERT(cursor->path_len < MAX_BTREE_HEIGHT);
    cursor->path_nodes[cursor->path_len] = node;
    cursor->path_cells[cursor->path_len] = cell_idx;
    cursor->path_len++;
}

static bool cursor_goto_next_node (BCursor *cursor) {
    BEngine *engine = cursor->tree->engine;
    Node *node = cursor_node(cursor);

    if (! node) {
        Node *root = node_from_page_id(engine, cursor->tree->root);
        cursor_push(cursor, root, 0);
        return true;
    } else if (node_is_inner(node)) {
        ASSERT(cursor_idx(cursor) < node->cell_count);
        Node *child = node_get_child(engine, node, cursor_idx(cursor));
        cursor_push(cursor, child, 0);
        return true;
    } else {
        while (1) {
            if (cursor->flags & F_CURSOR_DELETE_NODE_ON_EXIT) {
                node_delete(engine, node);
                cursor_pop(cursor);
            } else {
                cursor_pop_unref(cursor);
            }

            if (cursor->path_len == 0) return false;

            node = cursor_node(cursor);
            u16 cell_idx = cursor_idx(cursor) + 1;

            if (cell_idx <= node->cell_count) {
                cursor->path_cells[cursor->path_len - 1] = cell_idx;
                Node *child = node_get_child(engine, node, cell_idx);
                cursor_push(cursor, child, 0);
                return true;
            }
        }
    }

    unreachable;
}

static void cursor_goto_leftmost_leaf (BCursor *cursor) {
    while (1) {
        Node *node = cursor_node(cursor);
        if (node_is_leaf(node)) break;
        Node *child = node_get_child(cursor->tree->engine, node, cursor_idx(cursor));
        cursor_push(cursor, child, 0);
    }
}

static void cursor_goto_rightmost_leaf (BCursor *cursor) {
    while (1) {
        Node *node = cursor_node(cursor);
        if (node_is_leaf(node)) break;
        Node *child = node_get_child(cursor->tree->engine, node, cursor_idx(cursor));
        u16 idx = child->cell_count - node_is_leaf(child);
        cursor_push(cursor, child, idx);
    }
}

bool bcursor_goto_next (BCursor *cursor) {
    Node *node = cursor_node(cursor);

    if (cursor->flags & F_CURSOR_SKIP_NEXT) {
        ASSERT(node);
        ASSERT(node_is_leaf(node));
        ASSERT(cursor_idx(cursor) < node->cell_count);
        cursor->flags &= ~F_CURSOR_SKIP_NEXT;
        return true;
    } else if (!node || node_is_inner(node)) {
        return false;
    } else if (cursor_idx(cursor) < node->cell_count - 1) {
        cursor_next_cell(cursor);
        return true;
    } else {
        while (1) {
            cursor_pop_unref(cursor);

            if (cursor->path_len == 0) {
                return false;
            } else if (cursor_idx(cursor) < cursor_node(cursor)->cell_count) {
                cursor_next_cell(cursor);
                cursor_goto_leftmost_leaf(cursor);
                if (cursor_node(cursor)->cell_count) return true;
            }
        }
    }
}

bool bcursor_goto_prev (BCursor *cursor) {
    Node *node = cursor_node(cursor);

    if (cursor->flags & F_CURSOR_SKIP_NEXT) {
        ASSERT(node);
        ASSERT(node_is_leaf(node));
        ASSERT(cursor_idx(cursor) < node->cell_count);
        cursor->flags &= ~F_CURSOR_SKIP_NEXT;
    } else if (!node || node_is_inner(node)) {
        return false;
    }

    if (cursor_idx(cursor) > 0) {
        cursor_prev_cell(cursor);
        return true;
    } else {
        while (1) {
            cursor_pop_unref(cursor);

            if (cursor->path_len == 0) {
                return false;
            } else if (cursor_idx(cursor)) {
                cursor_prev_cell(cursor);
                cursor_goto_rightmost_leaf(cursor);
                if (cursor_node(cursor)->cell_count) return true;
            }
        }
    }
}

bool bcursor_goto_first (BCursor *cursor) {
    bcursor_reset(cursor);

    BTree *tree     = cursor->tree;
    BEngine *engine = tree->engine;
    Node *node      = node_from_page_id(engine, tree->root);

    while (1) {
        cursor_push(cursor, node, 0);
        if (node_is_leaf(node)) break;
        node = node_get_child(engine, node, 0);
    }

    return node->cell_count > 0;
}

#define cursor_goto_key(CURSOR, KEY, KEY_CMP) do{                       \
    DEF(BCursor *, cursor, CURSOR);                                     \
    DEF(key, KEY);                                                      \
    DEF(key_cmp, KEY_CMP);                                              \
                                                                        \
    BTree *tree     = cursor->tree;                                     \
    BEngine *engine = tree->engine;                                     \
                                                                        \
    bcursor_reset(cursor);                                              \
    Node *node = node_from_page_id(engine, cursor->tree->root);         \
                                                                        \
    repeat: if (node_is_inner(node)) {                                  \
        cell_iter (node) {                                              \
            if (key_cmp(key, cell_get_key(CELL, node)) < 1) {           \
                cursor_push(cursor, node, CELL_IDX);                    \
                node = node_from_page_id(engine, cell_get_child(CELL)); \
                goto repeat;                                            \
            }                                                           \
        }                                                               \
                                                                        \
        cursor_push(cursor, node, node->cell_count);                    \
        node = node_from_page_id(engine, node->rightmost_child);        \
        goto repeat;                                                    \
    } else {                                                            \
        cell_iter (node) {                                              \
            int cmp_result = key_cmp(key, cell_get_key(CELL, node));    \
            if (cmp_result < 1) {                                       \
                cursor_push(cursor, node, CELL_IDX);                    \
                return cmp_result == 0;                                 \
            }                                                           \
        }                                                               \
                                                                        \
        cursor_push(cursor, node, node->cell_count);                    \
        return false;                                                   \
    }                                                                   \
}while(0)

bool bcursor_goto_ukey (BCursor *cursor, UKey key) {
    cursor_goto_key(cursor, key, cursor->tree->type->key_cmp);
}

bool bcursor_goto_key (BCursor *cursor, Key key) {
    cursor_goto_key(cursor, key, cursor->tree->type->key_cmp2);
}

void bcursor_close (BCursor *cursor) {
    bcursor_reset(cursor);
    MEM_FREE(cursor->tree->engine->mem, cursor, sizeof(BCursor));
}

BCursor *bcursor_new (BTree *tree) {
    BCursor *cursor = MEM_ALLOC_Z(tree->engine->mem, sizeof(BCursor));
    cursor->tree = tree;
    return cursor;
}

// The cursor will continue pointing at the same cell.
static void node_ensure_cell_space (BCursor *cursor, u16 cell_size) {
    BEngine *engine = cursor->tree->engine;

    while (! node_can_fit_cell(cursor_node(cursor), cell_size)) {
        Node *left  = cursor_try_get_left_sibling(cursor);
        Node *right = cursor_try_get_right_sibling(cursor);

        u16 min_bytes_to_rotate = 2 + cell_size;
        u16 min_bytes_to_remain = (engine->page_size / 2) - min_bytes_to_rotate;

        u16 idx; Node *node = cursor_pop_get(cursor, &idx);

        bool rotated = right && node_try_rotate_bytes_right(cursor, node, right, min_bytes_to_rotate, min_bytes_to_remain, idx + 1);

        if (!rotated && left) {
            u16 prev_cell_count = node->cell_count;
            cursor_prev_cell(cursor);
            rotated = node_try_rotate_bytes_left(cursor, left, node, min_bytes_to_rotate, min_bytes_to_remain, (node->cell_count - idx));
            cursor_next_cell(cursor);
            if (rotated) idx -= (prev_cell_count - node->cell_count);
        }

        if (left) node_unref(engine, left);
        if (right) node_unref(engine, right);
        cursor_push(cursor, node, idx);

        if (! rotated) split_node(cursor);
    }
}

// The cursor will continue pointing at the same cell.
static void split_node (BCursor *cursor) {
    BTree *tree     = cursor->tree;
    BEngine *engine = tree->engine;

    Node *right = cursor_node(cursor);
    Node *left  = node_new(engine, (right->flags & F_NODE_IS_LEAF));

    if (cursor->path_len == 1) { // Make a new root:
        Node *new_root = right;
        right = node_new(engine, 0);
        node_copy(engine, right, new_root);
        node_reset(engine, new_root);
        new_root->rightmost_child = right->page->id;

        u16 idx = cursor_idx(cursor);
        cursor_pop(cursor);
        cursor_push(cursor, new_root, 0);
        cursor_push(cursor, right, idx);

        CHECK(tree, right);
        CHECK(tree, new_root);
    }

    ASSERT(cursor->path_len > 1);

    u16 n_cells_to_move = 0;

    { // Figure out how many cells should be moved:
        u16 total = 0;

        cell_iter (right) {
            total += cell_get_size(tree, CELL, right);
            if (total >= engine->page_size / 2) break;
            n_cells_to_move++;
        }

        // We assert that both nodes will contain some cells.
        // This follows from max cell size being half the page.
        ASSERT(n_cells_to_move != 0);
        ASSERT(n_cells_to_move < right->cell_count);
    }

    { // Insert separator key into parent:
        u16 idx; Node *node = cursor_pop_get(cursor, &idx);

        u8 *cell = node_get_cell(right, n_cells_to_move - 1);
        Key key = cell_get_key(cell, right);

        node_ensure_cell_space(cursor, 4 + tree->type->sizeof_key(key));
        node_add_inner_cell(tree, cursor_node(cursor), cursor_idx(cursor), key, left->page->id);

        cursor_push(cursor, node, idx);
    }

    { // Move cells:
        node_move_cells_left(tree, left, right, n_cells_to_move);

        if (node_is_inner(left)) {
            u8 *cell = node_get_cell(left, left->cell_count - 1);
            left->rightmost_child = cell_get_child(cell);
            node_delete_cell(tree, left, left->cell_count - 1);
        }

        CHECK(tree, left);
    }

    // Adjust the cursor:
    if (cursor_idx(cursor) < n_cells_to_move) {
        cursor->path_nodes[cursor->path_len - 1] = left;
        node_unref(engine, right);
    } else {
        cursor->path_cells[cursor->path_len - 1] -= n_cells_to_move;
        node_unref(engine, left);
    }
}

// For simplicity, the maximum cell size is half the page.
//
// TODO: We should either implement overflow pages and thus
// allow cells of arbitrary size or keep this limit and have
// the engine gracefully return an error.
static void check_cell_size (BEngine *engine, u32 key_size, u32 val_size) {
    u32 size = key_size + MAX(val_size, sizeof(Page_Id)) + 2; // +2 for the cell offset.
    ASSERT(size < (engine->page_size / 2));
}

// Insert the entry right before the entry currently pointed at.
// After that, the cursor will point at the newly inserted entry.
void bcursor_insert (BCursor *cursor, UKey key, Val val) {
    BTree *tree     = cursor->tree;
    BEngine *engine = tree->engine;

    u32 key_size = tree->type->sizeof_ukey(key);
    u32 val_size = tree->type->sizeof_val(val);

    check_cell_size(engine, key_size, val_size);
    node_ensure_cell_space(cursor, key_size + val_size);

    Node *node = cursor_node(cursor);
    u8 *cell   = node_add_cell(tree, node, cursor_idx(cursor), key_size + val_size);

    tree->type->serialize_key(cell_get_key(cell, node), key);
    memcpy(cell_get_val(tree, cell, node).ptr, val.ptr, val_size);

    CHECK(tree, node);
}

static bool try_merge_right (BCursor *cursor, Node **left_ptr, Node **right_ptr) {
    BTree *tree     = cursor->tree;
    BEngine *engine = tree->engine;

    Node *left      = *left_ptr;
    Node *right     = *right_ptr;
    Node *parent    = cursor_node(cursor);
    u8 *parent_cell = node_get_cell(parent, cursor_idx(cursor));

    u64 bytes_to_move = engine->page_size - node_get_logical_free_space(left);
    if (node_is_inner(left)) bytes_to_move += 2 + cell_get_size(tree, parent_cell, parent);

    if (bytes_to_move > node_get_logical_free_space(right)) return false;

    { // Move cells:
        Key parent_key = cell_get_key(parent_cell, parent);
        if (node_is_inner(left)) node_add_inner_cell(tree, right, 0, parent_key, left->rightmost_child);
        node_move_cells_right(tree, left, right, left->cell_count);
    }

    node_delete(engine, left);
    *left_ptr = NULL;

    CHECK(tree, right);
    CHECK(tree, parent);

    if (cursor->path_len == 1 && parent->cell_count == 1) {
        // We must delete the root node.
        node_copy(engine, parent, right);
        node_delete(engine, right);
        *right_ptr = NULL;
        cursor_pop_unref(cursor);
    } else {
        cursor_remove(cursor);
    }

    return true;
}

static void cursor_remove (BCursor *cursor) {
    BTree *tree     = cursor->tree;
    BEngine *engine = tree->engine;
    Node *node      = cursor_node(cursor);
    u16 half_page   = engine->page_size / 2;
    u16 free_space  = node_get_logical_free_space(node);

    node_delete_cell(tree, node, cursor_idx(cursor));
    if (free_space <= half_page) return;

    Node *left  = cursor_try_get_left_sibling(cursor);
    Node *right = cursor_try_get_right_sibling(cursor);

    cursor_pop(cursor);

    { // See if we can rotate cells from one of the siblings:
        u16 min_bytes_to_remain = half_page;
        u16 min_bytes_to_rotate = free_space - half_page;

        bool rotated = right && node_try_rotate_bytes_left(cursor, node, right, min_bytes_to_rotate, min_bytes_to_remain, 1);
        if (!rotated && left) {
            --cursor->path_cells[cursor->path_len - 1];
            rotated = node_try_rotate_bytes_right(cursor, left, node, min_bytes_to_rotate, min_bytes_to_remain, 1);
            ++cursor->path_cells[cursor->path_len - 1];
        }

        if (rotated) goto done;
    }

    if (right && try_merge_right(cursor, &node, &right)) {
        // done
    } else if (left) {
        --cursor->path_cells[cursor->path_len - 1];
        try_merge_right(cursor, &left, &node);
    }

    done: {
        if (node)  node_unref(engine, node);
        if (left)  node_unref(engine, left);
        if (right) node_unref(engine, right);
    }
}

// After the removal, calling bcursor_goto_next() moves
// the cursor to the entry after the one that was removed.
void bcursor_remove (BCursor *cursor) {
    BTree *tree     = cursor->tree;
    BEngine *engine = tree->engine;
    Node *node      = cursor_node(cursor);
    u8 *cell        = node_get_cell(node, cursor_idx(cursor));
    u32 free_space  = node_get_logical_free_space(node) + cell_get_size(tree, cell, node) + 2;

    if (free_space <= engine->page_size / 2) {
        node_delete_cell(tree, node, cursor_idx(cursor));
    } else {
        Key key = cell_get_key(cell, node);
        u32 key_size = tree->type->sizeof_key(key);
        key.ptr = mem_copy((Mem*)engine->key_saver, key.ptr, key_size);
        cursor_remove(cursor);
        bcursor_goto_key(cursor, key);
        mem_arena_clear(engine->key_saver);
    }

    if (cursor_idx(cursor) < cursor_node(cursor)->cell_count) cursor->flags |= F_CURSOR_SKIP_NEXT;
}

// After the update the cursor will still point at the same entry.
void bcursor_update (BCursor *cursor, Val new_val) {
    BTree *tree      = cursor->tree;
    BEngine *engine  = tree->engine;
    Node *node       = cursor_node(cursor);
    u8 *cell         = node_get_cell(node, cursor_idx(cursor));
    u32 old_val_size = cell_get_size(tree, cell, node);
    u32 new_val_size = tree->type->sizeof_val(new_val);

    if (new_val_size == old_val_size) {
        Val old_val = cell_get_val(tree, cell, node);
        memcpy(old_val.ptr, new_val.ptr, new_val_size);
        return;
    }

    Key key = cell_get_key(cell, node);
    u32 key_size = tree->type->sizeof_key(key);
    key.ptr = mem_copy((Mem*)engine->key_saver, key.ptr, key_size);

    check_cell_size(engine, key_size, new_val_size);
    node_delete_cell(tree, node, cursor_idx(cursor));

    u32 new_cell_size = key_size + new_val_size;
    node_ensure_cell_space(cursor, new_cell_size);
    u8 *new_cell = node_add_cell(tree, node, cursor_idx(cursor), new_cell_size);

    memcpy(cell_get_key(new_cell, node).ptr, key.ptr, key_size);
    memcpy(cell_get_val(tree, new_cell, node).ptr, new_val.ptr, new_val_size);
    mem_arena_clear(engine->key_saver);

    CHECK(tree, node);
}

Val bcursor_read (BCursor *cursor) {
    Node *node = cursor_node(cursor);
    u8 *cell   = node_get_cell(node, cursor_idx(cursor));
    return cell_get_val(cursor->tree, cell, node);
}

void btree_delete (BTree *tree) {
    BCursor *cursor = bcursor_new(tree);
    cursor->flags |= F_CURSOR_DELETE_NODE_ON_EXIT;
    while (cursor_goto_next_node(cursor));
    bcursor_close(cursor);
}

static BTree *btree_alloc (BEngine *engine, Type_Table *type, Page_Id id) {
    BTree *tree  = MEM_ALLOC(type->mem, sizeof(BTree));
    tree->type   = get_btype_for_table(type);
    tree->engine = engine;
    tree->root   = id;
    return tree;
}

BTree *btree_load (BEngine *engine, Type_Table *type, s64 tag) {
    return btree_alloc(engine, type, (Page_Id)tag);
}

BTree *btree_new (BEngine *engine, Type_Table *type) {
    Node *root = node_new(engine, F_NODE_IS_LEAF);
    Page_Id root_id = root->page->id;
    node_unref(engine, root);
    return btree_alloc(engine, type, root_id);
}

void btree_print (BTree *tree) {
    BEngine *engine = tree->engine;

    File file = fs_open_file(engine->fs, str("/tmp/btree.dot"));

    DString nodes = ds_new(engine->mem);
    DString edges = ds_new(engine->mem);

    ds_add_cstr(&nodes, "digraph {\n"
                        "    graph [fillcolor=\"#332717\" color=\"#0f0b06\" bgcolor=\"#221A0F\" splines=true ranksep=.5 nodesep=.1 rankdir=TB]\n"
                        "    node  [fontcolor=\"#B48E56\" label=\"\" labelloc=\"b\" shape=box penwidth=0 width=0 height=0 margin=0 style=plaintext]\n"
                        "    edge  [color=\"#B48E56\" arrowsize=.5]\n");
    fs_overwrite_file(engine->fs, file, ds_to_str(&nodes));
    ds_clear(&nodes);

    BCursor *cursor = bcursor_new(tree);
    while (cursor_goto_next_node(cursor)) {
        Node *node = cursor_node(cursor);
        ds_add_fmt(&nodes, "\n    subgraph \"cluster_%i\" { style=filled\n", node->page->id);

        if (node_is_inner(node)) {
            ds_add_fmt(&nodes, "        { rank=same\n        \"node_%i\"\n", node->page->id);

            cell_iter (node) {
                Page_Id child = cell_get_child(CELL);
                ds_add_fmt(&nodes, "        \"ptr_%i_%i\"\n", node->page->id, CELL_IDX);
                ds_add_fmt(&nodes, "        \"cell_%i_%i\" [label=<", node->page->id, CELL_IDX);
                tree->type->key_print(&nodes, cell_get_key(CELL, node));
                ds_add_cstr(&nodes, ">]\n");
                ds_add_fmt(&edges, "    \"ptr_%i_%i\" -> \"node_%i\" [lhead=\"cluster_%i\"];\n", node->page->id, CELL_IDX, child, child);
            }

            ds_add_fmt(&nodes, "        \"ptr_%i_%i\" }\n\n", node->page->id, node->cell_count);
            ds_add_fmt(&nodes, "        \"node_%i\" -> ", node->page->id);
            ds_add_fmt(&edges, "    \"ptr_%i_%i\" -> \"node_%i\" [lhead=\"cluster_%i\"];\n", node->page->id, node->cell_count, node->rightmost_child, node->rightmost_child);

            // This invisible edge between the cells ensures a correct render order.
            cell_iter_reverse (node) ds_add_fmt(&nodes, "\"ptr_%i_%i\" -> \"cell_%i_%i\" -> ", node->page->id, CELL_IDX, node->page->id, CELL_IDX);
            ds_add_fmt(&nodes, "\"ptr_%i_%i\" [style=invis]\n", node->page->id, node->cell_count);
        } else {
            cell_iter_reverse (node) {
                ds_add_fmt(&nodes, "        \"cell_%i_%i\" [label=<", node->page->id, CELL_IDX);
                tree->type->key_print(&nodes, cell_get_key(CELL, node));
                ds_add_cstr(&nodes, ">]\n");
            }

            ds_add_fmt(&nodes, "        \"node_%i\"\n", node->page->id);
        }

        ds_add_cstr(&nodes, "    }\n");
    }

    ds_add_cstr(&nodes, "\n");
    ds_add_cstr(&edges, "}\n");

    fs_append_to_file(engine->fs, file, ds_to_str(&nodes));
    fs_append_to_file(engine->fs, file, ds_to_str(&edges));

    ds_free(&nodes);
    ds_free(&edges);
    fs_close_file(engine->fs, file);
    bcursor_close(cursor);
}

BEngine *bengine_new (Files *fs, Mem *mem, String db_file_path) {
    BEngine *engine = MEM_ALLOC_Z(mem, sizeof(BEngine));

    engine->fs             = fs;
    engine->mem            = mem;
    engine->key_saver      = mem_arena_new(mem, 512);
    engine->pager          = pager_new(fs, mem, db_file_path);
    engine->full_page_size = pager_get_page_size(engine->pager);
    engine->page_size      = engine->full_page_size - NODE_HEADER_SIZE;
    engine->scratch_page   = MEM_ALLOC(mem, engine->full_page_size);

    ASSERT((engine->page_size % 2) == 0);

    pager_init_user_buffers(engine->pager, sizeof(Node));

    return engine;
}

void bengine_close (BEngine *engine) {
    mem_arena_destroy(engine->key_saver);
    MEM_FREE(engine->mem, engine->scratch_page, engine->full_page_size);
    MEM_FREE(engine->mem, engine, sizeof(BEngine));
}

s64 bengine_get_tag (Type_Table *table) {
    BTree *tree = table->engine_specific_info;
    return (s64)tree->root;
}

bool bengine_db_is_empty (BEngine *engine) {
    return pager_file_is_empty(engine->pager);
}

// =============================================================================
// Engine specific type functions.
// =============================================================================
u32  sizeof_val         (Val val)              { return 4 + read_u32_le(val.ptr); }

void int_key_print      (DString *ds, Key key) { ds_add_fmt(ds, "%li", read_s64_le(key.ptr)); }
u32  int_sizeof_key     (Key key)              { return 8; }
u32  int_sizeof_ukey    (UKey ukey)            { return 8; }
void int_serialize_key  (Key key, UKey ukey)   { write_s64_le(key.ptr, *(s64*)ukey.ptr); }
int  int_key_cmp        (UKey ukey, Key key)   { s64 K1 = *(s64*)ukey.ptr; s64 K2 = read_s64_le(key.ptr); if (K1 < K2) return -1; if (K1 == K2) return 0; return 1; }
int  int_key_cmp2       (Key key1, Key key2)   { s64 K1 = read_s64_le(key1.ptr); s64 K2 = read_s64_le(key2.ptr); if (K1 < K2) return -1; if (K1 == K2) return 0; return 1; }

void bool_key_print     (DString *ds, Key key) { ds_add_fmt(ds, "%u", *(u8*)key.ptr); }
u32  bool_sizeof_key    (Key key)              { return 1; }
u32  bool_sizeof_ukey   (UKey ukey)            { return 1; }
void bool_serialize_key (Key key, UKey ukey)   { *(u8*)key.ptr = *(u8*)ukey.ptr; }
int  bool_key_cmp       (UKey ukey, Key key)   { u8 K1 = *(u8*)ukey.ptr; u8 K2 = *(u8*)key.ptr; if (K1 < K2) return -1; if (K1 == K2) return 0; return 1; }
int  bool_key_cmp2      (Key key1, Key key2)   { u8 K1 = *(u8*)key1.ptr; u8 K2 = *(u8*)key2.ptr; if (K1 < K2) return -1; if (K1 == K2) return 0; return 1; }

void str_key_print      (DString *ds, Key key) { ds_add_fmt(ds, "%.*s", read_u32_le(key.ptr), 4 + (char*)key.ptr); }
u32  str_sizeof_key     (Key key)              { return 4 + read_u32_le(key.ptr); }
u32  str_sizeof_ukey    (UKey ukey)            { return 4 + ((String*)ukey.ptr)->count; }
void str_serialize_key  (Key key, UKey ukey)   { String *str = (String*)ukey.ptr; write_u32_le(key.ptr, str->count); memcpy((char*)key.ptr + 4, str->data, str->count); }
int  str_key_cmp        (UKey ukey, Key key)   { String K1 = *(String*)ukey.ptr; String K2 = { .count = read_u32_le(key.ptr), .data = (char*)key.ptr + 4 }; return strncmp(K1.data, K2.data, MIN(K1.count, K2.count)); }
int  str_key_cmp2       (Key key1, Key key2)   { String K1 = { .count = read_u32_le(key1.ptr), .data = (char*)key1.ptr + 4 }; String K2 = { .count = read_u32_le(key2.ptr), .data = (char*)key2.ptr + 4 }; return strncmp(K1.data, K2.data, MIN(K1.count, K2.count)); }

BType btype_int  = { int_key_cmp, int_key_print, int_sizeof_key, int_sizeof_ukey, int_serialize_key, sizeof_val, int_key_cmp2 };
BType btype_bool = { bool_key_cmp, bool_key_print, bool_sizeof_key, bool_sizeof_ukey, bool_serialize_key, sizeof_val, bool_key_cmp2 };
BType btype_str  = { str_key_cmp, str_key_print, str_sizeof_key, str_sizeof_ukey, str_serialize_key, sizeof_val, str_key_cmp2 };

static BType *get_btype_for_table (Type_Table *table) {
    Array_Type_Column *col_types = &array_get_first(&table->row->scopes)->cols;
    Type *prim_key = array_get(col_types, table->prim_key_col)->field;

    switch (prim_key->tag) {
    case TYPE_INT:  return &btype_int;
    case TYPE_TEXT: return &btype_str;
    case TYPE_BOOL: return &btype_bool;
    default:        unreachable;
    }
}
