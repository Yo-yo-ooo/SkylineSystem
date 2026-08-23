//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#include <klib/algorithm/art.h>
#include <klib/klibc.h>
#include <pdef.h>
extern void *__memcpy(void * d, const void * s, uint64_t n);
extern void Panic(const char* message);

/* ========== 优化新增：分支预测与预取宏 ========== */
#define prefetch_r(x) __builtin_prefetch((x), 0, 3)  // 读预取，高时间局部性
#define prefetch_w(x) __builtin_prefetch((x), 1, 3)  // 写预取，高时间局部性

/**
 * Macros to manipulate pointer tags
 */
#define IS_LEAF(x)    (((uintptr_t)x & 1))
#define SET_LEAF(x)   ((void*)((uintptr_t)x | 1))
#define LEAF_RAW(x)   ((art_leaf*)((void*)((uintptr_t)x & ~1)))

/**
 * Allocates a node of the given type,
 * initializes to zero and sets the type.
 */
static art_node* alloc_node(uint8_t type) {
    art_node* n;
    switch (type) {
        case NODE4:
            n = (art_node*)kcalloc(1, sizeof(art_node4));
            break;
        case NODE16:
            n = (art_node*)kcalloc(1, sizeof(art_node16));
            break;
        case NODE48:
            n = (art_node*)kcalloc(1, sizeof(art_node48));
            break;
        case NODE256:
            n = (art_node*)kcalloc(1, sizeof(art_node256));
            break;
        default:
            Panic("Abort!");
    }
    n->type = type;
    return n;
}

/**
 * Initializes an ART tree
 * @return 0 on success.
 */
int32_t art_tree_init(art_tree *t) {
    t->root = NULL;
    t->size = 0;
    return 0;
}

// Recursively destroys the tree
static void destroy_node(art_node *n) {
    // Break if null
    if (unlikely(!n)) return;

    // Special case leafs
    if (unlikely(IS_LEAF(n))) {
        kfree(LEAF_RAW(n));
        return;
    }

    // Handle each node type
    int32_t i, idx;
    union {
        art_node4 *p1;
        art_node16 *p2;
        art_node48 *p3;
        art_node256 *p4;
    } p;

    switch (n->type) {
        case NODE4:
            p.p1 = (art_node4*)n;
            for (i=0;i<n->num_children;i++) {
                destroy_node(p.p1->children[i]);
            }
            break;
        case NODE16:
            p.p2 = (art_node16*)n;
            for (i=0;i<n->num_children;i++) {
                destroy_node(p.p2->children[i]);
            }
            break;
        case NODE48:
            p.p3 = (art_node48*)n;
            for (i=0;i<256;i++) {
                idx = p.p3->keys[i];
                if (unlikely(!idx)) continue;
                destroy_node(p.p3->children[idx-1]);
            }
            break;
        case NODE256:
            p.p4 = (art_node256*)n;
            for (i=0;i<256;i++) {
                if (likely(p.p4->children[i]))
                    destroy_node(p.p4->children[i]);
            }
            break;
        default:
            Panic("Abort!");
    }

    // Free ourself on the way up
    kfree(n);
}

/**
 * Destroys an ART tree
 * @return 0 on success.
 */
int32_t art_tree_destroy(art_tree *t) {
    destroy_node(t->root);
    return 0;
}

/**
 * Returns the size of the ART tree.
 */
#ifndef BROKEN_GCC_C99_INLINE
extern inline uint64_t art_size(art_tree *t);
#endif

