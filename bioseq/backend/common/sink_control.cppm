/**
 * @file
 * @brief Dependency-free flow signal shared by streaming sinks.
 */
module;

#include <cstdint>

export module bioseq.common.sink_control;

export namespace bioseq {

/**
 * @brief Tells a producer whether its sink accepts another item.
 *
 * The enum has an explicit one-byte representation so generated and manually
 * authored adapters share the same layout without importing either domain.
 */
enum class sink_control : std::uint8_t {
    /// Continue producing items.
    continue_,

    /// Stop production successfully.
    stop
};

} // namespace bioseq
