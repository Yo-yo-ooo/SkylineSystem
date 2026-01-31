#pragma once
#ifndef _KLIB_MAP_H_
#define _KLIB_MAP_H_

#include <stdint.h>
#include <stddef.h>
#include <klib/algorithm/pair.h>
#include <mem/heap.h>

enum COLOR
{
	BLACK,
	RED
};

template <class V>
struct RBTreeNode
{
	RBTreeNode<V>* _parent; //父节点
	RBTreeNode<V>* _left; //左孩子
	RBTreeNode<V>* _right; //右孩子
	V _val;
	COLOR _color; //颜色

	RBTreeNode(const V& val = V())
		:_parent(nullptr)
		, _left(nullptr)
		, _right(nullptr)
		, _val(val)
		, _color(RED)
	{}
};

template <class  V>
struct RBTreeIterator
{
	typedef RBTreeNode<V> Node;
	typedef RBTreeIterator<V> Self;
	Node* _node;

	RBTreeIterator(Node* node)
		:_node(node)
	{}

	//解引用
	V& operator*()
	{
		return _node->_val;
	}

	V* operator->()
	{
		return &_node->_val;
	}

	bool operator!=(const Self& it)
	{
		return _node != it._node;
	}

	Self& operator++()
	{
		if (_node->_right) //存在右节点
		{
			//右子树的最左结点
			_node = _node->_right;
			while (_node->_left)
			{
				_node = _node->_left;
			}
		}
		else //不存在右节点
		{
			Node* parent = _node->_parent;
			while (_node == parent->_right)//回溯
			{
				_node = parent;
				parent = parent->_parent;
			}
			//特殊情况：根节点没有右孩子，则不需要更新结点
			if (_node->_right != parent)
				_node = parent;
		}
		return *this;
	}

	Self& operator--()
	{
		if (_node->_left)
		{
			//右子树的最左结点
			_node = _node->_left;
			while (_node->_right)
			{
				_node = _node->_right;
			}
		}
		else
		{
			Node* parent = _node->_parent;
			while (_node == parent->_left)
			{
				_node = parent;
				parent = parent->_parent;
			}
			if (_node->_left != parent)
				_node = parent;
		}
		return *this;
	}
};

template <class K, class V, class KeyOfValue>
class RBTree
{
public:
	typedef RBTreeNode<V> Node;
	typedef RBTreeIterator<V> iterator;

	iterator begin() {
		return iterator(_header->_left);
	}
	iterator end() {
		return iterator(_header);
	}
	iterator rbegin() {
		return iterator(_header->_right);
	}

	RBTree()
		:_header(new Node)
	{
		//创建空树
		_header->_left = _header->_right = _header;
	}

	

	void RotateL(Node* parent)
	{
		Node* subR = parent->_right;
		Node* subRL = subR->_left;

		parent->_right = subRL;
		if (subRL)
			subRL->_parent = parent;
		if (parent == _header->_parent)
		{
			_header->_parent = subR;
			subR->_parent = _header;
		}
		else
		{
			Node* gfather = parent->_parent;
			if (gfather->_left == parent)
				gfather->_left = subR;
			else
				gfather->_right = subR;
			subR->_parent = gfather;
		}
		subR->_left = parent;
		parent->_parent = subR;
	}

	void RotateR(Node* parent)
	{
		Node* subL = parent->_left;
		Node* subLR = subL->_right;

		parent->_left = subLR;
		if (subLR)
			subLR->_parent = parent;

		if (parent == _header->_parent)
		{
			_header->_parent = subL;
			subL->_parent = _header;
		}
		else
		{
			Node* gfather = parent->_parent;
			if (gfather->_left == parent)
				gfather->_left = subL;
			else
				gfather->_right = subL;
			subL->_parent = gfather;
		}
		subL->_right = parent;
		parent->_parent = subL;
	}

	Node* leftMost(){
		Node* cur = _header->_parent;
		while (cur && cur->_left)
		{
			cur = cur->_left;
		}
		return cur;
	}

	Node* rightMost(){
		Node* cur = _header->_parent;
		while (cur && cur->_right)
		{
			cur = cur->_right;
		}
		return cur;
	}

