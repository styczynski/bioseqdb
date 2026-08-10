/**
 * @file common/postgres/support/lists.cppm
 * @brief Pointer-sized range adapters for PostgreSQL and intrusive lists.
 */
module;

extern "C" {
#include "postgres.h"

#include "nodes/pg_list.h"
}

#include <cstddef>
#include <iterator>
#include <type_traits>

export module bioseq.postgres.support:lists;

export namespace bioseq::postgres::support {

/** Forward iterator over pointer elements stored in a PostgreSQL List. */
template <class p_element> class postgres_list_iterator final {
  public:
    using difference_type = std::ptrdiff_t;
    using value_type = p_element*;

    /** Constructs the end iterator. */
    constexpr postgres_list_iterator() noexcept = default;

    /** Starts iteration at the first cell of @p source_list. */
    explicit postgres_list_iterator(const List* source_list) noexcept
        : cell_(list_head(source_list)), list_(source_list) {}

    /** Returns the pointer stored in the current PostgreSQL list cell. */
    [[nodiscard]] p_element* operator*() const noexcept {
        return static_cast<p_element*>(lfirst(cell_));
    }

    /** Advances to the next list cell. */
    postgres_list_iterator& operator++() noexcept {
        if (cell_ != nullptr && list_ != nullptr) {
            cell_ = lnext(list_, cell_);
        }
        return *this;
    }

    /** Advances and returns the iterator's previous position. */
    postgres_list_iterator operator++(int) noexcept {
        postgres_list_iterator previous{*this};
        ++*this;
        return previous;
    }

    /** Compares two iterators by their current PostgreSQL list cell. */
    [[nodiscard]] bool operator==(const postgres_list_iterator& other) const noexcept {
        return cell_ == other.cell_;
    }

    /** Returns whether this iterator has reached the range sentinel. */
    [[nodiscard]] bool operator==(const std::default_sentinel_t&) const noexcept {
        return cell_ == nullptr;
    }

  private:
    ListCell* cell_ = nullptr;
    const List* list_ = nullptr;
};

/**
 * Pointer-sized mutable view over a PostgreSQL List of pointers.
 *
 * @tparam p_element Pointee type stored in each PostgreSQL list cell.
 * The view never frees automatically; explicit mutation methods mirror the
 * ownership conventions of PostgreSQL's List API.
 */
template <class p_element> class postgres_list final {
  public:
    /** Constructs an empty PostgreSQL list view. */
    constexpr postgres_list() noexcept = default;

    /** Wraps @p source_list without copying or taking automatic ownership. */
    explicit constexpr postgres_list(List* source_list) noexcept : list_(source_list) {}

    /** Returns an iterator positioned at the first cell. */
    [[nodiscard]] postgres_list_iterator<p_element> begin() const noexcept {
        return postgres_list_iterator<p_element>{list_};
    }

    /** Returns the range sentinel iterator. */
    [[nodiscard]] postgres_list_iterator<p_element> end() const noexcept {
        return {};
    }

    /** Returns the wrapped PostgreSQL List pointer. */
    [[nodiscard]] List* get() const noexcept {
        return list_;
    }

    /** Returns the number of cells; PostgreSQL treats NIL as size zero. */
    [[nodiscard]] int size() const noexcept {
        return list_length(list_);
    }

    /** Returns whether the wrapped list is PostgreSQL NIL. */
    [[nodiscard]] bool is_empty() const noexcept {
        return list_ == nullptr;
    }

    /** Returns the pointer stored at @p element_index. */
    [[nodiscard]] p_element* operator[](std::size_t element_index) {
        return static_cast<p_element*>(list_nth(list_, element_index));
    }

    /** Returns the const pointer stored at @p element_index. */
    [[nodiscard]] const p_element* operator[](std::size_t element_index) const {
        return static_cast<p_element*>(list_nth(list_, element_index));
    }

    /** Returns the first pointer; the list must be non-empty. */
    [[nodiscard]] p_element* get_first() {
        return static_cast<p_element*>(list_nth(list_, 0));
    }

    /** Returns the first const pointer; the list must be non-empty. */
    [[nodiscard]] const p_element* get_first() const {
        return static_cast<p_element*>(list_nth(list_, 0));
    }

    /** Returns the second pointer; the list must contain two cells. */
    [[nodiscard]] p_element* get_second() {
        return static_cast<p_element*>(list_nth(list_, 1));
    }

    /** Returns the second const pointer; the list must contain two cells. */
    [[nodiscard]] const p_element* get_second() const {
        return static_cast<p_element*>(list_nth(list_, 1));
    }

    /** Returns the third pointer; the list must contain three cells. */
    [[nodiscard]] p_element* get_third() {
        return static_cast<p_element*>(list_nth(list_, 2));
    }

    /** Returns the third const pointer; the list must contain three cells. */
    [[nodiscard]] const p_element* get_third() const {
        return static_cast<p_element*>(list_nth(list_, 2));
    }

    /** Returns the last pointer; the list must be non-empty. */
    [[nodiscard]] p_element* get_last() {
        return static_cast<p_element*>(llast(list_));
    }

    /** Returns the last const pointer; the list must be non-empty. */
    [[nodiscard]] const p_element* get_last() const {
        return static_cast<p_element*>(llast(list_));
    }

    /** Appends @p element and retains PostgreSQL's possibly new list head. */
    postgres_list& append(p_element* element) {
        list_ = lappend(list_, element);
        return *this;
    }

    /** Prepends @p element and retains PostgreSQL's possibly new list head. */
    postgres_list& prepend(p_element* element) {
        list_ = lcons(element, list_);
        return *this;
    }

    /** Appends the cells of @p other, consuming them per list_concat. */
    postgres_list& concatenate(List* other) {
        list_ = list_concat(list_, other);
        return *this;
    }

    /** Appends the cells viewed by @p other per PostgreSQL list_concat. */
    postgres_list& concatenate(const postgres_list& other) {
        list_ = list_concat(list_, other.list_);
        return *this;
    }

    /** Returns a copied concatenation without modifying either input list. */
    [[nodiscard]] postgres_list copy_concatenated_with(List* other) const {
        return postgres_list{list_concat_copy(list_, other)};
    }

    /** Returns a copied concatenation without modifying either input view. */
    [[nodiscard]] postgres_list copy_concatenated_with(const postgres_list& other) const {
        return postgres_list{list_concat_copy(list_, other.list_)};
    }

    /** Returns the pointer-equality intersection with @p other. */
    [[nodiscard]] postgres_list make_intersection(const postgres_list& other) const {
        return postgres_list{list_intersection(list_, other.list_)};
    }

    /** Returns elements absent from @p other under pointer equality. */
    [[nodiscard]] postgres_list make_difference(const postgres_list& other) const {
        return postgres_list{list_difference(list_, other.list_)};
    }

    /** Removes and returns the first pointer; the list must be non-empty. */
    [[nodiscard]] p_element* pop_front() {
        p_element* first = get_first();
        list_ = list_delete_first(list_);
        return first;
    }

    /** Returns whether @p element occurs under pointer equality. */
    [[nodiscard]] bool has_element(const p_element* element) const {
        return list_member_ptr(list_, element);
    }

    /** Creates a one-cell PostgreSQL list containing @p element. */
    [[nodiscard]] static postgres_list make_singleton(p_element* element) {
        return postgres_list{list_make1(element)};
    }

    /** Frees all cells and resets this view to PostgreSQL NIL. */
    void clear() noexcept {
        list_free(list_);
        list_ = nullptr;
    }

    /** Releases the wrapped pointer and resets this view to NIL. */
    [[nodiscard]] List* release() noexcept {
        List* released = list_;
        list_ = nullptr;
        return released;
    }

  private:
    List* list_ = nullptr;
};

static_assert(sizeof(postgres_list<void>) == sizeof(List*));
static_assert(alignof(postgres_list<void>) == alignof(List*));
static_assert(std::is_standard_layout_v<postgres_list<void>>);

/** Forward iterator over integer values stored in a PostgreSQL List. */
class postgres_integer_list_iterator final {
  public:
    using difference_type = std::ptrdiff_t;
    using value_type = int;

