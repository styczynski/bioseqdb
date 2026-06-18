/**
 * \file
 */
#pragma once

#include <concepts>
#include <memory>

#include "guard.hpp"
#include "imports.h"

namespace cppgres {

struct abstract_memory_context {
  virtual ~abstract_memory_context() = default;

  template <typename T = std::byte> T *alloc(size_t n = 1) {
    return static_cast<T *>(ffi_guard{::MemoryContextAlloc}(_memory_context(), sizeof(T) * n));
  }
  template <typename T = void> void free(T *ptr) { ffi_guard{::pfree}(ptr); }

  void reset() { ffi_guard{::MemoryContextReset}(_memory_context()); }

  bool operator==(abstract_memory_context &c) noexcept {
    return _memory_context() == c._memory_context();
  }
  bool operator!=(abstract_memory_context &c) noexcept {
    return _memory_context() != c._memory_context();
  }

  operator ::MemoryContext() { return _memory_context(); }

  ::MemoryContextCallback *register_reset_callback(::MemoryContextCallbackFunction func,
                                                   void *arg) {
    auto cb = alloc<::MemoryContextCallback>();
    cb->func = func;
    cb->arg = arg;
    ffi_guard{::MemoryContextRegisterResetCallback}(_memory_context(), cb);
    return cb;
  }

  void delete_context() { ffi_guard{::MemoryContextDelete}(_memory_context()); }

  /**
   * Execute a callable within this memory context, respecting exceptions
   */
  auto operator()(auto thunk) { return memory_context_execution(thunk, *this)(); }

protected:
  virtual ::MemoryContext _memory_context() = 0;

  template <typename T> requires requires(T t) { t(); }
  struct memory_context_execution {
    memory_context_execution(T thunk, abstract_memory_context &ctx)
        : _ctx(::CurrentMemoryContext), _thunk(thunk) {
      ::CurrentMemoryContext = ctx;
    }
    ~memory_context_execution() { ::CurrentMemoryContext = _ctx; }

    auto operator()() { return _thunk(); }

  private:
    MemoryContext _ctx;
    T _thunk;
  };
};

struct owned_memory_context : public abstract_memory_context {
  friend struct memory_context;

protected:
  owned_memory_context(::MemoryContext context) : context(context), moved(false) {}
  owned_memory_context(const owned_memory_context &) = delete;
  owned_memory_context &operator=(const owned_memory_context &) = delete;
  owned_memory_context(owned_memory_context &&other) noexcept
      : context(other.context), moved(other.moved) {
    other.moved = true;
  }
  owned_memory_context &operator=(owned_memory_context &&other) {
    if (this != &other) {
      if (!moved) {
        delete_context();
      }
      context = other.context;
      moved = other.moved;
      other.moved = true;
    }
    return *this;
  }

  ~owned_memory_context() {
    if (!moved) {
      delete_context();
    }
  }

  ::MemoryContext context;
  bool moved;

  ::MemoryContext _memory_context() override { return context; }
};

struct memory_context : public abstract_memory_context {

  friend struct owned_memory_context;

  explicit memory_context() : context(::CurrentMemoryContext) {}
  explicit memory_context(::MemoryContext context) : context(context) {}
  explicit memory_context(abstract_memory_context &&context) : context(context) {}

  explicit memory_context(owned_memory_context &&ctx) : context(ctx) { ctx.moved = true; }

  static memory_context for_pointer(void *ptr) {
    if (ptr == nullptr || ptr != (void *)MAXALIGN(ptr)) {
      throw std::runtime_error("invalid pointer");
    }
    return memory_context(ffi_guard{::GetMemoryChunkContext}(ptr));
  }

  template <typename C> requires std::derived_from<C, abstract_memory_context>
  friend struct tracking_memory_context;

protected:
  ::MemoryContext context;

  ::MemoryContext _memory_context() noexcept override { return context; }
};

struct always_current_memory_context : public abstract_memory_context {
  always_current_memory_context() = default;

protected:
  ::MemoryContext _memory_context() override { return ::CurrentMemoryContext; }
};

struct alloc_set_memory_context : public owned_memory_context {
  using owned_memory_context::owned_memory_context;
  alloc_set_memory_context()
      : owned_memory_context(ffi_guard{::AllocSetContextCreateInternal}(
            ::CurrentMemoryContext, nullptr, ALLOCSET_DEFAULT_SIZES)) {}
  alloc_set_memory_context(memory_context &ctx)
      : owned_memory_context(
            ffi_guard{::AllocSetContextCreateInternal}(ctx, nullptr, ALLOCSET_DEFAULT_SIZES)) {}