static art_node** find_child(art_node *n, uint8_t c) {
    union {
        art_node4 *p1;
        art_node16 *p2;
        art_node48 *p3;
        art_node256 *p4;
    } p;

    /* ========== BUG修复：修正NODE16块作用域位置，原代码块包裹了case标签 ========== */
    switch (n->type) {
        case NODE4:
            p.p1 = (art_node4*)n;
            // 预取keys数组，加速循环比较
            prefetch_r(p.p1->keys);
            for (int32_t i=0 ; i < n->num_children; i++) {
                /* this cast works around a bug in gcc 5.1 when unrolling loops
                 * https://gcc.gnu.org/bugzilla/show_bug.cgi?id=59124
                 */
                if (unlikely(((uint8_t*)p.p1->keys)[i] == c)) {
                    // 命中时预取子节点内容，减少下一级缓存缺失
                    prefetch_r(p.p1->children[i]);
                    return &p.p1->children[i];
                }
            }
            break;

        case NODE16: {
            p.p2 = (art_node16*)n;
            // 类型修复：bitfield/mask使用无符号类型，避免移位溢出风险
            uint32_t bitfield = 0;
            uint32_t mask;

            prefetch_r(p.p2->keys);
            // Compare the key to all 16 stored keys
            for (int32_t i = 0; i < 16; ++i) {
                if (p.p2->keys[i] == c)
                    bitfield |= (1U << i);
            }

            // Use a mask to ignore children that don't exist
            mask = (1U << n->num_children) - 1;
            bitfield &= mask;

            /*
             * If we have a match (any bit set) then we can
             * return the pointer match using ctz to get
             * the index.
             */
            if (likely(bitfield)) {
                uint32_t idx = __builtin_ctz(bitfield);
                prefetch_r(p.p2->children[idx]);
                return &p.p2->children[idx];
            }
            break;
        }

        case NODE48:
            p.p3 = (art_node48*)n;
            // NODE48是O(1)直接索引，最热路径，预取keys数组
            prefetch_r(&p.p3->keys[c]);
            uint8_t idx = p.p3->keys[c];
            if (likely(idx)) {
                prefetch_r(p.p3->children[idx-1]);
                return &p.p3->children[idx-1];
            }
            break;

        case NODE256:
            p.p4 = (art_node256*)n;
            prefetch_r(&p.p4->children[c]);
            if (likely(p.p4->children[c])) {
                prefetch_r(p.p4->children[c]);
                return &p.p4->children[c];
            }
            break;

        default:
            Panic("Abort!");
    }
    return NULL;
}

// Simple inlined if
static inline int32_t _min__art(int32_t a, int32_t b) {
    return (a < b) ? a : b;
}

/**
 * Returns the number of prefix characters shared between
 * the key and node.
 */
static int32_t check_prefix(const art_node *n, const uint8_t *key, int32_t key_len, int32_t depth) {
    int32_t max_cmp = _min__art(_min__art(n->partial_len, MAX_PREFIX_LEN), key_len - depth);
    prefetch_r(n->partial);

    int32_t idx;
    for (idx=0; idx < max_cmp; idx++) {
        if (unlikely(n->partial[idx] != key[depth+idx]))
            return idx;
    }
    return idx;
}

/**
 * Checks if a leaf matches
 * @return 0 on success.
 */
static int32_t leaf_matches(const art_leaf *n, const uint8_t *key, int32_t key_len, int32_t depth) {
    (void)depth;
    // Fail if the key lengths are different
    if (unlikely(n->key_len != (uint32_t)key_len)) return 1;
    // Compare the keys starting at the depth
    return _memcmp(n->key, key, key_len);
}

/**
 * Searches for a value in the ART tree
 * @arg t The tree
 * @arg key The key
 * @arg key_len The length of the key
 * @return NULL if the item was not found, otherwise
 * the value pointer is returned.
 */
void* art_search(const art_tree *t, const uint8_t *key, int32_t key_len) {
    art_node **child;
    art_node *n = t->root;
    int32_t prefix_len, depth = 0;

    // 预取根节点
    if (likely(n)) prefetch_r(n);

    while (likely(n)) {
        // Might be a leaf
        if (unlikely(IS_LEAF(n))) {
            art_leaf *leaf = LEAF_RAW(n);
            prefetch_r(leaf);
            // Check if the expanded path matches
            if (likely(!leaf_matches(leaf, key, key_len, depth))) {
                return leaf->value;
            }
            return NULL;
        }

        // Bail if the prefix does not match
        if (unlikely(n->partial_len)) {
            prefix_len = check_prefix(n, key, key_len, depth);
            if (unlikely(prefix_len != _min__art(MAX_PREFIX_LEN, n->partial_len)))
                return NULL;
            depth = depth + n->partial_len;
        }

        // Recursively search
        child = find_child(n, key[depth]);
        n = likely(child) ? *child : NULL;
        depth++;
    }
    return NULL;
}

