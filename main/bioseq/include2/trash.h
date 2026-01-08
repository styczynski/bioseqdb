#pragma once

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

/*
 * ListCellAndListWrapper stores a list and list cell.
 * This struct is used for functionContext. When iterating a list
 * in separate function calls, we need both the list and the current cell.
 * Therefore this wrapper stores both of them.
 */
typedef struct ListCellAndListWrapper
{
	List *list;
	ListCell *listCell;
} ListCellAndListWrapper;

/*
 * foreach_declared_ptr -
 *	  a convenience macro which loops through a pointer list without needing a
 *	  ListCell, just a declared pointer variable to store the pointer of the
 *	  cell in.
 *
 *   How it works:
 *	  - A ListCell is declared with the name {var}CellDoNotUse and used
 *	    throughout the for loop using ## to concat.
 *	  - To assign to var it needs to be done in the condition of the for loop,
 *	    because we cannot use the initializer since a ListCell* variable is
 *	    declared there.
 *	  - || true is used to always enter the loop when cell is not null even if
 *	    var is NULL.
 */
#define foreach_declared_ptr(var, l) \
		for (ListCell *(var ## CellDoNotUse) = list_head(l); \
			 (var ## CellDoNotUse) != NULL && \
			 (((var) = lfirst(var ## CellDoNotUse)) || true); \
			 var ## CellDoNotUse = lnext(l, var ## CellDoNotUse))


/*
 * foreach_declared_int -
 *	  a convenience macro which loops through an int list without needing a
 *	  ListCell, just a declared int variable to store the int of the cell in.
 *	  For explanation of how it works see foreach_declared_ptr.
 */
#define foreach_declared_int(var, l) \
		for (ListCell *(var ## CellDoNotUse) = list_head(l); \
			 (var ## CellDoNotUse) != NULL && \
			 (((var) = lfirst_int(var ## CellDoNotUse)) || true); \
			 var ## CellDoNotUse = lnext(l, var ## CellDoNotUse))


/*
 * foreach_declared_oid -
 *	  a convenience macro which loops through an oid list without needing a
 *	  ListCell, just a declared Oid variable to store the oid of the cell in.
 *	  For explanation of how it works see foreach_declared_ptr.
 */
#define foreach_declared_oid(var, l) \
		for (ListCell *(var ## CellDoNotUse) = list_head(l); \
			 (var ## CellDoNotUse) != NULL && \
			 (((var) = lfirst_oid(var ## CellDoNotUse)) || true); \
			 var ## CellDoNotUse = lnext(l, var ## CellDoNotUse))

/*
 * forboth_ptr -
 *	  a convenience macro which loops through two lists of pointers at the same
 *	  time, without needing a ListCell. It only needs two declared pointer
 *	  variables to store the pointer of each of the two cells in.
 */
#define forboth_ptr(var1, l1, var2, l2) \
		for (ListCell *(var1 ## CellDoNotUse) = list_head(l1), \
			 *(var2 ## CellDoNotUse) = list_head(l2); \
			 (var1 ## CellDoNotUse) != NULL && \
			 (var2 ## CellDoNotUse) != NULL && \
			 (((var1) = lfirst(var1 ## CellDoNotUse)) || true) && \
			 (((var2) = lfirst(var2 ## CellDoNotUse)) || true); \
			 var1 ## CellDoNotUse = lnext(l1, var1 ## CellDoNotUse), \
			 var2 ## CellDoNotUse = lnext(l2, var2 ## CellDoNotUse) \
			 )

/*
 * forboth_ptr_oid -
 *	  a convenience macro which loops through two lists at the same time. The
 *	  first list should contain pointers and the second list should contain
 *	  Oids. It does not need a ListCell to do this. It only needs two declared
 *	  variables to store the pointer and the Oid of each of the two cells in.
 */
#define forboth_ptr_oid(var1, l1, var2, l2) \
		for (ListCell *(var1 ## CellDoNotUse) = list_head(l1), \
			 *(var2 ## CellDoNotUse) = list_head(l2); \
			 (var1 ## CellDoNotUse) != NULL && \
			 (var2 ## CellDoNotUse) != NULL && \
			 (((var1) = lfirst(var1 ## CellDoNotUse)) || true) && \
			 (((var2) = lfirst_oid(var2 ## CellDoNotUse)) || true); \
			 var1 ## CellDoNotUse = lnext(l1, var1 ## CellDoNotUse), \
			 var2 ## CellDoNotUse = lnext(l2, var2 ## CellDoNotUse) \
			 )

/*
 * forboth_int_oid -
 *	  a convenience macro which loops through two lists at the same time. The
 *	  first list should contain integers and the second list should contain
 *	  Oids. It does not need a ListCell to do this. It only needs two declared
 *	  variables to store the int and the Oid of each of the two cells in.
 */
#define forboth_int_oid(var1, l1, var2, l2) \
		for (ListCell *(var1 ## CellDoNotUse) = list_head(l1), \
			 *(var2 ## CellDoNotUse) = list_head(l2); \
			 (var1 ## CellDoNotUse) != NULL && \
			 (var2 ## CellDoNotUse) != NULL && \
			 (((var1) = lfirst_int(var1 ## CellDoNotUse)) || true) && \
			 (((var2) = lfirst_oid(var2 ## CellDoNotUse)) || true); \
			 var1 ## CellDoNotUse = lnext(l1, var1 ## CellDoNotUse), \
			 var2 ## CellDoNotUse = lnext(l2, var2 ## CellDoNotUse) \
			 )

/*
 * foreach_ptr_append -
 *	  a convenience macro which loops through a pointer List and can append list
 *	  elements without needing a ListCell or and index variable, just a declared
 *	  pointer variable to store the iterated values.
 *
 *	  PostgreSQL 13 changed the representation of Lists to expansible arrays,
 *	  not chains of cons-cells. This changes the costs for accessing and
 *	  mutating List contents. Therefore different implementations are provided.
 *
 *	  For more information, see postgres commit with sha
 *	  1cff1b95ab6ddae32faa3efe0d95a820dbfdc164
 *
 *	  How it works:
 *	  - An index is declared with the name {var}PositionDoNotUse and used
 *	    throughout the for loop using ## to concat.
 *	  - To assign to var it needs to be done in the condition of the for loop,
 *	    because we cannot use the initializer since the index variable is
 *	    declared there.
 *	  - || true is used to always enter the loop even if var is NULL.
 */
#define foreach_ptr_append(var, l) \
		for (int var ## PositionDoNotUse = 0; \
			 (var ## PositionDoNotUse) < list_length(l) && \
			 (((var) = list_nth(l, var ## PositionDoNotUse)) || true); \
			 var ## PositionDoNotUse++)


/*
 * foreach_decl_ptr -
 *	  a convenience macro which loops through a pointer list without needing a
 *	  ListCell, just a declared pointer variable to store the pointer of the
 *	  cell in.
 *
 *   How it works:
 *	  - A ListCell is declared with the name {var}CellDoNotUse and used
 *	    throughout the for loop using ## to concat.
 *	  - To assign to var it needs to be done in the condition of the for loop,
 *	    because we cannot use the initializer since a ListCell* variable is
 *	    declared there.
 *	  - || true is used to always enter the loop when cell is not null even if
 *	    var is NULL.
 */
#define foreach_decl_ptr(var, l) \
	for (ListCell *(var ## CellDoNotUse) = list_head(l); \
		 (var ## CellDoNotUse) != NULL && \
		 (((var) = lfirst(var ## CellDoNotUse)) || true); \
		 var ## CellDoNotUse = lnext_compat(l, var ## CellDoNotUse))


/*
 * foreach_decl_int -
 *	  a convenience macro which loops through an int list without needing a
 *	  ListCell, just a declared int variable to store the int of the cell in.
 *	  For explanation of how it works see foreach_decl_ptr.
 */
#define foreach_decl_int(var, l) \
	for (ListCell *(var ## CellDoNotUse) = list_head(l); \
		 (var ## CellDoNotUse) != NULL && \
		 (((var) = lfirst_int(var ## CellDoNotUse)) || true); \
		 var ## CellDoNotUse = lnext_compat(l, var ## CellDoNotUse))


/*
 * foreach_decl_oid -
 *	  a convenience macro which loops through an oid list without needing a
 *	  ListCell, just a declared Oid variable to store the oid of the cell in.
 *	  For explanation of how it works see foreach_decl_ptr.
 */
#define foreach_decl_oid(var, l) \
	for (ListCell *(var ## CellDoNotUse) = list_head(l); \
		 (var ## CellDoNotUse) != NULL && \
		 (((var) = lfirst_oid(var ## CellDoNotUse)) || true); \
		 var ## CellDoNotUse = lnext_compat(l, var ## CellDoNotUse))

/*
 * forboth_ptr -
 *	  a convenience macro which loops through two lists of pointers at the same
 *	  time, without needing a ListCell. It only needs two declared pointer
 *	  variables to store the pointer of each of the two cells in.
 */
#define forboth_ptr(var1, l1, var2, l2) \
	for (ListCell *(var1 ## CellDoNotUse) = list_head(l1), \
		 *(var2 ## CellDoNotUse) = list_head(l2); \
		 (var1 ## CellDoNotUse) != NULL && \
		 (var2 ## CellDoNotUse) != NULL && \
		 (((var1) = lfirst(var1 ## CellDoNotUse)) || true) && \
		 (((var2) = lfirst(var2 ## CellDoNotUse)) || true); \
		 var1 ## CellDoNotUse = lnext_compat(l1, var1 ## CellDoNotUse), \
		 var2 ## CellDoNotUse = lnext_compat(l2, var2 ## CellDoNotUse) \
		 )

/*
 * forboth_ptr_oid -
 *	  a convenience macro which loops through two lists at the same time. The
 *	  first list should contain pointers and the second list should contain
 *	  Oids. It does not need a ListCell to do this. It only needs two declared
 *	  variables to store the pointer and the Oid of each of the two cells in.
 */
#define forboth_ptr_oid(var1, l1, var2, l2) \
	for (ListCell *(var1 ## CellDoNotUse) = list_head(l1), \
		 *(var2 ## CellDoNotUse) = list_head(l2); \
		 (var1 ## CellDoNotUse) != NULL && \
		 (var2 ## CellDoNotUse) != NULL && \
		 (((var1) = lfirst(var1 ## CellDoNotUse)) || true) && \
		 (((var2) = lfirst_oid(var2 ## CellDoNotUse)) || true); \
		 var1 ## CellDoNotUse = lnext_compat(l1, var1 ## CellDoNotUse), \
		 var2 ## CellDoNotUse = lnext_compat(l2, var2 ## CellDoNotUse) \
		 )

/*
 * forboth_int_oid -
 *	  a convenience macro which loops through two lists at the same time. The
 *	  first list should contain integers and the second list should contain
 *	  Oids. It does not need a ListCell to do this. It only needs two declared
 *	  variables to store the int and the Oid of each of the two cells in.
 */
#define forboth_int_oid(var1, l1, var2, l2) \
	for (ListCell *(var1 ## CellDoNotUse) = list_head(l1), \
		 *(var2 ## CellDoNotUse) = list_head(l2); \
		 (var1 ## CellDoNotUse) != NULL && \
		 (var2 ## CellDoNotUse) != NULL && \
		 (((var1) = lfirst_int(var1 ## CellDoNotUse)) || true) && \
		 (((var2) = lfirst_oid(var2 ## CellDoNotUse)) || true); \
		 var1 ## CellDoNotUse = lnext_compat(l1, var1 ## CellDoNotUse), \
		 var2 ## CellDoNotUse = lnext_compat(l2, var2 ## CellDoNotUse) \
		 )

/*
 * foreach_decl_ptr_append -
 *	  a convenience macro which loops through a pointer List and can append list
 *	  elements without needing a ListCell or and index variable, just a declared
 *	  pointer variable to store the iterated values.
 *
 *	  PostgreSQL 13 changed the representation of Lists to expansible arrays,
 *	  not chains of cons-cells. This changes the costs for accessing and
 *	  mutating List contents. Therefore different implementations are provided.
 *
 *	  For more information, see postgres commit with sha
 *	  1cff1b95ab6ddae32faa3efe0d95a820dbfdc164
 *	  How it works:
 *	  - An index is declared with the name {var}PositionDoNotUse and used
 *	    throughout the for loop using ## to concat.
 *	  - To assign to var it needs to be done in the condition of the for loop,
 *	    because we cannot use the initializer since the index variable is
 *	    declared there.
 *	  - || true is used to always enter the loop even if var is NULL.
 */
#define foreach_decl_ptr_append(var, l) \
	for (int var ## PositionDoNotUse = 0; \
		 (var ## PositionDoNotUse) < list_length(l) && \
		 (((var) = list_nth(l, var ## PositionDoNotUse)) || true); \
		 var ## PositionDoNotUse ++)

static inline List*
CustomBuildTargetList(List* tlist, Index varNo)
{
	List		*result_tlist = NIL;
	ListCell	*lc;

	foreach (lc, tlist)
	{
		TargetEntry	*tle = (TargetEntry *) lfirst(lc);

		Var *var = makeVar(varNo,
						   tle->resno,
						   exprType((Node *) tle->expr),
						   exprTypmod((Node *) tle->expr),
						   exprCollation((Node *) tle->expr),
						   0);

		TargetEntry *newtle = makeTargetEntry((Expr *) var,
											  tle->resno,
											  tle->resname,
											  tle->resjunk);

		result_tlist = lappend(result_tlist, newtle);
	}

	return result_tlist;
}

__attribute__((always_inline))
static inline Relation safe_relation_open(Oid relationId, LOCKMODE lockmode) {
	//elog(NOTICE, "Columnar [safe_relation_open] " __FILE__ ":" TOSTRING(__LINE__));
	const Relation rel = relation_open(relationId, lockmode);
	//elog(NOTICE, "Columnar [safe_relation_open] [SUCCESS] " __FILE__ ":" TOSTRING(__LINE__));
	return rel;
}

__attribute__((always_inline))
static inline Relation try_safe_relation_open(Oid relationId, LOCKMODE lockmode) {
	//elog(NOTICE, "Columnar [try_safe_relation_open] " __FILE__ ":" TOSTRING(__LINE__));
	const Relation rel = try_relation_open(relationId, lockmode);
	//elog(NOTICE, "Columnar [safe_relation_open] [SUCCESS] " __FILE__ ":" TOSTRING(__LINE__));
	return rel;
}
