/*-------------------------------------------------------------------------
 *
 * pg_utils/lists.cppm
 *
 * Iteration adapters over PostgreSQL's List type plus an intrusive
 * singly-linked-list adapter for the SubXid stack-of-frames pattern used
 * by the write-state caches. Lets call sites use range-based for loops
 * instead of hand-rolling ListCell or `frame->next` walks.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

#include <iterator>
#include <type_traits>

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

    [[nodiscard]] T* third() { return static_cast<T*>(list_nth(list, 2)); }
    [[nodiscard]] const T* third() const { return static_cast<T*>(list_nth(list, 2)); }
};

/*
 * Forward iterator over a PostgreSQL List* of integer values
 * (lfirst_int). Mirrors p_list_iterator but yields int rather than T*.
 */
export class p_list_int_iterator
{
private:
    ListCell* cell;
    const List* list;

public:
    using difference_type = std::ptrdiff_t;
    using value_type = int;

    p_list_int_iterator() : cell(nullptr), list(nullptr) {}
    explicit p_list_int_iterator(const List* target_list) : cell(list_head(target_list)), list(target_list) {}

    int operator*() const { return lfirst_int(cell); }

    p_list_int_iterator& operator++()
    {
        if (cell != nullptr && list != nullptr)
        {
            cell = lnext(list, cell);
        }
        return *this;
    }

    p_list_int_iterator operator++(int)
    {
        p_list_int_iterator snapshot{ *this };
        ++*this;
        return snapshot;
    }

    bool operator==(const p_list_int_iterator& other) const { return other.cell == cell; }
    bool operator==(const std::default_sentinel_t&) const { return cell == nullptr; }
};

/*
 * Range view over a PostgreSQL List* of int values; replaces the
 * `foreach_int(var, list)` macro with a typed range-based for loop.
 */
export class PIntList
{
private:
    const List* list;

public:
    explicit PIntList(const List* target_list) : list(target_list) {}

    [[nodiscard]] p_list_int_iterator begin() const { return p_list_int_iterator(list); }
    [[nodiscard]] p_list_int_iterator end() const { return p_list_int_iterator(); }
};

/*
 * Forward iterator over an intrusively linked list of Node*, where each
 * node exposes a `Node* next` member named identically to the template
 * parameter. Stops when next is nullptr; comparable against
 * std::default_sentinel_t.
 */
export template<typename Node>
class intrusive_list_iterator
{
private:
    Node* current;

public:
    using difference_type = std::ptrdiff_t;
    using value_type = Node*;

    intrusive_list_iterator() : current(nullptr) {}
    explicit intrusive_list_iterator(Node* head) : current(head) {}

    Node* operator*() const { return current; }

    intrusive_list_iterator& operator++()
    {
        if (current != nullptr)
        {
            current = current->next;
        }
        return *this;
    }

    intrusive_list_iterator operator++(int)
    {
        intrusive_list_iterator snapshot{ *this };
        ++*this;
        return snapshot;
    }

    bool operator==(const intrusive_list_iterator& other) const { return other.current == current; }
    bool operator==(const std::default_sentinel_t&) const { return current == nullptr; }
};

/*
 * Range view over an intrusively linked stack of Node frames whose tail
 * link is named `next`. Used to replace hand-rolled
 *
 *     for (Frame* head = stack; head != nullptr; head = head->next) { ... }
 *
 * walks with `for (auto* frame : IntrusiveList<Frame>(stack))`.
 */
export template<typename Node>
class IntrusiveList
{
private:
    Node* head;

public:
    explicit IntrusiveList(Node* list_head) : head(list_head) {}

    [[nodiscard]] intrusive_list_iterator<Node> begin() const { return intrusive_list_iterator<Node>(head); }
    [[nodiscard]] intrusive_list_iterator<Node> end() const { return intrusive_list_iterator<Node>(); }
    [[nodiscard]] bool empty() const { return head == nullptr; }
    [[nodiscard]] Node* front() const { return head; }
};
