#include <iterator>

template<typename T>
static inline T* make() {
	return static_cast<T*>(palloc0(sizeof(T)));
}

template<typename T>
static inline T* make(uint32 size) {
    return static_cast<T*>(palloc0(size));
}



template<typename T>
class p_list_iterator
{
private:
    ListCell* cell;
    const List* l;
public:
    using difference_type = std::ptrdiff_t;
    using value_type = T*;

    p_list_iterator() : l(NULL), cell(NULL) {}
    p_list_iterator(const List* arg_l) : l(arg_l), cell(list_head(arg_l)) {}

    T* operator*() const
    {
        return static_cast<T*>(lfirst(cell));
    }

    p_list_iterator& operator++()
    {
        if (cell != NULL && l != NULL) {
            cell = lnext(l, cell);
        }
        return *this;
    }

    p_list_iterator operator++(int)
    {
        auto tmp{ *this };
        ++*this;
        return tmp;
    }

    bool operator==(const p_list_iterator& rhs) const
    {
        return rhs.cell == cell;
    }

    bool operator==(const std::default_sentinel_t&) const
    {
        return cell == NULL;
    }

    // p_list_iterator begin() const
    // {
    //     return *this;
    // }

    // std::default_sentinel_t end() const
    // {
    //     return {};
    // }
};

template <typename T>
class PList {
private:
    const List* list;
public:
    PList(const List* arg_list) : list(arg_list) {};

    p_list_iterator<T> begin() const;
    p_list_iterator<T> end() const;

    const List* ptr() const { return list; }

    T* operator[](size_t n) {
        return static_cast<T*>(list_nth(list, n));
    }
    const T* operator[](size_t n) const {
        return static_cast<T*>(list_nth(list, n));
    }

    T* first() { return static_cast<T*>(list_nth(list, 0)); }
    const T* first() const { return static_cast<T*>(list_nth(list, 0)); }

    T* second() { return static_cast<T*>(list_nth(list, 1)); }
    const T* second() const { return static_cast<T*>(list_nth(list, 1)); }

    template <int N> static PList<T> nulls();

    template <> static PList<T> nulls<1>() { return PList<T>(list_make1(NULL)); }
    template <> static PList<T> nulls<2>() { return PList<T>(list_make2(NULL, NULL)); }
    template <> static PList<T> nulls<3>() { return PList<T>(list_make3(NULL, NULL, NULL)); }
    template <> static PList<T> nulls<4>() { return PList<T>(list_make4(NULL, NULL, NULL, NULL)); }
    template <> static PList<T> nulls<5>() { return PList<T>(list_make5(NULL, NULL, NULL, NULL, NULL)); }
};

template <typename T>
p_list_iterator<T> PList<T>::begin() const {
    return p_list_iterator<T>(list);
}
template <typename T>
p_list_iterator<T> PList<T>::end() const {
    return p_list_iterator<T>();
}
