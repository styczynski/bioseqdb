/**
 * @file external/runtime.cppm
 * @brief Initialize Oat++ once and clean it up at process, exit.
 */
module;

#include <mutex>
#include <oatpp/Environment.hpp>

export module bioseq.external.runtime;

namespace bioseq::external::detail {

/** Keeps Oat++ initialized for  this object's lifetime. */
struct oatpp_runtime_lifetime {
    /** Initialize the Oat++ environment. */
    oatpp_runtime_lifetime() {
        oatpp::Environment::init();
    }

    oatpp_runtime_lifetime(const oatpp_runtime_lifetime&) = delete;
    oatpp_runtime_lifetime& operator=(const oatpp_runtime_lifetime&) = delete;

    /** Stops Oat++ when the process-local static object is destroyed. */
    ~oatpp_runtime_lifetime() {
        oatpp::Environment::destroy();
    }
};

} // namespace bioseq::external::detail

export namespace bioseq::external {

/** Initializes the process-wide Oat++ runtime on first use. */
inline void ensure_oatpp_runtime() {
    static detail::oatpp_runtime_lifetime runtime;
    static_cast<void>(runtime);
}

} // namespace bioseq::external
