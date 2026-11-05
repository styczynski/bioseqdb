/** SQL-owned random state, admitted native output, and scalar execution. */
module;
#include <bioseq/pg_compat.h>
extern "C" {
#include "common/pg_prng.h"
#include "utils/builtins.h"
}
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

export module bioseq.postgres.sequences.random:functions;

import bioseq.common.hashing;
import bioseq.common.checked_arithmetic;
import bioseq.sequences;
import bioseq.sequences.generation;
import bioseq.sequences.rank_pack;
import bioseq.sequences.cigar;
import bioseq.storage.lazy_sequence;
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.pool;
import bioseq.parallelism.concepts;
import bioseq.postgres.types.compact_sequence;
import bioseq.postgres.support;
import bioseq.postgres.sql_boundary;
import bioseq.io.identifier_parser.formats;
import bioseq.postgres.storage.io.sql_rows;

export namespace bioseq::postgres::random {
namespace generation = bioseq::sequences::generation;
namespace support = bioseq::postgres::support;
namespace identifiers = bioseq::io::identifier_parser;

/** The three SQL producers share ownership, length sampling, and ordinal policy. */
enum class request_kind { sequence, header, mutation };

/** Nullable SQL values are deliberately retained until the first demanded row. */
struct request {
    request_kind kind{request_kind::sequence};
    std::optional<std::string> alphabet{};
    std::optional<std::int32_t> storage_bits{};
    std::optional<std::int64_t> minimum{};
    std::optional<std::int64_t> maximum{};
    std::optional<double> mean{};
    std::optional<double> stddev{};
    std::optional<std::uint64_t> seed{};
    std::optional<Datum> source{};
    std::optional<Datum> header_format{};
    generation::generator_params mutation{};
};

/** Report a checked numeric-kernel failure only for the demanded output row. */
[[noreturn]] inline void fail_generation(generation::generation_error failure) {
    using error = generation::generation_error;
    const auto message = failure == error::invalid_parameter
        ? "invalid random generation parameters"
        : failure == error::size_overflow ? "random sequence exceeds the supported allocation size"
        : failure == error::allocation_rejected ? "random generation memory admission was rejected"
        : failure == error::invalid_source ? "random mutation source is invalid"
        : failure == error::unsupported_layout ? "unsupported random sequence layout"
        : "random sequence output did not match its measured size";
    support::raise_sql_error(failure == error::allocation_rejected ? ERRCODE_OUT_OF_MEMORY
        : failure == error::size_overflow ? ERRCODE_PROGRAM_LIMIT_EXCEEDED
        : ERRCODE_INVALID_PARAMETER_VALUE, {.message = message});
}

/** Resolve an explicit alphabet and width through the existing seq validator. */
[[nodiscard]] inline compact_sequence_stdc_type_params resolve_layout(
    std::optional<std::string> const& alphabet, std::optional<std::int32_t> bits) {
    if (!alphabet) {
        support::raise_sql_error(ERRCODE_INDETERMINATE_DATATYPE, {
            .message = "cannot infer the alphabet of bioseq.random_seq",
            .hint = "Specify alphabet and storage_bits, or cast to bioseq.seq('dna4', 64)."});
    }
    const auto code = support::invoke_pg_as_cpp([&alphabet] {
        return parse_alphabet_name_checked(alphabet->c_str(), "bioseq.random_seq");
    });
    const auto width = bits.value_or(pgseq_is_cigar_alphabet(code)
        ? static_cast<std::int32_t>(sizeof(bioseq::cigar_run) * 8u) : 64);
    if (width != 8 && width != 16 && width != 32 && width != 64) {
        fail_generation(generation::generation_error::unsupported_layout);
    }
    compact_sequence_stdc_type_params layout{
        .alphabet_code = code, .storage_unit_byte_width = static_cast<std::uint8_t>(width / 8),
        .encoding = pgseq_is_cigar_alphabet(code) ? bioseq::sequence_encoding::cigar_rle
            : bioseq::sequence_encoding::rank_packed,
        .space = pgseq_is_cigar_alphabet(code)
            ? bioseq::cigar_space_from_name(*alphabet).value()
            : bioseq::cigar_space::compatibility};
    return support::invoke_pg_as_cpp([layout] { return decode_seq_typmod_checked(layout.encode()); });
}

/** Convert nullable SQL bounds without signed-to-unsigned wraparound. */
[[nodiscard]] inline generation::generation_length_parameters resolve_lengths(request const& input) {
    const auto minimum = input.minimum.value_or(input.kind == request_kind::header ? 16 : 1000);
    const auto maximum = input.maximum.value_or(minimum);
    if (minimum < 0 || maximum < 0) fail_generation(generation::generation_error::invalid_parameter);
    generation::generation_length_parameters result{
        .minimum = static_cast<std::size_t>(minimum), .maximum = static_cast<std::size_t>(maximum),
        .mean = input.mean, .stddev = input.stddev.value_or(0.0)};
    const auto checked = generation::validate_generation_lengths(result,
        input.kind == request_kind::header ? 1u : 0u,
        input.kind == request_kind::header ? MaxAllocSize / 4u
            : std::numeric_limits<std::uint32_t>::max());
    if (!checked) fail_generation(checked.error());
    return result;
}

/** One call site's private stream; arguments are always resolved per invocation. */
struct stream_state {
    std::uint64_t entropy{};
    std::uint64_t ordinal{};
};

/** A measured output; measurement and emission replay exactly the same random seed. */
struct measured_row {
    std::uint64_t seed;
    std::uint64_t symbols;
    std::size_t runs;
    std::uint64_t bytes;
};

/** State owned by a memory-context callback, including the active shared permit. */
struct batch_state {
    request parameters;
    stream_state stream;
    MemoryContext output_context{};
    MemoryContext source_context{};
    MemoryContextCallback cleanup{};
    pg_sequence_read_session* session{};
    pg_sequence_descriptor source_descriptor{};
    bioseq::storage::borrowed_rank_window_source rank_source{};
    pg_sequence_read_handle cigar_source{};
    compact_sequence_stdc_type_params layout{};
    generation::generation_length_parameters lengths{};
    identifiers::header_format format{identifiers::header_format::plain};
    bioseq::parallelism::budgeted_wave admission{};
    std::span<Datum> outputs{};
    std::span<std::optional<generation::generation_error>> output_errors{};
    std::size_t next_output{};
    std::size_t capacity{32};
    std::uint64_t batches{};
    bool initialized{};
    bool null_source{};