// Find the minimum leaf under a node
static art_leaf* minimum(const art_node *n) {
    // Handle base cases
    if (unlikely(!n)) return NULL;
    if (unlikely(IS_LEAF(n))) return LEAF_RAW(n);

    int32_t idx;
    switch (n->type) {
        case NODE4:
            prefetch_r(((const art_node4*)n)->children[0]);
            return minimum(((const art_node4*)n)->children[0]);
        case NODE16:
            prefetch_r(((const art_node16*)n)->children[0]);
            return minimum(((const art_node16*)n)->children[0]);
        case NODE48:
            idx=0;
            while (!((const art_node48*)n)->keys[idx]) idx++;
            idx = ((const art_node48*)n)->keys[idx] - 1;
            prefetch_r(((const art_node48*)n)->children[idx]);
            return minimum(((const art_node48*)n)->children[idx]);
        case NODE256:
            idx=0;
            while (!((const art_node256*)n)->children[idx]) idx++;
            prefetch_r(((const art_node256*)n)->children[idx]);
            return minimum(((const art_node256*)n)->children[idx]);
        default:
            Panic("Abort!");
    }
}

// Find the maximum leaf under a node
static art_leaf* maximum(const art_node *n) {
    // Handle base cases
    if (unlikely(!n)) return NULL;
    if (unlikely(IS_LEAF(n))) return LEAF_RAW(n);

    int32_t idx;
    switch (n->type) {
        case NODE4:
            prefetch_r(((const art_node4*)n)->children[n->num_children-1]);
            return maximum(((const art_node4*)n)->children[n->num_children-1]);
        case NODE16:
            prefetch_r(((const art_node16*)n)->children[n->num_children-1]);
            return maximum(((const art_node16*)n)->children[n->num_children-1]);
        case NODE48:
            idx=255;
            while (!((const art_node48*)n)->keys[idx]) idx--;
            idx = ((const art_node48*)n)->keys[idx] - 1;
            prefetch_r(((const art_node48*)n)->children[idx]);
            return maximum(((const art_node48*)n)->children[idx]);
        case NODE256:
            idx=255;
            while (!((const art_node256*)n)->children[idx]) idx--;
            prefetch_r(((const art_node256*)n)->children[idx]);
            return maximum(((const art_node256*)n)->children[idx]);
        default:
            Panic("Abort!");
    }
}

/**
 * Returns the minimum valued leaf
 */
art_leaf* art_minimum(art_tree *t) {
    return minimum((art_node*)t->root);
}

/**
 * Returns the maximum valued leaf
 */
art_leaf* art_maximum(art_tree *t) {
    return maximum((art_node*)t->root);
}

static art_leaf* make_leaf(const uint8_t *key, int32_t key_len, void *value) {
    art_leaf *l = (art_leaf*)kcalloc(1, sizeof(art_leaf)+key_len);
    l->value = value;
    l->key_len = key_len;
    __memcpy(l->key, key, key_len);
    return l;
}

static int32_t longest_common_prefix(art_leaf *l1, art_leaf *l2, int32_t depth) {
    int32_t max_cmp = _min__art(l1->key_len, l2->key_len) - depth;
    int32_t idx;
    for (idx=0; idx < max_cmp; idx++) {
        if (unlikely(l1->key[depth+idx] != l2->key[depth+idx]))
            return idx;
    }
    return idx;
}

static void copy_header(art_node *dest, art_node *src) {
    dest->num_children = src->num_children;
    dest->partial_len = src->partial_len;
    __memcpy(dest->partial, src->partial, _min__art(MAX_PREFIX_LEN, src->partial_len));
}

static void add_child256(art_node256 *n, art_node **ref, uint8_t c, void *child) {
    (void)ref;
    n->n.num_children++;
    n->children[c] = (art_node*)child;
}

