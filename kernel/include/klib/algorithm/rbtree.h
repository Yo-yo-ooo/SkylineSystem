#pragma once
#ifndef _RB_TREE_H_
#define _RB_TREE_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef container_of
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
#endif

// 红黑树节点颜色
#define RB_RED   0
#define RB_BLACK 1

// 红黑树节点结构（无侵入式）
typedef struct rb_node {
    struct rb_node *parent;
    struct rb_node *left;
    struct rb_node *right;
    uint8_t color;
} rb_node_t;

// 比较函数原型
// 返回值: < 0 表示 a < b, 0 表示 a == b, > 0 表示 a > b
typedef int (*rb_compare_t)(const rb_node_t *a, const rb_node_t *b);

// 辅助函数：初始化节点
static inline void rb_init_node(rb_node_t *node) {
    node->parent = nullptr;
    node->left = nullptr;
    node->right = nullptr;
    node->color = RB_RED; // 新插入的节点默认为红色
}

// 内部旋转：左旋
static inline void rb_rotate_left(rb_node_t *node, rb_node_t **root) {
    rb_node_t *right = node->right;
    node->right = right->left;
    if (right->left)
        right->left->parent = node;
    right->parent = node->parent;
    if (node->parent) {
        if (node == node->parent->left)
            node->parent->left = right;
        else
            node->parent->right = right;
    } else {
        *root = right;
    }
    right->left = node;
    node->parent = right;
}

// 内部旋转：右旋
static inline void rb_rotate_right(rb_node_t *node, rb_node_t **root) {
    rb_node_t *left = node->left;
    node->left = left->right;
    if (left->right)
        left->right->parent = node;
    left->parent = node->parent;
    if (node->parent) {
        if (node == node->parent->right)
            node->parent->right = left;
        else
            node->parent->left = left;
    } else {
        *root = left;
    }
    left->right = node;
    node->parent = left;
}

// 插入节点并修复红黑树性质
static inline void rb_insert(rb_node_t **root, rb_node_t *node, rb_compare_t cmp) {
    rb_init_node(node);
    rb_node_t *parent = nullptr;
    rb_node_t *current = *root;

    // 1. 标准二叉搜索树插入
    while (current) {
        parent = current;
        if (cmp(node, current) < 0)
            current = current->left;
        else
            current = current->right;
    }

    node->parent = parent;
    if (!parent) {
        *root = node;
    } else if (cmp(node, parent) < 0) {
        parent->left = node;
    } else {
        parent->right = node;
    }

    node->color = RB_RED;

    // 2. 修复红黑树平衡
    while (node != *root && node->parent->color == RB_RED) {
        rb_node_t *grandparent = node->parent->parent;
        if (node->parent == grandparent->left) {
            rb_node_t *uncle = grandparent->right;
            if (uncle && uncle->color == RB_RED) {
                // Case 1: 叔叔是红色 -> 重新着色
                node->parent->color = RB_BLACK;
                uncle->color = RB_BLACK;
                grandparent->color = RB_RED;
                node = grandparent;
            } else {
                if (node == node->parent->right) {
                    // Case 2: 叔叔是黑色，当前节点是右孩子 -> 左旋
                    node = node->parent;
                    rb_rotate_left(node, root);
                }
                // Case 3: 叔叔是黑色，当前节点是左孩子 -> 右旋并着色
                node->parent->color = RB_BLACK;
                grandparent->color = RB_RED;
                rb_rotate_right(grandparent, root);
            }
        } else {
            rb_node_t *uncle = grandparent->left;
            if (uncle && uncle->color == RB_RED) {
                node->parent->color = RB_BLACK;
                uncle->color = RB_BLACK;
                grandparent->color = RB_RED;
                node = grandparent;
            } else {
                if (node == node->parent->left) {
                    node = node->parent;
                    rb_rotate_right(node, root);
                }
                node->parent->color = RB_BLACK;
                grandparent->color = RB_RED;
                rb_rotate_left(grandparent, root);
            }
        }
    }
    (*root)->color = RB_BLACK; // 根节点永远为黑
}

// 查找节点
static inline rb_node_t *rb_search(rb_node_t *root, rb_node_t *key, rb_compare_t cmp) {
    rb_node_t *current = root;
    while (current) {
        int res = cmp(key, current);
        if (res < 0)
            current = current->left;
        else if (res > 0)
            current = current->right;
        else
            return current;
    }
    return nullptr;
}

