/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include "raven_rtl.h"

/* The generic AVL tables, laid out as the Windows DDK documents RTL_AVL_TABLE: the tree hangs off BalancedRoot's right link and each element follows its
 * RTL_BALANCED_LINKS node. */

#define ROOT_OF(table) ((table)->BalancedRoot.RightChild)
#define DATA_OF(node) ((PVOID)((PRTL_BALANCED_LINKS)(node) + 1))
#define LEFT_HEAVY (-1)
#define RIGHT_HEAVY 1

/* Walks to the element or the place it would go, as the *Full functions report it. */
static TABLE_SEARCH_RESULT find_node(PRTL_AVL_TABLE table, PVOID key, PRTL_BALANCED_LINKS *place)
{
	PRTL_BALANCED_LINKS node = ROOT_OF(table);
	*place = NULL;
	if (!node)
		return TableEmptyTree;
	for (;;) {
		RTL_GENERIC_COMPARE_RESULTS order = table->CompareRoutine(table, key, DATA_OF(node));
		*place = node;
		if (order == GenericEqual)
			return TableFoundNode;
		PRTL_BALANCED_LINKS next = order == GenericLessThan ? node->LeftChild : node->RightChild;
		if (!next)
			return order == GenericLessThan ? TableInsertAsLeft : TableInsertAsRight;
		node = next;
	}
}

static void replace_child(PRTL_BALANCED_LINKS parent, PRTL_BALANCED_LINKS old_child, PRTL_BALANCED_LINKS new_child)
{
	if (parent->RightChild == old_child)
		parent->RightChild = new_child;
	else
		parent->LeftChild = new_child;
	if (new_child)
		new_child->Parent = parent;
}

/* Rotates so `high`'s child takes its place; balances are the caller's. */
static PRTL_BALANCED_LINKS rotate(PRTL_BALANCED_LINKS high, BOOLEAN to_left)
{
	PRTL_BALANCED_LINKS parent = high->Parent;
	PRTL_BALANCED_LINKS low = to_left ? high->RightChild : high->LeftChild;
	PRTL_BALANCED_LINKS moved = to_left ? low->LeftChild : low->RightChild;
	if (to_left) {
		high->RightChild = moved;
		low->LeftChild = high;
	} else {
		high->LeftChild = moved;
		low->RightChild = high;
	}
	if (moved)
		moved->Parent = high;
	high->Parent = low;
	low->Parent = parent;
	if (parent->RightChild == high)
		parent->RightChild = low;
	else
		parent->LeftChild = low;
	return low;
}

/* Restores balance at a node leaning two ways too far; says whether the subtree got shorter, which deletion keeps climbing for. */
static BOOLEAN rebalance(PRTL_BALANCED_LINKS node, CHAR side)
{
	PRTL_BALANCED_LINKS child = side == RIGHT_HEAVY ? node->RightChild : node->LeftChild;
	if (child->Balance == -side) {
		PRTL_BALANCED_LINKS grand = side == RIGHT_HEAVY ? child->LeftChild : child->RightChild;
		rotate(child, side != RIGHT_HEAVY);
		rotate(node, side == RIGHT_HEAVY);
		node->Balance = grand->Balance == side ? -side : 0;
		child->Balance = grand->Balance == -side ? side : 0;
		grand->Balance = 0;
		return TRUE;
	}
	rotate(node, side == RIGHT_HEAVY);
	if (child->Balance) {
		child->Balance = 0;
		node->Balance = 0;
		return TRUE;
	}
	child->Balance = -side;
	node->Balance = side;
	return FALSE;
}

static void climb_after_insert(PRTL_AVL_TABLE table, PRTL_BALANCED_LINKS node)
{
	PRTL_BALANCED_LINKS parent = node->Parent;
	PRTL_BALANCED_LINKS root = &table->BalancedRoot;
	while (parent != root) {
		CHAR side = parent->RightChild == node ? RIGHT_HEAVY : LEFT_HEAVY;
		if (parent->Balance == -side) {
			parent->Balance = 0;
			return;
		}
		if (parent->Balance == side) {
			rebalance(parent, side);
			return;
		}
		parent->Balance = side;
		node = parent;
		parent = node->Parent;
	}
	table->DepthOfTree++;
}

