module;


#include <string>
#include <utility>
#include <concepts>

export module bioseq.io.errors;

import bioseq.common.logging;

export namespace bioseq::io {

/// Concept: a callable that accepts a diagnostic string by rvalue.
/// Used for the `failure_fn` passed into `read_sequences_file`.
template <typename p_failure_function>
concept failure_callback = std::invocable<p_failure_function, std::string&&>;

/// Build a diagnostic via a compile-checked fmt format string and
/// dispatch to the user's failure callback. Wraps the boilerplate of
/// `failure_fn(fmt::format(...))` while keeping the format string
/// compile-time validated. Spec §3.5 / §15.1.
template <typename p_failure_function, typename... p_arguments>
inline void emit_failure(p_failure_function& failure_fn,
                         fmt::format_string<p_arguments...> fmt_str,
                         p_arguments&&... args) {
    failure_fn(fmt::format(fmt_str, std::forward<p_arguments>(args)...));
}

}  // namespace bioseq::io
