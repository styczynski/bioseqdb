module;

#include <foonathan/memory/container.hpp> // vector, list, list_node_size
#include <foonathan/memory/memory_pool.hpp> // memory_pool
#include <foonathan/memory/smart_ptr.hpp> // allocate_unique
#include <foonathan/memory/static_allocator.hpp> // static_allocator_storage, static_block_allocator
#include <foonathan/memory/temporary_allocator.hpp> // temporary_allocator

// alias namespace foonathan::memory as memory for easier access
#include <foonathan/memory/namespace_alias.hpp>

export module context;

using namespace memory::literals;

export {

namespace context {

template <typename T> using vector = memory::vector<T, memory::memory_pool<>>;


class context {
public:
    memory::memory_pool<> mem;
    context() : mem(sizeof(uint64_t), 100_MiB) {}
};


};

}