// 获取树中最小节点（最左侧）
static inline rb_node_t *rb_first(rb_node_t *root) {
    if (!root) return nullptr;
    while (root->left)
        root = root->left;
    return root;
}

// 获取树中最大节点（最右侧）
static inline rb_node_t *rb_last(rb_node_t *root) {
    if (!root) return nullptr;
    while (root->right)
        root = root->right;
    return root;
}

// 获取后继节点（中序遍历的下一个）
static inline rb_node_t *rb_next(rb_node_t *node) {
    if (!node) return nullptr;
    if (node->right) {
        return rb_first(node->right);
    }
    rb_node_t *parent = node->parent;
    while (parent && node == parent->right) {
        node = parent;
        parent = parent->parent;
    }
    return parent;
}

// 删除节点后的修复操作 (内部使用)
static inline void rb_erase_fixup(rb_node_t **root, rb_node_t *node, rb_node_t *parent) {
    while (node != *root && (!node || node->color == RB_BLACK)) {
        if (node == parent->left) {
            rb_node_t *sibling = parent->right;
            if (sibling->color == RB_RED) {
                sibling->color = RB_BLACK;
                parent->color = RB_RED;
                rb_rotate_left(parent, root);
                sibling = parent->right;
            }
            if ((!sibling->left || sibling->left->color == RB_BLACK) &&
                (!sibling->right || sibling->right->color == RB_BLACK)) {
                sibling->color = RB_RED;
                node = parent;
                parent = node->parent;
            } else {
                if (!sibling->right || sibling->right->color == RB_BLACK) {
                    if (sibling->left) sibling->left->color = RB_BLACK;
                    sibling->color = RB_RED;
                    rb_rotate_right(sibling, root);
                    sibling = parent->right;
                }
                sibling->color = parent->color;
                parent->color = RB_BLACK;
                if (sibling->right) sibling->right->color = RB_BLACK;
                rb_rotate_left(parent, root);
                node = *root;
                break;
            }
        } else {
            rb_node_t *sibling = parent->left;
            if (sibling->color == RB_RED) {
                sibling->color = RB_BLACK;
                parent->color = RB_RED;
                rb_rotate_right(parent, root);
                sibling = parent->left;
            }
            if ((!sibling->right || sibling->right->color == RB_BLACK) &&
                (!sibling->left || sibling->left->color == RB_BLACK)) {
                sibling->color = RB_RED;
                node = parent;
                parent = node->parent;
            } else {
                if (!sibling->left || sibling->left->color == RB_BLACK) {
                    if (sibling->right) sibling->right->color = RB_BLACK;
                    sibling->color = RB_RED;
                    rb_rotate_left(sibling, root);
                    sibling = parent->left;
                }
                sibling->color = parent->color;
                parent->color = RB_BLACK;
                if (sibling->left) sibling->left->color = RB_BLACK;
                rb_rotate_right(parent, root);
                node = *root;
                break;
            }
        }
    }
    if (node) node->color = RB_BLACK;
}

// 从树中移除节点
static inline void rb_erase(rb_node_t **root, rb_node_t *node) {
    rb_node_t *child, *parent;
    uint8_t color;

    if (!node->left) {
        child = node->right;
    } else if (!node->right) {
        child = node->left;
    } else {
        // 有两个孩子，找后继节点替换
        rb_node_t *old = node;
        rb_node_t *left = node->right;
        while (left->left) left = left->left;
        
        color = left->color;
        child = left->right;
        parent = left->parent;

        if (parent == old) {
            parent = left;
        } else {
            if (child) child->parent = parent;
            parent->left = child;
            left->right = old->right;
            old->right->parent = left;
        }

        left->color = old->color;
        left->left = old->left;
        old->left->parent = left;

        if (old->parent) {
            if (old == old->parent->left)
                old->parent->left = left;
            else
                old->parent->right = left;
        } else {
            *root = left;
        }
        left->parent = old->parent;

        if (color == RB_BLACK) {
            rb_erase_fixup(root, child, parent);
        }
        return;
    }

    parent = node->parent;
    color = node->color;

    if (child) child->parent = parent;

    if (parent) {
        if (node == parent->left)
            parent->left = child;
        else
            parent->right = child;
    } else {
        *root = child;
    }

    if (color == RB_BLACK) {
        rb_erase_fixup(root, child, parent);
    }
}

#ifdef __cplusplus
}
#endif

#endif // _RB_TREE_H_