    /** Constructs the end iterator. */
    constexpr postgres_integer_list_iterator() noexcept = default;

    /** Starts iteration at the first cell of @p source_list. */
    explicit postgres_integer_list_iterator(const List* source_list) noexcept
        : cell_(list_head(source_list)), list_(source_list) {}

    /** Returns the integer stored in the current cell. */
    [[nodiscard]] int operator*() const noexcept {
        return lfirst_int(cell_);
    }

    /** Advances to the next list cell. */
    postgres_integer_list_iterator& operator++() noexcept {
        if (cell_ != nullptr && list_ != nullptr) {
            cell_ = lnext(list_, cell_);
        }
        return *this;
    }

    /** Advances and returns the iterator's previous position. */
    postgres_integer_list_iterator operator++(int) noexcept {
        postgres_integer_list_iterator previous{*this};
        ++*this;
        return previous;
    }

    /** Compares two iterators by their current PostgreSQL list cell. */
    [[nodiscard]] bool operator==(const postgres_integer_list_iterator& other) const noexcept {
        return cell_ == other.cell_;
    }

    /** Returns whether this iterator has reached the range sentinel. */
    [[nodiscard]] bool operator==(const std::default_sentinel_t&) const noexcept {
        return cell_ == nullptr;
    }

  private:
    ListCell* cell_ = nullptr;
    const List* list_ = nullptr;
};

/** Pointer-sized mutable view over a PostgreSQL List of integers. */
class postgres_integer_list final {
  public:
    /** Constructs an empty integer-list view. */
    constexpr postgres_integer_list() noexcept = default;

    /** Wraps @p source_list without copying or automatic ownership. */
    explicit constexpr postgres_integer_list(List* source_list) noexcept : list_(source_list) {}

    /** Returns an iterator positioned at the first integer. */
    [[nodiscard]] postgres_integer_list_iterator begin() const noexcept {
        return postgres_integer_list_iterator{list_};
    }

