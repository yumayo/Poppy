#pragma once
#include <windows.h>
#include "tree_model.h"

namespace poppy {
inline constexpr wchar_t treeClassName[] = L"PoppyTree";
// Scalar-only messages also allow out-of-process GUI tests to inspect and
// select items without passing pointers across process boundaries.
enum class TreeRelation { Root, Child, Next, Parent };
constexpr UINT treeGetNext = WM_USER + 1, treeGetCount = WM_USER + 2,
    treeGetExpanded = WM_USER + 3, treeSelect = WM_USER + 4,
    treeGetSelection = WM_USER + 5, treeGetRow = WM_USER + 6,
    treeGetRowHeight = WM_USER + 7;
}