VOID NTAPI RtlInitializeGenericTableAvl(PRTL_AVL_TABLE table, PRTL_AVL_COMPARE_ROUTINE compare, PRTL_AVL_ALLOCATE_ROUTINE allocate, PRTL_AVL_FREE_ROUTINE release, PVOID context)
{
	RtlZeroMemory(table, sizeof(*table));
	table->BalancedRoot.Parent = &table->BalancedRoot;
	table->CompareRoutine = compare;
	table->AllocateRoutine = allocate;
	table->FreeRoutine = release;
	table->TableContext = context;
}

PVOID NTAPI RtlInsertElementGenericTableFullAvl(PRTL_AVL_TABLE table, PVOID buffer, ULONG size, PBOOLEAN new_element, PVOID where, TABLE_SEARCH_RESULT result)
{
	PRTL_BALANCED_LINKS parent = where;
	if (new_element)
		*new_element = FALSE;
	if (result == TableFoundNode)
		return DATA_OF(parent);
	PRTL_BALANCED_LINKS node = table->AllocateRoutine(table, (ULONG)sizeof(RTL_BALANCED_LINKS) + size);
	if (!node)
		return NULL;
	RtlZeroMemory(node, sizeof(*node));
	if (result == TableEmptyTree) {
		ROOT_OF(table) = node;
		node->Parent = &table->BalancedRoot;
		table->DepthOfTree = 1;
	} else {
		if (result == TableInsertAsLeft)
			parent->LeftChild = node;
		else
			parent->RightChild = node;
		node->Parent = parent;
		climb_after_insert(table, node);
	}
	table->NumberGenericTableElements++;
	/* Numbered access starts over: the cached position may have moved. */
	table->OrderedPointer = NULL;
	table->WhichOrderedElement = 0;
	RtlCopyMemory(DATA_OF(node), buffer, size);
	if (new_element)
		*new_element = TRUE;
	return DATA_OF(node);
}

PVOID NTAPI RtlInsertElementGenericTableAvl(PRTL_AVL_TABLE table, PVOID buffer, ULONG size, PBOOLEAN new_element)
{
	PRTL_BALANCED_LINKS place;
	TABLE_SEARCH_RESULT found = find_node(table, buffer, &place);
	return RtlInsertElementGenericTableFullAvl(table, buffer, size, new_element, place, found);
}

BOOLEAN NTAPI RtlIsGenericTableEmptyAvl(PRTL_AVL_TABLE table)
{
	return !table->NumberGenericTableElements;
}

ULONG NTAPI RtlNumberGenericTableElementsAvl(PRTL_AVL_TABLE table)
{
	return table->NumberGenericTableElements;
}

PVOID NTAPI RtlLookupElementGenericTableFullAvl(PRTL_AVL_TABLE table, PVOID buffer, PVOID *where, TABLE_SEARCH_RESULT *result)
{
	PRTL_BALANCED_LINKS place;
	*result = find_node(table, buffer, &place);
	*where = place;
	return *result == TableFoundNode ? DATA_OF(place) : NULL;
}

PVOID NTAPI RtlLookupElementGenericTableAvl(PRTL_AVL_TABLE table, PVOID buffer)
{
	PRTL_BALANCED_LINKS place;
	return find_node(table, buffer, &place) == TableFoundNode ? DATA_OF(place) : NULL;
}

static PRTL_BALANCED_LINKS first_node(PRTL_AVL_TABLE table)
{
	PRTL_BALANCED_LINKS node = ROOT_OF(table);
	while (node && node->LeftChild)
		node = node->LeftChild;
	return node;
}

static PRTL_BALANCED_LINKS node_after(PRTL_AVL_TABLE table, PRTL_BALANCED_LINKS node)
{
	if (node->RightChild) {
		node = node->RightChild;
		while (node->LeftChild)
			node = node->LeftChild;
		return node;
	}
	PRTL_BALANCED_LINKS parent = node->Parent;
	while (parent != &table->BalancedRoot && parent->RightChild == node) {
		node = parent;
		parent = node->Parent;
	}
	return parent == &table->BalancedRoot ? NULL : parent;
}

PVOID NTAPI RtlEnumerateGenericTableWithoutSplayingAvl(PRTL_AVL_TABLE table, PVOID *restart_key)
{
	PRTL_BALANCED_LINKS node = *restart_key ? node_after(table, (PRTL_BALANCED_LINKS)*restart_key) : first_node(table);
	*restart_key = node;
	return node ? DATA_OF(node) : NULL;
}