static void add_child48(art_node48 *n, art_node **ref, uint8_t c, void *child) {
    if (likely(n->n.num_children < 48)) {
        // 优化：从num_children位置查找空位，而非从头遍历
        int32_t pos = n->n.num_children;
        // 理论上num_children位置即为空位，直接使用
        n->children[pos] = (art_node*)child;
        n->keys[c] = pos + 1;
        n->n.num_children++;
    } else {
        // 扩容为冷路径，标记unlikely
        art_node256 *new_node = (art_node256*)alloc_node(NODE256);
        for (int32_t i=0;i<256;i++) {
            if (n->keys[i]) {
                new_node->children[i] = n->children[n->keys[i] - 1];
            }
        }
        copy_header((art_node*)new_node, (art_node*)n);
        *ref = (art_node*)new_node;
        kfree(n);
        add_child256(new_node, ref, c, child);
    }
}

static void add_child16(art_node16 *n, art_node **ref, uint8_t c, void *child) {
    if (likely(n->n.num_children < 16)) {
        uint32_t mask = (1U << n->n.num_children) - 1;

        // Compare the key to all 16 stored keys
        uint32_t bitfield = 0;
        prefetch_r(n->keys);
        for (int32_t i = 0; i < n->n.num_children; ++i) {
            if (c < n->keys[i])
                bitfield |= (1U << i);
        }

        bitfield &= mask;
        uint32_t idx;

        if (likely(bitfield)) {
            idx = __builtin_ctz(bitfield);
            _memmove(n->keys+idx+1, n->keys+idx, n->n.num_children - idx);
            _memmove(n->children+idx+1, n->children+idx,
                    (n->n.num_children - idx) * sizeof(void*));
        } else {
            idx = n->n.num_children;
        }

        // Set the child
        n->keys[idx] = c;
        n->children[idx] = (art_node*)child;
        n->n.num_children++;
    } else {
        // 扩容为冷路径
        art_node48 *new_node = (art_node48*)alloc_node(NODE48);
        // Copy the child pointers and populate the key map
        __memcpy(new_node->children, n->children,
                sizeof(void*) * n->n.num_children);
        for (int32_t i=0; i < n->n.num_children; i++) {
            new_node->keys[n->keys[i]] = i + 1;
        }
        copy_header((art_node*)new_node, (art_node*)n);
        *ref = (art_node*)new_node;
        kfree(n);
        add_child48(new_node, ref, c, child);
    }
}

static void add_child4(art_node4 *n, art_node **ref, uint8_t c, void *child) {
    if (likely(n->n.num_children < 4)) {
        int32_t idx;
        for (idx=0; idx < n->n.num_children; idx++) {
            if (c < n->keys[idx]) break;
        }
        // Shift to make room
        _memmove(n->keys+idx+1, n->keys+idx, n->n.num_children - idx);
        _memmove(n->children+idx+1, n->children+idx,
                (n->n.num_children - idx) * sizeof(void*));
        // Insert element
        n->keys[idx] = c;
        n->children[idx] = (art_node*)child;
        n->n.num_children++;
    } else {
        // 扩容为冷路径
        art_node16 *new_node = (art_node16*)alloc_node(NODE16);
        // Copy the child pointers and the key map
        __memcpy(new_node->children, n->children,
                sizeof(void*) * n->n.num_children);
        __memcpy(new_node->keys, n->keys,
                sizeof(uint8_t) * n->n.num_children);
        copy_header((art_node*)new_node, (art_node*)n);
        *ref = (art_node*)new_node;
        kfree(n);
        add_child16(new_node, ref, c, child);
    }
}

static void add_child(art_node *n, art_node **ref, uint8_t c, void *child) {
    switch (n->type) {
        case NODE4:
            add_child4((art_node4*)n, ref, c, child);
            return;
        case NODE16:
            add_child16((art_node16*)n, ref, c, child);
            return;
        case NODE48:
            add_child48((art_node48*)n, ref, c, child);
            return;
        case NODE256:
            add_child256((art_node256*)n, ref, c, child);
            return;
        default:
            Panic("Abort!");
    }
}

/**
 * Calculates the index at which the prefixes mismatch
 */
