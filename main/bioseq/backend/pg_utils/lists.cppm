/*-------------------------------------------------------------------------
 *
 * pg_utils/lists.cppm
 *
 * Iteration adapters over PostgreSQL's List type. Provides a forward
 * iterator (p_list_iterator<T>) and a range view (PList<T>) so that a
 * PostgreSQL List* of pointers can be traversed using C++ range-based
 * for loops without manually walking ListCells.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

#include <iterator>

export module pg_utils.lists;

/*
 * Forward iterator over a PostgreSQL List*, yielding T* for each ListCell.
 * The iterator is comparable against std::default_sentinel_t so a range-based
 * for loop terminates when the underlying cell reaches NULL.
 */
export template<typename T>
class p_list_iterator
{
private:
    ListCell* cell;
    const List* list;

public:
    using difference_type = std::ptrdiff_t;
    using value_type = T*;

    p_list_iterator() : cell(nullptr), list(nullptr) {}
    explicit p_list_iterator(const List* target_list) : cell(list_head(target_list)), list(target_list) {}

    T* operator*() const
    {
        return static_cast<T*>(lfirst(cell));
    }

    p_list_iterator& operator++()
    {
        if (cell != nullptr && list != nullptr)
        {
            cell = lnext(list, cell);
        }
        return *this;
    }

    p_list_iterator operator++(int)
    {
        p_list_iterator snapshot{ *this };
        ++*this;
        return snapshot;
    }

    bool operator==(const p_list_iterator& other) const
    {
        return other.cell == cell;
    }

    bool operator==(const std::default_sentinel_t&) const
    {
        return cell == nullptr;
    }
};

/*
 * Range view over a PostgreSQL List* of T* elements. Supports begin()/end(),
 * random access via operator[](size_t) and named accessors first()/second().
 */
export template <typename T>
class PList {
private:
    const List* list;

public:
    explicit PList(const List* target_list) : list(target_list) {}

    [[nodiscard]] p_list_iterator<T> begin() const { return p_list_iterator<T>(list); }
    [[nodiscard]] p_list_iterator<T> end() const { return p_list_iterator<T>(); }

    [[nodiscard]] const List* ptr() const { return list; }

    [[nodiscard]] T* operator[](size_t element_index) {
        return static_cast<T*>(list_nth(list, element_index));
    }
    [[nodiscard]] const T* operator[](size_t element_index) const {
        return static_cast<T*>(list_nth(list, element_index));
    }

    [[nodiscard]] T* first() { return static_cast<T*>(list_nth(list, 0)); }
    [[nodiscard]] const T* first() const { return static_cast<T*>(list_nth(list, 0)); }

    [[nodiscard]] T* second() { return static_cast<T*>(list_nth(list, 1)); }
    [[nodiscard]] const T* second() const { return static_cast<T*>(list_nth(list, 1)); }
};