  alloc_set_memory_context(memory_context &&ctx)
      : owned_memory_context(
            ffi_guard{::AllocSetContextCreateInternal}(ctx, nullptr, ALLOCSET_DEFAULT_SIZES)) {}
};

inline memory_context top_memory_context() { return memory_context(TopMemoryContext); };

template <typename C> requires std::derived_from<C, abstract_memory_context>
struct tracking_memory_context : public abstract_memory_context {
private:
  template <typename T> requires std::integral<T>
  struct shared_counter {
    T value;
    constexpr explicit shared_counter(T init = 0) noexcept : value(init) {}

    shared_counter &operator=(T v) noexcept {
      value = v;
      return *this;
    }

    shared_counter &operator++() noexcept {
      ++value;
      return *this;
    }

    T operator++(int) noexcept {
      T old = value;
      ++value;
      return old;
    }

    constexpr operator T() const noexcept { return value; }
  };

  struct callback_state {
    shared_counter<uint64_t> counter;
    ::MemoryContextCallback *callback = nullptr;
  };

  static void track_reset(void *arg) {
    auto *state = static_cast<callback_state *>(arg);
    state->counter++;
    state->callback = nullptr;
  }

public:
  tracking_memory_context(const tracking_memory_context<C> &other) noexcept
      : ctx(other.ctx), state(other.state) {}

  explicit tracking_memory_context(C ctx) : ctx(ctx), state(std::make_shared<callback_state>()) {
    state->callback = this->register_reset_callback(track_reset, state.get());
  }

  tracking_memory_context(tracking_memory_context &&other) noexcept
      : ctx(std::move(other.ctx)), state(std::move(other.state)) {}

  tracking_memory_context &operator=(const tracking_memory_context &other) noexcept {
    if (this != &other) {
      ctx = other.ctx;
      state = other.state;
    }
    return *this;
  }

  tracking_memory_context &operator=(tracking_memory_context &&other) noexcept {
    if (this != &other) {
      ctx = std::move(other.ctx);
      state = std::move(other.state);
    }
    return *this;
  }

  ~tracking_memory_context() {
    if (state != nullptr && state.use_count() == 1 && state->callback != nullptr) {
      state->callback->func = [](void *) {};
      state->callback->arg = nullptr;
      state->callback = nullptr;
    }
  }

  uint64_t resets() const { return state == nullptr ? 0 : state->counter; }
  C &get_memory_context() { return ctx; }

private:
  C ctx;
  std::shared_ptr<callback_state> state;

protected:
  ::MemoryContext _memory_context() override { return ctx._memory_context(); }
};

template <typename T>
concept a_memory_context =
    std::derived_from<T, abstract_memory_context> && std::default_initializable<T>;

template <a_memory_context Context> struct memory_context_scope {
  explicit memory_context_scope(Context &ctx)
      : previous(::CurrentMemoryContext), ctx(ctx.operator ::MemoryContext()) {
    ::CurrentMemoryContext = ctx;
  }
  explicit memory_context_scope(Context &&ctx)
      : previous(::CurrentMemoryContext), ctx(ctx.operator ::MemoryContext()) {
    ::CurrentMemoryContext = ctx;
  }

  ~memory_context_scope() { ::CurrentMemoryContext = previous; }

private:
  ::MemoryContext previous;
  ::MemoryContext ctx;
};

template <class T, a_memory_context Context = memory_context> struct memory_context_allocator {
  using value_type = T;
  memory_context_allocator() noexcept : context(Context()), explicit_deallocation(false) {}
  memory_context_allocator(Context &&ctx, bool explicit_deallocation) noexcept
      : context(std::move(ctx)), explicit_deallocation(explicit_deallocation) {}

  constexpr memory_context_allocator(const memory_context_allocator<T> &c) noexcept
      : context(c.context) {}

  [[nodiscard]] T *allocate(std::size_t n) {
    try {
      return context.template alloc<T>(n);
    } catch (pg_exception &e) {
      throw std::bad_alloc();
    }
  }

  void deallocate(T *p, std::size_t n) noexcept {
    if (explicit_deallocation || context == top_memory_context()) {
      context.free(p);
    }
  }

  bool operator==(const memory_context_allocator &c) { return context == c.context; }
  bool operator!=(const memory_context_allocator &c) { return context != c.context; }

  Context &memory_context() { return context; }

private:
  Context context;
  bool explicit_deallocation;
};

struct pointer_gone_exception : public std::exception {
  const char *what() const noexcept override {
    return "pointer belongs to a MemoryContext that has been reset or deleted";
  }
};

} // namespace cppgres