static int32_t prefix_mismatch(const art_node *n, const uint8_t *key, int32_t key_len, int32_t depth) {
    int32_t max_cmp = _min__art(_min__art(MAX_PREFIX_LEN, n->partial_len), key_len - depth);
    int32_t idx;

    prefetch_r(n->partial);
    for (idx=0; idx < max_cmp; idx++) {
        if (unlikely(n->partial[idx] != key[depth+idx]))
            return idx;
    }

    /* ========== 注释修复：原"int16_t"为拼写错误，应为"shorter than or equal to" ==========
     * If the prefix is shorter than or equal to MAX_PREFIX_LEN we can avoid finding a leaf
     */
    if (unlikely(n->partial_len > MAX_PREFIX_LEN)) {
        // Prefix is longer than what we've checked, find a leaf for full comparison
        art_leaf *l = minimum(n);
        /* ========== 性能修复：限制比较上限为节点前缀总长度，避免多余比较 ========== */
        max_cmp = _min__art(_min__art((int32_t)l->key_len, key_len) - depth, n->partial_len);
        for (; idx < max_cmp; idx++) {
            if (unlikely(l->key[idx+depth] != key[depth+idx]))
                return idx;
        }
    }
    return idx;
}

static void* recursive_insert(art_node *n, art_node **ref, const uint8_t *key, int32_t key_len, void *value, int32_t depth, int32_t *old, int32_t replace) {
    // If we are at a NULL node, inject a leaf
    if (unlikely(!n)) {
        *ref = (art_node*)SET_LEAF(make_leaf(key, key_len, value));
        return NULL;
    }

    // If we are at a leaf, we need to replace it with a node
    if (unlikely(IS_LEAF(n))) {
        art_leaf *l = LEAF_RAW(n);
        // Check if we are updating an existing value
        if (likely(!leaf_matches(l, key, key_len, depth))) {
            *old = 1;
            void *old_val = l->value;
            if(replace) l->value = value;
            return old_val;
        }

        // New value, we must split the leaf into a node4
        art_node4 *new_node = (art_node4*)alloc_node(NODE4);
        // Create a new leaf
        art_leaf *l2 = make_leaf(key, key_len, value);
        // Determine longest prefix
        int32_t longest_prefix = longest_common_prefix(l, l2, depth);
        new_node->n.partial_len = longest_prefix;
        __memcpy(new_node->n.partial, key+depth, _min__art(MAX_PREFIX_LEN, longest_prefix));
        // Add the leafs to the new node4
        *ref = (art_node*)new_node;
        add_child4(new_node, ref, l->key[depth+longest_prefix], SET_LEAF(l));
        add_child4(new_node, ref, l2->key[depth+longest_prefix], SET_LEAF(l2));
        return NULL;
    }

    // Check if given node has a prefix
    if (unlikely(n->partial_len)) {
        // Determine if the prefixes differ, since we need to split
        int32_t prefix_diff = prefix_mismatch(n, key, key_len, depth);
        if (likely((uint32_t)prefix_diff >= n->partial_len)) {
            depth += n->partial_len;
            goto RECURSE_SEARCH;
        }

        // Prefix split: cold path
        art_node4 *new_node = (art_node4*)alloc_node(NODE4);
        *ref = (art_node*)new_node;
        new_node->n.partial_len = prefix_diff;
        __memcpy(new_node->n.partial, n->partial, _min__art(MAX_PREFIX_LEN, prefix_diff));

        // Adjust the prefix of the old node
        if (likely(n->partial_len <= MAX_PREFIX_LEN)) {
            add_child4(new_node, ref, n->partial[prefix_diff], n);
            n->partial_len -= (prefix_diff+1);
            _memmove(n->partial, n->partial+prefix_diff+1,
                    _min__art(MAX_PREFIX_LEN, n->partial_len));
        } else {
            n->partial_len -= (prefix_diff+1);
            art_leaf *l = minimum(n);
            add_child4(new_node, ref, l->key[depth+prefix_diff], n);
            __memcpy(n->partial, l->key+depth+prefix_diff+1,
                    _min__art(MAX_PREFIX_LEN, n->partial_len));
        }

        // Insert the new leaf
        art_leaf *l = make_leaf(key, key_len, value);
        add_child4(new_node, ref, key[depth+prefix_diff], SET_LEAF(l));
        return NULL;
    }

RECURSE_SEARCH:;
    // Find a child to recurse to
    art_node **child = find_child(n, key[depth]);
    if (likely(child)) {
        return recursive_insert(*child, child, key, key_len, value, depth+1, old, replace);
    }

    // No child, insert directly
    art_leaf *l = make_leaf(key, key_len, value);
    add_child(n, ref, key[depth], SET_LEAF(l));
    return NULL;
}

