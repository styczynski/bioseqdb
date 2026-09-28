/**
 * @file
 * @brief Return values that let a streaming consumer stop its producer.
 */
module;

#include <cstdint>

export module bioseq.common.sink_control;

export namespace bioseq {

/**
 * @brief Tells a producer whether to continue sending items.
 *
 * The one-byte enum gives generated and handwritten adapters the same layout.
 */
enum class sink_control : std::uint8_t {
    /// Continue producing items.
    continue_ = 0u,

    /// Stop production successfully.
    stop = 1u
};

} // namespace bioseq