    /** Returns the range sentinel iterator. */
    [[nodiscard]] postgres_integer_list_iterator end() const noexcept {
        return {};
    }

    /** Returns the wrapped PostgreSQL List pointer. */
    [[nodiscard]] List* get() const noexcept {
        return list_;
    }

    /** Returns the number of integer cells. */
    [[nodiscard]] int size() const noexcept {
        return list_length(list_);
    }

    /** Returns whether the wrapped list is PostgreSQL NIL. */
    [[nodiscard]] bool is_empty() const noexcept {
        return list_ == nullptr;
    }

    /** Appends @p element and retains PostgreSQL's possibly new list head. */
    postgres_integer_list& append(int element) {
        list_ = lappend_int(list_, element);
        return *this;
    }

    /** Appends and consumes cells from @p other per list_concat. */
    postgres_integer_list& concatenate(List* other) {
        list_ = list_concat(list_, other);
        return *this;
    }

    /** Appends cells viewed by @p other per PostgreSQL list_concat. */
    postgres_integer_list& concatenate(const postgres_integer_list& other) {
        list_ = list_concat(list_, other.list_);
        return *this;
    }

    /** Returns a copied concatenation without modifying either input list. */
    [[nodiscard]] postgres_integer_list copy_concatenated_with(List* other) const {
        return postgres_integer_list{list_concat_copy(list_, other)};
    }

    /** Returns a copied concatenation without modifying either input view. */
    [[nodiscard]] postgres_integer_list
    copy_concatenated_with(const postgres_integer_list& other) const {
        return postgres_integer_list{list_concat_copy(list_, other.list_)};
    }

    /** Returns the last integer; the list must be non-empty. */
    [[nodiscard]] int get_last() const noexcept {
        return llast_int(list_);
    }

    /** Returns whether @p value occurs in the list. */
    [[nodiscard]] bool has_value(int value) const noexcept {
        return list_member_int(list_, value);
    }

    /** Creates a one-cell PostgreSQL list containing @p value. */
    [[nodiscard]] static postgres_integer_list make_singleton(int value) {
        return postgres_integer_list{list_make1_int(value)};
    }

    /** Frees all cells and resets this view to PostgreSQL NIL. */
    void clear() noexcept {
        list_free(list_);
        list_ = nullptr;
    }

    /** Releases the wrapped pointer and resets this view to NIL. */
    [[nodiscard]] List* release() noexcept {
        List* released = list_;
        list_ = nullptr;
        return released;
    }

  private:
    List* list_ = nullptr;
};

static_assert(sizeof(postgres_integer_list) == sizeof(List*));
static_assert(alignof(postgres_integer_list) == alignof(List*));
static_assert(std::is_standard_layout_v<postgres_integer_list>);

/** Forward iterator over nodes linked through a public `next` member. */
template <class p_node> class intrusive_list_iterator final {
  public:
    using difference_type = std::ptrdiff_t;
    using value_type = p_node*;

    /** Constructs the end iterator. */
    constexpr intrusive_list_iterator() noexcept = default;

    /** Starts iteration at @p head. */
    explicit constexpr intrusive_list_iterator(p_node* head) noexcept : current_(head) {}

    /** Returns the current node. */
    [[nodiscard]] p_node* operator*() const noexcept {
        return current_;
    }

    /** Advances through the current node's `next` link. */
    intrusive_list_iterator& operator++() noexcept {
        if (current_ != nullptr) {
            current_ = current_->next;
        }
        return *this;
    }

    /** Advances and returns the iterator's previous position. */
    intrusive_list_iterator operator++(int) noexcept {
        intrusive_list_iterator previous{*this};
        ++*this;
        return previous;
    }

    /** Compares two iterators by their current node. */
    [[nodiscard]] bool operator==(const intrusive_list_iterator& other) const noexcept {
        return current_ == other.current_;
    }

    /** Returns whether this iterator has reached the range sentinel. */
    [[nodiscard]] bool operator==(const std::default_sentinel_t&) const noexcept {
        return current_ == nullptr;
    }

  private:
    p_node* current_ = nullptr;
};

/** Non-owning range view over nodes linked through a public `next` member. */
template <class p_node> class intrusive_list final {
  public:
    /** Wraps the intrusive list beginning at @p head. */
    explicit constexpr intrusive_list(p_node* head) noexcept : head_(head) {}

    /** Returns an iterator positioned at the head node. */
    [[nodiscard]] intrusive_list_iterator<p_node> begin() const noexcept {
        return intrusive_list_iterator<p_node>{head_};
    }

    /** Returns the range sentinel iterator. */
    [[nodiscard]] intrusive_list_iterator<p_node> end() const noexcept {
        return {};
    }

    /** Returns whether the list has no head node. */
    [[nodiscard]] bool is_empty() const noexcept {
        return head_ == nullptr;
    }

    /** Returns the head node, or null for an empty list. */
    [[nodiscard]] p_node* get_front() const noexcept {
        return head_;
    }

  private:
    p_node* head_ = nullptr;
};

} // namespace bioseq::postgres::support
