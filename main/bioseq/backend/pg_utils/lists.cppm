/*-------------------------------------------------------------------------
 *
 * pg_utils/lists.cppm
 *
 * Iteration adapters over PostgreSQL's List type plus an intrusive
 * singly-linked-list adapter for the SubXid stack-of-frames pattern used
 * by the write-state caches. Lets call sites use range-based for loops
 * instead of hand-rolling ListCell or `frame->next` walks.
 *
 * PList<T> is layout-compatible with a raw List* field (same size,
 * standard-layout) so it can be embedded directly into structs shared
 * with PostgreSQL's C code without changing the ABI footprint. See the
 * static_assert block at the bottom of the file for the invariants.
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
 *
 * Holds a non-const List* internally so mutating helpers (append, prepend,
 * clear) can update the stored pointer in place. The empty list (NIL) is
 * represented by a null pointer, matching the underlying PG convention.
 */
export template <typename T>
class PList {
private:
    List* list;

public:
    PList() : list(nullptr) {}
    /*
     * Implicit construction from a raw List* lets call sites that still
     * hand us a bare PG pointer keep compiling, and also lets
     * `some_struct.list_field = lappend(...)` continue to work when
     * `list_field` has type PList<T>.
     */
    PList(List* target_list) : list(target_list) {}

    [[nodiscard]] p_list_iterator<T> begin() const { return p_list_iterator<T>(list); }
    [[nodiscard]] p_list_iterator<T> end() const { return p_list_iterator<T>(); }

    /* Raw pointer accessor for passing through to the PG C API. */
    [[nodiscard]] List* ptr() const { return list; }

    /*
     * Implicit conversion to List* so legacy call sites that still hand
     * the field off to pull_var_clause / lappend / copyObject / ... keep
     * compiling. All such functions are NIL-safe.
     */
    [[nodiscard]] operator List*() const { return list; }

    /* Length in elements (list_length handles NIL as 0). */
    [[nodiscard]] int length() const { return list_length(list); }

    /* True when the list is NIL / contains no cells. */
    [[nodiscard]] bool empty() const { return list == nullptr; }

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

    /*
     * Appends the given element; updates the stored pointer because
     * lappend may reallocate the backing ListCell array. Returns *this
     * so call sites can chain.
     */
    PList& append(T* element)
    {
        list = lappend(list, element);
        return *this;
    }

    /* Clears the list via list_free and resets the stored pointer to NIL. */
    void clear()
    {
        list_free(list);
        list = nullptr;
    }

    /* Releases ownership of the raw pointer, resetting to NIL. */
    [[nodiscard]] List* release()
    {
        List* released = list;
        list = nullptr;
        return released;
    }
};

/*
 * Layout invariants: PList<T> is embedded into state structs shared with
 * PostgreSQL C code. A single List* field is what the original struct
 * member was; PList<T> must be the same size/alignment and
 * standard-layout so embedding does not change the struct's ABI.
 */
static_assert(sizeof(PList<void>) == sizeof(List*),
              "PList<T> must match the memory footprint of List*");
static_assert(alignof(PList<void>) == alignof(List*),
              "PList<T> must match the alignment of List*");
static_assert(std::is_standard_layout_v<PList<void>>,
              "PList<T> must remain standard-layout so embedding inside ABI-shared structs stays safe");

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
 *
 * Also standard-layout and pointer-sized so it can be embedded directly
 * into structs that expose an int-valued List* field.
 */
export class PIntList
{
private:
    List* list;

public:
    PIntList() : list(nullptr) {}
    PIntList(List* target_list) : list(target_list) {}

    [[nodiscard]] p_list_int_iterator begin() const { return p_list_int_iterator(list); }
    [[nodiscard]] p_list_int_iterator end() const { return p_list_int_iterator(); }

    [[nodiscard]] List* ptr() const { return list; }
    [[nodiscard]] operator List*() const { return list; }
    [[nodiscard]] int length() const { return list_length(list); }
    [[nodiscard]] bool empty() const { return list == nullptr; }

    PIntList& append(int element)
    {
        list = lappend_int(list, element);
        return *this;
    }

    void clear()
    {
        list_free(list);
        list = nullptr;
    }

    [[nodiscard]] List* release()
    {
        List* released = list;
        list = nullptr;
        return released;
    }
};

static_assert(sizeof(PIntList) == sizeof(List*),
              "PIntList must match the memory footprint of List*");
static_assert(alignof(PIntList) == alignof(List*),
              "PIntList must match the alignment of List*");
static_assert(std::is_standard_layout_v<PIntList>,
              "PIntList must remain standard-layout so embedding inside ABI-shared structs stays safe");

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
