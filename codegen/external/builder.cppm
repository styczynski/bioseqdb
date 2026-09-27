/**
 * @file builder.cppm
 * @brief Regenerates changed API clients and removes output whose source was deleted.
 */
module;

#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module bioseq.codegen.external:builder;

import :annotation_parser;
import :artifact_handler;
import :incremental_state;
import :model;
import :openapi_parser;
import bioseq.codegen.support;
import bioseq.common.hashing;

namespace fs = std::filesystem;

namespace bioseq::codegen::external {

/// @brief Generated output waiting for all source files to pass validation.
struct pending_artifact {
    /// @brief Annotation name used to sort pending output.
    std::string annotation_name;
    /// @brief Output name and callback that writes its files.
    generated_artifact artifact;
};

/// @brief An output identified by its annotation name and output name.
struct annotation_output_identity {
    /// @brief Registered annotation spelling.
    std::string annotation_name;
    /// @brief Output name assigned by the annotation handler.
    std::string output_name;

    /// @brief Orders identities by annotation spelling and then output name.
    auto operator<=>(const annotation_output_identity&) const = default;
};

/// @brief Source file and annotation handler responsible for an output.
struct registered_annotation_output {
    /// @brief Handler that checks output files and writes CMake entries.
    const annotation_processor* processor{};
    /// @brief Relative annotated source path.
    std::string source_path;
    /// @brief Validated output path relative to the generator root.
    std::string owned_output_path;
};

/// @brief Source that owns a path in the generated output directory.
struct owned_output_owner {
    /// @brief Annotation and output identity claiming the path.
    annotation_output_identity identity;
    /// @brief Relative annotated source path used in collision diagnostics.
    std::string source_path;
};

/// @brief Output ownership indexed by output name, path and source file.
struct generation_inventory {
    /// @brief Outputs keyed by annotation and processor-derived name.
    std::map<annotation_output_identity, registered_annotation_output> outputs;
    /// @brief Generated paths and the source outputs that own them.
    std::map<std::string, owned_output_owner, std::less<>> owned_paths;
    /// @brief Annotated source paths mapped to their single output identity.
    std::map<std::string, annotation_output_identity, std::less<>> source_outputs;
};

/// @brief Parses an API specification and checks the generated client names.
/// @param request Annotation and its OpenAPI text.
/// @return Validated API description for the renderer.
[[nodiscard]] auto build_client_model(const annotation_request& request) -> generated_api {
    openapi_document document = parse_openapi(request.specification);
    const std::string client_name = request.source_path.stem().string();
    if (make_cpp_identifier(client_name) != client_name ||
        make_cpp_identifier(request.declared_type) != request.declared_type) {
        throw std::runtime_error(
            "API descriptor stem and annotated class must be simple C++ identifiers: " +
            request.source_path.string());
    }
    return generated_api{
        .client_name = client_name,
        .declared_type = request.declared_type,
        .document = std::move(document),
    };
}

/// @brief Prepares a callback that writes a validated client's files.
/// @param request API annotation.
/// @return Client output name and callback.
[[nodiscard]] auto prepare_client_artifact(const annotation_request& request)
    -> generated_artifact {
    generated_api api = build_client_model(request);
    const std::string output_name = api.client_name;
    return generated_artifact{
        .output_name = output_name,
        .emit_artifacts = [api = std::move(api)](const fs::path& output_root,
                                                 std::string_view compiler_id) -> void {
            emit_client_artifacts(api, output_root, compiler_id);
        },
    };
}

/// @brief Registers the annotation handlers supported by this generator.
/// @return Handlers indexed by attribute name.
[[nodiscard]] auto make_annotation_registry() -> annotation_registry {
    return annotation_registry{
        {"bioseq::api_client",
         annotation_processor{
             .prepare_artifact = &prepare_client_artifact,
             .derive_output_name = &derive_client_output_name,
             .derive_owned_output_path = &derive_client_owned_output_path,
             .has_complete_output = &has_complete_client_output,
             .append_build_inventory = &append_client_build_inventory,
         }},
    };
}

/// @brief Reads the external-client generator's command-line options.
/// @param argument_count Argument count including the executable name.
/// @param argument_values Process argument array.
/// @return Generator settings.
/// @throws std::runtime_error For missing or unknown options.
[[nodiscard]] auto parse_generation_options(int argument_count, char** argument_values)
    -> generation_options {
    if (argument_count < 1 || argument_values == nullptr || argument_values[0] == nullptr) {
        throw std::runtime_error("generator executable path is unavailable");
    }

    generation_options options{
        .generator_binary = fs::absolute(argument_values[0]),
    };
    for (int argument_index = 1; argument_index < argument_count; ++argument_index) {
        const std::string_view option = argument_values[argument_index];
        if (argument_index + 1 >= argument_count) {
            throw std::runtime_error("missing value for " + std::string(option));
        }
        const std::string option_value = argument_values[++argument_index];
        if (option == "--generator-binary") {
            options.generator_binary = fs::absolute(option_value);
        } else if (option == "--source-root") {
            options.source_root = option_value;
        } else if (option == "--output-root") {
            options.output_root = option_value;
        } else if (option == "--clang-binary") {
            options.clang_binary = option_value;
        } else if (option == "--compiler-id") {
            options.compiler_id = option_value;
        } else if (option == "--stamp") {
            options.stamp = option_value;
        } else {
            throw std::runtime_error("unknown argument: " + std::string(option));
        }
    }
    if (options.source_root.empty() || options.output_root.empty() ||
        options.clang_binary.empty() || options.compiler_id.empty() || options.stamp.empty()) {
        throw std::runtime_error("required arguments: --source-root --output-root --clang-binary "
                                 "--compiler-id --stamp");
    }
    return options;
}

/// @brief Finds module descriptors under the source directory.
/// @param source_root Directory to scan recursively.
/// @return Regular .cppm files sorted by path.
[[nodiscard]] auto discover_descriptor_sources(const fs::path& source_root)
    -> std::vector<fs::path> {
    std::vector<fs::path> sources;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(source_root)) {
        if (entry.is_regular_file() && entry.path().extension() == ".cppm") {
            sources.push_back(entry.path());
        }
    }
    std::ranges::sort(sources);
    return sources;
}