PVOID NTAPI RtlEnumerateGenericTableAvl(PRTL_AVL_TABLE table, BOOLEAN restart)
{
	if (restart)
		table->RestartKey = NULL;
	return RtlEnumerateGenericTableWithoutSplayingAvl(table, (PVOID *)&table->RestartKey);
}

PVOID NTAPI RtlLookupFirstMatchingElementGenericTableAvl(PRTL_AVL_TABLE table, PVOID buffer, PVOID *restart_key)
{
	PRTL_BALANCED_LINKS place;
	if (find_node(table, buffer, &place) != TableFoundNode) {
		*restart_key = NULL;
		return NULL;
	}
	*restart_key = place;
	return DATA_OF(place);
}

PVOID NTAPI RtlGetElementGenericTableAvl(PRTL_AVL_TABLE table, ULONG index)
{
	ULONG position = index + 1;
	if (index == 0xffffffffu || position > table->NumberGenericTableElements)
		return NULL;
	PRTL_BALANCED_LINKS node;
	ULONG at;
	if (table->OrderedPointer && table->WhichOrderedElement && table->WhichOrderedElement <= position) {
		node = table->OrderedPointer;
		at = table->WhichOrderedElement;
	} else {
		node = first_node(table);
		at = 1;
	}
	while (at < position) {
		node = node_after(table, node);
		at++;
	}
	table->OrderedPointer = node;
	table->WhichOrderedElement = position;
	return DATA_OF(node);
}

BOOLEAN NTAPI RtlDeleteElementGenericTableAvl(PRTL_AVL_TABLE table, PVOID buffer)
{
	PRTL_BALANCED_LINKS node;
	if (find_node(table, buffer, &node) != TableFoundNode)
		return FALSE;
	if (table->RestartKey == node)
		table->RestartKey = NULL;

	/* A node with two children changes place with its successor, which has at most one; the element keeps its own node, as callers hold pointers to it. */
	if (node->LeftChild && node->RightChild) {
		PRTL_BALANCED_LINKS successor = node->RightChild;
		while (successor->LeftChild)
			successor = successor->LeftChild;
		PRTL_BALANCED_LINKS node_parent = node->Parent, node_left = node->LeftChild, node_right = node->RightChild;
		PRTL_BALANCED_LINKS successor_parent = successor->Parent, successor_right = successor->RightChild;
		CHAR node_balance = node->Balance;
		node->Balance = successor->Balance;
		successor->Balance = node_balance;
		replace_child(node_parent, node, successor);
		successor->LeftChild = node_left;
		node_left->Parent = successor;
		if (successor_parent == node) {
			successor->RightChild = node;
			node->Parent = successor;
		} else {
			successor->RightChild = node_right;
			node_right->Parent = successor;
			successor_parent->LeftChild = node;
			node->Parent = successor_parent;
		}
		node->LeftChild = NULL;
		node->RightChild = successor_right;
		if (successor_right)
			successor_right->Parent = node;
	}

	PRTL_BALANCED_LINKS parent = node->Parent, child = node->LeftChild ? node->LeftChild : node->RightChild;
	CHAR removed_side = parent->RightChild == node ? RIGHT_HEAVY : LEFT_HEAVY;
	replace_child(parent, node, child);
	PRTL_BALANCED_LINKS root = &table->BalancedRoot;
	/* Climbs while the subtree it just left got shorter. */
	while (parent != root) {
		PRTL_BALANCED_LINKS above = parent->Parent;
		CHAR side_above = above->RightChild == parent ? RIGHT_HEAVY : LEFT_HEAVY;
		if (parent->Balance == removed_side) {
			parent->Balance = 0;
		} else if (!parent->Balance) {
			parent->Balance = -removed_side;
			break;
		} else if (!rebalance(parent, -removed_side)) {
			break;
		}
		parent = above;
		removed_side = side_above;
	}
	if (parent == root)
		table->DepthOfTree--;
	table->NumberGenericTableElements--;
	table->DeleteCount++;
	table->OrderedPointer = NULL;
	table->WhichOrderedElement = 0;
	table->FreeRoutine(table, node);
	return TRUE;
}