    /** Session control lives in TopMemoryContext. Its own payload reset callback
     * clears payload_context before this parent callback runs; abort safely releases
     * the surviving control block, as in source_abi's parent-reset lifecycle. */
    ~batch_state() {
        if (session != nullptr) abort_pg_sequence_read_session_noexcept(session);
    }

    /** Initialize fixed metadata and borrow a constant source once on first demand. */
    void initialize() {
        if (initialized) return;
        if (parameters.kind == request_kind::header) {
            lengths = resolve_lengths(parameters);
            format = bioseq::storage::load_pg::decode_header_format(
                parameters.header_format, identifiers::header_format::plain);
        } else if (parameters.kind == request_kind::sequence) {
            lengths = resolve_lengths(parameters);
            layout = resolve_layout(parameters.alphabet, parameters.storage_bits);
        } else {
            null_source = !parameters.source.has_value();
            if (null_source) { initialized = true; return; }
            const auto checked = generation::validate_mutation_parameters(parameters.mutation);
            if (!checked) fail_generation(checked.error());
            support::invoke_pg_as_cpp([this] {
                source_context = AllocSetContextCreate(MemoryContextGetParent(output_context),
                    "Bioseq random source", ALLOCSET_SMALL_SIZES);
                const auto previous = MemoryContextSwitchTo(source_context);
                Datum input = *parameters.source;
                const auto header = classify_and_describe_sequence_header(input);
                if (header.kind == pg_sequence_datum_kind::locator) {
                    ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                        errmsg("cannot resolve a bioseq locator outside its owning table scan")));
                }
                if (!VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(input))) {
                    input = PointerGetDatum(PG_DETOAST_DATUM(input));
                }
                source_descriptor = classify_and_describe_sequence(input);
                if (!source_descriptor.alphabet_bound) {
                    ereport(ERROR, (errcode(ERRCODE_INDETERMINATE_DATATYPE),
                        errmsg("random_mutate requires a sequence with a bound alphabet")));
                }
                layout = {.alphabet_code = source_descriptor.alphabet_code,
                    .storage_unit_byte_width = source_descriptor.storage_unit_byte_width,
                    .encoding = source_descriptor.encoding, .space = source_descriptor.space};
                session = begin_pg_sequence_read_session(source_context);
                if (pgseq_is_cigar_alphabet(layout.alphabet_code)) {
                    pg_cigar_backend backend{*session};
                    cigar_source = backend.borrow_input(input);
                } else {
                    pgseq_dispatch_rank_packed_instance(layout.alphabet_code,
                        layout.storage_unit_byte_width, [this, input]<class Instance>() {
                            using alphabet = typename compact_sequence_range<Instance>::traits::alphabet_type;
                            auto* owner = make_pg_sequence_call_owner<alphabet>(*session, input, source_descriptor);
                            rank_source = owner->view().template borrow_rank_source<false>();
                        });
                }
                MemoryContextSwitchTo(previous);
            });
        }
        initialized = true;
    }

    /** Count logical output and packed bytes without allocating output storage. */
    [[nodiscard]] std::expected<measured_row, generation::generation_error> measure(std::uint64_t ordinal) {
        measured_row result{.seed = bioseq::common::splitmix64(parameters.seed.value_or(stream.entropy) ^ ordinal)};
        auto random = generation::seed_stream(result.seed, 0u);
        if (parameters.kind != request_kind::mutation) {
            // Reserve the final stream ID for length sampling. Packed content chunk
            // indices cannot reach it under the uint32 symbol-length bound.
            constexpr auto length_stream_id = (std::numeric_limits<std::uint64_t>::max)();
            auto length_random = generation::seed_stream(result.seed, length_stream_id);
            const auto sampled = generation::sample_generation_length(lengths, length_random);
            if (!sampled) return std::unexpected(sampled.error());
            result.symbols = *sampled;
        }
        if (parameters.kind == request_kind::header) {
            // Includes temporary component, formatted string and PostgreSQL text.
            result.bytes = result.symbols * 4u + 1024u;
            return result;
        }
        if (pgseq_is_cigar_alphabet(layout.alphabet_code)) {
            generation::counting_cigar_sink<bioseq::cigar_symbol> counter;
            random = generation::seed_stream(result.seed, 0u);
            const auto counted = parameters.kind == request_kind::mutation
                ? generation::mutate_cigar_runs<bioseq::cigar_symbol>(
                    pg_cigar_run_cursor{*session, cigar_source.root(), 0u, cigar_source.bp_length()},
                    layout.space, parameters.mutation, random, counter)
                : generation::generate_cigar<bioseq::cigar_symbol>(
                    result.symbols, layout.space, random, counter);
            if (!counted) return std::unexpected(counted.error());
            result.symbols = counted->output_symbols;
            result.runs = counter.size();
            if (result.runs > std::numeric_limits<std::uint32_t>::max()
                || result.symbols > std::numeric_limits<std::uint32_t>::max())
                return std::unexpected(generation::generation_error::size_overflow);
            const auto payload = bioseq::measure_cigar_payload(static_cast<std::uint32_t>(result.runs));
            if (!payload || result.runs > MaxAllocSize / sizeof(bioseq::cigar_run)
                || *payload > MaxAllocSize - bioseq::pg_types::seq::header_bytes()) {
                return std::unexpected(generation::generation_error::size_overflow);
            }
            result.bytes = *payload + result.runs * sizeof(bioseq::cigar_run)
                + bioseq::pg_types::seq::header_bytes() + 256u;
            return result;
        }
        std::expected<measured_row, generation::generation_error> measured =
            std::unexpected(generation::generation_error::unsupported_layout);
        pgseq_dispatch_rank_packed_instance(layout.alphabet_code, layout.storage_unit_byte_width,
            [this, result, random, &measured]<class Instance>() {
                measured = measure_packed<Instance>(result, random);
            });
        return measured;
    }

    /** Measure one rank-packed layout without allocating destination words. */
    template<class Instance>
    [[nodiscard]] std::expected<measured_row, generation::generation_error> measure_packed(
        measured_row result, generation::taus_lcg_state random) {
        using traits = typename compact_sequence_range<Instance>::traits;
        if (parameters.kind == request_kind::mutation) {
            generation::counting_rank_sink<Instance> counter;
            const auto counted = generation::mutate_rank_windows<typename traits::alphabet_type>(
                rank_source, 0u, source_descriptor.bp_length, parameters.mutation, random, counter);
            if (!counted) return std::unexpected(counted.error());
            result.symbols = counted->output_symbols;
        }
        const auto units = result.symbols / traits::letters_per_unit
            + static_cast<std::uint64_t>(result.symbols % traits::letters_per_unit != 0u);
        if (result.symbols > std::numeric_limits<std::uint32_t>::max()
            || units > (MaxAllocSize - bioseq::pg_types::seq::header_bytes() - 8u)
                / sizeof(typename traits::storage_unit)) {
            return std::unexpected(generation::generation_error::size_overflow);
        }
        result.bytes = bioseq::pg_types::seq::header_bytes()
            + ((units * sizeof(typename traits::storage_unit) + 7u) & ~std::uint64_t{7u}) + 256u;
        return result;
    }

    /** Allocate and initialize flat output using the established native allocator. */
    template<class Instance>
    [[nodiscard]] static auto allocate_packed(std::uint64_t symbols) {
        using traits = typename compact_sequence_range<Instance>::traits;
        using unit = typename traits::storage_unit;
        using alphabet = typename traits::alphabet_type;
        const auto units = symbols / traits::letters_per_unit
            + static_cast<std::uint64_t>(symbols % traits::letters_per_unit != 0u);
        auto* value = support::invoke_pg_as_cpp([symbols, units] {
            auto* output = bioseq::pg_types::seq::template reserve<alphabet>(sizeof(unit),
                static_cast<std::uint32_t>(units), pg_alloc);
            if (output == nullptr) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY),
                errmsg("could not allocate random sequence output")));
            output->bp_size = static_cast<bioseq::pg_types::seq::bp_size_t>(symbols);
            output->padding_letters = static_cast<std::uint16_t>(units * traits::letters_per_unit - symbols);
            return output;
        });
        return std::pair{value, std::span<unit>{reinterpret_cast<unit*>(value->data),
            static_cast<std::size_t>(units)}};
    }

    /** Emit a measured CIGAR with canonical coalescing and no expanded coordinate array. */
    [[nodiscard]] Datum emit_cigar(measured_row const& row) {
        auto* storage = static_cast<bioseq::cigar_run*>(support::invoke_pg_as_cpp([&row] {
            return palloc(std::max<std::size_t>(1u, row.runs) * sizeof(bioseq::cigar_run));
        }));
        bioseq::cigar_run_span_sink<bioseq::cigar_symbol> sink{{storage, row.runs}};
        auto random = generation::seed_stream(row.seed, 0u);
        const auto emitted = parameters.kind == request_kind::mutation
            ? generation::mutate_cigar_runs<bioseq::cigar_symbol>(
                pg_cigar_run_cursor{*session, cigar_source.root(), 0u, cigar_source.bp_length()},
                layout.space, parameters.mutation, random, sink)
            : generation::generate_cigar<bioseq::cigar_symbol>(row.symbols, layout.space, random, sink);
        if (!emitted) fail_generation(emitted.error());
        if (sink.used != row.runs || emitted->output_symbols != row.symbols)
            fail_generation(generation::generation_error::output_contract);
        const Datum result = support::invoke_pg_as_cpp([this, storage, &row] {
            return PointerGetDatum(make_pg_cigar_from_runs({storage, row.runs}, layout.space));
        });
        pfree(storage);
        return result;
    }

    /** Emit bounded ASCII components through the existing pure structured-header writer. */
    [[nodiscard]] Datum emit_header(measured_row const& row, std::uint64_t ordinal) {
        constexpr std::string_view letters = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        auto random = generation::seed_stream(row.seed, 0u);
        std::string component(static_cast<std::size_t>(row.symbols), 'A');
        for (auto& symbol : component) {
            symbol = letters[generation::next_below64(random, letters.size())];
        }
        const auto formatted = identifiers::make_synthetic_header(format, component, ordinal);
        return support::invoke_pg_as_cpp([&formatted] {
            return PointerGetDatum(cstring_to_text_with_len(formatted.data(), static_cast<int>(formatted.size())));
        });
    }

    /** Drop unconsumed speculation; the consumed ordinal deliberately survives rescans. */
    void discard() {
        outputs = {};
        output_errors = {};
        next_output = 0u;
        support::invoke_pg_as_cpp([this] { MemoryContextReset(output_context); });
        admission = {};
    }

    /** Measure a bounded prefix, admit its real storage, then fill native destinations. */
    void refill(std::size_t demand_bound) {
        initialize();
        discard();
        if (null_source) return;
        constexpr std::size_t ceiling = 4096u;
        // The scalar path reserves one small stack block without zeroing a
        // maximum-sized batch. Larger measurement arrays hold their own shared
        // permit before allocation and are released after synchronous filling.
        std::array<measured_row, 32u> inline_measurements;
        auto requested = std::max<std::size_t>(1u, std::min({capacity, ceiling, demand_bound}));
        bioseq::parallelism::budgeted_wave measurement_admission;
        std::vector<measured_row> larger_measurements;
        std::span<measured_row> measured{inline_measurements.data(), std::min(requested, inline_measurements.size())};
        if (requested > inline_measurements.size()) {
            measurement_admission = bioseq::parallelism::try_reserve_budgeted_wave(
                1u, requested * sizeof(measured_row));
            if (measurement_admission.is_admitted) {
                larger_measurements.resize(requested);
                measured = larger_measurements;
            } else {
                requested = inline_measurements.size();
                capacity = requested;
            }
        }
        std::size_t count{};
        std::uint64_t bytes{ALLOCSET_SMALL_INITSIZE};
        constexpr auto ordinal_limit = std::numeric_limits<std::uint64_t>::max();
        for (; count < requested; ++count) {
            if (count >= ordinal_limit - stream.ordinal) {
                if (count == 0u) fail_generation(generation::generation_error::size_overflow);
                break;
            }
            const auto row = measure(stream.ordinal + count);
            if (!row) {
                if (count == 0u) fail_generation(row.error());
                break; // A speculative row's error is raised only when demanded.
            }
            if (count != 0u && bytes + row->bytes > 256u * 1024u) break;
            measured[count] = *row;
            bytes += row->bytes + sizeof(Datum);
        }
        for (;;) {
            admission = bioseq::parallelism::try_reserve_budgeted_wave(1u, bytes);
            if (admission.is_admitted) break;
            if (count == 1u) fail_generation(generation::generation_error::allocation_rejected);
            --count;
            bytes -= measured[count].bytes + sizeof(Datum);
        }
        support::invoke_pg_as_cpp([this, count] {
            const auto previous = MemoryContextSwitchTo(output_context);
            outputs = {static_cast<Datum*>(palloc(count * sizeof(Datum))), count};
            using pending_error = std::optional<generation::generation_error>;
            output_errors = {static_cast<pending_error*>(palloc(count * sizeof(pending_error))), count};
            for (auto& error : output_errors) std::construct_at(&error);
            MemoryContextSwitchTo(previous);
        });
        support::memory_context_scope output_scope{output_context};
        {
            if (parameters.kind == request_kind::header) {
                for (std::size_t index = 0u; index < count; ++index)
                    outputs[index] = emit_header(measured[index], stream.ordinal + index);
            } else if (pgseq_is_cigar_alphabet(layout.alphabet_code)) {
                for (std::size_t index = 0u; index < count; ++index) outputs[index] = emit_cigar(measured[index]);
            } else {
                pgseq_dispatch_rank_packed_instance(layout.alphabet_code, layout.storage_unit_byte_width,
                    [this, count, &measured]<class Instance>() {
                        using traits = typename compact_sequence_range<Instance>::traits;
                        using alphabet = typename traits::alphabet_type;
                        std::vector<generation::random_packed_destination<traits>> destinations;
                        destinations.reserve(count);
                        for (std::size_t index = 0u; index < count; ++index) {
                            const auto [value, units] = allocate_packed<Instance>(measured[index].symbols);
                            outputs[index] = PointerGetDatum(value);
                            destinations.push_back({.units = units, .symbol_count = measured[index].symbols,
                                .sequence_index = 0u, .first_output_unit = 0u, .row_seed = measured[index].seed});
                        }
                        if (parameters.kind == request_kind::sequence) {
                            constexpr auto cdf = generation::make_integer_cdf(
                                generation::base_alphabet_probability<alphabet>::probability());
                            const auto generated = generation::generate_packed_batch<traits>(destinations, cdf, 0u);
                            if (!generated) fail_generation(generated.error());
                        } else {
                            const auto emit_rows = [this, &destinations, &measured](std::size_t first, std::size_t last) {
                                for (auto index = first; index < last; ++index) {
                                    using sink = bioseq::sequences::fixed_rank_pack_unit_sink<typename traits::storage_unit>;
                                    bioseq::sequences::checked_rank_pack_sink<Instance,
                                        bioseq::sequences::invalid_rank_policy::reject,
                                        bioseq::sequences::pack_capacity_policy::fixed_span, sink> writer{sink{destinations[index].units}};
                                    auto random = generation::seed_stream(measured[index].seed, 0u);
                                    const auto generated = generation::mutate_rank_windows<alphabet>(rank_source, 0u,
                                        source_descriptor.bp_length, parameters.mutation, random, writer);
                                    if (!generated) output_errors[index] = generated.error();
                                    else if (!writer.finish(measured[index].symbols, destinations[index].units.size()))
                                        output_errors[index] = generation::generation_error::output_contract;
                                }
                            };
                            if (count > 1u && count * source_descriptor.bp_length >= 64u * 1024u
                                && rank_source.worker_shareable
                                && source_descriptor.kind == pg_sequence_datum_kind::flat_bound) {
                                bioseq::parallelism::global_pool::instance().parallel_for(
                                    0u, count, 8u, bioseq::parallelism::range_body{emit_rows});
                            } else emit_rows(0u, count);
                        }
                    });
            }
        }
        ++batches;
        capacity = count == requested ? std::min(ceiling, capacity * 2u) : count;
    }

    /** Consume exactly one output and advance only the externally observed ordinal. */
    [[nodiscard]] std::optional<Datum> next(std::size_t demand_bound) {
        initialize();
        if (stream.ordinal == std::numeric_limits<std::uint64_t>::max())
            fail_generation(generation::generation_error::size_overflow);
        if (null_source) { ++stream.ordinal; return std::nullopt; }
        if (next_output == outputs.size()) refill(demand_bound);
        if (output_errors[next_output]) fail_generation(*output_errors[next_output]);
        ++stream.ordinal;
        return outputs[next_output++];
    }
};