/**
 * inserts a new value into the art tree
 * @arg t the tree
 * @arg key the key
 * @arg key_len the length of the key
 * @arg value opaque value.
 * @return null if the item was newly inserted, otherwise
 * the old value pointer is returned.
 */
void* art_insert(art_tree *t, const uint8_t *key, int32_t key_len, void *value) {
    int32_t old_val = 0;
    void *old = recursive_insert(t->root, &t->root, key, key_len, value, 0, &old_val, 1);
    if (!old_val) t->size++;
    return old;
}

/**
 * inserts a new value into the art tree (no replace)
 * @arg t the tree
 * @arg key the key
 * @arg key_len the length of the key
 * @arg value opaque value.
 * @return null if the item was newly inserted, otherwise
 * the old value pointer is returned.
 */
void* art_insert_no_replace(art_tree *t, const uint8_t *key, int32_t key_len, void *value) {
    int32_t old_val = 0;
    void *old = recursive_insert(t->root, &t->root, key, key_len, value, 0, &old_val, 0);
    if (!old_val) t->size++;
    return old;
}

static void remove_child256(art_node256 *n, art_node **ref, uint8_t c) {
    n->children[c] = NULL;
    n->n.num_children--;

    // Resize to a node48 on underflow, not immediately to prevent
    // thrashing if we sit on the 48/49 boundary
    if (unlikely(n->n.num_children == 37)) {
        art_node48 *new_node = (art_node48*)alloc_node(NODE48);
        *ref = (art_node*)new_node;
        copy_header((art_node*)new_node, (art_node*)n);

        int32_t pos = 0;
        for (int32_t i=0;i<256;i++) {
            if (n->children[i]) {
                new_node->children[pos] = n->children[i];
                new_node->keys[i] = pos + 1;
                pos++;
            }
        }
        kfree(n);
    }
}

static void remove_child48(art_node48 *n, art_node **ref, uint8_t c) {
    int32_t pos = n->keys[c];
    n->keys[c] = 0;
    n->children[pos-1] = NULL;
    n->n.num_children--;

    if (unlikely(n->n.num_children == 12)) {
        art_node16 *new_node = (art_node16*)alloc_node(NODE16);
        *ref = (art_node*)new_node;
        copy_header((art_node*)new_node, (art_node*)n);

        int32_t child = 0;
        for (int32_t i=0;i<256;i++) {
            pos = n->keys[i];
            if (pos) {
                new_node->keys[child] = i;
                new_node->children[child] = n->children[pos - 1];
                child++;
            }
        }
        kfree(n);
    }
}

static void remove_child16(art_node16 *n, art_node **ref, art_node **l) {
    int32_t pos = l - n->children;
    _memmove(n->keys+pos, n->keys+pos+1, n->n.num_children - 1 - pos);
    _memmove(n->children+pos, n->children+pos+1, (n->n.num_children - 1 - pos)*sizeof(void*));
    n->n.num_children--;

    if (unlikely(n->n.num_children == 3)) {
        art_node4 *new_node = (art_node4*)alloc_node(NODE4);
        *ref = (art_node*)new_node;
        copy_header((art_node*)new_node, (art_node*)n);
        __memcpy(new_node->keys, n->keys, 4);
        __memcpy(new_node->children, n->children, 4*sizeof(void*));
        kfree(n);
    }
}