/// @brief Reads each descriptor and records its size, write time and hash.
/// @param source_root Directory used to resolve relative paths.
/// @return Source records sorted by path.
[[nodiscard]] auto load_descriptor_sources(const fs::path& source_root)
    -> std::vector<descriptor_source> {
    const std::vector<fs::path> paths = discover_descriptor_sources(source_root);
    std::vector<descriptor_source> sources;
    sources.reserve(paths.size());
    for (const fs::path& path : paths) {
        std::string text = read_artifact_bytes(path);
        const std::uintmax_t file_size_bytes = text.size();
        const std::uint64_t content_hash = bioseq::common::fnv1a_64(text);
        sources.push_back(descriptor_source{
            .path = path,
            .relative_path = fs::relative(path, source_root),
            .text = std::move(text),
            .file_size_bytes = file_size_bytes,
            .last_write_ticks =
                static_cast<std::int64_t>(fs::last_write_time(path).time_since_epoch().count()),
            .content_hash = content_hash,
        });
    }
    return sources;
}

/// @brief Finds a specification fragment's source among the loaded descriptors.
/// @param sources Loaded descriptors.
/// @param path Source path returned by annotation resolution.
/// @return Matching source record.
/// @throws std::runtime_error If the path was not discovered.
[[nodiscard]] auto find_descriptor_source(std::span<const descriptor_source> sources,
                                          const fs::path& path) -> const descriptor_source& {
    const auto source_position = std::ranges::find_if(
        sources, [&path](const descriptor_source& source) -> bool { return source.path == path; });
    if (source_position == sources.end()) {
        throw std::runtime_error(
            "annotation specification closure references an undiscovered source: " + path.string());
    }
    return *source_position;
}

/// @brief Records a descriptor and its imported fragments as one generation dependency.
/// @param primary Annotated source that owns the generated output.
/// @param dependency_paths Source files referenced by its fragment array.
/// @param sources Loaded descriptors.
/// @return Combined size, latest write time and a hash of the dependency records.
/// @details Each file contributes once. A change to any included file invalidates
/// the saved client output.
[[nodiscard]] auto make_specification_closure_snapshot(const descriptor_source& primary,
                                                       std::span<const fs::path> dependency_paths,
                                                       std::span<const descriptor_source> sources)
    -> source_snapshot {
    source_snapshot snapshot{
        .file_size_bytes = 0,
        .last_write_ticks = primary.last_write_ticks,
        .has_annotation = true,
    };
    std::ostringstream closure;
    closure << "bioseq-external-specification-closure-v1\n";
    std::set<std::string, std::less<>> included_paths;

    const auto include_source =
        [&snapshot, &closure, &included_paths](const descriptor_source& source) -> void {
        const std::string relative_path = source.relative_path.generic_string();
        if (!included_paths.insert(relative_path).second) {
            return;
        }
        if (std::numeric_limits<std::uintmax_t>::max() - snapshot.file_size_bytes <
            source.file_size_bytes) {
            throw std::runtime_error("annotation specification closure size overflow for " +
                                     relative_path);
        }
        snapshot.file_size_bytes += source.file_size_bytes;
        snapshot.last_write_ticks = std::max(snapshot.last_write_ticks, source.last_write_ticks);
        closure << relative_path.size() << ':' << relative_path << '\n'
                << source.file_size_bytes << '\n'
                << source.last_write_ticks << '\n'
                << std::hex << source.content_hash << std::dec << '\n';
    };

    include_source(primary);
    for (const fs::path& dependency_path : dependency_paths) {
        include_source(find_descriptor_source(sources, dependency_path));
    }
    snapshot.content_hash = bioseq::common::fnv1a_64(closure.str());
    return snapshot;
}