	bool erase( K key) {
		if (_header->_parent == nullptr) {
			return false;
		}

		Node* cur = _header->_parent;
		Node* parent = nullptr;
		KeyOfValue kov;

		// 1. 查找要删除的节点
		while (cur) {
			if (kov(cur->_val) == key) {
				break;
			}
			parent = cur;
			if (kov(cur->_val) > key) {
				cur = cur->_left;
			}
			else {
				cur = cur->_right;
			}
		}

		// 没找到要删除的节点
		if (cur == nullptr) {
			return false;
		}

		// 2. 如果要删除的节点有两个孩子
		if (cur->_left != nullptr && cur->_right != nullptr) {
			// 找到后继节点（右子树的最小节点）
			Node* succ = cur->_right;
			Node* succParent = cur;
			while (succ->_left) {
				succParent = succ;
				succ = succ->_left;
			}

			// 用后继节点的值替换当前节点的值
			cur->_val = succ->_val;

			// 转为删除后继节点（后继节点最多有一个孩子）
			cur = succ;
			parent = succParent;
		}

		// 3. 现在cur最多只有一个孩子
		Node* child = (cur->_left != nullptr) ? cur->_left : cur->_right;

		// 保存要删除节点的颜色
		COLOR originalColor = cur->_color;
		bool isLeftChild = false;

		if (parent) {
			isLeftChild = (parent->_left == cur);
		}

		// 4. 用child替换cur
		if (child != nullptr) {
			child->_parent = parent;
		}

		if (parent == nullptr) { // cur是根节点
			_header->_parent = child;
			if (child != nullptr) {
				child->_parent = _header;
			}
		}
		else {
			if (isLeftChild) {
				parent->_left = child;
			}
			else {
				parent->_right = child;
			}
		}

		// 5. 如果删除的是黑色节点，需要调整
		if (originalColor == BLACK) {
			Node* x  = child;
			while (x != _header->_parent && (x == nullptr || x->_color == BLACK)) {
				Node* w = nullptr; // x的兄弟节点

				if (isLeftChild) {
					w = parent->_right;

					// 情况1：兄弟节点是红色
					if (w != nullptr && w->_color == RED) {
						w->_color = BLACK;
						parent->_color = RED;
						RotateL(parent);
						w = parent->_right;
					}

					// 情况2：兄弟节点是黑色，且兄弟的两个孩子都是黑色
					if ((w == nullptr || w->_left == nullptr || w->_left->_color == BLACK) &&
						(w == nullptr || w->_right == nullptr || w->_right->_color == BLACK)) {
						if (w != nullptr) {
							w->_color = RED;
						}
						x = parent;
						if (x == _header->_parent) {
							parent = nullptr;
						}
						else {
							parent = x->_parent;
							if (parent != nullptr) {
								isLeftChild = (parent->_left == x);
							}
						}
					}
					else {
						// 情况3：兄弟节点是黑色，兄弟的左孩子是红色，右孩子是黑色
						if (w != nullptr && (w->_right == nullptr || w->_right->_color == BLACK)) {
							if (w->_left != nullptr) {
								w->_left->_color = BLACK;
							}
							w->_color = RED;
							RotateR(w);
							w = parent->_right;
						}

						// 情况4：兄弟节点是黑色，兄弟的右孩子是红色
						if (w != nullptr) {
							w->_color = parent->_color;
							if (w->_right != nullptr) {
								w->_right->_color = BLACK;
							}
						}
						parent->_color = BLACK;
						RotateL(parent);
						x = _header->_parent;
						break;
					}
				}
				else { // 对称的情况：x是右孩子
					w = parent->_left;

					// 情况1：兄弟节点是红色
					if (w != nullptr && w->_color == RED) {
						w->_color = BLACK;
						parent->_color = RED;
						RotateR(parent);
						w = parent->_left;
					}

					// 情况2：兄弟节点是黑色，且兄弟的两个孩子都是黑色
					if ((w == nullptr || w->_left == nullptr || w->_left->_color == BLACK) &&
						(w == nullptr || w->_right == nullptr || w->_right->_color == BLACK)) {
						if (w != nullptr) {
							w->_color = RED;
						}
						x = parent;
						if (x == _header->_parent) {
							parent = nullptr;
						}
						else {
							parent = x->_parent;
							if (parent != nullptr) {
								isLeftChild = (parent->_left == x);
							}
						}
					}
					else {
						// 情况3：兄弟节点是黑色，兄弟的右孩子是红色，左孩子是黑色
						if (w != nullptr && (w->_left == nullptr || w->_left->_color == BLACK)) {
							if (w->_right != nullptr) {
								w->_right->_color = BLACK;
							}
							w->_color = RED;
							RotateL(w);
							w = parent->_left;
						}

						// 情况4：兄弟节点是黑色，兄弟的左孩子是红色
						if (w != nullptr) {
							w->_color = parent->_color;
							if (w->_left != nullptr) {
								w->_left->_color = BLACK;
							}
						}
						parent->_color = BLACK;
						RotateR(parent);
						x = _header->_parent;
						break;
					}
				}
			}

			if (x != nullptr) {
				x->_color = BLACK;
			}
		}

		// 6. 删除节点
		delete cur;

		// 7. 更新header的左右指针
		if (_header->_parent != nullptr) {
			_header->_left = leftMost();
			_header->_right = rightMost();
			_header->_parent->_color = BLACK; // 根节点必须是黑色
		}
		else {
			_header->_left = _header->_right = _header;
		}

		return true;
	}