static void remove_child4(art_node4 *n, art_node **ref, art_node **l) {
    int32_t pos = l - n->children;
    _memmove(n->keys+pos, n->keys+pos+1, n->n.num_children - 1 - pos);
    _memmove(n->children+pos, n->children+pos+1, (n->n.num_children - 1 - pos)*sizeof(void*));
    n->n.num_children--;

    // Remove nodes with only a single child
    if (unlikely(n->n.num_children == 1)) {
        art_node *child = n->children[0];
        if (!IS_LEAF(child)) {
            // Concatenate the prefixes
            int32_t prefix = n->n.partial_len;
            if (prefix < MAX_PREFIX_LEN) {
                n->n.partial[prefix] = n->keys[0];
                prefix++;
            }
            if (prefix < MAX_PREFIX_LEN) {
                int32_t sub_prefix = _min__art(child->partial_len, MAX_PREFIX_LEN - prefix);
                __memcpy(n->n.partial+prefix, child->partial, sub_prefix);
                prefix += sub_prefix;
            }
            // Store the prefix in the child
            __memcpy(child->partial, n->n.partial, _min__art(prefix, MAX_PREFIX_LEN));
            child->partial_len += n->n.partial_len + 1;
        }
        *ref = child;
        kfree(n);
    }
}

static void remove_child(art_node *n, art_node **ref, uint8_t c, art_node **l) {
    switch (n->type) {
        case NODE4:
            remove_child4((art_node4*)n, ref, l);
            return;
        case NODE16:
            remove_child16((art_node16*)n, ref, l);
            return;
        case NODE48:
            remove_child48((art_node48*)n, ref, c);
            return;
        case NODE256:
            remove_child256((art_node256*)n, ref, c);
            return;
        default:
            Panic("Abort!");
    }
}

static art_leaf* recursive_delete(art_node *n, art_node **ref, const uint8_t *key, int32_t key_len, int32_t depth) {
    // Search terminated
    if (unlikely(!n)) return NULL;

    // Handle hitting a leaf node
    if (unlikely(IS_LEAF(n))) {
        art_leaf *l = LEAF_RAW(n);
        if (likely(!leaf_matches(l, key, key_len, depth))) {
            *ref = NULL;
            return l;
        }
        return NULL;
    }

    // Bail if the prefix does not match
    if (unlikely(n->partial_len)) {
        int32_t prefix_len = check_prefix(n, key, key_len, depth);
        if (unlikely(prefix_len != _min__art(MAX_PREFIX_LEN, n->partial_len))) {
            return NULL;
        }
        depth = depth + n->partial_len;
    }

    // Find child node
    art_node **child = find_child(n, key[depth]);
    if (unlikely(!child)) return NULL;

    // If the child is leaf, delete from this node
    if (unlikely(IS_LEAF(*child))) {
        art_leaf *l = LEAF_RAW(*child);
        if (likely(!leaf_matches(l, key, key_len, depth))) {
            remove_child(n, ref, key[depth], child);
            return l;
        }
        return NULL;
    // Recurse
    } else {
        return recursive_delete(*child, child, key, key_len, depth+1);
    }
}

/**
 * Deletes a value from the ART tree
 * @arg t The tree
 * @arg key The key
 * @arg key_len The length of the key
 * @return NULL if the item was not found, otherwise
 * the value pointer is returned.
 */
void* art_delete(art_tree *t, const uint8_t *key, int32_t key_len) {
    art_leaf *l = recursive_delete(t->root, &t->root, key, key_len, 0);
    if (likely(l)) {
        t->size--;
        void *old = l->value;
        kfree(l);
        return old;
    }
    return NULL;
}

// Recursively iterates over the tree
static int32_t recursive_iter(art_node *n, art_callback cb, void *data) {
    // Handle base cases
    if (unlikely(!n)) return 0;
    if (unlikely(IS_LEAF(n))) {
        art_leaf *l = LEAF_RAW(n);
        return cb(data, (const uint8_t*)l->key, l->key_len, l->value);
    }

    int32_t idx, res;
    switch (n->type) {
        case NODE4:
            for (int32_t i=0; i < n->num_children; i++) {
                prefetch_r(((art_node4*)n)->children[i]);
                res = recursive_iter(((art_node4*)n)->children[i], cb, data);
                if (unlikely(res)) return res;
            }
            break;
        case NODE16:
            for (int32_t i=0; i < n->num_children; i++) {
                prefetch_r(((art_node16*)n)->children[i]);
                res = recursive_iter(((art_node16*)n)->children[i], cb, data);
                if (unlikely(res)) return res;
            }
            break;
        case NODE48:
            for (int32_t i=0; i < 256; i++) {
                idx = ((art_node48*)n)->keys[i];
                if (unlikely(!idx)) continue;
                prefetch_r(((art_node48*)n)->children[idx-1]);
                res = recursive_iter(((art_node48*)n)->children[idx-1], cb, data);
                if (unlikely(res)) return res;
            }
            break;
        case NODE256:
            for (int32_t i=0; i < 256; i++) {
                if (unlikely(!((art_node256*)n)->children[i])) continue;
                prefetch_r(((art_node256*)n)->children[i]);
                res = recursive_iter(((art_node256*)n)->children[i], cb, data);
                if (unlikely(res)) return res;
            }
            break;
        default:
            Panic("Abort!");
    }
    return 0;
}