/// @brief Records which source owns a generated output and rejects conflicting owners.
/// @param registry Annotation handlers.
/// @param inventory Output ownership collected during this run.
/// @param annotation_name Registered annotation name.
/// @param output_name Name assigned by the handler.
/// @param relative_source Annotated source path relative to the scan root.
/// @return Validated output path relative to the generator root.
/// @throws std::runtime_error For an unknown annotation or conflicting ownership.
[[nodiscard]] auto register_annotation_output(const annotation_registry& registry,
                                              generation_inventory& inventory,
                                              std::string_view annotation_name,
                                              std::string_view output_name,
                                              std::string_view relative_source) -> std::string {
    const auto processor_position = registry.find(annotation_name);
    if (processor_position == registry.end()) {
        throw std::runtime_error("source state references unregistered annotation '" +
                                 std::string(annotation_name) +
                                 "': " + std::string(relative_source));
    }
    if (output_name.empty() || output_name.find_first_of("\t\r\n") != std::string_view::npos) {
        throw std::runtime_error("annotation processor produced an invalid output name for " +
                                 std::string(relative_source));
    }

    const fs::path owned_path = validate_owned_artifact_path(
        processor_position->second.derive_owned_output_path(output_name),
        artifact_owner_view{relative_source});
    const std::string owned_path_text = owned_path.generic_string();
    const annotation_output_identity identity{
        .annotation_name = std::string(annotation_name),
        .output_name = std::string(output_name),
    };

    const auto [path_owner_position, owns_path] =
        inventory.owned_paths.emplace(owned_path_text,
                                      owned_output_owner{
                                          .identity = identity,
                                          .source_path = std::string(relative_source),
                                      });
    if (!owns_path && path_owner_position->second.identity != identity) {
        throw std::runtime_error("annotation outputs collide at '" + owned_path_text +
                                 "': " + path_owner_position->second.source_path + " and " +
                                 std::string(relative_source));
    }

    const auto [output_position, output_inserted] =
        inventory.outputs.emplace(identity,
                                  registered_annotation_output{
                                      .processor = &processor_position->second,
                                      .source_path = std::string(relative_source),
                                      .owned_output_path = owned_path_text,
                                  });
    if (!output_inserted && output_position->second.source_path != relative_source) {
        throw std::runtime_error(
            "annotated sources produce duplicate output '" + std::string(output_name) + "' for " +
            std::string(annotation_name) + ": " + output_position->second.source_path + " and " +
            std::string(relative_source));
    }

    const auto [source_position, source_inserted] =
        inventory.source_outputs.emplace(std::string(relative_source), identity);
    if (!source_inserted && source_position->second != identity) {
        throw std::runtime_error("source contains multiple registered annotations: " +
                                 std::string(relative_source));
    }
    return owned_path_text;
}

/// @brief Checks that a saved annotation still has all its generated and parser files.
/// @param snapshot Saved source record.
/// @param relative_source Source path relative to the scan root.
/// @param options Generator settings.
/// @param registry Annotation handlers.
/// @return True if the handler still exists and every required output is present.
[[nodiscard]] auto has_reusable_annotation_output(const source_snapshot& snapshot,
                                                  const fs::path& relative_source,
                                                  const generation_options& options,
                                                  const annotation_registry& registry) -> bool {
    if (!snapshot.has_annotation || snapshot.owned_output_path.empty()) {
        return false;
    }
    const auto processor_position = registry.find(snapshot.annotation_name);
    return processor_position != registry.end() &&
           processor_position->second.has_complete_output(options.output_root,
                                                          snapshot.output_name) &&
           has_staged_parser_sources(options.output_root, relative_source);
}

