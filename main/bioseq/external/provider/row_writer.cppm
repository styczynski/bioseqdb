module;

#include <concepts>
#include <expected>

export module bioseq.external.provider.row_writer;

import bioseq.external.provider.base;      // sink_control (re-exported), ids
import bioseq.external.provider.errors;    // external_error
import bioseq.external.provider.model;     // row views
import bioseq.external.provider.messages;  // provider_diagnostic_view

// DOC0372 — consumer-side row-writer boundary. Mirrors identifier_writer_ref:
// validate the concrete sink at compile time, erase it once with void* plus
// function pointers, keep the reference non-owning. The facade/PG executor drains
// owning messages, builds one transient view on the caller/backend thread, then
// invokes the writer. It is never passed to a worker or provider session.
export namespace bioseq::external::provider {

template <class W>
concept is_external_row_writer =
    requires(W& writer,
             const taxonomy_row_view& taxon,
             const catalog_row_view& record,
             const record_gene_row_view& gene,
             const record_location_row_view& location,
             const provider_diagnostic_view& diagnostic) {
        { writer.write_taxon(taxon) } -> std::same_as<sink_control>;
        { writer.write_record(record) } -> std::same_as<sink_control>;
        { writer.write_gene(gene) } -> std::same_as<sink_control>;
        { writer.write_location(location) } -> std::same_as<sink_control>;
        { writer.write_diagnostic(diagnostic) } -> std::same_as<sink_control>;
        { writer.finish() } -> std::same_as<std::expected<void, external_error>>;
    };

struct external_row_writer_ref {
    void* object{};
    sink_control (*taxon_fn)(void*, const taxonomy_row_view&){};
    sink_control (*record_fn)(void*, const catalog_row_view&){};
    sink_control (*gene_fn)(void*, const record_gene_row_view&){};
    sink_control (*location_fn)(void*, const record_location_row_view&){};
    sink_control (*diagnostic_fn)(void*, const provider_diagnostic_view&){};
    std::expected<void, external_error> (*finish_fn)(void*){};

    [[nodiscard]] sink_control write_taxon(const taxonomy_row_view& row) const {
        return taxon_fn(object, row);
    }
    [[nodiscard]] sink_control write_record(const catalog_row_view& row) const {
        return record_fn(object, row);
    }
    [[nodiscard]] sink_control write_gene(const record_gene_row_view& row) const {
        return gene_fn(object, row);
    }
    [[nodiscard]] sink_control write_location(
        const record_location_row_view& row) const {
        return location_fn(object, row);
    }
    [[nodiscard]] sink_control write_diagnostic(
        const provider_diagnostic_view& row) const {
        return diagnostic_fn(object, row);
    }
    [[nodiscard]] std::expected<void, external_error> finish() const {
        return finish_fn(object);
    }
};

template <is_external_row_writer W>
[[nodiscard]] external_row_writer_ref make_external_row_writer_ref(
    W& writer) noexcept {
    return external_row_writer_ref{
        .object = &writer,
        .taxon_fn = [](void* self, const taxonomy_row_view& row) {
            return static_cast<W*>(self)->write_taxon(row);
        },
        .record_fn = [](void* self, const catalog_row_view& row) {
            return static_cast<W*>(self)->write_record(row);
        },
        .gene_fn = [](void* self, const record_gene_row_view& row) {
            return static_cast<W*>(self)->write_gene(row);
        },
        .location_fn = [](void* self, const record_location_row_view& row) {
            return static_cast<W*>(self)->write_location(row);
        },
        .diagnostic_fn = [](void* self, const provider_diagnostic_view& row) {
            return static_cast<W*>(self)->write_diagnostic(row);
        },
        .finish_fn = [](void* self) { return static_cast<W*>(self)->finish(); }};
}

}  // namespace bioseq::external::provider