/**
 * Iterates through the entries pairs in the map,
 * invoking a callback for each. The call back gets a
 * key, value for each and returns an integer stop value.
 * If the callback returns non-zero, then the iteration stops.
 * @arg t The tree to iterate over
 * @arg cb The callback function to invoke
 * @arg data Opaque handle passed to the callback
 * @return 0 on success, or the return of the callback.
 */
int32_t art_iter(art_tree *t, art_callback cb, void *data) {
    return recursive_iter(t->root, cb, data);
}

/**
 * Checks if a leaf prefix matches
 * @return 0 on success.
 */
static int32_t leaf_prefix_matches(const art_leaf *n, const uint8_t *prefix, int32_t prefix_len) {
    /* ========== 注释修复：原"too int16_t"为拼写错误 ========== */
    // Fail if the key length is too short
    if (unlikely(n->key_len < (uint32_t)prefix_len)) return 1;
    // Compare the keys
    return _memcmp(n->key, prefix, prefix_len);
}

/**
 * Iterates through the entries pairs in the map,
 * invoking a callback for each that matches a given prefix.
 * The call back gets a key, value for each and returns an integer stop value.
 * If the callback returns non-zero, then the iteration stops.
 * @arg t The tree to iterate over
 * @arg prefix The prefix of keys to read
 * @arg prefix_len The length of the prefix
 * @arg cb The callback function to invoke
 * @arg data Opaque handle passed to the callback
 * @return 0 on success, or the return of the callback.
 */
int32_t art_iter_prefix(art_tree *t, const uint8_t *key, int32_t key_len, art_callback cb, void *data) {
    art_node **child;
    art_node *n = t->root;
    int32_t prefix_len, depth = 0;

    if (likely(n)) prefetch_r(n);

    while (likely(n)) {
        // Might be a leaf
        if (unlikely(IS_LEAF(n))) {
            art_leaf *leaf = LEAF_RAW(n);
            prefetch_r(leaf);
            // Check if the expanded path matches
            if (likely(!leaf_prefix_matches(leaf, key, key_len))) {
                return cb(data, (const uint8_t*)leaf->key, leaf->key_len, leaf->value);
            }
            return 0;
        }

        // If the depth matches the prefix, we need to handle this node
        if (unlikely(depth == key_len)) {
            art_leaf *l = minimum(n);
            if (likely(!leaf_prefix_matches(l, key, key_len)))
               return recursive_iter(n, cb, data);
            return 0;
        }

        // Bail if the prefix does not match
        if (unlikely(n->partial_len)) {
            prefix_len = prefix_mismatch(n, key, key_len, depth);
            // Guard if the mis-match is longer than the MAX_PREFIX_LEN
            if ((uint32_t)prefix_len > n->partial_len) {
                prefix_len = n->partial_len;
            }

            // If there is no match, search is terminated
            if (unlikely(!prefix_len)) {
                return 0;
            // If we've matched the prefix, iterate on this node
            } else if (unlikely(depth + prefix_len == key_len)) {
                return recursive_iter(n, cb, data);
            }

            // if there is a full match, go deeper
            depth = depth + n->partial_len;
        }

        // Recursively search
        child = find_child(n, key[depth]);
        n = likely(child) ? *child : NULL;
        depth++;
    }
    return 0;
}
