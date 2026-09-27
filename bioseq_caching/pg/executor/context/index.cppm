/**
 * @file
 * @brief Creates and destroys C++ objects in PostgreSQL memory contexts.
 *
 * PostgreSQL allocates scan state without calling C++ constructors. These
 * helpers construct the C++ members separately. The scan registers a context
 * cleanup callback to destroy those members.
 */
module;

extern "C" {
#include "postgres.h"

#include "utils/memutils.h"
#include "utils/palloc.h"
}

#include <memory>
#include <type_traits>
#include <utility>

export module bioseq.cache.pg.executor.context;

export namespace bioseq::cache::pg::executor {

/** Construct an object in the given memory context. Return null on allocation or constructor failure. */
template <class p_value_type, class... p_argument_types>
[[nodiscard]] p_value_type* try_construct_context_owned(
    MemoryContext context, p_argument_types&&... arguments) noexcept {
    static_assert(std::is_nothrow_destructible_v<p_value_type>);
    if (context == nullptr) {
        return nullptr;
    }
    void* const storage = MemoryContextAllocAligned(
        context, sizeof(p_value_type), alignof(p_value_type), MCXT_ALLOC_NO_OOM);
    if (storage == nullptr) {
        return nullptr;
    }
    try {
        return std::construct_at(static_cast<p_value_type*>(storage),
                                 std::forward<p_argument_types>(arguments)...);
    } catch (...) {
        pfree(storage);
        return nullptr;
    }
}

/** Destroy the object, free its context allocation and clear the caller's pointer. */
template <class p_value_type> void destroy_context_owned(p_value_type*& value) noexcept {
    static_assert(std::is_nothrow_destructible_v<p_value_type>);
    if (value == nullptr) {
        return;
    }
    std::destroy_at(value);
    pfree(value);
    value = nullptr;
}

} // namespace bioseq::cache::pg::executor