/// @brief Checks whether unchanged source can keep its generated output.
/// @param snapshot Saved source record.
/// @param content_hash Current hash of the source or its specification dependencies.
/// @param relative_source Source path relative to the scan root.
/// @param options Generator settings.
/// @param registry Annotation handlers.
/// @return True if the hashes match and all required files still exist.
[[nodiscard]] auto can_reuse_source_content(const source_snapshot& snapshot,
                                            std::uint64_t content_hash,
                                            const fs::path& relative_source,
                                            const generation_options& options,
                                            const annotation_registry& registry) -> bool {
    return snapshot.has_annotation && snapshot.content_hash == content_hash &&
           has_reusable_annotation_output(snapshot, relative_source, options, registry);
}

/// @brief Deletes output and parser copies that current sources no longer own.
/// @param stored_snapshots Valid records from the previous state file.
/// @param inventory Current output ownership.
/// @param options Generator settings.
auto remove_stale_artifacts(const source_snapshots& stored_snapshots,
                            const generation_inventory& inventory,
                            const generation_options& options) -> void {
    for (const auto& snapshot_entry : stored_snapshots) {
        const std::string& relative_source = snapshot_entry.first;
        const source_snapshot& previous_snapshot = snapshot_entry.second;
        if (!previous_snapshot.has_annotation) {
            continue;
        }

        const annotation_output_identity identity{
            .annotation_name = previous_snapshot.annotation_name,
            .output_name = previous_snapshot.output_name,
        };
        const auto current_output = inventory.outputs.find(identity);
        const fs::path previous_owned_path = validate_owned_artifact_path(
            previous_snapshot.owned_output_path, artifact_owner_view{relative_source});
        const bool path_is_current =
            current_output != inventory.outputs.end() &&
            current_output->second.owned_output_path == previous_owned_path.generic_string();
        if (!path_is_current) {
            fs::remove_all(options.output_root / previous_owned_path);
        }

        if (inventory.source_outputs.contains(relative_source)) {
            continue;
        }
        const fs::path relative_path(relative_source);
        fs::remove(options.output_root / "annotation_sources" / relative_path);
        fs::path parser_relative = relative_path;
        parser_relative.replace_extension(".shadow.cpp");
        fs::remove(options.output_root / "parser_sources" / parser_relative);
    }
}

/// @brief Writes prepared output, the CMake build list and the new source state.
/// @param pending_artifacts Generated files waiting to be written.
/// @param inventory All current outputs, including reused ones.
/// @param state_path Path for the saved source state.
/// @param state_bytes New state file contents.
/// @param options Generator settings.
/// @details The completion stamp is updated after all files have been written.
auto publish_generation_results(const std::vector<pending_artifact>& pending_artifacts,
                                const generation_inventory& inventory,
                                const fs::path& state_path,
                                std::string_view state_bytes,
                                const generation_options& options) -> void {
    std::ostringstream build_inventory;
    build_inventory << "# Generated external code inventory.\n";

    for (const pending_artifact& pending : pending_artifacts) {
        pending.artifact.emit_artifacts(options.output_root, options.compiler_id);
    }
    for (const auto& output_entry : inventory.outputs) {
        const annotation_output_identity& identity = output_entry.first;
        const registered_annotation_output& registered_output = output_entry.second;
        registered_output.processor->append_build_inventory(build_inventory, identity.output_name);
    }

    publish_artifact_if_changed(state_path, state_bytes);
    publish_artifact_if_changed(options.output_root / "generated_apis.cmake",
                                build_inventory.str());
    mark_artifact_complete(options.stamp);
}

