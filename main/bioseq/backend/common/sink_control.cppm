module;

#include <cstdint>

export module bioseq.common.sink_control;

// DOC0372: the single dependency-free continue/stop signal shared by generated
// transport, provider writers, archive callbacks, and IO sinks. Each layer keeps
// its own `std::expected<..., error_type>`; no adapter maps between duplicate
// control enums. This module owns the type once and imports nothing beyond the
// standard library so isolated generated targets can consume it without pulling
// in any provider, IO, sequence, alphabet, or PostgreSQL edge.
export namespace bioseq {

enum class sink_control : std::uint8_t {
    continue_,
    stop
};

}  // namespace bioseq
