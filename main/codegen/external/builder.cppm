module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

module bioseq.codegen.external:builder;

import :annotation_parser;
import :openapi_parser;
import :client_renderer;
import :artifact_handler;
import :incremental_state;
import :model;
import bioseq.codegen.support;
import bioseq.common.hashing;

namespace fs = std::filesystem;

namespace bioseq::codegen::external {

/// @brief Lowers an API annotation into renderer-ready metadata.
/// @param request Resolved annotation and embedded specification.
/// @return Validated generated API model.
[[nodiscard]] generated_api generate_api_client(const annotation_request& request) {
    auto document = parse_openapi(request.spec);
    const std::string client_name = request.source.stem().string();
    if (identifier(client_name) != client_name || identifier(request.declared_type) != request.declared_type) {
        throw std::runtime_error(
            "API descriptor stem and annotated class must be simple C++ identifiers: "
            + request.source.string());
    }
    return generated_api{
        .client_name = client_name,
        .declared_type = request.declared_type,
        .document = std::move(document),
    };
}

/// @brief Wraps API metadata in the registry's type-erased emitter.
/// @param request Resolved API annotation.
/// @return Named deferred output emitter.
[[nodiscard]] generated_output generate_api_output(const annotation_request& request) {
    generated_api api = generate_api_client(request);
    const std::string name = api.client_name;
    return generated_output{
        .name = name,
        .emit = [api = std::move(api)](
                    const fs::path& output_root,
                    std::string_view compiler_id) {
            emit_api(api, output_root, compiler_id);
        },
    };
}

/// @brief Parses and validates generator command-line options.
/// @param argc Argument count.
/// @param argv Argument vector.
/// @return Normalized command configuration.
[[nodiscard]] command_line parse_command_line(int argc, char** argv) {
    command_line result{.generator_binary = fs::absolute(argv[0])};
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (index + 1 >= argc) {
            throw std::runtime_error("missing value for " + std::string(argument));
        }
        const std::string value = argv[++index];
        if (argument == "--source-root") result.source_root = value;
        else if (argument == "--output-root") result.output_root = value;
        else if (argument == "--clang-binary") result.clang_binary = value;
        else if (argument == "--compiler-id") result.compiler_id = value;
        else if (argument == "--stamp") result.stamp = value;
        else throw std::runtime_error("unknown argument: " + std::string(argument));
    }
    if (result.source_root.empty() || result.output_root.empty()
        || result.clang_binary.empty() || result.compiler_id.empty()
        || result.stamp.empty()) {
        throw std::runtime_error(
            "required arguments: --source-root --output-root --clang-binary "
            "--compiler-id --stamp");
    }
    return result;
}