/** Destroy admitted state even when PostgreSQL ERROR bypasses the C++ call stack. */
inline void release_batch_state(void* opaque) noexcept {
    std::destroy_at(static_cast<batch_state*>(opaque));
}

/** Create one site in an executor-owned context; all generated memory is its child. */
[[nodiscard]] inline batch_state* make_batch_state(request input, MemoryContext owner,
    stream_state stream, std::size_t capacity = 32u) {
    auto* state = static_cast<batch_state*>(support::invoke_pg_as_cpp([owner] {
        return MemoryContextAlloc(owner, sizeof(batch_state));
    }));
    std::construct_at(state);
    state->parameters = std::move(input);
    state->stream = stream;
    state->capacity = std::max<std::size_t>(1u, std::min<std::size_t>(4096u, capacity));
    state->cleanup = {.func = release_batch_state, .arg = state, .next = nullptr};
    MemoryContextRegisterResetCallback(owner, &state->cleanup);
    support::invoke_pg_as_cpp([state, owner] {
        state->output_context = AllocSetContextCreate(owner, "Bioseq random output", ALLOCSET_SMALL_SIZES);
    });
    return state;
}

/** Execute current SQL arguments using the same one-row kernel as CustomScan. */
[[nodiscard]] inline std::optional<Datum> generate_scalar(request input,
    support::function_call_context const& call) {
    auto*& extra = call.function_extra();
    if (extra == nullptr) {
        auto* stream = static_cast<stream_state*>(support::invoke_pg_as_cpp([&call] {
            return MemoryContextAllocZero(call.function_memory_context(), sizeof(stream_state));
        }));
        stream->entropy = pg_prng_uint64(&pg_global_prng_state);
        extra = stream;
    }
    auto& stream = *static_cast<stream_state*>(extra);
    auto* state = make_batch_state(std::move(input), CurrentMemoryContext, stream, 1u);
    auto result = state->next(1u);
    stream.ordinal = state->stream.ordinal;
    return result;
}

} // namespace bioseq::postgres::random