	std_::pair<iterator, bool> insert(const V& val) {
		if (_header->_parent == nullptr){
			Node* root = new Node(val);

			_header->_parent = root;
			root->_parent = _header;
			_header->_left = _header->_right = root;

			//根节点为黑色
			root->_color = BLACK;
			return std_::make_pair(iterator(root), true);
		}

		Node* cur = _header->_parent;
		Node* parent = nullptr;

		KeyOfValue kov;
		//1.寻找到要插入的结点的位置
		while (cur){
			parent = cur;
			if (kov(cur->_val) == kov(val))
				return std_::make_pair(iterator(cur), false);
			else if (kov(cur->_val) > kov(val))
				cur = cur->_left;
			else
				cur = cur->_right;
		}
		//2.创建节点
		cur = new Node(val);
		Node* node = cur;
		if (kov(parent->_val) > kov(cur->_val))
			parent->_left = cur;
		else
			parent->_right = cur;
		cur->_parent = parent;

		//3.颜色的修改或者结构的调整
		while (cur != _header->_parent && cur->_parent->_color == RED)//不为根且存在连续红色，则需要调整
		{
			parent = cur->_parent;
			Node* gfather = parent->_parent;

			if (gfather->_left == parent)
			{
				Node* uncle = gfather->_right;
				//情况1.uncle存在且为红
				if (uncle && uncle->_color == RED)
				{
					parent->_color = uncle->_color = BLACK;
					gfather->_color = RED;
					//向上追溯
					cur = gfather;
				}
				else
				{
					if (parent->_right == cur)//情况3
					{
						RotateL(parent);
						//swap(cur, parent);
						RBTreeNode<V>* Tmp = cur;
						cur = parent;
						parent = Tmp;
					}
					//情况2.uncle不存在或者uncle为黑
					RotateR(gfather);
					parent->_color = BLACK;
					gfather->_color = RED;
					break;
				}
			}
			else
			{
				Node* uncle = gfather->_left;
				if (uncle && uncle->_color == RED)
				{
					parent->_color = uncle->_color = BLACK;
					gfather->_color = RED;
					//向上追溯
					cur = gfather;
				}
				else
				{
					if (parent->_left == cur)
					{
						RotateR(parent);
						//swap(cur, parent);
						RBTreeNode<V>* Tmp = cur;
						cur = parent;
						parent = Tmp;
					}

					RotateL(gfather);
					parent->_color = BLACK;
					gfather->_color = RED;
					break;
				}
			}
		}

		//根节点为黑色
		_header->_parent->_color = BLACK;
		//更新头结点的左右指向
		_header->_left = leftMost();
		_header->_right = rightMost();
		//return true;
		return std_::make_pair(iterator(node), true);
	}


private:
	Node* _header;
};

template<class K, class T>
class Map
{
	struct MapKeyOfValue
	{
		const K& operator()(const std_::pair<K, T>& val)
		{
			return val.first;
		}
	};
	
public:
	typedef typename RBTree<K, std_::pair<K, T>, MapKeyOfValue>::iterator iterator;

	iterator begin() {
		return _rbt.begin();
	}
	iterator end() {
		return _rbt.end();
	}
	iterator rbegin() {
		return _rbt.rbegin();
	}

	std_::pair<iterator, bool> insert(const std_::pair<K, T>& kv)
	{
		return _rbt.insert(kv);
	}

	void erase(K Key) {
		_rbt.erase(Key);
	}

	T& operator[](const K& key)
	{
		std_::pair<iterator, bool> ret = _rbt.insert(std_::make_pair(key, T()));
		//ret.first 迭代器
		//迭代器-> pair<k,v>对象
		//pair<k,v>->second，获得v
		return ret.first->second;
	}

private:
	typedef RBTree<K, std_::pair<K, T>, MapKeyOfValue> rbt;
	rbt _rbt;
};



template <class K>
class Set
{
	struct SetKeyOfValue
	{
		const K& operator()(const K& val)
		{
			return val;
		}
	};

public:
	bool insert(const K& val)
	{
		return _rbt.insert(val);
	}

private:
	typedef RBTree<K, K, SetKeyOfValue> rbt;
	rbt _rbt;
};




#endif