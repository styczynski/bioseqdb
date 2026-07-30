/*-------------------------------------------------------------------------
 *
 * PostgreSQL-memory-context ownership for cache executor objects.
 *
 * Cache scan state is allocated by PostgreSQL without running C++
 * constructors. These helpers construct nontrivial leaf objects in the
 * owning query context without raw new/delete and release them explicitly
 * from the registered context cleanup callback.
 *
 *-------------------------------------------------------------------------
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

/** Construct one object in a selected PG context without raising on OOM. */
template<class Value, class... Arguments>
[[nodiscard]] Value* try_construct_context_owned(
        MemoryContext context,
        Arguments&&... arguments) noexcept {
    static_assert(std::is_nothrow_destructible_v<Value>);
    if (context == nullptr) {
        return nullptr;
    }
    void* const storage = MemoryContextAllocAligned(
        context,
        sizeof(Value),
        alignof(Value),
        MCXT_ALLOC_NO_OOM);
    if (storage == nullptr) {
        return nullptr;
    }
    try {
        return std::construct_at(
            static_cast<Value*>(storage),
            std::forward<Arguments>(arguments)...);
    } catch (...) {
        pfree(storage);
        return nullptr;
    }
}

/** Destroy and release one context-owned object, clearing its observer. */
template<class Value>
void destroy_context_owned(Value*& value) noexcept {
    static_assert(std::is_nothrow_destructible_v<Value>);
    if (value == nullptr) {
        return;
    }
    std::destroy_at(value);
    pfree(value);
    value = nullptr;
}

} // namespace bioseq::cache::pg::executor