/// @brief Scans descriptors, incrementally emits outputs, and removes stale artifacts.
/// @param command Validated generator configuration.
/// @return Zero after successful generation.
[[nodiscard]] int execute(const command_line& command) {
    const annotation_handlers handlers{
        {"bioseq::api_client", annotation_handler{
            .generate = &generate_api_output,
            .output_name = &api_output_name,
            .owned_output_path = &api_owned_output_path,
            .outputs_exist = &api_outputs_exist,
            .append_cmake_inventory = &append_api_cmake_inventory,
        }},
    };
    std::vector<fs::path> sources;
    for (const auto& entry : fs::recursive_directory_iterator(command.source_root)) {
        if (entry.is_regular_file() && entry.path().extension() == ".cppm") {
            sources.push_back(entry.path());
        }
    }
    std::ranges::sort(sources);

    struct pending_output {
        std::string annotation_name;
        generated_output output;
    };
    struct registered_output {
        const annotation_handler* handler{};
        std::string annotation_name;
        std::string name;
        std::string source;
        std::string owned_output_path;
    };
    std::vector<pending_output> generated;
    std::map<std::string, registered_output> output_inventory;
    std::map<std::string, std::pair<std::string, std::string>> owned_output_owners;
    std::map<std::string, std::string> source_outputs;
    std::ostringstream state;
    const fs::path state_path = command.output_root / "generation_state.tsv";
    const std::string signature = state_signature(command, handlers);
    state << "#signature\t" << signature << '\n';
    bool state_cache_valid = false;
    const auto stored_state = load_source_state(state_path, signature, state_cache_valid);
    const std::unordered_map<std::string, source_state> empty_state;
    const auto& previous_state = state_cache_valid ? stored_state : empty_state;
    const auto output_key = [](std::string_view annotation_name, std::string_view output_name) {
        std::string key(annotation_name);
        key.push_back('\n');
        key += output_name;
        return key;
    };
    const auto register_output = [
                                     &handlers,
                                     &owned_output_owners,
                                     &output_inventory,
                                     &output_key,
                                     &source_outputs](
                                     std::string_view annotation_name,
                                     std::string_view name,
                                     const std::string& relative) {
        const auto handler = handlers.find(std::string(annotation_name));
        if (handler == handlers.end()) {
            throw std::runtime_error(
                "source state references unregistered annotation '"
                + std::string(annotation_name) + "': " + relative);
        }
        if (name.empty() || name.find_first_of("\t\r\n") != std::string_view::npos) {
            throw std::runtime_error(
                "annotation handler produced an invalid output name for " + relative);
        }
        const std::string owned_output_path = checked_owned_output_path(
            handler->second.owned_output_path(name), relative).generic_string();
        const std::string key = output_key(annotation_name, name);
        const auto [owner, owns_path] = owned_output_owners.emplace(
            owned_output_path, std::pair{key, relative});
        if (!owns_path && owner->second.first != key) {
            throw std::runtime_error(
                "annotation outputs collide at '" + owned_output_path + "': "
                + owner->second.second + " and " + relative);
        }
        const auto [position, inserted] = output_inventory.emplace(
            key,
            registered_output{
                .handler = &handler->second,
                .annotation_name = std::string(annotation_name),
                .name = std::string(name),
                .source = relative,
                .owned_output_path = owned_output_path,
            });
        if (!inserted && position->second.source != relative) {
            throw std::runtime_error(
                "annotated sources produce duplicate output '" + std::string(name)
                + "' for " + std::string(annotation_name) + ": "
                + position->second.source + " and " + relative);
        }
        const auto [source_position, source_inserted] =
            source_outputs.emplace(relative, key);
        if (!source_inserted && source_position->second != key) {
            throw std::runtime_error(
                "source contains multiple registered annotations: " + relative);
        }
        return owned_output_path;
    };
    for (const auto& source : sources) {
        const fs::path relative_path = fs::relative(source, command.source_root);
        const std::string relative = relative_path.generic_string();
        const auto size = fs::file_size(source);
        const auto ticks = static_cast<std::int64_t>(
            fs::last_write_time(source).time_since_epoch().count());
        const auto previous = previous_state.find(relative);
        if (previous != previous_state.end()
            && previous->second.size == size && previous->second.ticks == ticks
            && (!previous->second.annotated
                || (handlers.contains(previous->second.annotation_name)
                    && !previous->second.owned_output_path.empty()
                    && handlers.at(previous->second.annotation_name).outputs_exist(
                        command.output_root, previous->second.output_name)
                    && staged_sources_exist(command.output_root, relative_path)))) {
            append_source_state(state, relative, previous->second);
            if (previous->second.annotated) {
                const std::string owned_output_path = register_output(
                    previous->second.annotation_name,
                    previous->second.output_name,
                    relative);
                if (owned_output_path != previous->second.owned_output_path) {
                    throw std::runtime_error(
                        "cached annotation output path changed without state invalidation: "
                        + relative);
                }
            }
            continue;
        }

        const std::string content = read_file(source);
        const bool annotated = contains_registered_annotation(content, handlers);
        source_state current{
            .size = size,
            .ticks = ticks,
            .hash = bioseq::common::fnv1a_64(content),
            .annotated = annotated,
        };
        if (!annotated) {
            append_source_state(state, relative, current);
            continue;
        }
        if (previous != previous_state.end()
            && previous->second.annotated && previous->second.hash == current.hash
            && handlers.contains(previous->second.annotation_name)
            && !previous->second.owned_output_path.empty()
            && handlers.at(previous->second.annotation_name).outputs_exist(
                command.output_root, previous->second.output_name)
            && staged_sources_exist(command.output_root, relative_path)) {
            current.annotation_name = previous->second.annotation_name;
            current.output_name = previous->second.output_name;
            current.owned_output_path = register_output(
                current.annotation_name, current.output_name, relative);
            append_source_state(state, relative, current);
            continue;
        }

        write_if_different(
            command.output_root / "annotation_sources" / relative_path, content);
        fs::path shadow_relative = relative_path;
        shadow_relative.replace_extension(".shadow.cpp");
        const fs::path shadow = command.output_root / "parser_sources" / shadow_relative;
        const std::string normalized = normalize_shadow(content);
        write_if_different(shadow, normalized);
        const auto request = parse_annotation(source, shadow, command.clang_binary, handlers);
        const auto handler = handlers.find(request.annotation_name);
        if (handler == handlers.end()) {
            throw std::runtime_error(
                "parser returned unregistered annotation '" + request.annotation_name + "'");
        }
        current.annotation_name = request.annotation_name;
        current.output_name = handler->second.output_name(source);
        current.owned_output_path = register_output(
            current.annotation_name, current.output_name, relative);
        generated_output output = handler->second.generate(request);
        if (output.name != current.output_name) {
            throw std::runtime_error(
                "annotation handler output name mismatch for " + source.string()
                + ": expected '" + current.output_name + "', generated '"
                + output.name + "'");
        }
        if (!output.emit) {
            throw std::runtime_error(
                "annotation handler returned no emitter for " + source.string());
        }
        generated.push_back(pending_output{
            .annotation_name = request.annotation_name,
            .output = std::move(output),
        });
        append_source_state(state, relative, current);
    }
    std::ranges::sort(generated, [](const pending_output& left, const pending_output& right) {
        if (left.annotation_name != right.annotation_name) {
            return left.annotation_name < right.annotation_name;
        }
        return left.output.name < right.output.name;
    });

    for (const auto& [relative, previous] : stored_state) {
        if (!previous.annotated) continue;
        const fs::path relative_path(relative);
        const std::string key = output_key(previous.annotation_name, previous.output_name);
        const auto current_output = output_inventory.find(key);
        const fs::path previous_owned_output_path = checked_owned_output_path(
            previous.owned_output_path, relative);
        const bool output_path_is_current = current_output != output_inventory.end()
            && current_output->second.owned_output_path
                == previous_owned_output_path.generic_string();
        if (!output_path_is_current) {
            fs::remove_all(command.output_root / previous_owned_output_path);
        }
        if (source_outputs.contains(relative)) {
            continue;
        }
        fs::remove(command.output_root / "annotation_sources" / relative_path);
        fs::path shadow_relative = relative_path;
        shadow_relative.replace_extension(".shadow.cpp");
        fs::remove(command.output_root / "parser_sources" / shadow_relative);
    }

    std::ostringstream cmake_manifest;
    cmake_manifest << "# Generated external code inventory.\n";
    for (const auto& pending : generated) {
        pending.output.emit(command.output_root, command.compiler_id);
    }
    for (const auto& [key, output] : output_inventory) {
        (void)key;
        output.handler->append_cmake_inventory(cmake_manifest, output.name);
    }
    write_if_different(state_path, state.str());
    write_if_different(command.output_root / "generated_apis.cmake", cmake_manifest.str());
    touch_stamp(command.stamp);
    return 0;
}

}
