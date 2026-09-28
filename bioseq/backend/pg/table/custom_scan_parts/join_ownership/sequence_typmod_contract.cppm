/**
 * @file
 * @brief Checks sequence type descriptors against the compiled dispatch types.
 */
export module bioseq.postgres.table:custom_scan_join_ownership_sequence_typmod_contract;

import std;
import bioseq.sequences.compact_sequence_typmod;
import bioseq.postgres.types.compact_sequence.dispatch_profile_contract;

export {
    /** @brief Checks that the typmod has a valid encoding and an alphabet and storage width this build supports. */
    [[nodiscard]] bool batch_join_typmod_is_supported(const std::int32_t typmod) noexcept {
        if (typmod < 0) {
            return false;
        }
        const auto decoded = compact_sequence_stdc_type_params::decode(typmod);
        const bool alphabet_supported =
            decoded.alphabet_code < pgseq_dispatch_profile_alphabet_count;
        const bool width_supported =
            pgseq_dispatch_profile_contains_storage_unit_byte_width(
                decoded.storage_unit_byte_width);
        return alphabet_supported && width_supported && decoded.encode() == typmod;
    }

    /** @brief Checks that two supported sequence typmods bind the same alphabet. */
    [[nodiscard]] bool mmsearch_typmods_share_alphabet(const std::int32_t target_typmod,
                                                       const std::int32_t query_typmod) noexcept {
        return batch_join_typmod_is_supported(target_typmod) &&
               batch_join_typmod_is_supported(query_typmod) &&
               compact_sequence_stdc_type_params::decode(target_typmod).alphabet_code ==
                   compact_sequence_stdc_type_params::decode(query_typmod).alphabet_code;
    }
}
