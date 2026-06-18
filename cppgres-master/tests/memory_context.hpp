#pragma once

#include <stdexcept>
#include <type_traits>

#include "tests.hpp"

namespace tests {

static_assert(!std::copy_constructible<cppgres::alloc_set_memory_context>);
static_assert(!std::is_copy_assignable_v<cppgres::alloc_set_memory_context>);
static_assert(std::move_constructible<cppgres::alloc_set_memory_context>);
static_assert(std::is_move_assignable_v<cppgres::alloc_set_memory_context>);

add_test(alloc_set_context, ([](test_case &) {
           bool result = true;
           cppgres::alloc_set_memory_context c;
           result = result && _assert(c.alloc(100));
           return result;
         }));

add_test(allocator, ([](test_case &) {
           bool result = true;

           cppgres::memory_context_allocator<std::string> alloc0;
           result = result && _assert(alloc0.memory_context() == cppgres::memory_context());

           auto old_ctx = ::CurrentMemoryContext;
           auto ctx = cppgres::alloc_set_memory_context();
           ::CurrentMemoryContext = ctx;
           result = result && _assert(alloc0.memory_context() != cppgres::memory_context());
           ::CurrentMemoryContext = old_ctx;

           cppgres::memory_context_allocator<std::string> alloc(cppgres::top_memory_context(),
                                                                true);
           std::vector<std::string, cppgres::memory_context_allocator<std::string>> vec(alloc);
           vec.emplace_back("a");

           return result;
         }));

add_test(current_memory_context, ([](test_case &) {
           bool result = true;

           cppgres::always_current_memory_context ctx;
           cppgres::memory_context ctx0(ctx);
           result = result && _assert(ctx == ctx0);

           ::CurrentMemoryContext = ::TopMemoryContext;

           result = result && _assert(ctx != ctx0);

           return result;
         }));

add_test(memory_context_for_ptr, ([](test_case &) {
           return _assert(cppgres::memory_context::for_pointer(::palloc0(100)) ==
                          cppgres::memory_context());
         }));

add_test(memory_context_scope, ([](test_case &) {
           bool result = true;
           cppgres::memory_context before;
           {
             [[maybe_unused]] auto scope =
                 cppgres::memory_context_scope(cppgres::top_memory_context());
             cppgres::memory_context now;
             result = result && _assert(now == cppgres::top_memory_context());
             result = result && _assert(now != before);
           }
           cppgres::memory_context now;
           result = result && _assert(now == before);
           return result;
         }));

add_test(owned_memory_context, ([](test_case &) {
           bool result = true;

           // owned context deletion/reset
           bool context_reset = false;
           {
             cppgres::alloc_set_memory_context ctx;
             ctx.register_reset_callback(
                 [](void *v) {
                   bool *val = reinterpret_cast<bool *>(v);
                   *val = true;
                 },
                 &context_reset);
           }
           result = result && _assert(context_reset);

           context_reset = false;
           {
             cppgres::alloc_set_memory_context ctx;
             ctx.register_reset_callback(
                 [](void *v) {
                   bool *val = reinterpret_cast<bool *>(v);
                   *val = true;
                 },
                 &context_reset);
             cppgres::memory_context mctx(std::move(ctx));
           }
           result = result && _assert(!context_reset);

           return result;
         }));

add_test(owned_memory_context_move_transfers_ownership, ([](test_case &) {
           bool result = true;
           bool context_reset = false;

           {
             cppgres::alloc_set_memory_context ctx;
             ctx.register_reset_callback(
                 [](void *v) {
                   bool *val = reinterpret_cast<bool *>(v);
                   *val = true;
                 },
                 &context_reset);

             {
               [[maybe_unused]] cppgres::alloc_set_memory_context moved(std::move(ctx));
               result = result && _assert(!context_reset);
             }

             result = result && _assert(context_reset);
           }

           return result;
         }));

add_test(tracking_memory_context_copy_keeps_original_valid, ([](test_case &) {
           bool result = true;
           cppgres::alloc_set_memory_context ctx;
           cppgres::tracking_memory_context<cppgres::memory_context> tracked{
               cppgres::memory_context(ctx)};

           {
             auto copy = tracked;
             result = result && _assert(tracked.resets() == 0);
             result = result && _assert(copy.resets() == 0);
           }

           ctx.reset();
           result = result && _assert(tracked.resets() == 1);

           return result;
         }));

add_test(tracking_memory_context_copy_shares_reset_state, ([](test_case &) {
           bool result = true;
           cppgres::alloc_set_memory_context ctx;
           cppgres::tracking_memory_context<cppgres::memory_context> tracked{
               cppgres::memory_context(ctx)};
           auto copy = tracked;

           ctx.reset();
           result = result && _assert(tracked.resets() == 1);
           result = result && _assert(copy.resets() == 1);

           return result;
         }));

add_test(executing_within_memory_context, ([](test_case &) {
           bool result = true;
           cppgres::alloc_set_memory_context mctx;
           result = result && _assert(mctx != cppgres::always_current_memory_context());

           auto mctx_used =
               mctx([&mctx]() { return mctx == cppgres::always_current_memory_context(); });
           result = result && _assert(mctx_used);
           result = result && _assert(mctx != cppgres::always_current_memory_context());

           // exception should be handled and the memory context should revert
           try {
             mctx([]() { throw std::runtime_error("error"); });
           } catch (std::runtime_error &e) {
           }
           result = result && _assert(mctx != cppgres::always_current_memory_context());

           return result;
         }));
} // namespace tests