/// @brief Generates clients whose source or required output has changed.
/// @param options Validated generator settings.
/// @return 0 after generation succeeds.
/// @details Specification fragments also count as dependencies. The builder checks
/// output ownership before publishing client files and removes obsolete output
/// before saving the new state.
[[nodiscard]] auto generate_external_clients(const generation_options& options) -> int {
    const annotation_registry registry = make_annotation_registry();
    const std::vector<descriptor_source> sources = load_descriptor_sources(options.source_root);
    const module_provider_index providers = build_module_provider_index(sources);

    std::vector<pending_artifact> pending_artifacts;
    generation_inventory inventory;
    std::ostringstream state_output;
    const fs::path state_path = options.output_root / "generation_state.tsv";
    const std::string signature = compute_generation_state_signature(options, registry);
    state_output << "#signature\t" << signature << '\n';

    const loaded_incremental_state loaded_state = load_incremental_state(state_path, signature);
    const source_snapshots empty_snapshots;
    const source_snapshots& previous_snapshots =
        loaded_state.is_reusable ? loaded_state.snapshots : empty_snapshots;

    for (const descriptor_source& source : sources) {
        const fs::path& source_path = source.path;
        const fs::path& relative_path = source.relative_path;
        const std::string relative_source = relative_path.generic_string();
        const auto previous_position = previous_snapshots.find(relative_source);
        const bool source_has_annotation = has_registered_annotation(source.text, registry);
        const bool source_has_fragments = has_fragment_annotation(source.text);

        if (source_has_fragments && !source_has_annotation) {
            throw std::runtime_error(
                "api_client_fragments requires api_client on the same declaration: " +
                source_path.string());
        }

        source_snapshot current_snapshot{
            .file_size_bytes = source.file_size_bytes,
            .last_write_ticks = source.last_write_ticks,
            .content_hash = source.content_hash,
            .has_annotation = source_has_annotation,
        };
        if (!current_snapshot.has_annotation) {
            append_source_snapshot(state_output, relative_source, current_snapshot);
            continue;
        }

        if (!source_has_fragments && previous_position != previous_snapshots.end() &&
            can_reuse_source_content(previous_position->second,
                                     current_snapshot.content_hash,
                                     relative_path,
                                     options,
                                     registry)) {
            const source_snapshot& previous_snapshot = previous_position->second;
            current_snapshot.annotation_name = previous_snapshot.annotation_name;
            current_snapshot.output_name = previous_snapshot.output_name;
            current_snapshot.owned_output_path =
                register_annotation_output(registry,
                                           inventory,
                                           current_snapshot.annotation_name,
                                           current_snapshot.output_name,
                                           relative_source);
            append_source_snapshot(state_output, relative_source, current_snapshot);
            continue;
        }

        publish_artifact_if_changed(options.output_root / "annotation_sources" / relative_path,
                                    source.text);
        fs::path parser_relative = relative_path;
        parser_relative.replace_extension(".shadow.cpp");
        const fs::path parser_source_path =
            options.output_root / "parser_sources" / parser_relative;
        publish_artifact_if_changed(parser_source_path,
                                    prepare_annotation_parser_translation_unit(source.text));

        const annotation_request request = parse_source_annotation(source_path,
                                                                   source.text,
                                                                   parser_source_path,
                                                                   options.clang_binary,
                                                                   registry,
                                                                   providers);
        if (!request.specification_dependencies.empty()) {
            current_snapshot = make_specification_closure_snapshot(
                source, request.specification_dependencies, sources);
        }
        const auto processor_position = registry.find(request.annotation_name);
        if (processor_position == registry.end()) {
            throw std::runtime_error("parser returned unregistered annotation '" +
                                     request.annotation_name + "'");
        }

        current_snapshot.annotation_name = request.annotation_name;
        current_snapshot.output_name = processor_position->second.derive_output_name(source_path);
        current_snapshot.owned_output_path =
            register_annotation_output(registry,
                                       inventory,
                                       current_snapshot.annotation_name,
                                       current_snapshot.output_name,
                                       relative_source);

        const bool cached_identity_matches =
            previous_position != previous_snapshots.end() &&
            previous_position->second.annotation_name == current_snapshot.annotation_name &&
            previous_position->second.output_name == current_snapshot.output_name &&
            previous_position->second.owned_output_path == current_snapshot.owned_output_path;
        if (cached_identity_matches && can_reuse_source_content(previous_position->second,
                                                                current_snapshot.content_hash,
                                                                relative_path,
                                                                options,
                                                                registry)) {
            append_source_snapshot(state_output, relative_source, current_snapshot);
            continue;
        }

        generated_artifact artifact = processor_position->second.prepare_artifact(request);
        if (artifact.output_name != current_snapshot.output_name) {
            throw std::runtime_error("annotation processor output name mismatch for " +
                                     source_path.string() + ": expected '" +
                                     current_snapshot.output_name + "', generated '" +
                                     artifact.output_name + "'");
        }
        if (!artifact.emit_artifacts) {
            throw std::runtime_error("annotation processor returned no artifact emitter for " +
                                     source_path.string());
        }
        pending_artifacts.push_back(pending_artifact{
            .annotation_name = request.annotation_name,
            .artifact = std::move(artifact),
        });
        append_source_snapshot(state_output, relative_source, current_snapshot);
    }

    std::ranges::sort(pending_artifacts,
                      [](const pending_artifact& left, const pending_artifact& right) -> bool {
                          if (left.annotation_name != right.annotation_name) {
                              return left.annotation_name < right.annotation_name;
                          }
                          return left.artifact.output_name < right.artifact.output_name;
                      });

    remove_stale_artifacts(loaded_state.snapshots, inventory, options);
    publish_generation_results(
        pending_artifacts, inventory, state_path, state_output.str(), options);
    return 0;
}

} // namespace bioseq::codegen::external
