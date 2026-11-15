#pragma once

#include <utility>
#include <vector>

namespace NQdb {

// Combines `items` pairwise, level by level, into a balanced binary tree that keeps
// their order. A left-deep fold of a long list is as deep as the list, and the
// recursive passes over such a tree overflow the stack. Two or three items fold
// as a left-deep chain does.
template <typename T, typename TCombine>
T FoldBalanced(std::vector<T> items, TCombine combine) {
    if (items.empty()) {
        return T{};
    }
    while (items.size() > 1) {
        std::vector<T> next;
        next.reserve((items.size() + 1) / 2);
        for (size_t i = 0; i + 1 < items.size(); i += 2) {
            next.push_back(combine(std::move(items[i]), std::move(items[i + 1])));
        }
        if (items.size() % 2 == 1) {
            next.push_back(std::move(items.back()));
        }
        items = std::move(next);
    }
    return std::move(items.front());
}

} // namespace NQdb
