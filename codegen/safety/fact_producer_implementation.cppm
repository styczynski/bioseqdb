module;

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/Mangle.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/StmtCXX.h>
#include <clang/AST/Type.h>
#include <clang/Analysis/CFG.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Basic/Version.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Index/USRGeneration.h>
#include <clang/Lex/Lexer.h>
#include <clang/Lex/MacroInfo.h>
#include <clang/Lex/PPCallbacks.h>
#include <clang/Lex/Preprocessor.h>
#include <clang/Tooling/ArgumentsAdjusters.h>
#include <clang/Tooling/CompilationDatabase.h>
#include <clang/Tooling/JSONCompilationDatabase.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

module bioseq.codegen.safety;

import :analyzer_model;
import :json;
import :manifest;
import :model;
import :serialization;
import :validation;
import bioseq.codegen.structural_analysis;
import bioseq.codegen.support;

namespace bioseq::codegen::safety {

namespace fact_detail {

namespace fs = std::filesystem;

[[noreturn]] void fact_failure(
    std::string_view context,
    std::string_view message) {
    throw std::runtime_error(
        "PostgreSQL compiler-fact producer rejected "
        + std::string{context} + ": " + std::string{message});
}

[[nodiscard]] std::string canonical_path(const fs::path& path) {
    std::error_code error;
    const fs::path canonical = fs::weakly_canonical(path, error);
    if (error || canonical.empty()) {
        fact_failure(path.string(), "cannot canonicalize path");
    }
    return canonical.generic_string();
}

[[nodiscard]] bool path_within(
    const fs::path& candidate,
    const fs::path& root) {
    const fs::path normalized_candidate =
        fs::weakly_canonical(candidate);
    const fs::path normalized_root = fs::weakly_canonical(root);
    auto candidate_part = normalized_candidate.begin();
    for (auto root_part = normalized_root.begin();
         root_part != normalized_root.end();
         ++root_part, ++candidate_part) {
        if (candidate_part == normalized_candidate.end()
            || *candidate_part != *root_part) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::string stable_usr(const clang::Decl* declaration) {
    if (declaration == nullptr) return {};
    llvm::SmallString<256> buffer;
    if (clang::index::generateUSRForDecl(declaration, buffer)) {
        return {};
    }
    return std::string{buffer};
}

[[nodiscard]] clang::SourceLocation file_location(
    clang::SourceLocation location,
    const clang::SourceManager& sources,
    bool spelling) {
    if (location.isInvalid()) return {};
    return spelling
        ? sources.getSpellingLoc(location)
        : sources.getExpansionLoc(location);
}

[[nodiscard]] source_position position_of(
    clang::SourceLocation location,
    const clang::SourceManager& sources,
    bool spelling) {
    const clang::SourceLocation file =
        file_location(location, sources, spelling);
    if (file.isInvalid() || !file.isFileID()) return {};
    return {
        .byte_offset =
            static_cast<std::size_t>(sources.getFileOffset(file)),
        .line = static_cast<std::size_t>(
            sources.getSpellingLineNumber(file)),
        .column = static_cast<std::size_t>(
            sources.getSpellingColumnNumber(file)),
    };
}

[[nodiscard]] bool same_source_position(
    const source_position& left,
    const source_position& right) noexcept {
    return left.byte_offset == right.byte_offset
        && left.line == right.line
        && left.column == right.column;
}

[[nodiscard]] bool same_source_range(
    const source_range& left,
    const source_range& right) noexcept {
    return same_source_position(left.begin, right.begin)
        && same_source_position(left.end, right.end);
}

[[nodiscard]] bool valid_source_range(
    const source_range& value) noexcept {
    return value.begin.line > 0 && value.begin.column > 0
        && value.end.line > 0 && value.end.column > 0
        && value.end.byte_offset >= value.begin.byte_offset
        && (value.end.line > value.begin.line
            || (value.end.line == value.begin.line
                && value.end.column >= value.begin.column));
}

[[nodiscard]] clang::SourceLocation token_end(
    clang::SourceLocation location,
    const clang::SourceManager& sources,
    const clang::LangOptions& language,
    bool spelling) {
    const clang::SourceLocation file =
        file_location(location, sources, spelling);
    if (file.isInvalid()) return {};
    return clang::Lexer::getLocForEndOfToken(
        file, 0, sources, language);
}

[[nodiscard]] source_range range_of(
    clang::SourceRange range,
    const clang::SourceManager& sources,
    const clang::LangOptions& language,
    bool spelling) {
    const clang::SourceLocation end =
        token_end(range.getEnd(), sources, language, spelling);
    return {
        .begin = position_of(
            range.getBegin(), sources, spelling),
        .end = position_of(
            end.isValid() ? end : range.getEnd(),
            sources,
            spelling),
    };
}

[[nodiscard]] std::string source_path_of(
    clang::SourceLocation location,
    const clang::SourceManager& sources,
    bool spelling) {
    const clang::SourceLocation file =
        file_location(location, sources, spelling);
    if (file.isInvalid()) return {};
    const llvm::StringRef filename = sources.getFilename(file);
    if (filename.empty()) return {};
    std::error_code error;
    const fs::path canonical =
        fs::weakly_canonical(fs::path{filename.str()}, error);
    return error
        ? fs::path{filename.str()}.lexically_normal().generic_string()
        : canonical.generic_string();
}

[[nodiscard]] located_source_range located_range_of(
    clang::SourceRange range,
    const clang::SourceManager& sources,
    const clang::LangOptions& language,
    bool spelling) {
    return {
        .source_file =
            source_path_of(range.getBegin(), sources, spelling),
        .span = range_of(range, sources, language, spelling),
    };
}

[[nodiscard]] std::string source_text(
    clang::SourceRange range,
    const clang::SourceManager& sources,
    const clang::LangOptions& language,
    bool spelling = true) {
    clang::SourceLocation begin =
        file_location(range.getBegin(), sources, spelling);
    clang::SourceLocation end =
        file_location(range.getEnd(), sources, spelling);
    if (begin.isInvalid() || end.isInvalid()) return {};
    bool invalid = false;
    const llvm::StringRef text = clang::Lexer::getSourceText(
        clang::CharSourceRange::getTokenRange(begin, end),
        sources,
        language,
        &invalid);
    return invalid ? std::string{} : text.str();
}

struct coherent_declaration_source {
    std::string source_file;
    source_range span;
    std::string source_file_sha256;
    std::string fragment_sha256;
};

[[nodiscard]] std::optional<coherent_declaration_source>
coherent_declaration_source_of(
    const clang::Decl* declaration,
    const clang::SourceManager& sources,
    const clang::LangOptions& language) {
    if (declaration == nullptr
        || declaration->getSourceRange().isInvalid()) {
        return std::nullopt;
    }
    const clang::CharSourceRange file_range =
        clang::Lexer::makeFileCharRange(
            clang::CharSourceRange::getTokenRange(
                declaration->getSourceRange()),
            sources,
            language);
    if (file_range.isInvalid() || !file_range.isCharRange()) {
        return std::nullopt;
    }
    const clang::SourceLocation begin = file_range.getBegin();
    const clang::SourceLocation end = file_range.getEnd();
    if (begin.isInvalid() || end.isInvalid()
        || !begin.isFileID() || !end.isFileID()
        || sources.getFileID(begin) != sources.getFileID(end)) {
        return std::nullopt;
    }
    const std::string source_file =
        source_path_of(begin, sources, true);
    const fs::path source_path{source_file};
    if (source_file.empty()
        || !source_path.is_absolute()
        || source_path.lexically_normal() != source_path
        || !fs::is_regular_file(source_path)) {
        return std::nullopt;
    }
    bool invalid_text = false;
    const llvm::StringRef fragment =
        clang::Lexer::getSourceText(
            file_range, sources, language, &invalid_text);
    const source_range span{
        .begin = position_of(begin, sources, true),
        .end = position_of(end, sources, true),
    };
    if (invalid_text || fragment.empty()
        || !valid_source_range(span)) {
        return std::nullopt;
    }
    return coherent_declaration_source{
        .source_file = source_file,
        .span = span,
        .source_file_sha256 =
            bioseq::common::sha256_hex(
                bioseq::codegen::read_artifact_bytes(source_path)),
        .fragment_sha256 =
            bioseq::common::sha256_hex(fragment.str()),
    };
}

[[nodiscard]] std::string qualified_name(
    const clang::NamedDecl* declaration) {
    if (declaration == nullptr) return {};
    std::string result = declaration->getQualifiedNameAsString();
    while (result.starts_with("::")) result.erase(0, 2);
    return result;
}

[[nodiscard]] std::string unqualified_name(
    std::string_view value) {
    const std::size_t arguments = value.find('(');
    if (arguments != std::string_view::npos) {
        value = value.substr(0, arguments);
    }
    const std::size_t separator = value.rfind("::");
    return std::string{
        separator == std::string_view::npos
            ? value
            : value.substr(separator + 2)};
}

[[nodiscard]] std::string owning_module(
    const clang::Decl* declaration) {
    if (declaration == nullptr) return {};
    if (const clang::Module* module =
            declaration->getOwningModule()) {
        return module->getFullModuleName();
    }
    return {};
}

[[nodiscard]] bool has_class_structural_template_argument(
    clang::QualType type,
    std::unordered_set<const clang::Type*>& visited);

[[nodiscard]] bool has_class_structural_template_argument(
    const clang::TemplateArgument& argument,
    std::unordered_set<const clang::Type*>& visited) {
    switch (argument.getKind()) {
        case clang::TemplateArgument::Type:
            return has_class_structural_template_argument(
                argument.getAsType(), visited);
        case clang::TemplateArgument::Declaration:
            if (const auto* object =
                    clang::dyn_cast<clang::TemplateParamObjectDecl>(
                        argument.getAsDecl())) {
                return object->getType()->isRecordType();
            }
            return false;
        case clang::TemplateArgument::StructuralValue:
            return argument.getStructuralValueType()->isRecordType();
        case clang::TemplateArgument::Pack:
            return std::ranges::any_of(
                argument.pack_elements(),
                [&visited](const clang::TemplateArgument& element) {
                    return has_class_structural_template_argument(
                        element, visited);
                });
        case clang::TemplateArgument::Null:
        case clang::TemplateArgument::NullPtr:
        case clang::TemplateArgument::Integral:
        case clang::TemplateArgument::Template:
        case clang::TemplateArgument::TemplateExpansion:
        case clang::TemplateArgument::Expression:
            return false;
    }
    return false;
}

[[nodiscard]] bool has_class_structural_template_argument(
    clang::QualType type,
    std::unordered_set<const clang::Type*>& visited) {
    if (type.isNull()) return false;
    const clang::Type* canonical =
        type.getCanonicalType().getTypePtrOrNull();
    if (canonical == nullptr || !visited.insert(canonical).second) {
        return false;
    }
    if (const auto* record =
            canonical->getAs<clang::RecordType>()) {
        if (const auto* specialization =
                clang::dyn_cast<
                    clang::ClassTemplateSpecializationDecl>(
                    record->getDecl())) {
            if (std::ranges::any_of(
                    specialization->getTemplateArgs().asArray(),
                    [&visited](
                        const clang::TemplateArgument& argument) {
                        return has_class_structural_template_argument(
                            argument, visited);
                    })) {
                return true;
            }
        }
    }
    if (const auto* pointer =
            clang::dyn_cast<clang::PointerType>(canonical)) {
        return has_class_structural_template_argument(
            pointer->getPointeeType(), visited);
    }
    if (const auto* reference =
            clang::dyn_cast<clang::ReferenceType>(canonical)) {
        return has_class_structural_template_argument(
            reference->getPointeeType(), visited);
    }
    if (const auto* array =
            clang::dyn_cast<clang::ArrayType>(canonical)) {
        return has_class_structural_template_argument(
            array->getElementType(), visited);
    }
    return false;
}

[[nodiscard]] bool has_class_structural_template_argument(
    const clang::FunctionDecl* function) {
    if (function == nullptr) return false;
    for (const clang::ParmVarDecl* parameter :
         function->parameters()) {
        std::unordered_set<const clang::Type*> visited;
        if (has_class_structural_template_argument(
                parameter->getType(), visited)) {
            return true;
        }
    }
    if (const auto* method =
            clang::dyn_cast<clang::CXXMethodDecl>(function)) {
        std::unordered_set<const clang::Type*> visited;
        return has_class_structural_template_argument(
            function->getASTContext().getCanonicalTypeDeclType(
                method->getParent()),
            visited);
    }
    return false;
}

[[nodiscard]] std::string compiler_owned_function_identity(
    const clang::FunctionDecl* function) {
    if (function == nullptr || function->getName().empty()
        || function->getType().isNull()
        || function->getType()->isDependentType()
        || function->getType()
                ->getAs<clang::FunctionProtoType>()
            == nullptr) {
        return {};
    }
    const bool exact_non_template =
        function->getTemplatedKind()
        == clang::FunctionDecl::TK_NonTemplate;
    const bool exact_template_specialization =
        function->isFunctionTemplateSpecialization()
        && function->getTemplateSpecializationArgs() != nullptr;
    if ((!exact_non_template && !exact_template_specialization)
        || !has_class_structural_template_argument(function)) {
        return {};
    }
    const clang::FunctionDecl* module_identity = function;
    if (const clang::FunctionTemplateDecl* primary =
            function->getPrimaryTemplate()) {
        module_identity = primary->getTemplatedDecl();
    } else {
        module_identity = function->getCanonicalDecl();
    }
    std::string module = owning_module(module_identity);
    if (module.empty()) module = owning_module(function);
    if (!module.starts_with("bioseq.")) return {};

    std::unique_ptr<clang::MangleContext> mangler{
        function->getASTContext().createMangleContext()};
    if (!mangler
        || !mangler->shouldMangleDeclName(function)) {
        return {};
    }
    llvm::SmallString<256> mangled;
    llvm::raw_svector_ostream output{mangled};
    mangler->mangleName(clang::GlobalDecl{function}, output);
    if (mangled.empty()) return {};
    return "clang-fallback:function:"
        + module + ":" + mangled.str().str();
}

[[nodiscard]] std::string stable_function_identity(
    const clang::FunctionDecl* function,
    clang::ASTContext& context) {
    const std::string usr = stable_usr(function);
    if (function == nullptr
        || qualified_name(function)
            != "bioseq::postgres::support::"
               "invoke_postgres_callback") {
        return usr.empty()
            ? compiler_owned_function_identity(function)
            : usr;
    }
    const clang::FunctionTemplateDecl* primary =
        function->getPrimaryTemplate();
    const clang::TemplateArgumentList* arguments =
        function->getTemplateSpecializationArgs();
    if (primary == nullptr || arguments == nullptr
        || arguments->size() != 1
        || arguments->get(0).getKind()
            != clang::TemplateArgument::Type) {
        return usr;
    }

    const clang::QualType callable =
        arguments->get(0).getAsType().getCanonicalType();
    const clang::QualType value_type =
        callable.getNonReferenceType().getUnqualifiedType();
    const clang::CXXRecordDecl* record =
        value_type.isNull()
        ? nullptr
        : value_type->getAsCXXRecordDecl();
    std::string material =
        stable_usr(primary->getTemplatedDecl()) + "\n"
        + callable.getAsString() + "\n";
    if (record != nullptr) {
        record = record->getCanonicalDecl();
        const located_source_range location = located_range_of(
            record->getSourceRange(),
            context.getSourceManager(),
            context.getLangOpts(),
            true);
        material += stable_usr(record) + "\n"
            + owning_module(record) + "\n"
            + location.source_file + "\n"
            + std::to_string(
                location.span.begin.byte_offset)
            + "\n"
            + std::to_string(
                location.span.end.byte_offset);
    }
    return "clang-boundary-specialization:"
        + bioseq::common::sha256_hex(material).substr(0, 32);
}

[[nodiscard]] bool nontrivial_type(
    clang::QualType type,
    clang::ASTContext& context) {
    if (type.isNull() || type->isReferenceType()) return false;
    if (type.isDestructedType()
        != clang::QualType::DK_none) {
        return true;
    }
    if (const clang::CXXRecordDecl* record =
            type->getAsCXXRecordDecl()) {
        const clang::CXXRecordDecl* definition =
            record->getDefinition();
        return definition != nullptr
            && !definition->hasTrivialDestructor();
    }
    static_cast<void>(context);
    return false;
}

[[nodiscard]] const json_value::object& require_object(
    const json_value::object& parent,
    std::string_view name,
    std::string_view context) {
    return json_object(
        json_field(parent, name, context),
        std::string{context} + "." + std::string{name});
}

[[nodiscard]] std::string require_string(
    const json_value::object& parent,
    std::string_view name,
    std::string_view context) {
    std::string result{
        json_string(
            json_field(parent, name, context),
            std::string{context} + "." + std::string{name})};
    if (result.empty()) {
        fact_failure(
            context,
            std::string{name} + " cannot be empty");
    }
    return result;
}

struct prepared_identity {
    fs::path clang_binary;
    std::string clang_sha256;
    fs::path clang_scan_deps;
    std::string clang_scan_deps_sha256;
    fs::path llvm_config;
    std::string llvm_config_sha256;
    fs::path compiler_module_inventory;
    std::string compiler_module_inventory_sha256;
    std::string compilation_signature;
};

[[nodiscard]] prepared_identity parse_prepared_identity(
    std::string_view manifest_text) {
    const json_value document = parse_json(manifest_text);
    const json_value::object& root =
        json_object(document, "PCM manifest");
    const std::int64_t schema_revision = json_integer(
        json_field(root, "schema_revision", "PCM manifest"),
        "PCM manifest.schema_revision");
    if (require_string(root, "schema", "PCM manifest")
            != "bioseq_clang_pcm_manifest"
        || schema_revision != 1) {
        fact_failure(
            "PCM manifest", "unsupported schema identity or revision");
    }
    return {
        .clang_binary =
            require_string(root, "clang_binary", "PCM manifest"),
        .clang_sha256 =
            require_string(
                root, "clang_binary_sha256", "PCM manifest"),
        .clang_scan_deps =
            require_string(
                root, "clang_scan_deps", "PCM manifest"),
        .clang_scan_deps_sha256 =
            require_string(
                root,
                "clang_scan_deps_sha256",
                "PCM manifest"),
        .llvm_config =
            require_string(root, "llvm_config", "PCM manifest"),
        .llvm_config_sha256 =
            require_string(
                root, "llvm_config_sha256", "PCM manifest"),
        .compiler_module_inventory =
            require_string(
                root,
                "compiler_module_inventory",
                "PCM manifest"),
        .compiler_module_inventory_sha256 =
            require_string(
                root,
                "compiler_module_inventory_sha256",
                "PCM manifest"),
        .compilation_signature =
            require_string(
                root,
                "compilation_signature",
                "PCM manifest"),
    };
}

[[nodiscard]] std::vector<fs::path> parse_source_list(
    const fs::path& source_list) {
    const std::string text =
        bioseq::codegen::read_artifact_bytes(source_list);
    std::vector<fs::path> result;
    std::set<std::string, std::less<>> seen;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const std::size_t newline = text.find('\n', begin);
        const std::size_t end =
            newline == std::string::npos ? text.size() : newline;
        std::string_view line{text.data() + begin, end - begin};
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (!line.empty()) {
            const fs::path path{line};
            if (!path.is_absolute() || !fs::is_regular_file(path)) {
                fact_failure(
                    source_list.string(),
                    "source list contains a missing or non-absolute "
                    "path: " + std::string{line});
            }
            const std::string canonical = canonical_path(path);
            if (!seen.insert(canonical).second) {
                fact_failure(
                    source_list.string(),
                    "source list contains a canonical duplicate: "
                    + canonical);
            }
            result.emplace_back(canonical);
        }
        if (newline == std::string::npos) break;
        begin = newline + 1;
    }
    if (result.empty()) {
        fact_failure(
            source_list.string(), "source list is empty");
    }
    std::ranges::sort(result);
    return result;
}

[[nodiscard]] const provenance_record* provenance_by_id(
    const provenance_bundle& provenance,
    std::string_view id) {
    const auto found = std::ranges::find(
        provenance.records, id, &provenance_record::record_id);
    return found == provenance.records.end()
        ? nullptr
        : std::addressof(*found);
}

[[nodiscard]] provenance_reference reference_from(
    const provenance_bundle& provenance,
    const provenance_record& record) {
    return {
        .record_id = record.record_id,
        .postgresql_revision = provenance.postgresql_revision,
        .upstream_file = record.upstream_file,
        .upstream_span = record.upstream_span,
        .fragment_sha256 = record.fragment_sha256,
        .rationale = record.rationale,
    };
}

struct analysis_environment {
    std::set<std::string, std::less<>> source_paths;
    std::set<std::string, std::less<>>
        postgresql_header_roots;
    std::set<std::string, std::less<>>
        rejected_postgresql_header_roots;
    const provenance_bundle* provenance{};
    std::string compiler_profile;
    std::string provenance_authority;
    struct provenance_candidate {
        provenance_kind kind{
            provenance_kind::standard_library_declaration};
        std::string target_usr;
        std::string target_qualified_name;
        std::string authority_id;
        std::string upstream_file;
        source_range upstream_span;
        std::string upstream_file_sha256;
        std::string fragment_sha256;
        std::string library_version;
        bool body_available{};
    };
    std::shared_ptr<std::map<
        std::string,
        provenance_candidate,
        std::less<>>> provenance_candidates;
};

struct bridge_macro_expansion {
    std::string name;
    clang::SourceLocation invocation_begin;
    located_source_range invocation;
    located_source_range definition;
    std::string definition_file_sha256;
    std::string pinned_fragment_sha256;
    bool direct_file_invocation{};
};

using bridge_macro_inventory =
    std::vector<bridge_macro_expansion>;

struct postgresql_header_authority {
    std::set<std::string, std::less<>> authenticated_roots;
    std::set<std::string, std::less<>> rejected_roots;
    std::map<std::string, std::string, std::less<>>
        authenticated_headers;
};

[[nodiscard]] std::set<std::string, std::less<>>
compile_include_roots(
    const std::vector<clang::tooling::CompileCommand>& commands) {
    static constexpr std::array<std::string_view, 4>
        separate_options{
            "-I", "-isystem", "-iquote", "-idirafter",
        };
    static constexpr std::array<std::string_view, 4>
        joined_options{
            "-isystem", "-iquote", "-idirafter", "-I",
        };
    std::set<std::string, std::less<>> result;
    for (const clang::tooling::CompileCommand& command : commands) {
        for (std::size_t index = 0;
             index < command.CommandLine.size();
             ++index) {
            const std::string_view argument =
                command.CommandLine[index];
            std::string include_path;
            if (std::ranges::find(
                    separate_options, argument)
                != separate_options.end()) {
                if (index + 1 == command.CommandLine.size()) {
                    fact_failure(
                        command.Filename,
                        "compile include option has no path");
                }
                include_path = command.CommandLine[++index];
            } else {
                for (const std::string_view option :
                     joined_options) {
                    if (argument.starts_with(option)
                        && argument.size() > option.size()) {
                        include_path =
                            argument.substr(option.size());
                        break;
                    }
                }
            }
            if (include_path.empty()) continue;
            if (include_path.front() == '=') {
                fact_failure(
                    command.Filename,
                    "sysroot-relative include paths cannot "
                    "establish PostgreSQL header authority");
            }
            fs::path resolved{include_path};
            if (resolved.is_relative()) {
                resolved =
                    fs::path{command.Directory} / resolved;
            }
            if (fs::is_directory(resolved)) {
                result.insert(canonical_path(resolved));
            }
        }
    }
    return result;
}

[[nodiscard]] postgresql_header_authority
derive_postgresql_header_authority(
    const std::vector<clang::tooling::CompileCommand>& commands,
    const provenance_bundle& provenance) {
    std::map<std::string, std::string, std::less<>>
        pinned_headers;
    static constexpr std::string_view include_prefix{
        "src/include/"};
    for (const provenance_record& record : provenance.records) {
        if (!record.upstream_file.starts_with(include_prefix)) {
            continue;
        }
        const std::string relative{
            std::string_view{record.upstream_file}.substr(
                include_prefix.size())};
        const auto [existing, inserted] =
            pinned_headers.emplace(
                relative, record.upstream_file_sha256);
        if (!inserted
            && existing->second
                != record.upstream_file_sha256) {
            fact_failure(
                relative,
                "pinned PostgreSQL header has conflicting "
                "whole-file hashes");
        }
    }
    if (pinned_headers.empty()) {
        fact_failure(
            "PostgreSQL header authority",
            "provenance has no src/include header pin");
    }
    if (pinned_headers.size() < 2) {
        fact_failure(
            "PostgreSQL header authority",
            "one pinned header cannot authenticate a PostgreSQL "
            "include root");
    }

    postgresql_header_authority authority;
    const std::set<std::string, std::less<>> include_roots =
        compile_include_roots(commands);
    for (const std::string& include_root : include_roots) {
        for (const fs::path& candidate_root : {
                 fs::path{include_root},
                 fs::path{include_root} / "server"}) {
            if (!fs::is_directory(candidate_root)) continue;
            const std::string canonical_root =
                canonical_path(candidate_root);
            bool has_postgresql_marker =
                fs::is_regular_file(candidate_root / "postgres.h")
                || fs::is_regular_file(
                    candidate_root / "pg_config.h");
            std::size_t authenticated_header_count = 0;
            bool stale_pin = false;
            std::optional<std::string> physical_root;
            std::set<std::string, std::less<>>
                observed_physical_roots;
            std::map<std::string, std::string, std::less<>>
                candidate_authenticated_headers;
            for (const auto& [relative, expected_sha256] :
                 pinned_headers) {
                const fs::path relative_path{relative};
                const fs::path header =
                    candidate_root / relative_path;
                if (!fs::is_regular_file(header)) {
                    stale_pin = true;
                    continue;
                }
                has_postgresql_marker = true;
                const fs::path canonical_header{
                    canonical_path(header)};
                fs::path derived_physical_root =
                    canonical_header;
                bool valid_relative_path =
                    !relative_path.empty()
                    && !relative_path.is_absolute()
                    && !relative_path.has_root_path();
                for (const fs::path& component :
                     relative_path) {
                    if (component.empty()
                        || component == "."
                        || component == "..") {
                        valid_relative_path = false;
                        break;
                    }
                    derived_physical_root =
                        derived_physical_root.parent_path();
                }
                if (!valid_relative_path
                    || derived_physical_root.empty()
                    || canonical_header.lexically_relative(
                           derived_physical_root)
                        != relative_path) {
                    stale_pin = true;
                    continue;
                }
                const std::string derived_root =
                    canonical_path(derived_physical_root);
                observed_physical_roots.insert(derived_root);
                const std::string actual_sha256 =
                    bioseq::common::sha256_hex(
                        bioseq::codegen::read_artifact_bytes(header));
                if (actual_sha256 != expected_sha256) {
                    stale_pin = true;
                    continue;
                }
                if (physical_root.has_value()
                    && *physical_root != derived_root) {
                    stale_pin = true;
                    continue;
                }
                physical_root = derived_root;
                ++authenticated_header_count;
                candidate_authenticated_headers.emplace(
                    canonical_header.generic_string(),
                    actual_sha256);
            }
            if (!stale_pin
                && authenticated_header_count
                    == pinned_headers.size()
                && physical_root.has_value()) {
                authority.authenticated_roots.insert(
                    *physical_root);
                authority.authenticated_headers.insert(
                    candidate_authenticated_headers.begin(),
                    candidate_authenticated_headers.end());
            } else if (has_postgresql_marker) {
                authority.rejected_roots.insert(
                    canonical_root);
                authority.rejected_roots.insert(
                    observed_physical_roots.begin(),
                    observed_physical_roots.end());
            }
        }
    }
    for (const std::string& root :
         authority.authenticated_roots) {
        authority.rejected_roots.erase(root);
    }
    if (authority.authenticated_roots.empty()) {
        fact_failure(
            "PostgreSQL header authority",
            "compile inputs contain no include root authenticated "
            "by exact pinned PostgreSQL header bytes");
    }
    return authority;
}

[[nodiscard]] callee_domain domain_of(
    const clang::FunctionDecl* target,
    clang::ASTContext& context,
    const analysis_environment& environment) {
    if (target == nullptr) return callee_domain::unknown;
    if (target->getBuiltinID() != 0) {
        return callee_domain::compiler_builtin;
    }
    const clang::SourceManager& sources =
        context.getSourceManager();
    const std::string path =
        source_path_of(target->getLocation(), sources, true);
    if (!path.empty()) {
        for (const std::string& root :
             environment.postgresql_header_roots) {
            if (path_within(path, root)) {
                return callee_domain::postgresql_server;
            }
        }
        for (const std::string& root :
             environment.rejected_postgresql_header_roots) {
            if (path_within(path, root)) {
                return callee_domain::unknown;
            }
        }
    }
    const std::string name = qualified_name(target);
    if (name.starts_with("std::")
        || path.contains("/c++/v1/")
        || path.contains("/libcxx/")) {
        return callee_domain::standard_library;
    }
    if (!path.empty()
        && (environment.source_paths.contains(path)
            || (!sources.isInSystemHeader(
                    target->getLocation())
                && target->doesThisDeclarationHaveABody()))) {
        return callee_domain::project;
    }
    if (sources.isInSystemHeader(target->getLocation())) {
        return callee_domain::system_library;
    }
    return callee_domain::unknown;
}

[[nodiscard]] std::string target_usr_for(
    const clang::FunctionDecl* target,
    callee_domain domain) {
    std::string usr = stable_usr(target);
    if (usr.empty()
        && domain == callee_domain::compiler_builtin) {
        usr = "clang-builtin:"
            + bioseq::common::sha256_hex(
                  std::to_string(target->getBuiltinID()) + "\n"
                  + qualified_name(target) + "\n"
                  + target->getType().getCanonicalType().getAsString())
                  .substr(0, 32);
    }
    return usr;
}

[[nodiscard]] const clang::FunctionDecl*
external_provenance_declaration(
    const clang::FunctionDecl* target) noexcept {
    if (target == nullptr) return nullptr;
    if (const clang::FunctionTemplateDecl* primary =
            target->getPrimaryTemplate()) {
        target = primary->getTemplatedDecl();
    } else if (const clang::FunctionDecl* pattern =
                   target->getTemplateInstantiationPattern()) {
        target = pattern;
    }
    const clang::FunctionDecl* canonical =
        target->getCanonicalDecl();
    if (const clang::FunctionDecl* definition =
            canonical->getDefinition()) {
        return definition;
    }
    return target;
}

void record_provenance_candidate(
    const clang::FunctionDecl* target,
    callee_domain domain,
    provenance_kind kind,
    clang::ASTContext& context,
    const analysis_environment& environment) {
    if (!environment.provenance_candidates) return;
    const std::string target_name = qualified_name(target);
    const std::string target_usr = target_usr_for(target, domain);
    const bool postgresql_candidate =
        kind == provenance_kind::pg_no_longjmp_leaf;
    const std::string authority =
        postgresql_candidate
            && environment.provenance != nullptr
        ? environment.provenance->postgresql_revision
        : environment.provenance_authority;
    analysis_environment::provenance_candidate candidate{
        .kind = kind,
        .target_usr = target_usr,
        .target_qualified_name = target_name,
        .authority_id = authority,
        .library_version = postgresql_candidate
            ? std::string{}
            : environment.provenance_authority,
        .body_available =
            target->doesThisDeclarationHaveABody(),
    };
    if (domain == callee_domain::compiler_builtin) {
        candidate.upstream_file =
            "clang-builtin:" + environment.provenance_authority;
        candidate.upstream_span = {
            .begin = {
                .byte_offset = 0,
                .line = 1,
                .column = 1,
            },
            .end = {
                .byte_offset = 1,
                .line = 1,
                .column = 2,
            },
        };
        candidate.upstream_file_sha256 =
            bioseq::common::sha256_hex(
                environment.provenance_authority);
        candidate.fragment_sha256 =
            bioseq::common::sha256_hex(
                std::to_string(target->getBuiltinID()) + "\n"
                + target_name + "\n"
                + target->getType()
                      .getCanonicalType()
                      .getAsString());
    } else {
        const clang::SourceManager& sources =
            context.getSourceManager();
        const clang::LangOptions& language =
            context.getLangOpts();
        const std::optional<coherent_declaration_source>
            declaration_source =
                coherent_declaration_source_of(
                    target, sources, language);
        if (!declaration_source.has_value()) {
            fact_failure(
                target_name,
                "compiler provenance candidate has an incoherent "
                "or ambiguous declaration range");
        }
        candidate.upstream_file =
            declaration_source->source_file;
        candidate.upstream_span =
            declaration_source->span;
        candidate.upstream_file_sha256 =
            declaration_source->source_file_sha256;
        candidate.fragment_sha256 =
            declaration_source->fragment_sha256;
    }
    if (candidate.target_usr.empty()
        || candidate.target_qualified_name.empty()
        || candidate.authority_id.empty()
        || (postgresql_candidate
            && !candidate.library_version.empty())
        || (!postgresql_candidate
            && candidate.library_version.empty())
        || candidate.upstream_file.empty()
        || !valid_source_range(candidate.upstream_span)
        || candidate.upstream_file_sha256.empty()
        || candidate.fragment_sha256.empty()
        || (domain != callee_domain::compiler_builtin
            && (!fs::path{candidate.upstream_file}.is_absolute()
                || fs::path{candidate.upstream_file}
                        .lexically_normal()
                    != fs::path{candidate.upstream_file}))) {
        fact_failure(
            target_name,
            "compiler provenance candidate lacks an absolute "
            "source, exact target, or authority");
    }
    const std::string key =
        target_usr + "\n" + target_name + "\n"
        + authority;
    const auto [existing, inserted] =
        environment.provenance_candidates->emplace(
            key, candidate);
    if (!inserted) {
        const analysis_environment::provenance_candidate&
            previous = existing->second;
        if (previous.kind != candidate.kind
            || previous.target_usr != candidate.target_usr
            || previous.target_qualified_name
                != candidate.target_qualified_name
            || previous.authority_id != candidate.authority_id
            || previous.upstream_file != candidate.upstream_file
            || !same_source_range(
                previous.upstream_span,
                candidate.upstream_span)
            || previous.upstream_file_sha256
                != candidate.upstream_file_sha256
            || previous.fragment_sha256
                != candidate.fragment_sha256
            || previous.library_version
                != candidate.library_version
            || previous.body_available
                != candidate.body_available) {
            fact_failure(
                target_name,
                "conflicting compiler provenance candidates "
                "were observed for one target");
        }
    }
}

[[nodiscard]] std::optional<external_declaration_provenance>
external_provenance_for(
    const clang::FunctionDecl* target,
    callee_domain domain,
    clang::ASTContext& context,
    const analysis_environment& environment) {
    if (environment.provenance == nullptr
        || (domain != callee_domain::standard_library
            && domain != callee_domain::system_library
            && domain != callee_domain::compiler_builtin)) {
        return std::nullopt;
    }
    target = external_provenance_declaration(target);
    if (target == nullptr) {
        fact_failure(
            "external provenance",
            "call target has no declaration authority");
    }
    const provenance_kind kind =
        domain == callee_domain::standard_library
        ? provenance_kind::standard_library_declaration
        : domain == callee_domain::system_library
        ? provenance_kind::system_library_declaration
        : provenance_kind::compiler_builtin_declaration;
    const std::string target_name = qualified_name(target);
    const std::string target_usr =
        target_usr_for(target, domain);
    const provenance_record* match = nullptr;
    for (const provenance_record& record :
         environment.provenance->records) {
        if (record.kind != kind
            || record.target_usr != target_usr
            || record.target_qualified_name != target_name
            || record.authority_id
                != environment.provenance_authority) {
            continue;
        }
        if (match != nullptr) {
            fact_failure(
                target_name,
                "multiple pinned external declaration records "
                "match one target");
        }
        match = std::addressof(record);
    }
    if (match == nullptr) {
        record_provenance_candidate(
            target, domain, kind, context, environment);
        return std::nullopt;
    }

    if (domain == callee_domain::compiler_builtin) {
        const std::string authority_file =
            "clang-builtin:" + environment.provenance_authority;
        const std::string material =
            std::to_string(target->getBuiltinID()) + "\n"
            + target_name + "\n"
            + target->getType().getCanonicalType().getAsString();
        if (match->upstream_file != authority_file
            || match->upstream_file_sha256
                != bioseq::common::sha256_hex(
                    environment.provenance_authority)
            || match->fragment_sha256
                != bioseq::common::sha256_hex(material)) {
            fact_failure(
                target_name,
                "pinned compiler builtin authority/hash is stale");
        }
        return external_declaration_provenance{
            .record_id = match->record_id,
            .source_file = match->upstream_file,
            .declaration_span = match->upstream_span,
            .declaration_sha256 = match->fragment_sha256,
            .library_version = match->library_version,
            .user_code_callback_free =
                match->user_code_callback_free,
        };
    }

    const clang::SourceManager& sources = context.getSourceManager();
    const clang::LangOptions& language = context.getLangOpts();
    const std::optional<coherent_declaration_source>
        declaration_source =
            coherent_declaration_source_of(
                target, sources, language);
    if (!declaration_source.has_value()) {
        fact_failure(
            target_name,
            "pinned external declaration has an incoherent "
            "or ambiguous declaration range");
    }
    if (match->upstream_file
            != declaration_source->source_file
        || declaration_source->source_file_sha256
            != match->upstream_file_sha256) {
        fact_failure(
            target_name,
            "pinned external declaration file hash is stale");
    }
    if (declaration_source->span.begin.byte_offset
            != match->upstream_span.begin.byte_offset
        || declaration_source->span.end.byte_offset
            != match->upstream_span.end.byte_offset
        || declaration_source->fragment_sha256
            != match->fragment_sha256) {
        fact_failure(
            target_name,
            "pinned external declaration range/hash is stale");
    }
    return external_declaration_provenance{
        .record_id = match->record_id,
        .source_file = match->upstream_file,
        .declaration_span = match->upstream_span,
        .declaration_sha256 = match->fragment_sha256,
        .library_version = match->library_version,
        .user_code_callback_free =
            match->user_code_callback_free,
    };
}

[[nodiscard]] std::optional<provenance_reference>
pg_allowlist_for(
    const clang::FunctionDecl* target,
    clang::ASTContext& context,
    const analysis_environment& environment) {
    if (environment.provenance == nullptr) return std::nullopt;
    const std::string target_name = qualified_name(target);
    const std::string target_usr = stable_usr(target);
    const provenance_record* match = nullptr;
    for (const provenance_record& record :
         environment.provenance->records) {
        if (record.kind != provenance_kind::pg_no_longjmp_leaf
            || record.target_usr != target_usr
            || record.target_qualified_name != target_name
            || record.authority_id
                != environment.provenance->postgresql_revision) {
            continue;
        }
        if (match != nullptr) {
            fact_failure(
                target_name,
                "multiple PostgreSQL no-longjmp records match");
        }
        match = std::addressof(record);
    }
    if (match == nullptr) {
        record_provenance_candidate(
            target,
            callee_domain::postgresql_server,
            provenance_kind::pg_no_longjmp_leaf,
            context,
            environment);
        return std::nullopt;
    }
    const clang::SourceManager& sources = context.getSourceManager();
    const clang::LangOptions& language = context.getLangOpts();
    const std::optional<coherent_declaration_source>
        declaration_source =
            coherent_declaration_source_of(
                target, sources, language);
    if (!declaration_source.has_value()) {
        fact_failure(
            target_name,
            "pinned PostgreSQL no-longjmp declaration has an "
            "incoherent or ambiguous declaration range");
    }
    const std::string& path =
        declaration_source->source_file;
    static constexpr std::string_view include_prefix{
        "src/include/"};
    bool declaration_under_authenticated_root = false;
    if (match->upstream_file.starts_with(include_prefix)) {
        const fs::path expected_relative{
            std::string_view{match->upstream_file}.substr(
                include_prefix.size())};
        for (const std::string& root :
             environment.postgresql_header_roots) {
            if (fs::path{path}.lexically_relative(
                    fs::path{root})
                    == expected_relative) {
                declaration_under_authenticated_root = true;
                break;
            }
        }
    }
    if (path.empty() || !declaration_under_authenticated_root
        || declaration_source->source_file_sha256
            != match->upstream_file_sha256
        || !match->definition
        || declaration_source->span.begin.byte_offset
            != match->upstream_span.begin.byte_offset
        || declaration_source->span.end.byte_offset
            != match->upstream_span.end.byte_offset
        || declaration_source->fragment_sha256
            != match->fragment_sha256) {
        fact_failure(
            target_name,
            "pinned PostgreSQL no-longjmp declaration is stale");
    }
    return reference_from(*environment.provenance, *match);
}

[[nodiscard]] flow_object flow_object_for(
    const clang::VarDecl* declaration,
    clang::ASTContext& context) {
    const clang::SourceManager& sources =
        context.getSourceManager();
    const clang::LangOptions& language = context.getLangOpts();
    std::string usr = stable_usr(declaration);
    if (usr.empty()) {
        const located_source_range location = located_range_of(
            declaration->getSourceRange(),
            sources,
            language,
            true);
        usr = "clang-local:"
            + bioseq::common::sha256_hex(
                  location.source_file + "\n"
                  + std::to_string(
                      location.span.begin.byte_offset)
                  + "\n" + declaration->getNameAsString())
                  .substr(0, 32);
    }
    return {
        .stable_object_id = usr,
        .declaration_usr = usr,
        .identity_kind =
            object_identity_kind::declaration_usr,
        .canonical_type =
            declaration->getType()
                .getCanonicalType()
                .getAsString(),
        .origin = declaration->isExceptionVariable()
            ? crossing_origin::catch_parameter
            : crossing_origin::automatic_object,
        .declaration = located_range_of(
            declaration->getSourceRange(),
            sources,
            language,
            true),
        .trivially_destructible =
            !nontrivial_type(declaration->getType(), context),
    };
}

[[nodiscard]] flow_object temporary_object_for(
    const clang::CXXBindTemporaryExpr* expression,
    std::string_view function_usr,
    clang::ASTContext& context) {
    const clang::SourceManager& sources =
        context.getSourceManager();
    const clang::LangOptions& language = context.getLangOpts();
    const located_source_range location = located_range_of(
        expression->getSourceRange(), sources, language, false);
    const std::string id_material =
        std::string{function_usr} + "\n"
        + location.source_file + "\n"
        + std::to_string(location.span.begin.byte_offset) + "\n"
        + std::to_string(location.span.end.byte_offset)
        + "\ntemporary";
    return {
        .stable_object_id = "clang-temporary:"
            + bioseq::common::sha256_hex(id_material).substr(0, 32),
        .identity_kind =
            object_identity_kind::synthetic_temporary,
        .canonical_type =
            expression->getType()
                .getCanonicalType()
                .getAsString(),
        .origin = crossing_origin::temporary,
        .declaration = location,
        .trivially_destructible =
            !nontrivial_type(expression->getType(), context),
    };
}

[[nodiscard]] flow_object coroutine_object_for(
    const clang::FunctionDecl* function,
    clang::ASTContext& context) {
    const clang::SourceManager& sources =
        context.getSourceManager();
    const clang::LangOptions& language = context.getLangOpts();
    const located_source_range location = located_range_of(
        function->getSourceRange(), sources, language, true);
    return {
        .stable_object_id = "clang-coroutine:"
            + bioseq::common::sha256_hex(
                  stable_function_identity(
                      function, context)
                  + "\n"
                  + location.source_file)
                  .substr(0, 32),
        .identity_kind =
            object_identity_kind::synthetic_coroutine_state,
        .canonical_type = "<clang-coroutine-frame>",
        .origin = crossing_origin::coroutine_state,
        .declaration = location,
        .trivially_destructible = false,
    };
}

[[nodiscard]] flow_object capture_object_for(
    const clang::LambdaExpr* lambda,
    const clang::LambdaCapture& capture,
    std::size_t ordinal,
    std::string_view function_usr,
    clang::ASTContext& context) {
    const clang::SourceManager& sources =
        context.getSourceManager();
    const clang::LangOptions& language = context.getLangOpts();
    const clang::ValueDecl* captured_declaration =
        capture.capturesVariable()
        ? capture.getCapturedVar()
        : nullptr;
    const clang::VarDecl* variable =
        clang::dyn_cast_or_null<clang::VarDecl>(
            captured_declaration);
    const clang::Decl* identity_declaration =
        variable != nullptr
        ? static_cast<const clang::Decl*>(variable)
        : static_cast<const clang::Decl*>(captured_declaration);
    clang::QualType type;
    if (captured_declaration != nullptr) {
        type = captured_declaration->getType();
    }
    if (capture.getCaptureKind() == clang::LCK_ByRef
        && !type.isNull()) {
        type = context.getLValueReferenceType(type);
    }
    const located_source_range location = located_range_of(
        clang::SourceRange{
            capture.getLocation(), capture.getLocation()},
        sources,
        language,
        false);
    const std::string variable_usr =
        identity_declaration == nullptr
        ? std::string{}
        : stable_usr(identity_declaration);
    const std::string material =
        std::string{function_usr} + "\n"
        + std::to_string(ordinal) + "\n"
        + variable_usr + "\n"
        + std::to_string(location.span.begin.byte_offset);
    return {
        .stable_object_id = "clang-capture:"
            + bioseq::common::sha256_hex(material).substr(0, 32),
        .identity_kind =
            object_identity_kind::synthetic_capture,
        .canonical_type = type.isNull()
            ? std::string{"<implicit-capture>"}
            : type.getCanonicalType().getAsString(),
        .origin = crossing_origin::lambda_capture,
        .declaration = location,
        .trivially_destructible =
            capture.getCaptureKind() == clang::LCK_ByRef
            || type.isNull()
            || !nontrivial_type(type, context),
    };
}

[[nodiscard]] std::vector<macro_expansion_record> macro_chain_of(
    clang::SourceLocation location,
    clang::Preprocessor& preprocessor,
    clang::ASTContext& context,
    const analysis_environment& environment) {
    const clang::SourceManager& sources =
        context.getSourceManager();
    const clang::LangOptions& language = context.getLangOpts();
    struct observed_macro_expansion {
        macro_expansion_record expansion;
        bool authenticated_report_expansion{};
    };
    const auto report_record_id =
        [](std::string_view name) -> std::string_view {
            if (name == "ereport"
                || name == "ereport_domain") {
                return "pg17.ereport_macro";
            }
            if (name == "elog" || name == "elog_start") {
                return "pg17.elog_macro";
            }
            return {};
        };
    const auto authentic_report_expansion =
        [&](const macro_expansion_record& expansion) {
            const std::string_view record_id =
                report_record_id(expansion.name);
            if (record_id.empty()) return false;
            if (environment.provenance == nullptr
                || environment.provenance->postgresql_tag
                    != "REL_17_10"
                || environment.provenance->postgresql_revision
                    != "25c49f3a4a742ba283f5cc43cc7f1d361552e917") {
                return false;
            }
            const provenance_record* record =
                provenance_by_id(
                    *environment.provenance, record_id);
            if (record == nullptr
                || record->kind
                    != provenance_kind::recoverable_error_seed
                || record->upstream_file
                    != "src/include/utils/elog.h"
                || expansion.definition.source_file.empty()
                || !expansion.definition.source_file.ends_with(
                    "/utils/elog.h")
                || expansion.definition.span.begin.byte_offset
                    < record->upstream_span.begin.byte_offset
                || expansion.definition.span.end.byte_offset
                    > record->upstream_span.end.byte_offset) {
                return false;
            }
            return std::ranges::any_of(
                environment.postgresql_header_roots,
                [&](const std::string& root) {
                    return path_within(
                        expansion.definition.source_file, root);
                });
        };

    std::vector<observed_macro_expansion> inner_to_outer;
    std::set<unsigned> seen_raw_locations;
    while (location.isValid() && location.isMacroID()) {
        const std::string name =
            clang::Lexer::getImmediateMacroName(
                location, sources, language)
                .str();
        if (!seen_raw_locations.insert(
                location.getRawEncoding()).second) {
            fact_failure(
                name.empty() ? "macro expansion" : name,
                "macro expansion chain is cyclic");
        }
        const auto expansion =
            sources.getImmediateExpansionRange(location);
        const clang::SourceLocation caller =
            sources.getImmediateMacroCallerLoc(location);
        const clang::IdentifierInfo* identifier =
            name.empty()
            ? nullptr
            : preprocessor.getIdentifierInfo(name);
        const clang::MacroDefinition macro_definition =
            identifier == nullptr
            ? clang::MacroDefinition{}
            : preprocessor.getMacroDefinitionAtLoc(
                  identifier,
                  sources.getExpansionLoc(location));
        const clang::MacroInfo* macro =
            macro_definition.isAmbiguous()
            ? nullptr
            : macro_definition.getMacroInfo();
        if (name.empty() || macro == nullptr
            || macro->getDefinitionLoc().isInvalid()) {
            fact_failure(
                "macro expansion",
                "cannot resolve a macro definition");
        }
        clang::SourceLocation definition_end =
            macro->getDefinitionEndLoc();
        if (definition_end.isInvalid()) {
            definition_end = macro->getDefinitionLoc();
        }
        const clang::SourceRange definition_range{
            macro->getDefinitionLoc(), definition_end};
        const located_source_range invocation = located_range_of(
            clang::SourceRange{
                expansion.getBegin(), expansion.getEnd()},
            sources,
            language,
            false);
        const located_source_range definition = located_range_of(
            definition_range, sources, language, true);
        if (invocation.source_file.empty()
            || definition.source_file.empty()) {
            fact_failure(
                name, "macro expansion chain is unlocated");
        }
        macro_expansion_record record{
            .name = name,
            .invocation = invocation,
            .definition = definition,
            .definition_sha256 =
                bioseq::common::sha256_hex(
                    source_text(
                        definition_range,
                        sources,
                        language,
                        true)),
        };
        inner_to_outer.push_back({
            .expansion = record,
            .authenticated_report_expansion =
                authentic_report_expansion(record),
        });
        location = caller;
    }

    // Clang gives forwarded arguments distinct MacroID frames for the macro
    // body and each argument substitution.  In ereport -> ereport_domain,
    // those raw frames can describe the same located ereport expansion more
    // than once.  Raw identities above prove the traversal is acyclic; now
    // retain the outermost copy of each semantic expansion.
    std::ranges::reverse(inner_to_outer);

    using semantic_macro_identity = std::tuple<
        std::string,
        std::string,
        std::size_t,
        std::string,
        std::size_t,
        std::string>;
    std::map<semantic_macro_identity, std::size_t>
        normalized_indexes;
    std::vector<observed_macro_expansion> normalized;
    normalized.reserve(inner_to_outer.size());
    for (observed_macro_expansion& observed : inner_to_outer) {
        const macro_expansion_record& expansion =
            observed.expansion;
        semantic_macro_identity identity{
            expansion.name,
            expansion.invocation.source_file,
            expansion.invocation.span.begin.byte_offset,
            expansion.definition.source_file,
            expansion.definition.span.begin.byte_offset,
            expansion.definition_sha256,
        };
        const auto [position, inserted] =
            normalized_indexes.emplace(
                std::move(identity), normalized.size());
        if (inserted) {
            normalized.push_back(std::move(observed));
        } else {
            normalized[position->second]
                .authenticated_report_expansion |=
                    observed.authenticated_report_expansion;
        }
    }

    std::vector<macro_expansion_record> result;
    result.reserve(normalized.size());
    for (observed_macro_expansion& observed : normalized) {
        if (!report_record_id(observed.expansion.name).empty()
            && !observed.authenticated_report_expansion) {
            fact_failure(
                observed.expansion.name,
                "report macro expansion is not authenticated by "
                "exact REL_17_10 header provenance");
        }
        result.push_back(std::move(observed.expansion));
    }
    return result;
}

struct resolved_target_set {
    resolution_status resolution{resolution_status::direct};
    bool complete{};
    std::vector<const clang::FunctionDecl*> targets;
    std::vector<const clang::LambdaExpr*> lambdas;
};

void resolve_callable_expression(
    const clang::Expr* expression,
    std::set<const clang::FunctionDecl*>& targets,
    std::set<const clang::VarDecl*>& variables,
    std::set<const clang::LambdaExpr*>& lambdas,
    bool& complete) {
    if (expression == nullptr) {
        complete = false;
        return;
    }
    expression = expression->IgnoreParenImpCasts();
    if (const auto* reference =
            clang::dyn_cast<clang::DeclRefExpr>(expression)) {
        if (const auto* function =
                clang::dyn_cast<clang::FunctionDecl>(
                    reference->getDecl())) {
            targets.insert(function);
            return;
        }
        if (const auto* variable =
                clang::dyn_cast<clang::VarDecl>(
                    reference->getDecl())) {
            if (!variables.insert(variable).second
                || !variable->hasInit()
                || (!variable->isConstexpr()
                    && !variable->getType().isConstQualified())) {
                complete = false;
                return;
            }
            resolve_callable_expression(
                variable->getInit(),
                targets,
                variables,
                lambdas,
                complete);
            return;
        }
    }
    if (const auto* lambda =
            clang::dyn_cast<clang::LambdaExpr>(expression)) {
        targets.insert(lambda->getCallOperator());
        lambdas.insert(lambda);
        return;
    }
    if (const auto* unary =
            clang::dyn_cast<clang::UnaryOperator>(expression)) {
        if (unary->getOpcode() == clang::UO_AddrOf
            || unary->getOpcode() == clang::UO_Deref) {
            resolve_callable_expression(
                unary->getSubExpr(),
                targets,
                variables,
                lambdas,
                complete);
            return;
        }
    }
    if (const auto* conditional =
            clang::dyn_cast<clang::ConditionalOperator>(expression)) {
        resolve_callable_expression(
            conditional->getTrueExpr(),
            targets,
            variables,
            lambdas,
            complete);
        resolve_callable_expression(
            conditional->getFalseExpr(),
            targets,
            variables,
            lambdas,
            complete);
        return;
    }
    if (const auto* binary =
            clang::dyn_cast<clang::BinaryConditionalOperator>(
                expression)) {
        resolve_callable_expression(
            binary->getCommon(),
            targets,
            variables,
            lambdas,
            complete);
        resolve_callable_expression(
            binary->getFalseExpr(),
            targets,
            variables,
            lambdas,
            complete);
        return;
    }
    if (const auto* bind =
            clang::dyn_cast<clang::CXXBindTemporaryExpr>(expression)) {
        resolve_callable_expression(
            bind->getSubExpr(),
            targets,
            variables,
            lambdas,
            complete);
        return;
    }
    if (const auto* materialized =
            clang::dyn_cast<clang::MaterializeTemporaryExpr>(
                expression)) {
        resolve_callable_expression(
            materialized->getSubExpr(),
            targets,
            variables,
            lambdas,
            complete);
        return;
    }
    complete = false;
}

[[nodiscard]] resolved_target_set targets_of(
    const clang::CallExpr* call) {
    if (const clang::FunctionDecl* direct =
            call->getDirectCallee()) {
        if (const auto* method =
                clang::dyn_cast<clang::CXXMethodDecl>(direct);
            method != nullptr && method->isVirtual()
            && !method->hasAttr<clang::FinalAttr>()
            && !(method->getParent() != nullptr
                 && method->getParent()->hasAttr<clang::FinalAttr>())) {
            return {
                .resolution =
                    resolution_status::unresolved_indirect,
                .complete = false,
            };
        }
        return {
            .resolution = resolution_status::direct,
            .complete = true,
            .targets = {direct},
        };
    }
    std::set<const clang::FunctionDecl*> targets;
    std::set<const clang::VarDecl*> variables;
    std::set<const clang::LambdaExpr*> lambdas;
    bool complete = true;
    resolve_callable_expression(
        call->getCallee(),
        targets,
        variables,
        lambdas,
        complete);
    return {
        .resolution = complete && !targets.empty()
            ? resolution_status::resolved_indirect
            : resolution_status::unresolved_indirect,
        .complete = complete && !targets.empty(),
        .targets = {targets.begin(), targets.end()},
        .lambdas = {lambdas.begin(), lambdas.end()},
    };
}

struct call_candidate {
    const clang::Stmt* expression{};
    const clang::CFGElement* implicit_element{};
    bool require_exact_cfg_stmt{};
    std::string discriminator;
    resolved_target_set resolution;
    std::vector<const clang::Expr*> arguments;
    std::vector<const clang::LambdaExpr*> bridge_lambdas;
    const clang::FunctionDecl* bridge_implementation{};
    bridge_kind bridge{bridge_kind::none};
    bool generated_boundary_tail{};
};

class explicit_call_collector final
    : public clang::RecursiveASTVisitor<explicit_call_collector> {
  public:
    explicit explicit_call_collector(
        clang::ASTContext& context)
        : context_{context} {
    }

    bool VisitCallExpr(clang::CallExpr* call) {
        call_candidate candidate{
            .expression = call,
            .require_exact_cfg_stmt = true,
            .discriminator = "call",
            .resolution = targets_of(call),
        };
        candidate.arguments.reserve(call->getNumArgs());
        for (const clang::Expr* argument : call->arguments()) {
            candidate.arguments.push_back(argument);
        }
        const clang::FunctionDecl* direct =
            call->getDirectCallee();
        const std::string direct_name =
            direct == nullptr ? std::string{} : qualified_name(direct);
        const bridge_kind exact_bridge =
            direct_name
                    == "bioseq::postgres::support::invoke_pg_as_cpp"
            ? bridge_kind::invoke_pg_as_cpp
            : direct_name
                    == "bioseq::postgres::support::"
                       "invoke_pg_across_transactions_as_cpp"
            ? bridge_kind::cross_transaction_callback
            : bridge_kind::none;
        if (exact_bridge != bridge_kind::none
            && call->getNumArgs() != 0) {
            const clang::Expr* callable =
                call->getArg(0)->IgnoreParenImpCasts();
            std::set<const clang::FunctionDecl*> targets;
            std::set<const clang::VarDecl*> variables;
            std::set<const clang::LambdaExpr*> lambdas;
            bool complete = true;
            resolve_callable_expression(
                callable,
                targets,
                variables,
                lambdas,
                complete);
            candidate.bridge = exact_bridge;
            candidate.bridge_implementation = direct;
            candidate.resolution = {
                .resolution =
                    complete && targets.size() == 1
                    ? (variables.empty()
                           ? resolution_status::direct
                           : resolution_status::resolved_indirect)
                    : resolution_status::unresolved_indirect,
                .complete = complete && targets.size() == 1,
                .targets = {targets.begin(), targets.end()},
                .lambdas = {lambdas.begin(), lambdas.end()},
            };
            candidate.bridge_lambdas.assign(
                lambdas.begin(), lambdas.end());
        }
        calls_.push_back(std::move(candidate));
        return true;
    }

    bool VisitCXXConstructExpr(
        clang::CXXConstructExpr* expression) {
        call_candidate candidate{
            .expression = expression,
            .discriminator = "constructor",
            .resolution = {
                .resolution = resolution_status::direct,
                .complete = true,
                .targets = {expression->getConstructor()},
            },
        };
        candidate.arguments.reserve(expression->getNumArgs());
        for (const clang::Expr* argument : expression->arguments()) {
            candidate.arguments.push_back(argument);
        }
        calls_.push_back(std::move(candidate));
        return true;
    }

    bool VisitCXXNewExpr(clang::CXXNewExpr* expression) {
        if (const clang::FunctionDecl* allocation =
                expression->getOperatorNew()) {
            calls_.push_back({
                .expression = expression,
                .discriminator = "operator-new",
                .resolution = {
                    .resolution = resolution_status::direct,
                    .complete = true,
                    .targets = {allocation},
                },
            });
        }
        return true;
    }

    bool VisitCXXDeleteExpr(clang::CXXDeleteExpr* expression) {
        if (const clang::FunctionDecl* deallocation =
                expression->getOperatorDelete()) {
            calls_.push_back({
                .expression = expression,
                .discriminator = "operator-delete",
                .resolution = {
                    .resolution = resolution_status::direct,
                    .complete = true,
                    .targets = {deallocation},
                },
            });
        }
        return true;
    }

    bool VisitGCCAsmStmt(clang::GCCAsmStmt* statement) {
        assembly_.push_back(statement);
        return true;
    }

    bool VisitMSAsmStmt(clang::MSAsmStmt* statement) {
        assembly_.push_back(statement);
        return true;
    }

    bool TraverseLambdaExpr(clang::LambdaExpr* expression) {
        for (const clang::Expr* initializer :
             expression->capture_inits()) {
            if (!TraverseStmt(
                    const_cast<clang::Expr*>(initializer))) {
                return false;
            }
        }
        return true;
    }

    bool TraverseIfStmt(clang::IfStmt* statement) {
        if (!statement->isConstexpr()) {
            return clang::RecursiveASTVisitor<
                explicit_call_collector>::TraverseIfStmt(statement);
        }
        const std::optional<const clang::Stmt*> active =
            statement->getNondiscardedCase(context_);
        if (!active) {
            return clang::RecursiveASTVisitor<
                explicit_call_collector>::TraverseIfStmt(statement);
        }
        if (!TraverseStmt(statement->getInit())
            || !TraverseStmt(
                statement->getConditionVariableDeclStmt())
            || !TraverseStmt(statement->getCond())) {
            return false;
        }
        return *active == nullptr
            || TraverseStmt(
                const_cast<clang::Stmt*>(*active));
    }

    [[nodiscard]] std::vector<call_candidate> take_calls() {
        return std::move(calls_);
    }

    [[nodiscard]] const std::vector<const clang::Stmt*>&
    assembly() const noexcept {
        return assembly_;
    }

  private:
    clang::ASTContext& context_;
    std::vector<call_candidate> calls_;
    std::vector<const clang::Stmt*> assembly_;
};

class descendant_collector final
    : public clang::RecursiveASTVisitor<descendant_collector> {
  public:
    explicit descendant_collector(
        clang::ASTContext& context)
        : context_{context} {
    }

    bool VisitStmt(clang::Stmt* statement) {
        statements.insert(statement);
        return true;
    }

    bool TraverseLambdaExpr(clang::LambdaExpr* expression) {
        statements.insert(expression);
        for (const clang::Expr* initializer :
             expression->capture_inits()) {
            if (!TraverseStmt(
                    const_cast<clang::Expr*>(initializer))) {
                return false;
            }
        }
        return true;
    }

    bool TraverseIfStmt(clang::IfStmt* statement) {
        if (!statement->isConstexpr()) {
            return clang::RecursiveASTVisitor<
                descendant_collector>::TraverseIfStmt(statement);
        }
        const std::optional<const clang::Stmt*> active =
            statement->getNondiscardedCase(context_);
        if (!active) {
            return clang::RecursiveASTVisitor<
                descendant_collector>::TraverseIfStmt(statement);
        }
        if (!TraverseStmt(statement->getInit())
            || !TraverseStmt(
                statement->getConditionVariableDeclStmt())
            || !TraverseStmt(statement->getCond())) {
            return false;
        }
        return *active == nullptr
            || TraverseStmt(
                const_cast<clang::Stmt*>(*active));
    }

    std::set<const clang::Stmt*> statements;

  private:
    clang::ASTContext& context_;
};

class temporary_collector final
    : public clang::RecursiveASTVisitor<temporary_collector> {
  public:
    explicit temporary_collector(
        clang::ASTContext& context)
        : context_{context} {
    }

    bool VisitCXXBindTemporaryExpr(
        clang::CXXBindTemporaryExpr* expression) {
        temporaries.insert(expression);
        return true;
    }

    bool TraverseLambdaExpr(clang::LambdaExpr* expression) {
        for (const clang::Expr* initializer :
             expression->capture_inits()) {
            if (!TraverseStmt(
                    const_cast<clang::Expr*>(initializer))) {
                return false;
            }
        }
        return true;
    }

    bool TraverseIfStmt(clang::IfStmt* statement) {
        if (!statement->isConstexpr()) {
            return clang::RecursiveASTVisitor<
                temporary_collector>::TraverseIfStmt(statement);
        }
        const std::optional<const clang::Stmt*> active =
            statement->getNondiscardedCase(context_);
        if (!active) {
            return clang::RecursiveASTVisitor<
                temporary_collector>::TraverseIfStmt(statement);
        }
        if (!TraverseStmt(statement->getInit())
            || !TraverseStmt(
                statement->getConditionVariableDeclStmt())
            || !TraverseStmt(statement->getCond())) {
            return false;
        }
        return *active == nullptr
            || TraverseStmt(
                const_cast<clang::Stmt*>(*active));
    }

    std::set<const clang::CXXBindTemporaryExpr*> temporaries;

  private:
    clang::ASTContext& context_;
};

[[nodiscard]] const clang::CXXDestructorDecl* destructor_for(
    clang::QualType type) {
    while (!type.isNull()) {
        const clang::ArrayType* array =
            type->getAsArrayTypeUnsafe();
        if (array == nullptr) break;
        type = array->getElementType();
    }
    const clang::CXXRecordDecl* record =
        type->getAsCXXRecordDecl();
    if (record == nullptr) return nullptr;
    record = record->getDefinition();
    return record == nullptr ? nullptr : record->getDestructor();
}

[[nodiscard]] std::vector<call_candidate>
implicit_dtor_candidates(
    const clang::CFGElement& element) {
    std::vector<call_candidate> result;
    if (const auto automatic =
            element.getAs<clang::CFGAutomaticObjDtor>()) {
        const clang::VarDecl* variable =
            automatic->getVarDecl();
        const clang::CXXDestructorDecl* destructor =
            destructor_for(variable->getType());
        if (destructor != nullptr) {
            result.push_back({
                .expression = automatic->getTriggerStmt(),
                .implicit_element = std::addressof(element),
                .discriminator = "automatic-destructor",
                .resolution = {
                    .resolution = resolution_status::direct,
                    .complete = true,
                    .targets = {destructor},
                },
            });
        }
    } else if (const auto temporary =
                   element.getAs<clang::CFGTemporaryDtor>()) {
        const clang::CXXBindTemporaryExpr* expression =
            temporary->getBindTemporaryExpr();
        const clang::CXXDestructorDecl* destructor =
            destructor_for(expression->getType());
        if (destructor != nullptr) {
            result.push_back({
                .expression = expression,
                .implicit_element = std::addressof(element),
                .discriminator = "temporary-destructor",
                .resolution = {
                    .resolution = resolution_status::direct,
                    .complete = true,
                    .targets = {destructor},
                },
            });
        }
    } else if (const auto deleted =
                   element.getAs<clang::CFGDeleteDtor>()) {
        const clang::CXXRecordDecl* record =
            deleted->getCXXRecordDecl();
        const clang::CXXDestructorDecl* destructor =
            record == nullptr ? nullptr : record->getDestructor();
        if (destructor != nullptr) {
            result.push_back({
                .expression = deleted->getDeleteExpr(),
                .implicit_element = std::addressof(element),
                .discriminator = "delete-destructor",
                .resolution = {
                    .resolution = resolution_status::direct,
                    .complete = true,
                    .targets = {destructor},
                },
            });
        }
    } else if (const auto member =
                   element.getAs<clang::CFGMemberDtor>()) {
        const clang::FieldDecl* field = member->getFieldDecl();
        const clang::CXXDestructorDecl* destructor =
            destructor_for(field->getType());
        if (destructor != nullptr) {
            result.push_back({
                .expression = nullptr,
                .implicit_element = std::addressof(element),
                .discriminator =
                    "member-destructor:"
                    + field->getNameAsString(),
                .resolution = {
                    .resolution = resolution_status::direct,
                    .complete = true,
                    .targets = {destructor},
                },
            });
        }
    } else if (const auto base =
                   element.getAs<clang::CFGBaseDtor>()) {
        const clang::CXXBaseSpecifier* specification =
            base->getBaseSpecifier();
        const clang::CXXDestructorDecl* destructor =
            specification == nullptr
            ? nullptr
            : destructor_for(specification->getType());
        if (destructor != nullptr) {
            result.push_back({
                .expression = nullptr,
                .implicit_element = std::addressof(element),
                .discriminator = "base-destructor",
                .resolution = {
                    .resolution = resolution_status::direct,
                    .complete = true,
                    .targets = {destructor},
                },
            });
        }
    } else if (const auto cleanup =
                   element.getAs<clang::CFGCleanupFunction>()) {
        if (const clang::FunctionDecl* function =
                cleanup->getFunctionDecl()) {
            result.push_back({
                .expression = nullptr,
                .implicit_element = std::addressof(element),
                .discriminator = "cleanup-function:"
                    + cleanup->getVarDecl()->getNameAsString(),
                .resolution = {
                    .resolution = resolution_status::direct,
                    .complete = true,
                    .targets = {function},
                },
            });
        }
    }
    return result;
}

[[nodiscard]] std::size_t expected_implicit_call_count(
    const clang::CFGElement& element) {
    return element.getAs<clang::CFGAutomaticObjDtor>()
            || element.getAs<clang::CFGTemporaryDtor>()
            || element.getAs<clang::CFGDeleteDtor>()
            || element.getAs<clang::CFGMemberDtor>()
            || element.getAs<clang::CFGBaseDtor>()
            || element.getAs<clang::CFGCleanupFunction>()
        ? 1U
        : 0U;
}

struct cfg_element_key {
    unsigned block{};
    std::size_t element{};

    auto operator<=>(const cfg_element_key&) const = default;
};

using object_state = std::set<std::string, std::less<>>;

struct cfg_lifetime_facts {
    std::map<cfg_element_key, object_state> before;
    std::map<std::string, flow_object, std::less<>> objects;
    std::string canonical_cfg;
    std::size_t block_count{};
    std::size_t element_count{};
};

void add_variable(
    const clang::VarDecl* variable,
    object_state& state,
    std::map<std::string, flow_object, std::less<>>& objects,
    clang::ASTContext& context) {
    if (variable == nullptr || !variable->hasLocalStorage()) return;
    flow_object object = flow_object_for(variable, context);
    state.insert(object.stable_object_id);
    objects.emplace(object.stable_object_id, std::move(object));
}

void add_temporary(
    const clang::CXXBindTemporaryExpr* temporary,
    std::string_view function_usr,
    object_state& state,
    std::map<std::string, flow_object, std::less<>>& objects,
    clang::ASTContext& context) {
    if (temporary == nullptr) return;
    flow_object object =
        temporary_object_for(temporary, function_usr, context);
    state.insert(object.stable_object_id);
    objects.emplace(object.stable_object_id, std::move(object));
}

void remove_variable(
    const clang::VarDecl* variable,
    object_state& state,
    clang::ASTContext& context) {
    if (variable == nullptr || !variable->hasLocalStorage()) return;
    state.erase(
        flow_object_for(variable, context).stable_object_id);
}

void remove_temporary(
    const clang::CXXBindTemporaryExpr* temporary,
    std::string_view function_usr,
    object_state& state,
    clang::ASTContext& context) {
    if (temporary == nullptr) return;
    state.erase(
        temporary_object_for(
            temporary, function_usr, context)
            .stable_object_id);
}

void transfer_lifetimes(
    const clang::CFGElement& element,
    std::string_view function_usr,
    object_state& state,
    std::map<std::string, flow_object, std::less<>>& objects,
    clang::ASTContext& context) {
    if (const auto statement = element.getAs<clang::CFGStmt>()) {
        if (const auto* declaration =
                clang::dyn_cast<clang::DeclStmt>(
                    statement->getStmt())) {
            for (const clang::Decl* item :
                 declaration->decls()) {
                add_variable(
                    clang::dyn_cast<clang::VarDecl>(item),
                    state,
                    objects,
                    context);
            }
        }
        if (const auto* handler =
                clang::dyn_cast<clang::CXXCatchStmt>(
                    statement->getStmt())) {
            add_variable(
                handler->getExceptionDecl(),
                state,
                objects,
                context);
        }
        temporary_collector temporaries{context};
        temporaries.TraverseStmt(
            const_cast<clang::Stmt*>(statement->getStmt()));
        for (const clang::CXXBindTemporaryExpr* temporary :
             temporaries.temporaries) {
            add_temporary(
                temporary,
                function_usr,
                state,
                objects,
                context);
        }
    }
    if (const auto destructor =
            element.getAs<clang::CFGAutomaticObjDtor>()) {
        remove_variable(
            destructor->getVarDecl(), state, context);
    }
    if (const auto lifetime =
            element.getAs<clang::CFGLifetimeEnds>()) {
        remove_variable(
            lifetime->getVarDecl(), state, context);
    }
    if (const auto scope =
            element.getAs<clang::CFGScopeEnd>()) {
        remove_variable(scope->getVarDecl(), state, context);
    }
    if (const auto temporary =
            element.getAs<clang::CFGTemporaryDtor>()) {
        remove_temporary(
            temporary->getBindTemporaryExpr(),
            function_usr,
            state,
            context);
    }
}

[[nodiscard]] cfg_lifetime_facts analyze_cfg_lifetimes(
    const clang::FunctionDecl* function,
    const clang::CFG& cfg,
    clang::ASTContext& context) {
    const std::string function_usr =
        stable_function_identity(function, context);
    std::map<unsigned, object_state> input;
    std::map<unsigned, object_state> output;
    std::map<std::string, flow_object, std::less<>> objects;
    object_state entry_state;
    for (const clang::ParmVarDecl* parameter :
         function->parameters()) {
        add_variable(parameter, entry_state, objects, context);
    }
    bool changed = true;
    std::size_t iterations = 0;
    const std::size_t maximum_iterations =
        std::max<std::size_t>(16, cfg.size() * 8);
    while (changed && iterations++ < maximum_iterations) {
        changed = false;
        for (const clang::CFGBlock* block : cfg) {
            object_state incoming;
            if (block->getBlockID()
                == cfg.getEntry().getBlockID()) {
                incoming = entry_state;
            }
            for (auto predecessor = block->pred_begin();
                 predecessor != block->pred_end();
                 ++predecessor) {
                const clang::CFGBlock* source =
                    predecessor->getReachableBlock();
                if (source == nullptr) continue;
                const object_state& predecessor_output =
                    output[source->getBlockID()];
                incoming.insert(
                    predecessor_output.begin(),
                    predecessor_output.end());
            }
            object_state state = incoming;
            for (const clang::CFGElement& element : *block) {
                transfer_lifetimes(
                    element,
                    function_usr,
                    state,
                    objects,
                    context);
            }
            if (input[block->getBlockID()] != incoming
                || output[block->getBlockID()] != state) {
                input[block->getBlockID()] = std::move(incoming);
                output[block->getBlockID()] = std::move(state);
                changed = true;
            }
        }
    }
    if (changed) {
        fact_failure(
            qualified_name(function),
            "CFG lifetime dataflow did not reach a fixed point");
    }

    cfg_lifetime_facts result{
        .objects = std::move(objects),
        .block_count = cfg.size(),
    };
    std::ostringstream canonical;
    std::vector<const clang::CFGBlock*> blocks;
    blocks.reserve(cfg.size());
    for (const clang::CFGBlock* block : cfg) {
        blocks.push_back(block);
    }
    std::ranges::sort(
        blocks,
        [](const clang::CFGBlock* left,
           const clang::CFGBlock* right) {
            return left->getBlockID() < right->getBlockID();
        });
    for (const clang::CFGBlock* block : blocks) {
        object_state state = input[block->getBlockID()];
        canonical << "B\t" << block->getBlockID() << "\tIN";
        for (const std::string& id : state) {
            canonical << "\t" << id;
        }
        canonical << "\tSUCC";
        std::vector<unsigned> successors;
        for (auto successor = block->succ_begin();
             successor != block->succ_end();
             ++successor) {
            if (const clang::CFGBlock* target =
                    successor->getReachableBlock()) {
                successors.push_back(target->getBlockID());
            }
        }
        std::ranges::sort(successors);
        for (unsigned successor : successors) {
            canonical << "\t" << successor;
        }
        canonical << "\n";
        std::size_t element_index = 0;
        for (const clang::CFGElement& element : *block) {
            const cfg_element_key key{
                .block = block->getBlockID(),
                .element = element_index++,
            };
            result.before.emplace(key, state);
            canonical << "E\t" << key.block << "\t"
                << key.element << "\t"
                << static_cast<unsigned>(element.getKind())
                << "\tLIVE";
            for (const std::string& id : state) {
                canonical << "\t" << id;
            }
            canonical << "\n";
            transfer_lifetimes(
                element,
                function_usr,
                state,
                result.objects,
                context);
            ++result.element_count;
        }
    }
    result.canonical_cfg = canonical.str();
    return result;
}

[[nodiscard]] flow_call_target flow_target_for(
    const clang::FunctionDecl* target,
    clang::ASTContext& context,
    const analysis_environment& environment) {
    const callee_domain domain =
        domain_of(target, context, environment);
    const clang::FunctionDecl* identity_target =
        domain == callee_domain::standard_library
            || domain == callee_domain::system_library
        ? external_provenance_declaration(target)
        : target;
    std::string usr =
        stable_function_identity(identity_target, context);
    if (usr.empty()) {
        usr = target_usr_for(identity_target, domain);
    }
    const std::string name =
        qualified_name(identity_target);
    if (usr.empty()) {
        fact_failure(
            name.empty() ? std::string_view{"call target"}
                         : std::string_view{name},
            "Clang target has no stable USR");
    }
    flow_call_target result{
        .target_usr = std::move(usr),
        .target_qualified_name = name.empty()
            ? target->getNameAsString()
            : name,
        .domain = domain,
    };
    if (domain == callee_domain::standard_library
        || domain == callee_domain::system_library
        || domain == callee_domain::compiler_builtin) {
        result.external_provenance =
            external_provenance_for(
                identity_target, domain, context, environment);
        if (result.external_provenance) {
            result.external_mode =
                result.external_provenance
                        ->user_code_callback_free
                ? external_call_mode::audited_no_user_code
                : external_call_mode::analyze_body;
        } else {
            result.external_mode = external_call_mode::unknown;
        }
    } else if (domain == callee_domain::postgresql_server) {
        result.pg_no_longjmp_allowlist_evidence =
            pg_allowlist_for(target, context, environment);
    }
    return result;
}

[[nodiscard]] std::optional<std::int64_t> constant_integer(
    const clang::Expr* expression,
    clang::ASTContext& context) {
    if (expression == nullptr || expression->isValueDependent()) {
        return std::nullopt;
    }
    clang::Expr::EvalResult evaluated;
    if (!expression->EvaluateAsInt(evaluated, context)) {
        return std::nullopt;
    }
    const llvm::APSInt& integer = evaluated.Val.getInt();
    if (!integer.isRepresentableByInt64()) {
        return std::nullopt;
    }
    return integer.getExtValue();
}

[[nodiscard]] std::optional<bridge_transition_evidence>
bridge_evidence_for(
    bridge_kind kind,
    const flow_call_target& target,
    const located_source_range& callsite,
    const clang::Stmt* transition_expression,
    const clang::FunctionDecl* implementation,
    const bridge_macro_inventory& macro_inventory,
    clang::ASTContext& context,
    const analysis_environment& environment);

class statement_presence_collector final
    : public clang::RecursiveASTVisitor<
          statement_presence_collector> {
  public:
    explicit statement_presence_collector(
        const clang::Stmt* target)
        : target_{target} {
    }

    bool VisitStmt(clang::Stmt* statement) {
        found |= statement == target_;
        return !found;
    }

    bool found{};

  private:
    const clang::Stmt* target_{};
};

[[nodiscard]] bool statement_contains(
    const clang::Stmt* root,
    const clang::Stmt* target) {
    if (root == nullptr || target == nullptr) return false;
    statement_presence_collector collector{target};
    collector.TraverseStmt(
        const_cast<clang::Stmt*>(root));
    return collector.found;
}

[[nodiscard]] bool range_contains(
    const located_source_range& outer,
    const located_source_range& inner) noexcept {
    return outer.source_file == inner.source_file
        && outer.span.begin.byte_offset
            <= inner.span.begin.byte_offset
        && inner.span.end.byte_offset
            <= outer.span.end.byte_offset;
}

[[nodiscard]] std::optional<bridge_macro_expansion>
macro_expansion_at(
    clang::SourceLocation location,
    std::string_view expected_name,
    const bridge_macro_inventory& inventory,
    const provenance_record& record,
    clang::ASTContext& context) {
    const clang::SourceManager& sources =
        context.getSourceManager();
    const clang::LangOptions& language = context.getLangOpts();
    if (location.isInvalid() || !location.isMacroID()
        || !sources.isMacroBodyExpansion(location)) {
        return std::nullopt;
    }
    const std::string immediate_name =
        clang::Lexer::getImmediateMacroName(
            location, sources, language)
            .str();
    if (immediate_name != expected_name) {
        return std::nullopt;
    }
    const clang::SourceLocation invocation_begin =
        sources.getImmediateMacroCallerLoc(location);
    if (invocation_begin.isInvalid()) return std::nullopt;

    if (sources.isLoadedSourceLocation(location)) {
        if (!invocation_begin.isFileID()) return std::nullopt;
        const clang::SourceLocation spelling =
            sources.getSpellingLoc(location);
        if (spelling.isInvalid() || !spelling.isFileID()) {
            return std::nullopt;
        }
        const clang::FileID definition_file_id =
            sources.getFileID(spelling);
        bool invalid_buffer =
            definition_file_id.isInvalid();
        llvm::StringRef definition_file;
        if (!invalid_buffer) {
            definition_file = sources.getBufferData(
                definition_file_id, &invalid_buffer);
        }
        if (invalid_buffer
            || record.upstream_span.begin.byte_offset
                > record.upstream_span.end.byte_offset
            || record.upstream_span.end.byte_offset
                > definition_file.size()) {
            return std::nullopt;
        }
        const std::size_t fragment_begin =
            record.upstream_span.begin.byte_offset;
        const std::size_t fragment_size =
            record.upstream_span.end.byte_offset
            - fragment_begin;
        return bridge_macro_expansion{
            .name = std::string{expected_name},
            .invocation_begin = invocation_begin,
            .invocation = located_range_of(
                clang::SourceRange{
                    invocation_begin, invocation_begin},
                sources,
                language,
                false),
            .definition = located_range_of(
                clang::SourceRange{spelling, spelling},
                sources,
                language,
                true),
            .definition_file_sha256 =
                bioseq::common::sha256_hex(
                    std::string_view{
                        definition_file.data(),
                        definition_file.size()}),
            .pinned_fragment_sha256 =
                bioseq::common::sha256_hex(
                    std::string_view{
                        definition_file.data()
                            + fragment_begin,
                        fragment_size}),
            .direct_file_invocation = true,
        };
    }
    if (!sources.isLocalSourceLocation(location)) {
        return std::nullopt;
    }

    const bridge_macro_expansion* match = nullptr;
    for (const bridge_macro_expansion& expansion : inventory) {
        if (expansion.name != expected_name
            || expansion.invocation_begin
                != invocation_begin) {
            continue;
        }
        if (match != nullptr) {
            fact_failure(
                expected_name,
                "one AST macro location maps to multiple active "
                "expansions");
        }
        match = std::addressof(expansion);
    }
    return match == nullptr
        ? std::nullopt
        : std::optional<bridge_macro_expansion>{*match};
}

void authenticate_bridge_macro(
    const bridge_macro_expansion& expansion,
    const provenance_record& record,
    const analysis_environment& environment,
    std::string_view implementation_name) {
    static constexpr std::string_view include_prefix{
        "src/include/"};
    if (!record.upstream_file.starts_with(include_prefix)) {
        fact_failure(
            implementation_name,
            "active PG macro definition is not pinned");
    }
    const std::string suffix =
        "/" + record.upstream_file.substr(include_prefix.size());
    const bool authenticated_root = std::ranges::any_of(
        environment.postgresql_header_roots,
        [&](const std::string& root) {
            return path_within(
                expansion.definition.source_file, root);
        });
    if (!expansion.direct_file_invocation
        || !authenticated_root
        || !expansion.definition.source_file.ends_with(suffix)
        || expansion.definition_file_sha256
            != record.upstream_file_sha256
        || expansion.pinned_fragment_sha256
            != record.fragment_sha256
        || expansion.definition.span.begin.byte_offset
            < record.upstream_span.begin.byte_offset
        || expansion.definition.span.end.byte_offset
            > record.upstream_span.end.byte_offset) {
        fact_failure(
            implementation_name,
            "active PG macro definition is not pinned");
    }
}

class do_statement_collector final
    : public clang::RecursiveASTVisitor<do_statement_collector> {
  public:
    bool VisitDoStmt(clang::DoStmt* statement) {
        statements.push_back(statement);
        return true;
    }

    std::vector<const clang::DoStmt*> statements;
};

class if_statement_collector final
    : public clang::RecursiveASTVisitor<if_statement_collector> {
  public:
    bool VisitIfStmt(clang::IfStmt* statement) {
        statements.push_back(statement);
        return true;
    }

    std::vector<const clang::IfStmt*> statements;
};

struct authenticated_pg_try_region {
    const clang::Stmt* try_branch{};
    const clang::Stmt* catch_branch{};
};

[[nodiscard]] authenticated_pg_try_region
require_authenticated_pg_try_region(
    const clang::FunctionDecl* definition,
    const clang::Stmt* protected_expression,
    const bridge_macro_inventory& inventory,
    const provenance_record& record,
    clang::ASTContext& context,
    const analysis_environment& environment,
    std::string_view protected_role) {
    const clang::Stmt* body =
        definition == nullptr ? nullptr : definition->getBody();
    if (body == nullptr || protected_expression == nullptr) {
        fact_failure(
            definition == nullptr
                ? protected_role
                : std::string_view{qualified_name(definition)},
            "no unique authenticated PG_TRY region");
    }
    const std::string implementation_name =
        qualified_name(definition);
    const located_source_range body_location =
        located_range_of(
            body->getSourceRange(),
            context.getSourceManager(),
            context.getLangOpts(),
            true);
    std::size_t body_macro_count = 0;
    for (const bridge_macro_expansion& expansion : inventory) {
        if ((expansion.name == "PG_TRY"
             || expansion.name == "PG_CATCH"
             || expansion.name == "PG_END_TRY")
            && range_contains(
                body_location, expansion.invocation)) {
            ++body_macro_count;
            authenticate_bridge_macro(
                expansion,
                record,
                environment,
                implementation_name);
        }
    }

    do_statement_collector do_statements;
    do_statements.TraverseStmt(
        const_cast<clang::Stmt*>(body));
    std::vector<authenticated_pg_try_region> regions;
    for (const clang::DoStmt* do_statement :
         do_statements.statements) {
        const std::optional<bridge_macro_expansion>
            try_expansion =
            macro_expansion_at(
                do_statement->getDoLoc(),
                "PG_TRY",
                inventory,
                record,
                context);
        const std::optional<bridge_macro_expansion>
            end_expansion =
            macro_expansion_at(
                do_statement->getWhileLoc(),
                "PG_END_TRY",
                inventory,
                record,
                context);
        if (!try_expansion || !end_expansion) {
            continue;
        }
        authenticate_bridge_macro(
            *try_expansion,
            record,
            environment,
            implementation_name);
        authenticate_bridge_macro(
            *end_expansion,
            record,
            environment,
            implementation_name);
        if_statement_collector if_statements;
        if_statements.TraverseStmt(
            const_cast<clang::Stmt*>(
                do_statement->getBody()));
        const clang::IfStmt* protected_if = nullptr;
        for (const clang::IfStmt* if_statement :
             if_statements.statements) {
            const std::optional<bridge_macro_expansion>
                if_try =
                macro_expansion_at(
                    if_statement->getIfLoc(),
                    "PG_TRY",
                    inventory,
                    record,
                    context);
            const std::optional<bridge_macro_expansion>
                catch_expansion =
                macro_expansion_at(
                    if_statement->getElseLoc(),
                    "PG_CATCH",
                    inventory,
                    record,
                    context);
            if (!if_try || !catch_expansion
                || if_try->invocation_begin
                    != try_expansion->invocation_begin) {
                continue;
            }
            authenticate_bridge_macro(
                *catch_expansion,
                record,
                environment,
                implementation_name);
            if (protected_if != nullptr) {
                fact_failure(
                    implementation_name,
                    "no unique authenticated PG_TRY region");
            }
            protected_if = if_statement;
        }
        if (protected_if != nullptr) {
            regions.push_back({
                .try_branch = protected_if->getThen(),
                .catch_branch = protected_if->getElse(),
            });
        }
    }
    const bool local_body =
        context.getSourceManager().isLocalSourceLocation(
            body->getBeginLoc());
    if ((local_body && body_macro_count != 3)
        || regions.size() != 1) {
        fact_failure(
            implementation_name,
            "no unique authenticated PG_TRY region");
    }
    const authenticated_pg_try_region& region = regions.front();
    if (!statement_contains(
            region.try_branch, protected_expression)
        || statement_contains(
            region.catch_branch, protected_expression)) {
        fact_failure(
            implementation_name,
            std::string{protected_role}
                + " is outside authenticated PG_TRY then branch");
    }
    return region;
}

class active_call_collector final
    : public clang::RecursiveASTVisitor<active_call_collector> {
  public:
    explicit active_call_collector(clang::ASTContext& context)
        : context_{context} {
    }

    bool VisitCallExpr(clang::CallExpr* call) {
        calls.push_back(call);
        return true;
    }

    bool TraverseLambdaExpr(clang::LambdaExpr*) {
        return true;
    }

    bool TraverseIfStmt(clang::IfStmt* statement) {
        if (!statement->isConstexpr()) {
            return clang::RecursiveASTVisitor<
                active_call_collector>::TraverseIfStmt(statement);
        }
        if (!TraverseStmt(statement->getInit())
            || !TraverseStmt(
                statement->getConditionVariableDeclStmt())
            || !TraverseStmt(statement->getCond())) {
            return false;
        }
        const std::optional<const clang::Stmt*> active =
            statement->getNondiscardedCase(context_);
        if (!active) {
            dependent_constexpr = true;
            return true;
        }
        if (*active == nullptr) return true;
        return TraverseStmt(
            const_cast<clang::Stmt*>(*active));
    }

    std::vector<const clang::CallExpr*> calls;
    bool dependent_constexpr{};

  private:
    clang::ASTContext& context_;
};

class lambda_collector final
    : public clang::RecursiveASTVisitor<lambda_collector> {
  public:
    bool VisitLambdaExpr(clang::LambdaExpr* expression) {
        lambdas.push_back(expression);
        return true;
    }

    std::vector<const clang::LambdaExpr*> lambdas;
};

inline constexpr std::string_view error_capture_contract_id{
    "bioseq.postgres.support.error_boundary/error_capture"};

inline constexpr std::string_view error_capture_contract_material{
    "bioseq.postgres.support.error_boundary/error_capture\n"
    "outer_catch_helper_call=1\n"
    "copy_error_data_in_nested_pg_try=1\n"
    "flush_error_state_on_copy_success=1\n"
    "flush_error_state_on_copy_failure=1\n"
    "capture_failure_flag_in_nested_pg_catch=1\n"
    "capture_failure_cpp_throw_after_outer_pg_end_try=1\n"
    "memory_context_switch_and_flush_are_reviewed_no_error_leaves=1\n"};

class named_member_reference_collector final
    : public clang::RecursiveASTVisitor<
          named_member_reference_collector> {
  public:
    explicit named_member_reference_collector(
        std::string_view member)
        : member_{member} {
    }

    bool VisitMemberExpr(clang::MemberExpr* expression) {
        found |= expression->getMemberNameInfo()
                     .getAsString()
            == member_;
        return !found;
    }

    bool found{};

  private:
    std::string member_;
};

[[nodiscard]] bool references_named_member(
    const clang::Stmt* statement,
    std::string_view member) {
    if (statement == nullptr) return false;
    named_member_reference_collector collector{member};
    collector.TraverseStmt(
        const_cast<clang::Stmt*>(statement));
    return collector.found;
}

class true_member_assignment_collector final
    : public clang::RecursiveASTVisitor<
          true_member_assignment_collector> {
  public:
    explicit true_member_assignment_collector(
        std::string_view member)
        : member_{member} {
    }

    bool VisitBinaryOperator(clang::BinaryOperator* expression) {
        if (!expression->isAssignmentOp()) return true;
        const auto* left = clang::dyn_cast<clang::MemberExpr>(
            expression->getLHS()->IgnoreParenImpCasts());
        const auto* value = clang::dyn_cast<clang::CXXBoolLiteralExpr>(
            expression->getRHS()->IgnoreParenImpCasts());
        if (left != nullptr && value != nullptr
            && value->getValue()
            && left->getMemberNameInfo().getAsString()
                == member_) {
            ++count;
        }
        return true;
    }

    std::size_t count{};

  private:
    std::string member_;
};

[[nodiscard]] std::size_t true_member_assignment_count(
    const clang::Stmt* statement,
    std::string_view member) {
    if (statement == nullptr) return 0;
    true_member_assignment_collector collector{member};
    collector.TraverseStmt(
        const_cast<clang::Stmt*>(statement));
    return collector.count;
}

class throw_expression_collector final
    : public clang::RecursiveASTVisitor<
          throw_expression_collector> {
  public:
    bool VisitCXXThrowExpr(clang::CXXThrowExpr* expression) {
        expressions.push_back(expression);
        return true;
    }

    std::vector<const clang::CXXThrowExpr*> expressions;
};

[[nodiscard]] std::string require_error_capture_contract(
    const clang::FunctionDecl* erased_definition,
    const authenticated_pg_try_region& outer_region,
    const std::vector<const clang::CallExpr*>& erased_calls,
    const bridge_macro_inventory& inventory,
    const provenance_record& record,
    clang::ASTContext& context,
    const analysis_environment& environment) {
    const clang::CallExpr* helper_call = nullptr;
    const clang::FunctionDecl* helper_definition = nullptr;
    for (const clang::CallExpr* call : erased_calls) {
        const clang::FunctionDecl* direct =
            call->getDirectCallee();
        if (direct == nullptr
            || direct->getName()
                != "capture_current_postgres_error_as_cpp") {
            continue;
        }
        if (helper_call != nullptr) {
            fact_failure(
                qualified_name(erased_definition),
                "error-capture contract has multiple helper calls");
        }
        helper_call = call;
        helper_definition = direct->getDefinition();
    }
    if (helper_call == nullptr || helper_definition == nullptr
        || !statement_contains(
            outer_region.catch_branch, helper_call)
        || statement_contains(
            outer_region.try_branch, helper_call)
        || owning_module(helper_definition)
            != "bioseq.postgres.support:error_boundary") {
        fact_failure(
            qualified_name(erased_definition),
            "outer PG_CATCH does not call the exact error-capture "
            "helper");
    }

    active_call_collector helper_calls{context};
    helper_calls.TraverseStmt(
        const_cast<clang::Stmt*>(
            helper_definition->getBody()));
    const clang::CallExpr* copy_error_data = nullptr;
    std::vector<const clang::CallExpr*> flush_error_state;
    std::size_t memory_context_switches = 0;
    for (const clang::CallExpr* call : helper_calls.calls) {
        const clang::FunctionDecl* direct =
            call->getDirectCallee();
        if (direct == nullptr) continue;
        const std::string leaf =
            unqualified_name(qualified_name(direct));
        if (leaf == "CopyErrorData") {
            if (copy_error_data != nullptr) {
                fact_failure(
                    qualified_name(helper_definition),
                    "error-capture helper has multiple "
                    "CopyErrorData calls");
            }
            copy_error_data = call;
        } else if (leaf == "FlushErrorState") {
            flush_error_state.push_back(call);
        } else if (leaf == "MemoryContextSwitchTo") {
            ++memory_context_switches;
        }
    }
    if (helper_calls.dependent_constexpr
        || copy_error_data == nullptr
        || flush_error_state.size() != 2
        || memory_context_switches != 3) {
        fact_failure(
            qualified_name(helper_definition),
            "error-capture helper call inventory differs from the "
            "reviewed semantic contract");
    }
    const authenticated_pg_try_region nested_region =
        require_authenticated_pg_try_region(
            helper_definition,
            copy_error_data,
            inventory,
            record,
            context,
            environment,
            "CopyErrorData");
    std::size_t success_flushes = 0;
    std::size_t failure_flushes = 0;
    for (const clang::CallExpr* flush : flush_error_state) {
        success_flushes += statement_contains(
            nested_region.try_branch, flush);
        failure_flushes += statement_contains(
            nested_region.catch_branch, flush);
    }
    if (success_flushes != 1 || failure_flushes != 1
        || true_member_assignment_count(
            nested_region.catch_branch,
            "error_capture_failed")
            != 1
        || true_member_assignment_count(
               nested_region.try_branch,
               "error_capture_failed")
            != 0) {
        fact_failure(
            qualified_name(helper_definition),
            "CopyErrorData/FlushErrorState recovery is not exact");
    }

    throw_expression_collector throws;
    throws.TraverseStmt(
        const_cast<clang::Stmt*>(
            erased_definition->getBody()));
    const clang::CXXThrowExpr* capture_failure_throw = nullptr;
    for (const clang::CXXThrowExpr* expression :
         throws.expressions) {
        const std::string text = source_text(
            expression->getSourceRange(),
            context.getSourceManager(),
            context.getLangOpts(),
            true);
        if (!text.contains("postgres_error_capture_failure")) {
            continue;
        }
        if (capture_failure_throw != nullptr) {
            fact_failure(
                qualified_name(erased_definition),
                "error-capture failure has multiple C++ throws");
        }
        capture_failure_throw = expression;
    }
    if (capture_failure_throw == nullptr) {
        fact_failure(
            qualified_name(erased_definition),
            "error-capture failure is not converted to C++");
    }
    if_statement_collector ifs;
    ifs.TraverseStmt(
        const_cast<clang::Stmt*>(
            erased_definition->getBody()));
    std::size_t guarded_throws = 0;
    for (const clang::IfStmt* statement : ifs.statements) {
        if (statement_contains(
                statement->getThen(),
                capture_failure_throw)
            && references_named_member(
                statement->getCond(),
                "error_capture_failed")) {
            ++guarded_throws;
        }
    }
    if (guarded_throws != 1) {
        fact_failure(
            qualified_name(erased_definition),
            "error-capture failure C++ throw is not exactly "
            "flag-guarded");
    }
    return bioseq::common::sha256_hex(
        error_capture_contract_material);
}

[[nodiscard]] const clang::FunctionDecl*
require_erased_bridge_definition(
    bridge_kind kind,
    const clang::FunctionDecl* wrapper_definition,
    const bridge_macro_inventory& inventory,
    const provenance_record& record,
    clang::ASTContext& context,
    const analysis_environment& environment) {
    const std::string expected_name =
        kind == bridge_kind::invoke_pg_as_cpp
        ? "bioseq::postgres::support::invoke_pg_as_cpp_erased"
        : "bioseq::postgres::support::"
          "invoke_pg_across_transactions_as_cpp_erased";
    active_call_collector calls{context};
    calls.TraverseStmt(
        const_cast<clang::Stmt*>(
            wrapper_definition->getBody()));
    const clang::CallExpr* erased_call = nullptr;
    for (const clang::CallExpr* call : calls.calls) {
        const clang::FunctionDecl* direct =
            call->getDirectCallee();
        const std::string name =
            direct == nullptr ? std::string{} : qualified_name(direct);
        if (name != expected_name) continue;
        if (erased_call != nullptr) {
            fact_failure(
                qualified_name(wrapper_definition),
                "bridge wrapper has no exact single erased call");
        }
        erased_call = call;
    }
    if (calls.dependent_constexpr || erased_call == nullptr
        || erased_call->getNumArgs() != 2) {
        fact_failure(
            qualified_name(wrapper_definition),
            "bridge wrapper has no exact single erased call");
    }
    lambda_collector lambdas;
    lambdas.TraverseStmt(
        const_cast<clang::Expr*>(erased_call->getArg(1)));
    if (lambdas.lambdas.size() != 1
        || lambdas.lambdas.front()->capture_size() != 0) {
        fact_failure(
            qualified_name(wrapper_definition),
            "erased bridge call lacks one noncapturing trampoline");
    }
    const clang::LambdaExpr* trampoline =
        lambdas.lambdas.front();
    std::size_t trampoline_conversion_calls = 0;
    for (const clang::CallExpr* call : calls.calls) {
        if (call == erased_call) continue;
        const clang::FunctionDecl* direct =
            call->getDirectCallee();
        const std::string name =
            direct == nullptr ? std::string{} : qualified_name(direct);
        const auto* conversion =
            clang::dyn_cast_or_null<clang::CXXConversionDecl>(
                direct);
        const bool exact_trampoline_conversion =
            conversion != nullptr
            && conversion->getParent()
                == trampoline->getLambdaClass()
            && conversion->getConversionType()
                ->isFunctionPointerType()
            && statement_contains(
                erased_call->getArg(1), call);
        if (exact_trampoline_conversion) {
            ++trampoline_conversion_calls;
            continue;
        }
        const std::string leaf = unqualified_name(name);
        const bool audited_standard_helper =
            name.starts_with("std::")
            && leaf != "invoke"
            && leaf != "operator()";
        if (direct == nullptr || !audited_standard_helper) {
            const std::string active_callee =
                name.empty() ? "<indirect>" : name;
            fact_failure(
                qualified_name(wrapper_definition),
                "bridge wrapper calls its callable outside the "
                "exact erased trampoline: active "
                + std::string{call->getStmtClassName()}
                + " callee " + active_callee);
        }
    }
    if (trampoline_conversion_calls > 1) {
        fact_failure(
            qualified_name(wrapper_definition),
            "erased bridge call has multiple active trampoline "
            "conversions");
    }

    const clang::FunctionDecl* erased =
        erased_call->getDirectCallee();
    const clang::FunctionDecl* definition =
        erased == nullptr ? nullptr : erased->getDefinition();
    if (definition == nullptr
        || qualified_name(definition) != expected_name
        || owning_module(definition)
            != "bioseq.postgres.support:error_boundary") {
        fact_failure(
            qualified_name(wrapper_definition),
            "erased bridge definition identity is unavailable");
    }
    if (!definition->isInlineSpecified()) {
        fact_failure(
            qualified_name(wrapper_definition),
            "erased bridge definition is not explicitly inline");
    }
    const located_source_range definition_location =
        located_range_of(
            definition->getSourceRange(),
            context.getSourceManager(),
            context.getLangOpts(),
            true);
    if (!definition_location.source_file.ends_with(
            "/common/postgres/support/error_boundary.cppm")) {
        fact_failure(
            qualified_name(wrapper_definition),
            "erased bridge definition identity is unavailable");
    }

    active_call_collector erased_calls{context};
    erased_calls.TraverseStmt(
        const_cast<clang::Stmt*>(definition->getBody()));
    const clang::CallExpr* callback_call = nullptr;
    for (const clang::CallExpr* call : erased_calls.calls) {
        if (call->getDirectCallee() != nullptr) continue;
        const clang::Expr* callee =
            call->getCallee()->IgnoreParenImpCasts();
        const auto* member =
            clang::dyn_cast<clang::MemberExpr>(callee);
        const auto* field =
            member == nullptr
            ? nullptr
            : clang::dyn_cast<clang::FieldDecl>(
                  member->getMemberDecl());
        if (field == nullptr || field->getName() != "body"
            || callback_call != nullptr) {
            fact_failure(
                expected_name,
                "erased bridge callback identity is not exact");
        }
        callback_call = call;
    }
    if (callback_call == nullptr) {
        fact_failure(
            expected_name,
            "erased bridge callback identity is not exact");
    }
    const authenticated_pg_try_region outer_region =
        require_authenticated_pg_try_region(
        definition,
        callback_call,
        inventory,
        record,
        context,
        environment,
        "erased callback");
    static_cast<void>(require_error_capture_contract(
        definition,
        outer_region,
        erased_calls.calls,
        inventory,
        record,
        context,
        environment));
    return definition;
}

class parameter_reference_collector final
    : public clang::RecursiveASTVisitor<
          parameter_reference_collector> {
  public:
    explicit parameter_reference_collector(
        const clang::FunctionDecl* function) {
        for (const clang::ParmVarDecl* parameter :
             function->parameters()) {
            parameters.insert(parameter);
        }
    }

    bool VisitDeclRefExpr(clang::DeclRefExpr* expression) {
        if (parameters.contains(expression->getDecl())) {
            found = true;
        }
        return !found;
    }

    std::set<const clang::ValueDecl*> parameters;
    bool found{};
};

[[nodiscard]] bool is_exact_std_invoke_call(
    const clang::CallExpr* call) {
    if (call == nullptr) return false;
    if (const clang::FunctionDecl* direct =
            call->getDirectCallee()) {
        return direct->getName() == "invoke"
            && direct->isInStdNamespace();
    }
    const clang::Expr* callee =
        call->getCallee()->IgnoreParenImpCasts();
    const auto* unresolved =
        clang::dyn_cast<clang::UnresolvedLookupExpr>(callee);
    if (unresolved == nullptr
        || unresolved->getName().getAsString() != "invoke"
        || unresolved->getNumDecls() == 0) {
        return false;
    }
    for (const clang::NamedDecl* declaration :
         unresolved->decls()) {
        if (declaration == nullptr
            || declaration->getName() != "invoke"
            || !declaration->isInStdNamespace()) {
            return false;
        }
    }
    return true;
}

void require_generated_tail_structure(
    const clang::FunctionDecl* definition,
    const clang::Stmt* transition_expression,
    clang::ASTContext& context) {
    const std::string implementation_name =
        qualified_name(definition);
    const auto reject = [&](std::string_view predicate) {
        fact_failure(
            implementation_name,
            "generated tail is not after exact catch/unwind "
            "region: " + std::string{predicate});
    };
    const auto* body =
        clang::dyn_cast_or_null<clang::CompoundStmt>(
            definition->getBody());
    if (body == nullptr) {
        reject("implementation body is not a compound statement");
    }
    if (transition_expression == nullptr) {
        reject("transition expression is absent");
    }
    const clang::CXXTryStmt* try_statement = nullptr;
    std::size_t try_index = 0;
    std::size_t transition_index = 0;
    std::size_t index = 0;
    bool transition_found = false;
    for (const clang::Stmt* statement : body->body()) {
        if (const auto* candidate =
                clang::dyn_cast<clang::CXXTryStmt>(statement)) {
            if (try_statement != nullptr) {
                reject("multiple top-level try statements");
            }
            try_statement = candidate;
            try_index = index;
        }
        if (statement_contains(
                statement, transition_expression)) {
            if (transition_found) {
                reject(
                    "transition appears in multiple top-level "
                    "statements");
            }
            transition_found = true;
            transition_index = index;
        }
        ++index;
    }
    if (try_statement == nullptr) {
        reject("top-level try statement is absent");
    }
    if (!transition_found) {
        reject(
            "transition expression is outside the selected "
            "implementation body");
    }
    if (transition_index <= try_index) {
        reject("transition is not lexically after the try statement");
    }
    if (try_statement->getNumHandlers() != 4) {
        reject(
            "handler count is "
            + std::to_string(try_statement->getNumHandlers())
            + ", expected 4");
    }
    const auto require_typed_handler =
        [&](std::size_t handler_index,
            std::string_view ordinal,
            std::string_view expected_record,
            bool const_referent,
            bool standard_record) {
            const clang::CXXCatchStmt* handler =
                try_statement->getHandler(handler_index);
            if (handler->getExceptionDecl() == nullptr) {
                reject(
                    std::string{ordinal}
                    + " handler has no exception declaration");
            }
            const clang::QualType caught =
                handler->getCaughtType();
            if (!caught->isLValueReferenceType()) {
                reject(
                    std::string{ordinal}
                    + " handler is not an lvalue reference: "
                    + caught.getAsString());
            }
            const clang::QualType referent =
                caught.getNonReferenceType();
            const clang::Qualifiers qualifiers =
                referent.getQualifiers();
            if ((const_referent && !qualifiers.hasOnlyConst())
                || (!const_referent && !qualifiers.empty())) {
                reject(
                    std::string{ordinal}
                    + " handler has incorrect referent qualifiers: "
                    + referent.getAsString());
            }
            const clang::CXXRecordDecl* record =
                referent.getUnqualifiedType()
                    ->getAsCXXRecordDecl();
            if (record == nullptr) {
                reject(
                    std::string{ordinal}
                    + " handler referent is not a record type");
            }
            if (standard_record) {
                if (record->getNameAsString() != expected_record
                    || !record->isInStdNamespace()) {
                    reject(
                        std::string{ordinal}
                        + " handler is not std::"
                        + std::string{expected_record});
                }
            } else if (qualified_name(record)
                       != expected_record) {
                reject(
                    std::string{ordinal}
                    + " handler record is "
                    + qualified_name(record));
            }
        };
    require_typed_handler(
        0,
        "first",
        "bioseq::postgres::support::postgres_error",
        false,
        false);
    require_typed_handler(
        1,
        "second",
        "bioseq::postgres::support::"
        "postgres_error_capture_failure",
        true,
        false);
    require_typed_handler(
        2,
        "third",
        "exception",
        true,
        true);
    const clang::CXXCatchStmt* catch_all =
        try_statement->getHandler(3);
    if (catch_all->getExceptionDecl() != nullptr) {
        reject("fourth handler has an exception declaration");
    }
    if (!catch_all->getCaughtType().isNull()) {
        reject(
            "fourth handler has a non-null caught type: "
            + catch_all->getCaughtType().getAsString());
    }
    active_call_collector try_calls{context};
    try_calls.TraverseStmt(
        const_cast<clang::CompoundStmt*>(
            try_statement->getTryBlock()));
    bool invokes_parameter = false;
    for (const clang::CallExpr* call : try_calls.calls) {
        if (!is_exact_std_invoke_call(call)) continue;
        parameter_reference_collector references{definition};
        references.TraverseStmt(
            const_cast<clang::CallExpr*>(call));
        invokes_parameter |= references.found;
    }
    if (!invokes_parameter) {
        reject(
            "try block has no exact std::invoke call referencing "
            "a boundary parameter");
    }
}

[[nodiscard]] std::optional<bridge_transition_evidence>
bridge_evidence_for(
    bridge_kind kind,
    const flow_call_target& target,
    const located_source_range& callsite,
    const clang::Stmt* transition_expression,
    const clang::FunctionDecl* implementation,
    const bridge_macro_inventory& macro_inventory,
    clang::ASTContext& context,
    const analysis_environment& environment) {
    if (kind == bridge_kind::none
        || environment.provenance == nullptr) {
        return std::nullopt;
    }
    const std::string_view record_id =
        kind == bridge_kind::generated_boundary_tail
        ? std::string_view{"pg17.rethrow_declarations"}
        : std::string_view{"pg17.pg_try_rethrow_macros"};
    const provenance_record* record = provenance_by_id(
        *environment.provenance, record_id);
    if (record == nullptr
        || record->kind != provenance_kind::bridge_primitive) {
        fact_failure(
            target.target_qualified_name,
            "required pinned bridge primitive is absent");
    }
    if (implementation == nullptr
        || !implementation->doesThisDeclarationHaveABody()) {
        fact_failure(
            target.target_qualified_name,
            "bridge implementation body is unavailable");
    }
    const clang::FunctionDecl* definition =
        implementation->getDefinition();
    if (definition == nullptr) definition = implementation;
    const clang::Stmt* body = definition->getBody();
    if (body == nullptr) {
        fact_failure(
            target.target_qualified_name,
            "bridge implementation definition has no body");
    }
    const std::string implementation_name =
        qualified_name(definition);
    const std::string implementation_module =
        owning_module(definition);
    const clang::SourceManager& sources =
        context.getSourceManager();
    const clang::LangOptions& language = context.getLangOpts();
    const located_source_range definition_location =
        located_range_of(
            definition->getSourceRange(),
            sources,
            language,
            true);
    const std::string body_text = source_text(
        body->getSourceRange(), sources, language, true);
    std::string expected_name;
    std::string expected_module;
    std::string expected_source_suffix;
    std::string region_id;
    if (kind == bridge_kind::invoke_pg_as_cpp) {
        expected_name =
            "bioseq::postgres::support::invoke_pg_as_cpp";
        expected_module =
            "bioseq.postgres.support:error_boundary";
        expected_source_suffix =
            "/common/postgres/support/error_boundary.cppm";
        region_id =
            "bioseq.postgres.support.error_boundary/invoke_pg_as_cpp";
    } else if (
        kind == bridge_kind::cross_transaction_callback) {
        expected_name =
            "bioseq::postgres::support::"
            "invoke_pg_across_transactions_as_cpp";
        expected_module =
            "bioseq.postgres.support:error_boundary";
        expected_source_suffix =
            "/common/postgres/support/error_boundary.cppm";
        region_id =
            "bioseq.postgres.support.error_boundary/"
            "invoke_pg_across_transactions_as_cpp";
    } else if (kind == bridge_kind::subtransaction) {
        static const std::set<std::string, std::less<>>
            exact_regions{
                "bioseq::postgres::support::subtransaction::begin",
                "bioseq::postgres::support::subtransaction::commit",
                "bioseq::postgres::support::subtransaction::rollback",
                "bioseq::postgres::support::subtransaction::~subtransaction",
            };
        if (!exact_regions.contains(implementation_name)) {
            fact_failure(
                implementation_name,
                "SQL subtransaction bridge is not an exact "
                "reviewed implementation");
        }
        expected_name = implementation_name;
        expected_module = "bioseq.postgres.sql_boundary";
        expected_source_suffix =
            "/common/postgres/sql_boundary/"
            "call_context_implementation.cppm";
        region_id =
            "bioseq.postgres.support.sql_boundary.context/"
            + implementation_name.substr(
                implementation_name.rfind("::") + 2);
    } else if (kind == bridge_kind::generated_boundary_tail) {
        expected_name =
            "bioseq::postgres::support::invoke_postgres_callback";
        expected_module = "bioseq.postgres.sql_boundary";
        expected_source_suffix =
            "/common/postgres/sql_boundary/index.cppm";
        region_id =
            "bioseq.postgres.support.sql_boundary/rethrow_tail";
    } else {
        fact_failure(
            target.target_qualified_name,
            "specialized bridge has no exact reviewed region");
    }
    if (implementation_name != expected_name
        || implementation_module != expected_module
        || !definition_location.source_file.ends_with(
            expected_source_suffix)
        || body_text.empty()) {
        fact_failure(
            implementation_name,
            "bridge implementation identity/module/body does not "
            "match the exact protected region");
    }
    if (kind == bridge_kind::subtransaction) {
        static_cast<void>(require_authenticated_pg_try_region(
            definition,
            transition_expression,
            macro_inventory,
            *record,
            context,
            environment,
            "transition"));
    } else if (
        kind == bridge_kind::invoke_pg_as_cpp
        || kind == bridge_kind::cross_transaction_callback) {
        static_cast<void>(require_erased_bridge_definition(
            kind,
            definition,
            macro_inventory,
            *record,
            context,
            environment));
    } else if (
        kind == bridge_kind::generated_boundary_tail) {
        require_generated_tail_structure(
            definition, transition_expression, context);
    }
    const std::string implementation_usr =
        stable_function_identity(definition, context);
    if (implementation_usr.empty()) {
        fact_failure(
            implementation_name,
            "bridge implementation has no stable Clang USR");
    }
    const bool has_error_capture_contract =
        kind == bridge_kind::invoke_pg_as_cpp
        || kind == bridge_kind::cross_transaction_callback;
    return bridge_transition_evidence{
        .primitive =
            reference_from(*environment.provenance, *record),
        .target_usr = target.target_usr,
        .target_qualified_name = target.target_qualified_name,
        .transition_callsite = callsite,
        .implementation_usr = implementation_usr,
        .implementation_qualified_name = implementation_name,
        .implementation_module = implementation_module,
        .implementation_definition = definition_location,
        .implementation_body_sha256 =
            bioseq::common::sha256_hex(body_text),
        .implementation_contract_id =
            has_error_capture_contract
            ? std::string{error_capture_contract_id}
            : std::string{},
        .implementation_contract_sha256 =
            has_error_capture_contract
            ? bioseq::common::sha256_hex(
                  error_capture_contract_material)
            : std::string{},
        .protected_region_id = region_id,
    };
}

[[nodiscard]] std::vector<flow_object> objects_for_state(
    const object_state& state,
    const std::map<std::string, flow_object, std::less<>>& objects) {
    std::vector<flow_object> result;
    result.reserve(state.size());
    for (const std::string& id : state) {
        const auto found = objects.find(id);
        if (found == objects.end()) {
            fact_failure(
                id, "CFG references an unknown live object");
        }
        result.push_back(found->second);
    }
    return result;
}

[[nodiscard]] located_source_range fallback_callsite(
    const clang::FunctionDecl* function,
    clang::ASTContext& context) {
    const clang::Stmt* body = function->getBody();
    const clang::SourceRange range =
        body == nullptr
        ? function->getSourceRange()
        : clang::SourceRange{
              body->getEndLoc(), body->getEndLoc()};
    return located_range_of(
        range,
        context.getSourceManager(),
        context.getLangOpts(),
        false);
}

[[nodiscard]] flow_call_site materialize_call(
    const call_candidate& candidate,
    const cfg_element_key& cfg_key,
    const clang::FunctionDecl* function,
    const object_state& live_state,
    const std::map<std::string, flow_object, std::less<>>& objects,
    clang::Preprocessor& preprocessor,
    const bridge_macro_inventory& macro_inventory,
    clang::ASTContext& context,
    const analysis_environment& environment) {
    const clang::SourceManager& sources =
        context.getSourceManager();
    const clang::LangOptions& language = context.getLangOpts();
    const std::string caller_usr =
        stable_function_identity(function, context);
    const clang::SourceRange expression_range =
        candidate.expression == nullptr
        ? function->getSourceRange()
        : candidate.expression->getSourceRange();
    std::vector<macro_expansion_record> macro_chain =
        macro_chain_of(
            expression_range.getBegin(),
            preprocessor,
            context,
            environment);
    located_source_range spelling = located_range_of(
        expression_range, sources, language, true);
    located_source_range expansion = located_range_of(
        expression_range, sources, language, false);
    if (!macro_chain.empty()) {
        expansion = macro_chain.front().invocation;
    }
    if (spelling.source_file.empty()
        || expansion.source_file.empty()) {
        spelling = fallback_callsite(function, context);
        expansion = spelling;
        macro_chain.clear();
    }

    std::vector<flow_call_target> targets;
    targets.reserve(candidate.resolution.targets.size());
    for (const clang::FunctionDecl* target :
         candidate.resolution.targets) {
        targets.push_back(
            flow_target_for(target, context, environment));
    }
    std::ranges::sort(
        targets,
        [](const flow_call_target& left,
           const flow_call_target& right) {
            return std::tie(
                       left.target_usr,
                       left.target_qualified_name)
                < std::tie(
                       right.target_usr,
                       right.target_qualified_name);
        });

    bridge_kind bridge = candidate.bridge;
    const clang::FunctionDecl* bridge_implementation =
        candidate.bridge_implementation;
    const std::string caller_name = qualified_name(function);
    if (bridge == bridge_kind::none
        && targets.size() == 1
        && targets.front().domain
            == callee_domain::postgresql_server) {
        const std::string leaf =
            unqualified_name(
                targets.front().target_qualified_name);
        const bool exact_subtransaction_region =
            (caller_name
                    == "bioseq::postgres::support::subtransaction::begin"
                && leaf == "BeginInternalSubTransaction")
            || (caller_name
                    == "bioseq::postgres::support::subtransaction::commit"
                && leaf == "ReleaseCurrentSubTransaction")
            || ((caller_name
                     == "bioseq::postgres::support::subtransaction::rollback"
                 || caller_name
                     == "bioseq::postgres::support::subtransaction::~subtransaction")
                && leaf
                    == "RollbackAndReleaseCurrentSubTransaction");
        if (exact_subtransaction_region) {
            bridge = bridge_kind::subtransaction;
            bridge_implementation = function;
        } else if (caller_name
                == "bioseq::postgres::support::invoke_postgres_callback"
            && (leaf == "ReThrowError"
                || leaf == "pg_re_throw"
                || leaf == "errfinish"
                || leaf == "errfinish_cold")) {
            bridge = bridge_kind::generated_boundary_tail;
            bridge_implementation = function;
        }
    }

    std::vector<flow_object> captures;
    for (const clang::LambdaExpr* bridge_lambda :
         candidate.bridge_lambdas) {
        std::size_t ordinal = 0;
        for (const clang::LambdaCapture& capture :
             bridge_lambda->captures()) {
            captures.push_back(
                capture_object_for(
                    bridge_lambda,
                    capture,
                    ordinal++,
                    caller_usr,
                    context));
        }
    }
    std::ranges::sort(
        captures,
        {},
        &flow_object::stable_object_id);

    std::vector<constant_integer_argument> constants;
    constants.reserve(candidate.arguments.size());
    for (std::size_t ordinal = 0;
         ordinal < candidate.arguments.size();
         ++ordinal) {
        constants.push_back({
            .ordinal = ordinal,
            .value = constant_integer(
                candidate.arguments[ordinal], context),
        });
    }

    const std::string site_material =
        caller_usr + "\n" + candidate.discriminator + "\n"
        + expansion.source_file + "\n"
        + std::to_string(expansion.span.begin.byte_offset) + "\n"
        + std::to_string(expansion.span.end.byte_offset) + "\nCFG\t"
        + std::to_string(cfg_key.block) + "\t"
        + std::to_string(cfg_key.element);
    flow_call_site result{
        .stable_site_id = "clang-site:"
            + bioseq::common::sha256_hex(site_material).substr(0, 32),
        .caller_usr = caller_usr,
        .spelling = std::move(spelling),
        .expansion = std::move(expansion),
        .macro_chain = std::move(macro_chain),
        .resolution = candidate.resolution.resolution,
        .target_set_complete =
            candidate.resolution.complete,
        .targets = std::move(targets),
        .constant_integer_arguments = std::move(constants),
        .live_objects =
            objects_for_state(live_state, objects),
        .callable_capture_objects = std::move(captures),
        .bridge_transition = bridge,
        .analyzer_entered_bridge_callable =
            bridge == bridge_kind::invoke_pg_as_cpp
            || bridge
                == bridge_kind::cross_transaction_callback,
        .generated_boundary_tail =
            bridge
            == bridge_kind::generated_boundary_tail,
    };
    if (bridge != bridge_kind::none
        && result.targets.size() == 1) {
        result.bridge_primitive_evidence =
            bridge_evidence_for(
                bridge,
                result.targets.front(),
                result.expansion,
                candidate.expression,
                bridge_implementation,
                macro_inventory,
                context,
                environment);
    }
    return result;
}

enum class runtime_marker_kind : std::uint8_t {
    site,
    visit,
    trigger,
};

struct materialized_call_record {
    const call_candidate* candidate{};
    cfg_element_key cfg_key;
    flow_call_site fact;
};

struct runtime_marker {
    runtime_marker_kind kind{runtime_marker_kind::site};
    std::string key;
    const materialized_call_record* call{};
};

[[nodiscard]] std::string_view runtime_probe_target_name(
    runtime_marker_kind kind) noexcept {
    switch (kind) {
        case runtime_marker_kind::visit:
            return "bioseq::postgres::support::safety_runtime_probe::"
                   "record_site_visit";
        case runtime_marker_kind::trigger:
            return "bioseq::postgres::support::safety_runtime_probe::"
                   "consume_fault";
        case runtime_marker_kind::site:
            break;
    }
    return {};
}

[[nodiscard]] std::optional<runtime_marker_kind>
runtime_probe_target_kind(
    const call_candidate& candidate) {
    bool names_visit = false;
    bool names_trigger = false;
    for (const clang::FunctionDecl* target :
         candidate.resolution.targets) {
        const std::string name = qualified_name(target);
        names_visit |= name
            == runtime_probe_target_name(
                runtime_marker_kind::visit);
        names_trigger |= name
            == runtime_probe_target_name(
                runtime_marker_kind::trigger);
    }
    if (!names_visit && !names_trigger) return std::nullopt;
    if (names_visit && names_trigger) {
        fact_failure(
            "runtime probe",
            "one call resolves both visit and trigger targets");
    }
    return names_visit
        ? runtime_marker_kind::visit
        : runtime_marker_kind::trigger;
}

[[nodiscard]] bool exact_narrow_std_string_view_construction(
    const clang::CXXConstructExpr& constructed) {
    if (constructed.getNumArgs() != 1) return false;
    const auto* record = constructed.getType()
        .getCanonicalType()
        ->getAs<clang::RecordType>();
    const auto* specialization = record == nullptr
        ? nullptr
        : clang::dyn_cast<
              clang::ClassTemplateSpecializationDecl>(
              record->getDecl());
    const clang::ClassTemplateDecl* primary =
        specialization == nullptr
        ? nullptr
        : specialization->getSpecializedTemplate();
    if (primary == nullptr
        || primary->getName() != "basic_string_view"
        || !primary->isInStdNamespace()) {
        return false;
    }
    const clang::TemplateArgumentList& arguments =
        specialization->getTemplateArgs();
    if (arguments.size() == 0
        || arguments.get(0).getKind()
            != clang::TemplateArgument::Type) {
        return false;
    }
    const clang::QualType character =
        arguments.get(0)
            .getAsType()
            .getCanonicalType()
            .getUnqualifiedType();
    if (character.isNull()
        || (!character->isSpecificBuiltinType(
                clang::BuiltinType::Char_S)
            && !character->isSpecificBuiltinType(
                clang::BuiltinType::Char_U))) {
        return false;
    }
    const clang::CXXConstructorDecl* constructor =
        constructed.getConstructor();
    return constructor != nullptr
        && constructor->getParent()->getCanonicalDecl()
            == specialization->getCanonicalDecl();
}

[[nodiscard]] const clang::Expr* peel_runtime_literal(
    const clang::Expr* expression) {
    const clang::Expr* current = expression;
    while (current != nullptr) {
        const clang::Expr* ignored =
            current->IgnoreParenImpCasts();
        if (ignored != current) {
            current = ignored;
            continue;
        }
        if (const auto* cleanup =
                clang::dyn_cast<clang::ExprWithCleanups>(current)) {
            current = cleanup->getSubExpr();
            continue;
        }
        if (const auto* materialized =
                clang::dyn_cast<clang::MaterializeTemporaryExpr>(
                    current)) {
            current = materialized->getSubExpr();
            continue;
        }
        if (const auto* bound =
                clang::dyn_cast<clang::CXXBindTemporaryExpr>(
                    current)) {
            current = bound->getSubExpr();
            continue;
        }
        if (const auto* constructed =
                clang::dyn_cast<clang::CXXConstructExpr>(current)) {
            if (!exact_narrow_std_string_view_construction(
                    *constructed)) {
                return nullptr;
            }
            current = constructed->getArg(0);
            continue;
        }
        return current;
    }
    return nullptr;
}

[[nodiscard]] bool valid_runtime_probe_key(
    std::string_view key) noexcept {
    if (key.empty() || key.size() >= 129
        || key.find('\0') != std::string_view::npos) {
        return false;
    }
    return std::ranges::all_of(
        key,
        [](const char value) {
            return (value >= 'a' && value <= 'z')
                || (value >= '0' && value <= '9')
                || value == '_'
                || value == '.';
        });
}

[[nodiscard]] std::string ordinary_runtime_literal(
    const clang::Expr* argument,
    std::string_view context) {
    const clang::Expr* expression =
        peel_runtime_literal(argument);
    const auto* literal =
        clang::dyn_cast_or_null<clang::StringLiteral>(expression);
    if (literal == nullptr || !literal->isOrdinary()
        || literal->getCharByteWidth() != 1) {
        fact_failure(
            context,
            "runtime probe value must be one ordinary narrow "
            "string literal");
    }
    const llvm::StringRef bytes = literal->getBytes();
    return std::string{bytes.data(), bytes.size()};
}

[[nodiscard]] std::string runtime_literal_key(
    const call_candidate& candidate,
    std::string_view target_name) {
    if (candidate.arguments.size() != 1) {
        fact_failure(
            target_name,
            "runtime probe requires exactly one literal key");
    }
    const std::string key = ordinary_runtime_literal(
        candidate.arguments.front(), target_name);
    if (!valid_runtime_probe_key(key)) {
        fact_failure(
            target_name,
            "runtime probe literal key is empty or has invalid "
            "bytes");
    }
    return key;
}

[[nodiscard]] std::string runtime_marker_fault_key(
    const call_candidate& candidate,
    runtime_marker_kind kind) {
    return runtime_literal_key(
        candidate, runtime_probe_target_name(kind));
}

[[nodiscard]] bool call_has_macro(
    const flow_call_site& call,
    std::string_view name) noexcept {
    return std::ranges::any_of(
        call.macro_chain,
        [name](const macro_expansion_record& macro) {
            return macro.name == name;
        });
}

[[nodiscard]] const constant_integer_argument*
constant_argument(
    const flow_call_site& call,
    std::size_t ordinal) noexcept {
    const auto found = std::ranges::find(
        call.constant_integer_arguments,
        ordinal,
        &constant_integer_argument::ordinal);
    return found == call.constant_integer_arguments.end()
        ? nullptr
        : std::addressof(*found);
}

[[nodiscard]] bool exact_recoverable_postgres_site(
    const flow_call_site& call) {
    if (!call.target_set_complete
        || call.resolution != resolution_status::direct
        || call.targets.size() != 1) {
        return false;
    }
    const flow_call_target& target = call.targets.front();
    if (target.domain != callee_domain::postgresql_server) {
        return false;
    }
    const std::string leaf =
        unqualified_name(target.target_qualified_name);
    if (leaf == "ReThrowError"
        || leaf == "errfinish"
        || leaf == "errfinish_cold") {
        return true;
    }
    if (leaf == "pg_re_throw") {
        return false;
    }
    const bool report_macro =
        call_has_macro(call, "ereport")
        || call_has_macro(call, "elog")
        || call_has_macro(call, "ereport_domain")
        || call_has_macro(call, "elog_start");
    if (leaf == "errstart" || leaf == "errstart_cold") {
        const constant_integer_argument* severity =
            constant_argument(call, 0);
        return severity != nullptr
            && severity->value
            && *severity->value == 20;
    }
    if (report_macro) return true;
    return !target.pg_no_longjmp_allowlist_evidence;
}

class runtime_if_collector final
    : public clang::RecursiveASTVisitor<runtime_if_collector> {
  public:
    explicit runtime_if_collector(const clang::CallExpr* trigger)
        : trigger_{trigger} {
    }

    bool VisitIfStmt(clang::IfStmt* statement) {
        if (statement_contains(
                statement->getCond(), trigger_)) {
            matches.push_back(statement);
        }
        return true;
    }

    std::vector<const clang::IfStmt*> matches;

  private:
    const clang::CallExpr* trigger_{};
};

[[nodiscard]] const clang::Expr* peel_runtime_condition(
    const clang::Expr* expression) {
    const clang::Expr* current = expression;
    while (current != nullptr) {
        const clang::Expr* ignored =
            current->IgnoreParenImpCasts();
        if (ignored != current) {
            current = ignored;
            continue;
        }
        if (const auto* cleanup =
                clang::dyn_cast<clang::ExprWithCleanups>(current)) {
            current = cleanup->getSubExpr();
            continue;
        }
        return current;
    }
    return nullptr;
}

[[nodiscard]] const clang::IfStmt* exact_trigger_if(
    const materialized_call_record& trigger,
    const clang::Stmt* body) {
    const auto* call = clang::dyn_cast_or_null<clang::CallExpr>(
        trigger.candidate == nullptr
        ? nullptr
        : trigger.candidate->expression);
    if (call == nullptr) {
        fact_failure(
            trigger.fact.stable_site_id,
            "runtime trigger is not an exact CallExpr");
    }
    runtime_if_collector collector{call};
    collector.TraverseStmt(const_cast<clang::Stmt*>(body));
    std::erase_if(
        collector.matches,
        [call](const clang::IfStmt* statement) {
            return peel_runtime_condition(statement->getCond())
                != call;
        });
    if (collector.matches.size() != 1) {
        fact_failure(
            trigger.fact.stable_site_id,
            "runtime trigger must be the unnegated direct "
            "condition of exactly one if statement");
    }
    return collector.matches.front();
}

[[nodiscard]] bool cfg_reaches_statement_subtree(
    const clang::CFGBlock* start,
    const clang::Stmt* subtree) {
    if (start == nullptr || subtree == nullptr) return false;
    std::deque<const clang::CFGBlock*> pending{start};
    std::set<unsigned> visited;
    while (!pending.empty()) {
        const clang::CFGBlock* block = pending.front();
        pending.pop_front();
        if (block == nullptr
            || !visited.insert(block->getBlockID()).second) {
            continue;
        }
        for (const clang::CFGElement& element : *block) {
            if (const auto statement =
                    element.getAs<clang::CFGStmt>();
                statement
                && statement_contains(
                    subtree, statement->getStmt())) {
                return true;
            }
        }
        for (auto successor = block->succ_begin();
             successor != block->succ_end();
             ++successor) {
            if (const clang::CFGBlock* next =
                    successor->getReachableBlock()) {
                pending.push_back(next);
            }
        }
    }
    return false;
}

struct runtime_cfg_branches {
    const clang::CFGBlock* true_branch{};
    const clang::CFGBlock* false_branch{};
};

[[nodiscard]] runtime_cfg_branches exact_runtime_branches(
    const materialized_call_record& trigger,
    const clang::IfStmt* statement,
    const clang::CFG& cfg) {
    const clang::CFGBlock* condition_block = nullptr;
    for (const clang::CFGBlock* block : cfg) {
        if (block->getBlockID() == trigger.cfg_key.block) {
            condition_block = block;
            break;
        }
    }
    if (condition_block == nullptr
        || condition_block->getTerminatorStmt() != statement) {
        fact_failure(
            trigger.fact.stable_site_id,
            "runtime trigger CFG block is not terminated by its "
            "exact if statement");
    }
    std::vector<const clang::CFGBlock*> successors;
    for (auto successor = condition_block->succ_begin();
         successor != condition_block->succ_end();
         ++successor) {
        if (const clang::CFGBlock* block =
                successor->getReachableBlock()) {
            successors.push_back(block);
        }
    }
    if (successors.size() != 2) {
        fact_failure(
            trigger.fact.stable_site_id,
            "runtime trigger if statement does not have two "
            "reachable CFG branches");
    }
    const bool first_is_true =
        cfg_reaches_statement_subtree(
            successors[0], statement->getThen());
    const bool second_is_true =
        cfg_reaches_statement_subtree(
            successors[1], statement->getThen());
    if (first_is_true == second_is_true) {
        fact_failure(
            trigger.fact.stable_site_id,
            "runtime trigger true CFG branch is ambiguous");
    }
    return first_is_true
        ? runtime_cfg_branches{
              .true_branch = successors[0],
              .false_branch = successors[1]}
        : runtime_cfg_branches{
              .true_branch = successors[1],
              .false_branch = successors[0]};
}

struct runtime_frontier {
    std::set<std::string, std::less<>> call_ids;
    std::set<unsigned> terminal_blocks;
    std::set<unsigned> cfg_exit_blocks;
    bool reaches_exit{};
    bool contains_cycle{};
};

[[nodiscard]] std::string render_runtime_frontier(
    const runtime_frontier& frontier,
    const std::vector<runtime_marker>& markers) {
    std::string result = "{calls=[";
    bool first = true;
    for (const std::string& call_id : frontier.call_ids) {
        if (!first) result += ",";
        first = false;
        result += call_id;
        const auto marker = std::ranges::find_if(
            markers,
            [&](const runtime_marker& value) {
                return value.call->fact.stable_site_id
                    == call_id;
            });
        if (marker != markers.end()) {
            result += ":";
            switch (marker->kind) {
                case runtime_marker_kind::site:
                    result += "site";
                    break;
                case runtime_marker_kind::visit:
                    result += "visit";
                    break;
                case runtime_marker_kind::trigger:
                    result += "trigger";
                    break;
            }
            if (marker->call->fact.targets.size() == 1) {
                result += ":";
                result += marker->call->fact.targets.front()
                              .target_qualified_name;
            }
        }
    }
    result += "],terminal_blocks=[";
    first = true;
    for (const unsigned block : frontier.terminal_blocks) {
        if (!first) result += ",";
        first = false;
        result += std::to_string(block);
    }
    result += "],cfg_exit_blocks=[";
    first = true;
    for (const unsigned block : frontier.cfg_exit_blocks) {
        if (!first) result += ",";
        first = false;
        result += std::to_string(block);
    }
    result += "],exit=";
    result += frontier.reaches_exit ? "true" : "false";
    result += ",cycle=";
    result += frontier.contains_cycle ? "true" : "false";
    result += "}";
    return result;
}

[[nodiscard]] bool is_spurious_noexcept_member_exit(
    const clang::CFG& cfg,
    const clang::CFGBlock& block,
    std::size_t begin_element) {
    if (block.getTerminatorStmt() != nullptr
        || block.hasNoReturnElement()) {
        return false;
    }

    std::size_t exit_successors = 0;
    std::size_t normal_successors = 0;
    for (auto successor = block.succ_begin();
         successor != block.succ_end();
         ++successor) {
        const clang::CFGBlock* next =
            successor->getReachableBlock();
        if (next == nullptr) continue;
        if (next == std::addressof(cfg.getExit())) {
            ++exit_successors;
        } else {
            ++normal_successors;
        }
    }
    if (exit_successors != 1 || normal_successors != 1) {
        return false;
    }

    const clang::CXXMemberCallExpr* noexcept_member = nullptr;
    std::size_t element_index = 0;
    for (const clang::CFGElement& element : block) {
        if (element_index++ < begin_element) continue;
        if (expected_implicit_call_count(element) != 0) {
            return false;
        }
        const auto statement = element.getAs<clang::CFGStmt>();
        if (!statement) continue;
        const clang::Stmt* value = statement->getStmt();
        if (const auto* member =
                clang::dyn_cast<clang::CXXMemberCallExpr>(value)) {
            const auto* method =
                clang::dyn_cast_or_null<clang::CXXMethodDecl>(
                    member->getDirectCallee());
            const auto* prototype =
                method == nullptr
                ? nullptr
                : method->getType()
                      ->getAs<clang::FunctionProtoType>();
            if (noexcept_member != nullptr || method == nullptr
                || method->isStatic() || prototype == nullptr
                || !prototype->isNothrow()) {
                return false;
            }
            noexcept_member = member;
        } else if (clang::isa<
                       clang::CallExpr,
                       clang::CXXConstructExpr,
                       clang::CXXNewExpr,
                       clang::CXXDeleteExpr,
                       clang::CXXThrowExpr>(value)) {
            return false;
        }
    }
    return noexcept_member != nullptr;
}

[[nodiscard]] runtime_frontier runtime_event_frontier(
    const clang::CFG& cfg,
    const std::map<
        cfg_element_key,
        const runtime_marker*,
        std::less<>>& markers,
    std::vector<cfg_element_key> starts) {
    std::map<unsigned, const clang::CFGBlock*> blocks;
    for (const clang::CFGBlock* block : cfg) {
        blocks.emplace(block->getBlockID(), block);
    }
    std::map<cfg_element_key, runtime_frontier, std::less<>>
        memoized;
    std::set<cfg_element_key> active;
    const auto merge_frontier = [](
                                    runtime_frontier& destination,
                                    const runtime_frontier& source) {
        destination.call_ids.insert(
            source.call_ids.begin(), source.call_ids.end());
        destination.terminal_blocks.insert(
            source.terminal_blocks.begin(),
            source.terminal_blocks.end());
        destination.cfg_exit_blocks.insert(
            source.cfg_exit_blocks.begin(),
            source.cfg_exit_blocks.end());
        destination.reaches_exit |= source.reaches_exit;
        destination.contains_cycle |= source.contains_cycle;
    };
    std::function<runtime_frontier(cfg_element_key)> visit =
        [&](const cfg_element_key point) {
            if (const auto found = memoized.find(point);
                found != memoized.end()) {
                return found->second;
            }
            if (!active.insert(point).second) {
                return runtime_frontier{
                    .contains_cycle = true,
                };
            }
            runtime_frontier current;
            const auto block = blocks.find(point.block);
            if (block == blocks.end()) {
                fact_failure(
                    "runtime probe CFG",
                    "frontier references an unknown CFG block");
            }
            const runtime_marker* next_marker = nullptr;
            std::size_t next_element =
                std::numeric_limits<std::size_t>::max();
            for (const auto& [key, marker] : markers) {
                if (key.block == point.block
                    && key.element >= point.element
                    && key.element < next_element) {
                    next_marker = marker;
                    next_element = key.element;
                }
            }
            if (next_marker != nullptr) {
                current.call_ids.insert(
                    next_marker->call->fact.stable_site_id);
            } else {
                bool has_successor = false;
                const bool skip_spurious_exit =
                    is_spurious_noexcept_member_exit(
                        cfg, *block->second, point.element);
                for (auto successor =
                         block->second->succ_begin();
                     successor != block->second->succ_end();
                     ++successor) {
                    if (const clang::CFGBlock* next =
                            successor->getReachableBlock()) {
                        if (skip_spurious_exit
                            && next
                                == std::addressof(
                                    cfg.getExit())) {
                            continue;
                        }
                        has_successor = true;
                        merge_frontier(
                            current,
                            visit({
                                .block = next->getBlockID(),
                                .element = 0,
                            }));
                    }
                }
                if (!has_successor) {
                    current.reaches_exit = true;
                    current.terminal_blocks.insert(point.block);
                    if (block->second
                        == std::addressof(cfg.getExit())) {
                        current.cfg_exit_blocks.insert(point.block);
                    }
                }
            }
            active.erase(point);
            memoized.emplace(point, current);
            return current;
        };
    runtime_frontier result;
    for (const cfg_element_key start : starts) {
        merge_frontier(result, visit(start));
    }
    return result;
}

[[nodiscard]] runtime_frontier runtime_event_frontier_after(
    const clang::CFG& cfg,
    const std::map<
        cfg_element_key,
        const runtime_marker*,
        std::less<>>& markers,
    const materialized_call_record& call) {
    return runtime_event_frontier(
        cfg,
        markers,
        {{
            .block = call.cfg_key.block,
            .element = call.cfg_key.element + 1,
        }});
}

[[nodiscard]] runtime_frontier runtime_event_frontier_from(
    const clang::CFG& cfg,
    const std::map<
        cfg_element_key,
        const runtime_marker*,
        std::less<>>& markers,
    const clang::CFGBlock* block) {
    if (block == nullptr) {
        fact_failure(
            "runtime probe CFG",
            "frontier starts at an absent CFG branch");
    }
    return runtime_event_frontier(
        cfg,
        markers,
        {{
            .block = block->getBlockID(),
            .element = 0,
        }});
}

[[nodiscard]] bool exact_frontier_call(
    const runtime_frontier& frontier,
    std::string_view stable_site_id) noexcept {
    return !frontier.reaches_exit
        && !frontier.contains_cycle
        && frontier.call_ids.size() == 1
        && *frontier.call_ids.begin() == stable_site_id;
}

[[nodiscard]] std::set<unsigned>
runtime_reachable_blocks(const clang::CFG& cfg) {
    std::set<unsigned> result;
    std::deque<const clang::CFGBlock*> pending{
        std::addressof(cfg.getEntry())};
    while (!pending.empty()) {
        const clang::CFGBlock* block = pending.front();
        pending.pop_front();
        if (block == nullptr
            || !result.insert(block->getBlockID()).second) {
            continue;
        }
        for (auto successor = block->succ_begin();
             successor != block->succ_end();
             ++successor) {
            if (const clang::CFGBlock* next =
                    successor->getReachableBlock()) {
                pending.push_back(next);
            }
        }
    }
    return result;
}

[[nodiscard]] bool runtime_call_dominates(
    const clang::CFG& cfg,
    const std::set<unsigned>& reachable,
    const materialized_call_record& dominator,
    const materialized_call_record& dominated) {
    if (!reachable.contains(dominator.cfg_key.block)
        || !reachable.contains(dominated.cfg_key.block)) {
        return false;
    }
    if (dominator.cfg_key.block
        == dominated.cfg_key.block) {
        return dominator.cfg_key.element
            < dominated.cfg_key.element;
    }
    std::deque<const clang::CFGBlock*> pending{
        std::addressof(cfg.getEntry())};
    std::set<unsigned> visited;
    while (!pending.empty()) {
        const clang::CFGBlock* block = pending.front();
        pending.pop_front();
        if (block == nullptr
            || !visited.insert(block->getBlockID()).second
            || block->getBlockID()
                == dominator.cfg_key.block) {
            continue;
        }
        if (block->getBlockID()
            == dominated.cfg_key.block) {
            return false;
        }
        for (auto successor = block->succ_begin();
             successor != block->succ_end();
             ++successor) {
            if (const clang::CFGBlock* next =
                    successor->getReachableBlock()) {
                pending.push_back(next);
            }
        }
    }
    return true;
}

struct normal_runtime_projection {
    std::map<
        cfg_element_key,
        const runtime_marker*,
        std::less<>>
        markers;
    std::map<
        std::string,
        cfg_element_key,
        std::less<>>
        call_keys;
};

[[nodiscard]] normal_runtime_projection
project_runtime_markers_to_cfg(
    const clang::CFG& cfg,
    const std::vector<runtime_marker>& marker_storage) {
    std::map<
        const clang::Stmt*,
        std::vector<cfg_element_key>,
        std::less<>>
        statement_keys;
    for (const clang::CFGBlock* block : cfg) {
        std::size_t element_index = 0;
        for (const clang::CFGElement& element : *block) {
            const cfg_element_key key{
                .block = block->getBlockID(),
                .element = element_index++,
            };
            if (const auto statement =
                    element.getAs<clang::CFGStmt>()) {
                statement_keys[statement->getStmt()]
                    .push_back(key);
            }
        }
    }
    normal_runtime_projection result;
    for (const runtime_marker& marker : marker_storage) {
        const clang::Stmt* expression =
            marker.call->candidate == nullptr
            ? nullptr
            : marker.call->candidate->expression;
        const auto found = statement_keys.find(expression);
        if (expression == nullptr
            || found == statement_keys.end()
            || found->second.size() != 1) {
            continue;
        }
        const cfg_element_key key = found->second.front();
        if (!result.markers.emplace(
                key, std::addressof(marker)).second
            || !result.call_keys.emplace(
                    marker.call->fact.stable_site_id,
                    key).second) {
            fact_failure(
                marker.call->fact.stable_site_id,
                "runtime markers collide in the normal-return "
                "CFG projection");
        }
    }
    return result;
}

[[nodiscard]] runtime_frontier
normal_runtime_event_frontier_after(
    const clang::CFG& cfg,
    const normal_runtime_projection& projection,
    const runtime_marker& marker) {
    const auto key = projection.call_keys.find(
        marker.call->fact.stable_site_id);
    if (key == projection.call_keys.end()) {
        fact_failure(
            marker.call->fact.stable_site_id,
            "runtime marker disappeared from the normal-return "
            "CFG projection");
    }
    return runtime_event_frontier(
        cfg,
        projection.markers,
        {{
            .block = key->second.block,
            .element = key->second.element + 1,
        }});
}

[[nodiscard]] std::vector<runtime_probe_fact>
extract_runtime_probe_facts(
    const clang::FunctionDecl* function,
    const clang::CFG& cfg,
    const clang::CFG* normal_cfg,
    const std::vector<materialized_call_record>& calls) {
    const bool generated_boundary =
        qualified_name(function)
        == "bioseq::postgres::support::invoke_postgres_callback";
    std::vector<runtime_marker> marker_storage;
    marker_storage.reserve(calls.size());
    std::map<
        std::string,
        std::vector<const runtime_marker*>,
        std::less<>>
        visits;
    std::map<
        std::string,
        std::vector<const runtime_marker*>,
        std::less<>>
        triggers;
    for (const materialized_call_record& call : calls) {
        const std::optional<runtime_marker_kind> probe_kind =
            call.candidate == nullptr
            ? std::nullopt
            : runtime_probe_target_kind(*call.candidate);
        if (probe_kind) {
            const auto* expression =
                clang::dyn_cast_or_null<clang::CallExpr>(
                    call.candidate->expression);
            const clang::FunctionDecl* direct =
                expression == nullptr
                ? nullptr
                : expression->getDirectCallee();
            const auto* prototype =
                direct == nullptr
                ? nullptr
                : direct->getType()
                      ->getAs<clang::FunctionProtoType>();
            const std::string expected{
                runtime_probe_target_name(*probe_kind)};
            if (expression == nullptr || direct == nullptr
                || prototype == nullptr
                || !prototype->isNothrow()
                || call.candidate->resolution.resolution
                    != resolution_status::direct
                || call.candidate->resolution.targets.size() != 1
                || qualified_name(direct) != expected) {
                fact_failure(
                    call.fact.stable_site_id,
                    "runtime probe call must resolve directly to "
                    "its exact noexcept declaration");
            }
            marker_storage.push_back({
                .kind = *probe_kind,
                .key = runtime_marker_fault_key(
                    *call.candidate, *probe_kind),
                .call = std::addressof(call),
            });
            continue;
        }
        if (exact_recoverable_postgres_site(call.fact)) {
            marker_storage.push_back({
                .kind = runtime_marker_kind::site,
                .call = std::addressof(call),
            });
        }
    }
    std::map<
        cfg_element_key,
        const runtime_marker*,
        std::less<>>
        markers;
    for (const runtime_marker& marker : marker_storage) {
        if (!markers.emplace(
                marker.call->cfg_key,
                std::addressof(marker)).second) {
            fact_failure(
                marker.call->fact.stable_site_id,
                "multiple runtime-relevant calls occupy one exact "
                "CFG element");
        }
        if (marker.kind == runtime_marker_kind::visit) {
            visits[marker.key].push_back(std::addressof(marker));
        } else if (marker.kind
                   == runtime_marker_kind::trigger) {
            triggers[marker.key].push_back(
                std::addressof(marker));
        }
    }
    const std::set<unsigned> reachable =
        runtime_reachable_blocks(cfg);
    const std::optional<normal_runtime_projection>
        normal_projection =
            normal_cfg == nullptr
            ? std::nullopt
            : std::optional<normal_runtime_projection>{
                  project_runtime_markers_to_cfg(
                      *normal_cfg, marker_storage)};

    std::vector<runtime_probe_fact> result;
    for (const auto& [key, key_visits] : visits) {
        if (key_visits.size() != 1) {
            fact_failure(
                key,
                "runtime probe key has duplicate site visits in "
                "one function");
        }
        const runtime_marker& visit = *key_visits.front();
        const auto trigger_row = triggers.find(key);
        const std::size_t trigger_count =
            trigger_row == triggers.end()
            ? 0
            : trigger_row->second.size();
        if (trigger_count > 1) {
            fact_failure(
                key,
                "runtime probe key has duplicate fault triggers in "
                "one function");
        }
        const runtime_frontier after_visit =
            runtime_event_frontier_after(
                cfg, markers, *visit.call);
        if (trigger_count == 0) {
            if (after_visit.reaches_exit
                || after_visit.contains_cycle
                || after_visit.call_ids.size() != 1) {
                fact_failure(
                    key,
                    "natural runtime visit does not immediately "
                    "dominate one exact recoverable ERROR call");
            }
            const std::string& site_id =
                *after_visit.call_ids.begin();
            const auto site = std::ranges::find_if(
                marker_storage,
                [&](const runtime_marker& marker) {
                    return marker.kind
                            == runtime_marker_kind::site
                        && marker.call->fact.stable_site_id
                            == site_id;
                });
            if (site == marker_storage.end()) {
                fact_failure(
                    key,
                    "natural runtime visit reaches a non-site "
                    "runtime marker");
            }
            if (!runtime_call_dominates(
                    cfg,
                    reachable,
                    *visit.call,
                    *site->call)) {
                fact_failure(
                    key,
                    "natural runtime visit does not dominate its "
                    "recoverable ERROR call");
            }
            result.push_back({
                .fault_key = key,
                .relation =
                    runtime_relation::natural_at_site,
                .site_call_stable_id = site_id,
                .visit_call_stable_id =
                    visit.call->fact.stable_site_id,
                .trigger_call_stable_id = std::nullopt,
            });
            continue;
        }

        const runtime_marker& trigger =
            *trigger_row->second.front();
        const clang::IfStmt* trigger_if =
            exact_trigger_if(
                *trigger.call, function->getBody());
        const runtime_cfg_branches branches =
            exact_runtime_branches(
                *trigger.call, trigger_if, cfg);
        const runtime_frontier true_frontier =
            runtime_event_frontier_from(
                cfg, markers, branches.true_branch);
        const runtime_frontier false_frontier =
            runtime_event_frontier_from(
                cfg, markers, branches.false_branch);
        // Clang conservatively retains the post-catch ReThrowError
        // branch for the injected std::exception path.  Only the exact
        // generated boundary may carry those authenticated tail markers
        // beside the visit on its true-path frontier.
        const bool true_routes_to_visit = [&] {
            if (exact_frontier_call(
                    true_frontier,
                    visit.call->fact.stable_site_id)) {
                return true;
            }
            if (!generated_boundary
                || true_frontier.reaches_exit
                || true_frontier.contains_cycle
                || !true_frontier.call_ids.contains(
                    visit.call->fact.stable_site_id)) {
                return false;
            }
            return std::ranges::all_of(
                true_frontier.call_ids,
                [&](const std::string& id) {
                    if (id
                        == visit.call->fact.stable_site_id) {
                        return true;
                    }
                    const auto marker =
                        std::ranges::find_if(
                            marker_storage,
                            [&](const runtime_marker& value) {
                                return value.call->fact
                                           .stable_site_id
                                    == id;
                            });
                    return marker != marker_storage.end()
                        && marker->kind
                            == runtime_marker_kind::site
                        && marker->call->fact
                               .generated_boundary_tail;
                });
        }();

        std::optional<std::string> routed_site;
        if (true_routes_to_visit
            && after_visit.call_ids.size() == 1
            && !after_visit.reaches_exit
            && !after_visit.contains_cycle) {
            const std::string candidate =
                *after_visit.call_ids.begin();
            const auto site = std::ranges::find_if(
                marker_storage,
                [&](const runtime_marker& marker) {
                    return marker.kind
                            == runtime_marker_kind::site
                        && marker.call->fact.stable_site_id
                            == candidate;
                });
            if (site != marker_storage.end()
                && runtime_call_dominates(
                    cfg,
                    reachable,
                    *visit.call,
                    *site->call)
                && !false_frontier.contains_cycle
                && !false_frontier.call_ids.contains(
                    visit.call->fact.stable_site_id)
                && !false_frontier.call_ids.contains(candidate)) {
                routed_site = candidate;
            }
        }

        std::optional<std::string> successful_site;
        std::optional<std::string> injected_error;
        if (exact_frontier_call(
                after_visit,
                trigger.call->fact.stable_site_id)
            && true_frontier.call_ids.size() == 1
            && !true_frontier.reaches_exit
            && !true_frontier.contains_cycle) {
            const std::string candidate_injected =
                *true_frontier.call_ids.begin();
            const auto generated = std::ranges::find_if(
                marker_storage,
                [&](const runtime_marker& marker) {
                    return marker.kind
                            == runtime_marker_kind::site
                        && marker.call->fact.stable_site_id
                            == candidate_injected;
                });
            std::vector<const runtime_marker*>
                preceding_sites;
            if (normal_cfg == nullptr
                || !normal_projection) {
                fact_failure(
                    key,
                    "after-success runtime probe lacks an exact "
                    "normal-return CFG");
            }
            for (const runtime_marker& marker : marker_storage) {
                if (marker.kind != runtime_marker_kind::site) {
                    continue;
                }
                if (exact_frontier_call(
                        normal_runtime_event_frontier_after(
                            *normal_cfg,
                            *normal_projection,
                            marker),
                        visit.call->fact.stable_site_id)) {
                    preceding_sites.push_back(
                        std::addressof(marker));
                }
            }
            if (generated != marker_storage.end()
                && preceding_sites.size() == 1
                && preceding_sites.front()
                        ->call->fact.stable_site_id
                    != candidate_injected
                && runtime_call_dominates(
                    cfg,
                    reachable,
                    *preceding_sites.front()->call,
                    *visit.call)
                && runtime_call_dominates(
                    cfg,
                    reachable,
                    *visit.call,
                    *trigger.call)
                && runtime_call_dominates(
                    cfg,
                    reachable,
                    *trigger.call,
                    *generated->call)
                && !false_frontier.contains_cycle
                && !false_frontier.call_ids.contains(
                    candidate_injected)) {
                successful_site =
                    preceding_sites.front()
                        ->call->fact.stable_site_id;
                injected_error = candidate_injected;
            }
        }

        if (routed_site.has_value()
            == successful_site.has_value()) {
            fact_failure(
                key,
                "injected runtime probe does not prove exactly one "
                "supported CFG relation; true="
                    + render_runtime_frontier(
                        true_frontier, marker_storage)
                    + "; false="
                    + render_runtime_frontier(
                        false_frontier, marker_storage)
                    + "; after_visit="
                    + render_runtime_frontier(
                        after_visit, marker_storage)
                    + "; true_routes_to_visit="
                    + (true_routes_to_visit
                           ? std::string{"true"}
                           : std::string{"false"})
                    + "; routed_site="
                    + (routed_site
                           ? *routed_site
                           : std::string{"<none>"})
                    + "; successful_site="
                    + (successful_site
                           ? *successful_site
                           : std::string{"<none>"}));
        }
        if (routed_site) {
            result.push_back({
                .fault_key = key,
                .relation =
                    runtime_relation::injected_routes_to_site,
                .site_call_stable_id = *routed_site,
                .visit_call_stable_id =
                    visit.call->fact.stable_site_id,
                .trigger_call_stable_id =
                    trigger.call->fact.stable_site_id,
            });
        } else {
            static_cast<void>(injected_error);
            result.push_back({
                .fault_key = key,
                .relation =
                    runtime_relation::
                        injected_after_site_success,
                .site_call_stable_id = *successful_site,
                .visit_call_stable_id =
                    visit.call->fact.stable_site_id,
                .trigger_call_stable_id =
                    trigger.call->fact.stable_site_id,
            });
        }
    }
    for (const auto& [key, key_triggers] : triggers) {
        static_cast<void>(key_triggers);
        if (!visits.contains(key)) {
            fact_failure(
                key,
                "runtime fault trigger has no matching "
                "authenticated site visit");
        }
    }
    std::ranges::sort(
        result,
        [](const runtime_probe_fact& left,
           const runtime_probe_fact& right) {
            return std::tie(
                       left.fault_key,
                       left.site_call_stable_id,
                       left.visit_call_stable_id)
                < std::tie(
                       right.fault_key,
                       right.site_call_stable_id,
                       right.visit_call_stable_id);
        });
    return result;
}

struct analyzed_function {
    function_flow facts;
    std::vector<runtime_probe_fact> runtime_probes;
    std::vector<const clang::FunctionDecl*> body_targets;
};

[[nodiscard]] analyzed_function analyze_function(
    const clang::FunctionDecl* function,
    clang::Preprocessor& preprocessor,
    const bridge_macro_inventory& macro_inventory,
    clang::ASTContext& context,
    const analysis_environment& environment) {
    const clang::Stmt* body = function->getBody();
    if (body == nullptr) {
        fact_failure(
            qualified_name(function),
            "function selected for analysis has no body");
    }
    const std::string function_usr =
        stable_function_identity(function, context);
    if (function_usr.empty()) {
        fact_failure(
            qualified_name(function),
            "function definition has no stable Clang USR");
    }

    clang::CFG::BuildOptions options;
    options.PruneTriviallyFalseEdges = false;
    options.AddEHEdges = true;
    options.AddInitializers = true;
    options.AddImplicitDtors = true;
    options.AddLifetime = true;
    options.AddTemporaryDtors = true;
    options.AddScopes = true;
    options.AddCXXNewAllocator = true;
    options.AddCXXDefaultInitExprInCtors = true;
    options.AddCXXDefaultInitExprInAggregates = true;
    options.AddRichCXXConstructors = true;
    options.MarkElidedCXXConstructors = true;
    options.AddVirtualBaseBranches = true;
    options.setAllAlwaysAdd();
    std::unique_ptr<clang::CFG> cfg = clang::CFG::buildCFG(
        function,
        const_cast<clang::Stmt*>(body),
        std::addressof(context),
        options);
    if (!cfg) {
        fact_failure(
            qualified_name(function),
            "Clang could not build a complete CFG");
    }
    cfg_lifetime_facts lifetimes =
        analyze_cfg_lifetimes(function, *cfg, context);

    explicit_call_collector collector{context};
    if (const auto* constructor =
            clang::dyn_cast<clang::CXXConstructorDecl>(function)) {
        for (const clang::CXXCtorInitializer* initializer :
             constructor->inits()) {
            if (initializer != nullptr
                && initializer->getInit() != nullptr) {
                collector.TraverseStmt(
                    initializer->getInit());
            }
        }
    }
    collector.TraverseStmt(const_cast<clang::Stmt*>(body));
    std::vector<call_candidate> candidates =
        collector.take_calls();
    const std::size_t ast_explicit_callsite_count =
        candidates.size();
    std::size_t cfg_implicit_callsite_count = 0;

    struct element_record {
        cfg_element_key key;
        const clang::CFGElement* element{};
        std::set<const clang::Stmt*> descendants;
    };
    std::vector<element_record> elements;
    for (const clang::CFGBlock* block : *cfg) {
        std::size_t element_index = 0;
        for (const clang::CFGElement& element : *block) {
            element_record record{
                .key = {
                    .block = block->getBlockID(),
                    .element = element_index++,
                },
                .element = std::addressof(element),
            };
            if (const auto statement =
                    element.getAs<clang::CFGStmt>()) {
                descendant_collector descendants{context};
                descendants.TraverseStmt(
                    const_cast<clang::Stmt*>(
                        statement->getStmt()));
                record.descendants =
                    std::move(descendants.statements);
            } else if (const auto initializer =
                           element.getAs<clang::CFGInitializer>()) {
                const clang::CXXCtorInitializer* declaration =
                    initializer->getInitializer();
                if (declaration != nullptr
                    && declaration->getInit() != nullptr) {
                    descendant_collector descendants{context};
                    descendants.TraverseStmt(
                        declaration->getInit());
                    record.descendants =
                        std::move(descendants.statements);
                }
            }
            std::vector<call_candidate> implicit =
                implicit_dtor_candidates(element);
            const std::size_t expected_implicit =
                expected_implicit_call_count(element);
            if (implicit.size() != expected_implicit) {
                fact_failure(
                    qualified_name(function),
                    "CFG implicit call element has no exact emitted "
                    "callee edge");
            }
            cfg_implicit_callsite_count += expected_implicit;
            for (call_candidate& candidate : implicit) {
                candidates.push_back(std::move(candidate));
            }
            elements.push_back(std::move(record));
        }
    }

    std::vector<materialized_call_record> materialized_calls;
    materialized_calls.reserve(candidates.size());
    std::vector<std::string> unresolved;
    std::vector<const clang::FunctionDecl*> body_targets;
    std::set<std::string, std::less<>> call_ids;
    for (const call_candidate& candidate : candidates) {
        const element_record* matched = nullptr;
        if (candidate.implicit_element != nullptr) {
            std::size_t exact_matches = 0;
            for (const element_record& element : elements) {
                if (candidate.implicit_element == element.element) {
                    matched = std::addressof(element);
                    ++exact_matches;
                }
            }
            if (exact_matches != 1) {
                fact_failure(
                    qualified_name(function),
                    "implicit call edge lacks one exact CFG element: "
                        + candidate.discriminator);
            }
        } else if (candidate.expression != nullptr) {
            std::size_t exact_matches = 0;
            for (const element_record& element : elements) {
                const auto statement =
                    element.element->getAs<clang::CFGStmt>();
                if (statement
                    && statement->getStmt()
                        == candidate.expression) {
                    matched = std::addressof(element);
                    ++exact_matches;
                }
            }
            if (exact_matches > 1) {
                fact_failure(
                    qualified_name(function),
                    "AST call maps to multiple exact CFGStmt "
                    "elements: " + candidate.discriminator);
            }
            if (matched == nullptr
                && candidate.require_exact_cfg_stmt) {
                fact_failure(
                    qualified_name(function),
                    "CallExpr lacks one exact CFGStmt element");
            }
            if (matched == nullptr) {
                std::size_t best_descendant_count = 0;
                bool ambiguous = false;
                for (const element_record& element : elements) {
                    if (!element.descendants.contains(
                            candidate.expression)) {
                        continue;
                    }
                    const std::size_t descendant_count =
                        element.descendants.size();
                    if (matched == nullptr
                        || descendant_count
                            < best_descendant_count) {
                        matched = std::addressof(element);
                        best_descendant_count =
                            descendant_count;
                        ambiguous = false;
                    } else if (
                        descendant_count
                        == best_descendant_count) {
                        ambiguous = true;
                    }
                }
                if (ambiguous) {
                    fact_failure(
                        qualified_name(function),
                        "AST call maps ambiguously to containing "
                        "CFG elements: "
                            + candidate.discriminator);
                }
            }
        }
        if (matched == nullptr) {
            fact_failure(
                qualified_name(function),
                "AST call/implicit destructor is absent from the "
                "constructed CFG: " + candidate.discriminator);
        }
        object_state live = lifetimes.before.at(matched->key);
        if (candidate.expression != nullptr) {
            temporary_collector temporaries{context};
            temporaries.TraverseStmt(
                const_cast<clang::Stmt*>(
                    candidate.expression));
            for (const clang::CXXBindTemporaryExpr* temporary :
                 temporaries.temporaries) {
                add_temporary(
                    temporary,
                    function_usr,
                    live,
                    lifetimes.objects,
                    context);
            }
        }
        if (clang::isa<clang::CoroutineBodyStmt>(body)) {
            flow_object coroutine =
                coroutine_object_for(function, context);
            live.insert(coroutine.stable_object_id);
            lifetimes.objects.emplace(
                coroutine.stable_object_id,
                std::move(coroutine));
        }
        flow_call_site call = materialize_call(
            candidate,
            matched->key,
            function,
            live,
            lifetimes.objects,
            preprocessor,
            macro_inventory,
            context,
            environment);
        for (const flow_object& capture :
             call.callable_capture_objects) {
            const auto [existing, inserted] =
                lifetimes.objects.emplace(
                    capture.stable_object_id, capture);
            if (!inserted
                && (existing->second.canonical_type
                        != capture.canonical_type
                    || existing->second.declaration.source_file
                        != capture.declaration.source_file
                    || existing->second.declaration.span.begin.byte_offset
                        != capture.declaration.span.begin.byte_offset
                    || existing->second.declaration.span.end.byte_offset
                        != capture.declaration.span.end.byte_offset)) {
                fact_failure(
                    capture.stable_object_id,
                    "one callable-capture identity has conflicting "
                    "lifetime facts");
            }
        }
        if (!call_ids.insert(call.stable_site_id).second) {
            fact_failure(
                qualified_name(function),
                "two compiler call facts have one stable site ID");
        }
        if (!call.target_set_complete) {
            unresolved.push_back(call.stable_site_id);
        }
        for (const clang::FunctionDecl* target :
             candidate.resolution.targets) {
            const callee_domain domain =
                domain_of(target, context, environment);
            bool analyze_body =
                domain == callee_domain::project;
            if (domain == callee_domain::standard_library
                || domain == callee_domain::system_library
                || domain == callee_domain::compiler_builtin) {
                const auto provenance = external_provenance_for(
                    target, domain, context, environment);
                analyze_body =
                    provenance
                    && !provenance->user_code_callback_free;
            }
            if (analyze_body
                && target->doesThisDeclarationHaveABody()) {
                const clang::FunctionDecl* definition =
                    target->getDefinition();
                body_targets.push_back(
                    definition == nullptr ? target : definition);
            }
        }
        const auto* call_expression =
            clang::dyn_cast_or_null<clang::CallExpr>(
                candidate.expression);
        const clang::FunctionDecl* direct =
            call_expression == nullptr
            ? nullptr
            : call_expression->getDirectCallee();
        if (direct != nullptr
            && qualified_name(direct)
                == "bioseq::postgres::support::"
                   "invoke_postgres_callback") {
            if (candidate.resolution.resolution
                    != resolution_status::direct
                || !candidate.resolution.complete
                || candidate.resolution.targets.size() != 1
                || candidate.arguments.size() != 1) {
                fact_failure(
                    call.stable_site_id,
                    "generated callback does not resolve to one "
                    "exact specialization");
            }
            // Keep the selected specialization: getDefinition() may return
            // the shared function-template pattern for an implicit instance.
            body_targets.push_back(direct);
        }
        materialized_calls.push_back({
            .candidate = std::addressof(candidate),
            .cfg_key = matched->key,
            .fact = std::move(call),
        });
    }
    const bool needs_normal_cfg =
        std::ranges::any_of(
            materialized_calls,
            [](const materialized_call_record& call) {
                return call.candidate != nullptr
                    && runtime_probe_target_kind(
                           *call.candidate)
                        == runtime_marker_kind::trigger;
            });
    std::unique_ptr<clang::CFG> normal_cfg;
    if (needs_normal_cfg) {
        // The authoritative CFG retains EH edges for injected
        // throw/catch routing.  A separate projection is required to
        // prove what follows a PostgreSQL call only when it returns.
        options.AddEHEdges = false;
        normal_cfg = clang::CFG::buildCFG(
            function,
            const_cast<clang::Stmt*>(body),
            std::addressof(context),
            options);
        if (!normal_cfg) {
            fact_failure(
                qualified_name(function),
                "Clang could not build a complete normal-return "
                "CFG");
        }
    }
    std::vector<runtime_probe_fact> runtime_probes =
        extract_runtime_probe_facts(
            function,
            *cfg,
            normal_cfg.get(),
            materialized_calls);
    std::vector<flow_call_site> calls;
    calls.reserve(materialized_calls.size());
    for (materialized_call_record& call :
         materialized_calls) {
        calls.push_back(std::move(call.fact));
    }
    std::ranges::sort(
        calls,
        [](const flow_call_site& left,
           const flow_call_site& right) {
            return left.stable_site_id < right.stable_site_id;
        });
    std::ranges::sort(unresolved);
    unresolved.erase(
        std::unique(unresolved.begin(), unresolved.end()),
        unresolved.end());

    std::vector<std::string> assembly_sites;
    const clang::SourceManager& sources =
        context.getSourceManager();
    const clang::LangOptions& language = context.getLangOpts();
    for (const clang::Stmt* statement : collector.assembly()) {
        const located_source_range location = located_range_of(
            statement->getSourceRange(),
            sources,
            language,
            false);
        assembly_sites.push_back(
            location.source_file + ":"
            + std::to_string(location.span.begin.line) + ":"
            + std::to_string(location.span.begin.column));
    }
    std::ranges::sort(assembly_sites);

    const located_source_range declaration = located_range_of(
        function->getSourceRange(),
        sources,
        language,
        true);
    const located_source_range body_location = located_range_of(
        body->getSourceRange(),
        sources,
        language,
        true);
    const std::string body_text =
        source_text(
            body->getSourceRange(), sources, language, true);
    if (declaration.source_file.empty()
        || body_location.source_file.empty()
        || body_text.empty()) {
        fact_failure(
            qualified_name(function),
            "function definition lacks an exact source body");
    }
    std::vector<std::string> lifetime_object_ids;
    lifetime_object_ids.reserve(lifetimes.objects.size());
    std::string lifetime_inventory_material;
    for (const auto& [object_id, object] : lifetimes.objects) {
        static_cast<void>(object);
        lifetime_object_ids.push_back(object_id);
        lifetime_inventory_material +=
            std::to_string(object_id.size()) + ":"
            + object_id + "\n";
    }
    return {
        .facts = {
            .stable_usr = function_usr,
            .qualified_name = qualified_name(function),
            .source_file = declaration.source_file,
            .module_name = owning_module(function),
            .declaration_span = declaration.span,
            .body_span = body_location.span,
            .body_available = true,
            .template_instantiation_complete =
                !function->isDependentContext(),
            .macro_expansion_complete = true,
            .cfg_complete = true,
            .lifetime_analysis_complete = true,
            .implicit_call_edges_complete = true,
            .body_sha256 =
                bioseq::common::sha256_hex(body_text),
            .cfg_sha256 = bioseq::common::sha256_hex(
                lifetimes.canonical_cfg),
            .cfg_block_count = lifetimes.block_count,
            .cfg_element_count = lifetimes.element_count,
            .ast_callsite_count =
                ast_explicit_callsite_count,
            .cfg_implicit_callsite_count =
                cfg_implicit_callsite_count,
            .emitted_callsite_count = calls.size(),
            .lifetime_object_ids =
                std::move(lifetime_object_ids),
            .lifetime_inventory_sha256 =
                bioseq::common::sha256_hex(
                    lifetime_inventory_material),
            .calls = std::move(calls),
            .unresolved_indirect_targets =
                std::move(unresolved),
            .control_flow_assembly_sites =
                std::move(assembly_sites),
        },
        .runtime_probes = std::move(runtime_probes),
        .body_targets = std::move(body_targets),
    };
}

class main_function_collector final
    : public clang::RecursiveASTVisitor<main_function_collector> {
  public:
    main_function_collector(
        clang::ASTContext& context,
        std::string main_file)
        : context_{context},
          main_file_{std::move(main_file)} {
    }

    bool VisitFunctionDecl(clang::FunctionDecl* function) {
        if (!function->doesThisDeclarationHaveABody()
            || function->isImplicit()) {
            return true;
        }
        const clang::Stmt* body = function->getBody();
        if (body == nullptr) return true;
        const std::string body_file = source_path_of(
            body->getBeginLoc(),
            context_.getSourceManager(),
            true);
        if (body_file == main_file_) {
            functions.push_back(function);
        }
        return true;
    }

    std::vector<const clang::FunctionDecl*> functions;

  private:
    clang::ASTContext& context_;
    std::string main_file_;
};

[[nodiscard]] const clang::FunctionDecl*
callback_function_from_expression(const clang::Expr* expression) {
    if (expression == nullptr) return nullptr;
    expression = expression->IgnoreParenImpCasts();
    if (const auto* unary =
            clang::dyn_cast<clang::UnaryOperator>(expression);
        unary != nullptr
        && unary->getOpcode() == clang::UO_AddrOf) {
        return callback_function_from_expression(
            unary->getSubExpr());
    }
    const auto* reference =
        clang::dyn_cast<clang::DeclRefExpr>(expression);
    return reference == nullptr
        ? nullptr
        : clang::dyn_cast<clang::FunctionDecl>(
              reference->getDecl());
}

[[nodiscard]] std::optional<std::string_view>
callback_record_family(const clang::RecordDecl* record) {
    if (record == nullptr) return std::nullopt;
    const std::string_view name{record->getName().data(),
                                record->getName().size()};
    if (name == "BackgroundWorker") return "background_worker";
    if (name == "TableAmRoutine") return "table_am";
    if (name == "TupleTableSlotOps") return "tuple_table_slot";
    if (name == "CustomPathMethods") return "custom_path";
    if (name == "CustomScanMethods") return "custom_scan";
    if (name == "CustomExecMethods") return "custom_exec";
    if (name == "ErrorContextCallback") return "error_context";
    if (name == "MemoryContextCallback") {
        return "memory_context";
    }
    if (name == "postgres_enum_registration") {
        return "guc_callback";
    }
    return std::nullopt;
}

class postgres_callback_collector final
    : public clang::RecursiveASTVisitor<
          postgres_callback_collector> {
  public:
    postgres_callback_collector(
        clang::ASTContext& context,
        std::string main_file)
        : context_{context},
          main_file_{std::move(main_file)} {
    }

    bool TraverseFunctionDecl(clang::FunctionDecl* function) {
        const clang::FunctionDecl* saved = current_function_;
        if (function != nullptr
            && function->doesThisDeclarationHaveABody()
            && source_path_of(
                   function->getBody()->getBeginLoc(),
                   context_.getSourceManager(),
                   true)
                == main_file_) {
            current_function_ = function;
        }
        const bool result =
            clang::RecursiveASTVisitor<
                postgres_callback_collector>::
                TraverseFunctionDecl(function);
        current_function_ = saved;
        return result;
    }

    bool TraverseVarDecl(clang::VarDecl* variable) {
        const clang::NamedDecl* saved = current_owner_;
        if (variable != nullptr
            && in_main_file(variable->getLocation())) {
            current_owner_ = variable;
        }
        const bool result =
            clang::RecursiveASTVisitor<
                postgres_callback_collector>::
                TraverseVarDecl(variable);
        current_owner_ = saved;
        return result;
    }

    bool VisitFunctionDecl(clang::FunctionDecl* function) {
        if (function == nullptr
            || !function->doesThisDeclarationHaveABody()
            || function->isImplicit()
            || !function->isExternC()
            || !in_main_file(function->getBody()->getBeginLoc())) {
            return true;
        }
        named_function_definitions_[
            function->getNameAsString()].push_back(function);
        return true;
    }

    bool VisitVarDecl(clang::VarDecl* variable) {
        if (variable == nullptr || !variable->hasInit()
            || !in_main_file(variable->getInit()->getBeginLoc())) {
            return true;
        }
        const clang::RecordType* record_type =
            variable->getType()->getAs<clang::RecordType>();
        const clang::RecordDecl* record =
            record_type == nullptr
            ? nullptr
            : record_type->getDecl()->getDefinition();
        const std::optional<std::string_view> family =
            callback_record_family(record);
        if (!family) return true;
        const clang::Expr* initializer =
            variable->getInit()->IgnoreParenImpCasts();
        const auto* list =
            clang::dyn_cast<clang::InitListExpr>(initializer);
        if (list == nullptr || record == nullptr) return true;
        const clang::InitListExpr* semantic =
            list->getSemanticForm() == nullptr
            ? list
            : list->getSemanticForm();
        processed_initializers_.insert(list);
        processed_initializers_.insert(semantic);
        add_roots_from_initializer(
            *family,
            record,
            semantic,
            variable,
            variable->getInit()->getSourceRange());
        return true;
    }

    bool VisitInitListExpr(clang::InitListExpr* expression) {
        if (expression == nullptr
            || processed_initializers_.contains(expression)
            || !in_main_file(expression->getBeginLoc())) {
            return true;
        }
        const clang::RecordType* record_type =
            expression->getType()->getAs<clang::RecordType>();
        const clang::RecordDecl* record =
            record_type == nullptr
            ? nullptr
            : record_type->getDecl()->getDefinition();
        const std::optional<std::string_view> family =
            callback_record_family(record);
        if (!family || record == nullptr) return true;
        const clang::InitListExpr* semantic =
            expression->getSemanticForm() == nullptr
            ? expression
            : expression->getSemanticForm();
        processed_initializers_.insert(expression);
        processed_initializers_.insert(semantic);
        const clang::NamedDecl* owner = current_owner_ != nullptr
            ? current_owner_
            : current_function_;
        add_roots_from_initializer(
            *family,
            record,
            semantic,
            owner,
            expression->getSourceRange());
        return true;
    }

    bool VisitBinaryOperator(clang::BinaryOperator* expression) {
        if (expression == nullptr
            || !expression->isAssignmentOp()
            || !in_main_file(expression->getBeginLoc())) {
            return true;
        }
        const clang::FunctionDecl* target =
            callback_function_from_expression(
                expression->getRHS());
        if (target == nullptr || current_function_ == nullptr) {
            return true;
        }
        const clang::Expr* left =
            expression->getLHS()->IgnoreParenImpCasts();
        if (const auto* reference =
                clang::dyn_cast<clang::DeclRefExpr>(left)) {
            const auto* variable =
                clang::dyn_cast<clang::VarDecl>(
                    reference->getDecl());
            if (variable == nullptr) return true;
            std::string name = variable->getNameAsString();
            std::string normalized = name;
            std::ranges::transform(
                normalized,
                normalized.begin(),
                [](unsigned char character) {
                    return static_cast<char>(
                        std::tolower(character));
                });
            if (!normalized.ends_with("_hook")) return true;
            add_root(
                "postgres_hook",
                std::move(name),
                current_function_,
                expression->getSourceRange(),
                target);
            return true;
        }
        const auto* member =
            clang::dyn_cast<clang::MemberExpr>(left);
        const auto* field = member == nullptr
            ? nullptr
            : clang::dyn_cast<clang::FieldDecl>(
                  member->getMemberDecl());
        const std::optional<std::string_view> family =
            field == nullptr
            ? std::nullopt
            : callback_record_family(field->getParent());
        if (family) {
            add_root(
                *family,
                field->getNameAsString(),
                current_function_,
                expression->getSourceRange(),
                target);
        }
        return true;
    }

    bool VisitCallExpr(clang::CallExpr* expression) {
        if (expression == nullptr || current_function_ == nullptr
            || !in_main_file(expression->getBeginLoc())) {
            return true;
        }
        const clang::FunctionDecl* callee =
            expression->getDirectCallee();
        if (callee == nullptr) return true;
        const std::string api = callee->getNameAsString();
        static const std::map<
            std::string,
            std::array<unsigned, 3>,
            std::less<>>
            guc_registrations{
                {"DefineCustomBoolVariable", {7, 8, 9}},
                {"DefineCustomEnumVariable", {8, 9, 10}},
                {"DefineCustomIntVariable", {9, 10, 11}},
                {"DefineCustomRealVariable", {9, 10, 11}},
                {"DefineCustomStringVariable", {7, 8, 9}},
            };
        if (const auto registration =
                guc_registrations.find(api);
            registration != guc_registrations.end()) {
            static constexpr std::array<std::string_view, 3>
                slots{"check_hook", "assign_hook", "show_hook"};
            for (std::size_t index = 0; index < slots.size();
                 ++index) {
                const unsigned argument =
                    registration->second[index];
                if (argument >= expression->getNumArgs()) {
                    fact_failure(
                        api,
                        "PostgreSQL GUC registration arity differs "
                        "from the reviewed callback contract");
                }
                const clang::FunctionDecl* target =
                    callback_function_from_expression(
                        expression->getArg(argument));
                if (target == nullptr) continue;
                add_root(
                    "guc_callback",
                    std::string{slots[index]},
                    current_function_,
                    expression->getSourceRange(),
                    target);
            }
            return true;
        }
        if (api == "pg_snprintf" && expression->getNumArgs() >= 3) {
            const clang::VarDecl* worker =
                background_worker_destination(
                    expression->getArg(0));
            if (worker != nullptr) {
                const bool inserted =
                    background_worker_names_
                        .emplace(
                            worker->getCanonicalDecl(),
                            background_worker_name{
                                .target_name =
                                    ordinary_runtime_literal(
                                        expression->getArg(2),
                                        "BackgroundWorker."
                                        "bgw_function_name"),
                                .assignment =
                                    expression->getSourceRange(),
                            })
                        .second;
                if (!inserted) {
                    fact_failure(
                        qualified_name(current_function_),
                        "BackgroundWorker has multiple "
                        "bgw_function_name bindings");
                }
            }
            return true;
        }
        if (api == "RegisterBackgroundWorker"
            && expression->getNumArgs() == 1) {
            const clang::VarDecl* worker =
                variable_from_expression(expression->getArg(0));
            if (worker != nullptr) {
                pending_background_workers_.push_back({
                    .worker = worker->getCanonicalDecl(),
                    .owner = current_function_,
                    .registration = expression->getSourceRange(),
                });
            }
            return true;
        }
        static const std::map<
            std::string,
            std::pair<std::string, unsigned>,
            std::less<>>
            registrations{
                {"CacheRegisterRelcacheCallback",
                 {"relcache_callback", 0}},
                {"CacheRegisterSyscacheCallback",
                 {"syscache_callback", 1}},
                {"RegisterSubXactCallback",
                 {"subxact_callback", 0}},
                {"RegisterXactCallback",
                 {"xact_callback", 0}},
                {"before_shmem_exit",
                 {"process_exit_callback", 0}},
                {"on_proc_exit",
                 {"process_exit_callback", 0}},
                {"on_shmem_exit",
                 {"process_exit_callback", 0}},
            };
        const auto registration = registrations.find(api);
        if (registration == registrations.end()
            || registration->second.second
                >= expression->getNumArgs()) {
            return true;
        }
        const clang::FunctionDecl* target =
            callback_function_from_expression(
                expression->getArg(
                    registration->second.second));
        if (target != nullptr) {
            add_root(
                registration->second.first,
                api,
                current_function_,
                expression->getSourceRange(),
                target);
        }
        return true;
    }

    void finalize() {
        for (const pending_background_worker& pending :
             pending_background_workers_) {
            const auto binding =
                background_worker_names_.find(pending.worker);
            if (binding == background_worker_names_.end()) {
                fact_failure(
                    qualified_name(pending.owner),
                    "registered BackgroundWorker has no exact "
                    "bgw_function_name binding");
            }
            if (!context_.getSourceManager()
                     .isBeforeInTranslationUnit(
                         binding->second.assignment.getBegin(),
                         pending.registration.getBegin())) {
                fact_failure(
                    qualified_name(pending.owner),
                    "BackgroundWorker entry point is not bound before "
                    "registration");
            }
            const auto definitions =
                named_function_definitions_.find(
                    binding->second.target_name);
            if (definitions == named_function_definitions_.end()
                || definitions->second.size() != 1) {
                fact_failure(
                    binding->second.target_name,
                    "registered BackgroundWorker entry point does not "
                    "resolve to one exact owned definition");
            }
            add_root(
                "background_worker",
                "bgw_function_name",
                pending.owner,
                pending.registration,
                definitions->second.front());
        }
    }

    std::vector<postgres_callback_root> roots;

  private:
    struct background_worker_name {
        std::string target_name;
        clang::SourceRange assignment;
    };

    struct pending_background_worker {
        const clang::VarDecl* worker;
        const clang::FunctionDecl* owner;
        clang::SourceRange registration;
    };

    [[nodiscard]] static const clang::VarDecl*
    variable_from_expression(const clang::Expr* expression) {
        if (expression == nullptr) return nullptr;
        expression = expression->IgnoreParenImpCasts();
        if (const auto* unary =
                clang::dyn_cast<clang::UnaryOperator>(expression);
            unary != nullptr
            && unary->getOpcode() == clang::UO_AddrOf) {
            return variable_from_expression(unary->getSubExpr());
        }
        const auto* reference =
            clang::dyn_cast<clang::DeclRefExpr>(expression);
        return reference == nullptr
            ? nullptr
            : clang::dyn_cast<clang::VarDecl>(
                  reference->getDecl());
    }

    [[nodiscard]] static const clang::VarDecl*
    background_worker_destination(const clang::Expr* expression) {
        if (expression == nullptr) return nullptr;
        expression = expression->IgnoreParenImpCasts();
        const auto* member =
            clang::dyn_cast<clang::MemberExpr>(expression);
        const auto* field = member == nullptr
            ? nullptr
            : clang::dyn_cast<clang::FieldDecl>(
                  member->getMemberDecl());
        if (field == nullptr
            || field->getName() != "bgw_function_name"
            || !callback_record_family(field->getParent())
            || *callback_record_family(field->getParent())
                != "background_worker") {
            return nullptr;
        }
        return variable_from_expression(member->getBase());
    }

    void add_roots_from_initializer(
        std::string_view family,
        const clang::RecordDecl* record,
        const clang::InitListExpr* initializer,
        const clang::NamedDecl* owner,
        clang::SourceRange registration_range) {
        auto field = record->field_begin();
        for (unsigned index = 0;
             index < initializer->getNumInits()
             && field != record->field_end();
             ++index, ++field) {
            const clang::FunctionDecl* target =
                callback_function_from_expression(
                    initializer->getInit(index));
            if (target == nullptr) continue;
            add_root(
                family,
                (*field)->getNameAsString(),
                owner,
                registration_range,
                target);
        }
    }

    [[nodiscard]] bool in_main_file(
        clang::SourceLocation location) const {
        return source_path_of(
                   location,
                   context_.getSourceManager(),
                   true)
            == main_file_;
    }

    void add_root(
        std::string_view family,
        std::string slot,
        const clang::NamedDecl* owner,
        clang::SourceRange registration_range,
        const clang::FunctionDecl* target) {
        const clang::FunctionDecl* definition =
            target == nullptr ? nullptr : target->getDefinition();
        if (definition == nullptr
            || !definition->doesThisDeclarationHaveABody()) {
            const std::string context =
                target == nullptr
                ? std::string{slot}
                : qualified_name(target);
            fact_failure(
                context,
                "PostgreSQL callback target body is unavailable");
        }
        std::string owner_usr = stable_usr(owner);
        std::string owner_name = qualified_name(owner);
        if (owner_usr.empty() && current_function_ != nullptr) {
            owner_usr = stable_function_identity(
                current_function_, context_);
            owner_name = qualified_name(current_function_);
        }
        const std::string target_usr =
            stable_function_identity(definition, context_);
        const located_source_range registration =
            located_range_of(
                registration_range,
                context_.getSourceManager(),
                context_.getLangOpts(),
                true);
        const located_source_range target_location =
            located_range_of(
                definition->getSourceRange(),
                context_.getSourceManager(),
                context_.getLangOpts(),
                true);
        if (owner_usr.empty() || owner_name.empty()
            || target_usr.empty()
            || registration.source_file != main_file_
            || target_location.source_file.empty()
            || slot.empty()) {
            fact_failure(
                slot,
                "PostgreSQL callback provenance/ownership is "
                "incomplete");
        }
        std::string material =
            std::string{family} + "\n" + slot + "\n"
            + owner_usr + "\n" + target_usr + "\n"
            + registration.source_file + "\n"
            + std::to_string(
                registration.span.begin.byte_offset)
            + "\n"
            + std::to_string(
                registration.span.end.byte_offset);
        postgres_callback_root root{
            .stable_callback_id =
                "pg-callback:"
                + bioseq::common::sha256_hex(material)
                      .substr(0, 32),
            .family = std::string{family},
            .slot = std::move(slot),
            .owner_usr = std::move(owner_usr),
            .owner_qualified_name = std::move(owner_name),
            .registration = registration,
            .target_usr = target_usr,
            .target_qualified_name =
                qualified_name(definition),
            .target_definition = target_location,
        };
        const auto duplicate = std::ranges::find(
            roots,
            root.stable_callback_id,
            &postgres_callback_root::stable_callback_id);
        if (duplicate == roots.end()) {
            roots.push_back(std::move(root));
        } else if (
            duplicate->target_usr != root.target_usr
            || duplicate->family != root.family
            || duplicate->slot != root.slot) {
            fact_failure(
                root.stable_callback_id,
                "PostgreSQL callback stable identity conflicts");
        }
    }

    clang::ASTContext& context_;
    std::string main_file_;
    const clang::FunctionDecl* current_function_{};
    const clang::NamedDecl* current_owner_{};
    std::set<const clang::InitListExpr*> processed_initializers_;
    std::map<
        std::string,
        std::vector<const clang::FunctionDecl*>,
        std::less<>>
        named_function_definitions_;
    std::map<
        const clang::VarDecl*,
        background_worker_name>
        background_worker_names_;
    std::vector<pending_background_worker>
        pending_background_workers_;
};

struct translation_function_facts {
    function_flow function;
    std::vector<runtime_probe_fact> runtime_probes;
};

struct translation_output {
    std::vector<translation_function_facts> functions;
    std::vector<postgres_callback_root> callback_roots;
};

class bridge_macro_tracker final : public clang::PPCallbacks {
  public:
    bridge_macro_tracker(
        bridge_macro_inventory& inventory,
        clang::Preprocessor& preprocessor,
        const provenance_bundle* provenance)
        : inventory_{inventory},
          preprocessor_{preprocessor},
          provenance_{provenance} {
    }

    void MacroExpands(
        const clang::Token& macro_name,
        const clang::MacroDefinition& definition,
        clang::SourceRange invocation_range,
        const clang::MacroArgs*) override {
        const clang::IdentifierInfo* identifier =
            macro_name.getIdentifierInfo();
        if (identifier == nullptr) return;
        const std::string name = identifier->getName().str();
        if (name != "PG_TRY"
            && name != "PG_CATCH"
            && name != "PG_END_TRY") {
            return;
        }
        const bool ambiguous_definition =
            definition.isAmbiguous();
        const clang::MacroInfo* macro =
            ambiguous_definition
            ? nullptr
            : definition.getMacroInfo();
        const clang::SourceManager& sources =
            preprocessor_.getSourceManager();
        const clang::LangOptions& language =
            preprocessor_.getLangOpts();
        if (ambiguous_definition || macro == nullptr
            || macro->getDefinitionLoc().isInvalid()) {
            inventory_.push_back({
                .name = name,
                .invocation_begin =
                    invocation_range.getBegin(),
                .invocation = located_range_of(
                    invocation_range,
                    sources,
                    language,
                    false),
                .direct_file_invocation = false,
            });
            return;
        }
        clang::SourceLocation definition_end =
            macro->getDefinitionEndLoc();
        if (definition_end.isInvalid()) {
            definition_end = macro->getDefinitionLoc();
        }
        const clang::SourceRange definition_range{
            macro->getDefinitionLoc(), definition_end};
        const clang::SourceLocation spelling_definition =
            sources.getSpellingLoc(
                macro->getDefinitionLoc());
        const clang::FileID definition_file_id =
            sources.getFileID(spelling_definition);
        bool invalid_buffer =
            definition_file_id.isInvalid();
        llvm::StringRef definition_file;
        if (!invalid_buffer) {
            definition_file = sources.getBufferData(
                definition_file_id, &invalid_buffer);
        }
        std::string definition_file_sha256;
        std::string pinned_fragment_sha256;
        if (!invalid_buffer) {
            definition_file_sha256 =
                bioseq::common::sha256_hex(
                    std::string_view{
                        definition_file.data(),
                        definition_file.size()});
            const provenance_record* record =
                provenance_ == nullptr
                ? nullptr
                : provenance_by_id(
                      *provenance_,
                      "pg17.pg_try_rethrow_macros");
            if (record != nullptr
                && record->upstream_span.end.byte_offset
                    <= definition_file.size()
                && record->upstream_span.begin.byte_offset
                    <= record->upstream_span.end.byte_offset) {
                const std::size_t begin =
                    record->upstream_span.begin.byte_offset;
                const std::size_t size =
                    record->upstream_span.end.byte_offset
                    - begin;
                pinned_fragment_sha256 =
                    bioseq::common::sha256_hex(
                        std::string_view{
                            definition_file.data() + begin,
                            size});
            }
        }
        inventory_.push_back({
            .name = name,
            .invocation_begin =
                invocation_range.getBegin(),
            .invocation = located_range_of(
                invocation_range,
                sources,
                language,
                false),
            .definition = located_range_of(
                definition_range,
                sources,
                language,
                true),
            .definition_file_sha256 =
                std::move(definition_file_sha256),
            .pinned_fragment_sha256 =
                std::move(pinned_fragment_sha256),
            .direct_file_invocation =
                invocation_range.getBegin().isFileID(),
        });
    }

  private:
    bridge_macro_inventory& inventory_;
    clang::Preprocessor& preprocessor_;
    const provenance_bundle* provenance_{};
};

class fact_consumer final : public clang::ASTConsumer {
  public:
    fact_consumer(
        translation_output& output,
        clang::Preprocessor& preprocessor,
        analysis_environment environment,
        std::shared_ptr<bridge_macro_inventory> macro_inventory,
        std::string main_file)
        : output_{output},
          preprocessor_{preprocessor},
          environment_{std::move(environment)},
          macro_inventory_{std::move(macro_inventory)},
          main_file_{std::move(main_file)} {
    }

    void HandleTranslationUnit(clang::ASTContext& context) override {
        postgres_callback_collector callbacks{
            context, main_file_};
        callbacks.TraverseDecl(
            context.getTranslationUnitDecl());
        callbacks.finalize();
        output_.callback_roots.insert(
            output_.callback_roots.end(),
            std::make_move_iterator(callbacks.roots.begin()),
            std::make_move_iterator(callbacks.roots.end()));

        main_function_collector roots{context, main_file_};
        roots.TraverseDecl(
            context.getTranslationUnitDecl());
        std::deque<const clang::FunctionDecl*> pending{
            roots.functions.begin(), roots.functions.end()};
        std::set<std::string, std::less<>> analyzed;
        while (!pending.empty()) {
            const clang::FunctionDecl* function = pending.front();
            pending.pop_front();
            const std::string usr =
                stable_function_identity(function, context);
            if (usr.empty() || !analyzed.insert(usr).second) {
                continue;
            }
            analyzed_function result = analyze_function(
                function,
                preprocessor_,
                *macro_inventory_,
                context,
                environment_);
            for (const clang::FunctionDecl* target :
                 result.body_targets) {
                if (target != nullptr
                    && target->doesThisDeclarationHaveABody()) {
                    pending.push_back(target);
                }
            }
            output_.functions.push_back({
                .function = std::move(result.facts),
                .runtime_probes =
                    std::move(result.runtime_probes),
            });
        }
    }

  private:
    translation_output& output_;
    clang::Preprocessor& preprocessor_;
    analysis_environment environment_;
    std::shared_ptr<bridge_macro_inventory> macro_inventory_;
    std::string main_file_;
};

class fact_action final : public clang::ASTFrontendAction {
  public:
    fact_action(
        translation_output& output,
        analysis_environment environment)
        : output_{output},
          environment_{std::move(environment)},
          macro_inventory_{
              std::make_shared<bridge_macro_inventory>()} {
    }

    std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
        clang::CompilerInstance& compiler,
        llvm::StringRef input) override {
        std::string main_file;
        std::error_code error;
        const fs::path canonical = fs::weakly_canonical(
            fs::path{input.str()}, error);
        main_file = error
            ? fs::path{input.str()}
                  .lexically_normal()
                  .generic_string()
            : canonical.generic_string();
        compiler.getPreprocessor().addPPCallbacks(
            std::make_unique<bridge_macro_tracker>(
                *macro_inventory_,
                compiler.getPreprocessor(),
                environment_.provenance));
        return std::make_unique<fact_consumer>(
            output_,
            compiler.getPreprocessor(),
            environment_,
            macro_inventory_,
            std::move(main_file));
    }

  private:
    translation_output& output_;
    analysis_environment environment_;
    std::shared_ptr<bridge_macro_inventory> macro_inventory_;
};

class fact_factory final
    : public clang::tooling::FrontendActionFactory {
  public:
    fact_factory(
        translation_output& output,
        analysis_environment environment)
        : output_{output},
          environment_{std::move(environment)} {
    }

    std::unique_ptr<clang::FrontendAction> create() override {
        return std::make_unique<fact_action>(
            output_, environment_);
    }

  private:
    translation_output& output_;
    analysis_environment environment_;
};

[[nodiscard]] clang::tooling::ArgumentsAdjuster
fact_arguments_adjuster() {
    return [](
        const clang::tooling::CommandLineArguments& arguments,
        llvm::StringRef) {
        static const std::set<std::string, std::less<>>
            drop_with_value{
                "-o",
                "-MF",
                "-MT",
                "-MQ",
                "-MJ",
                "--serialize-diagnostics",
            };
        static const std::set<std::string, std::less<>>
            drop_flags{
                "-c", "-MD", "-MMD", "-MG", "-MP", "-M", "-MM",
            };
        clang::tooling::CommandLineArguments result;
        result.reserve(arguments.size() + 3);
        for (std::size_t index = 0;
             index < arguments.size();
             ++index) {
            const std::string& argument = arguments[index];
            if (drop_flags.contains(argument)) continue;
            if (drop_with_value.contains(argument)) {
                if (index + 1 == arguments.size()) {
                    fact_failure(
                        argument,
                        "compile command option has no value");
                }
                ++index;
                continue;
            }
            if (argument.starts_with("-fmodule-output")
                || argument.starts_with("-MF")
                || argument.starts_with("-MT")
                || argument.starts_with("-MQ")
                || argument.starts_with("-MJ")) {
                continue;
            }
            result.push_back(argument);
        }
        result.emplace_back("-fsyntax-only");
        result.emplace_back("-Wno-unknown-attributes");
        result.emplace_back("-Wno-ignored-attributes");
        return result;
    };
}

[[nodiscard]] std::unique_ptr<
    clang::tooling::CompilationDatabase>
load_compile_database(const fs::path& path) {
    std::string error;
    std::unique_ptr<clang::tooling::CompilationDatabase> database =
        clang::tooling::JSONCompilationDatabase::loadFromFile(
            path.string(),
            error,
            clang::tooling::JSONCommandLineSyntax::AutoDetect);
    if (!database) {
        fact_failure(
            path.string(),
            "cannot load compile database: " + error);
    }
    return database;
}

[[nodiscard]] std::vector<clang::tooling::CompileCommand>
validate_compile_commands(
    const clang::tooling::CompilationDatabase& database,
    const std::vector<fs::path>& sources,
    const fs::path& pcm_root) {
    std::map<std::string, clang::tooling::CompileCommand, std::less<>>
        command_by_source;
    for (const clang::tooling::CompileCommand& command :
         database.getAllCompileCommands()) {
        const fs::path filename =
            fs::path{command.Filename}.is_absolute()
            ? fs::path{command.Filename}
            : fs::path{command.Directory} / command.Filename;
        const std::string canonical = canonical_path(filename);
        if (!command_by_source.emplace(canonical, command).second) {
            fact_failure(
                canonical,
                "owned compile database contains more than one "
                "command for a source");
        }
        if (command.CommandLine.empty()) {
            fact_failure(
                canonical, "compile command has no executable");
        }
        for (std::size_t index = 0;
             index < command.CommandLine.size();
             ++index) {
            const std::string_view argument =
                command.CommandLine[index];
            if (!argument.empty() && argument.front() == '@') {
                fact_failure(
                    canonical,
                    "owned compile command retains a response file");
            }
            std::string_view pcm;
            if (argument.starts_with("-fmodule-file=")) {
                pcm = argument.substr(
                    std::string_view{"-fmodule-file="}.size());
                const std::size_t separator = pcm.find('=');
                if (separator != std::string_view::npos) {
                    pcm.remove_prefix(separator + 1);
                }
            } else if (argument == "-fmodule-file"
                       && index + 1
                           < command.CommandLine.size()) {
                pcm = command.CommandLine[++index];
            }
            if (!pcm.empty()
                && (!fs::is_regular_file(fs::path{pcm})
                    || !path_within(fs::path{pcm}, pcm_root))) {
                fact_failure(
                    canonical,
                    "compile command references a missing or "
                    "out-of-root PCM");
            }
        }
    }
    std::set<std::string, std::less<>> expected;
    for (const fs::path& source : sources) {
        expected.insert(canonical_path(source));
    }
    std::set<std::string, std::less<>> actual;
    for (const auto& [source, command] : command_by_source) {
        static_cast<void>(command);
        actual.insert(source);
    }
    if (actual != expected) {
        fact_failure(
            "compile database",
            "source closure differs from the exact source list");
    }
    std::vector<clang::tooling::CompileCommand> result;
    result.reserve(sources.size());
    for (const fs::path& source : sources) {
        result.push_back(
            command_by_source.at(canonical_path(source)));
    }
    return result;
}

[[nodiscard]] json_value::array command_arguments_json(
    const std::vector<std::string>& arguments) {
    json_value::array result;
    result.reserve(arguments.size());
    for (const std::string& argument : arguments) {
        result.emplace_back(argument);
    }
    return result;
}

[[nodiscard]] std::string canonical_arguments(
    const std::vector<std::string>& arguments) {
    std::string result;
    for (const std::string& argument : arguments) {
        result += std::to_string(argument.size()) + ":"
            + argument + "\n";
    }
    return result;
}

[[nodiscard]] std::string render_compile_graph(
    const compiler_fact_request& request,
    const std::vector<fs::path>& sources,
    const std::vector<clang::tooling::CompileCommand>& commands,
    std::string_view source_list_text,
    std::string_view compile_database_text,
    std::string_view pcm_manifest_text,
    const prepared_identity& identity,
    const postgresql_header_authority& postgresql_authority) {
    json_value::array units;
    units.reserve(sources.size());
    for (std::size_t index = 0; index < sources.size(); ++index) {
        const fs::path& source = sources[index];
        const clang::tooling::CompileCommand& command =
            commands[index];
        units.emplace_back(json_value::object{
            {"arguments",
             json_value{
                 command_arguments_json(command.CommandLine)}},
            {"arguments_sha256",
             bioseq::common::sha256_hex(
                 canonical_arguments(command.CommandLine))},
            {"directory",
             canonical_path(command.Directory)},
            {"source", canonical_path(source)},
            {"source_sha256",
             bioseq::common::sha256_hex(
                 bioseq::codegen::read_artifact_bytes(source))},
        });
    }

    const json_value pcm_document =
        parse_json(pcm_manifest_text);
    const json_value::object& pcm_root =
        json_object(pcm_document, "PCM manifest");
    json_value::array modules;
    const json_value::array& manifest_modules =
        json_array(
            json_field(pcm_root, "modules", "PCM manifest"),
            "PCM manifest.modules");
    std::vector<const json_value*> sorted_modules;
    sorted_modules.reserve(manifest_modules.size());
    for (const json_value& module : manifest_modules) {
        sorted_modules.push_back(std::addressof(module));
    }
    std::ranges::sort(
        sorted_modules,
        [](const json_value* left, const json_value* right) {
            const json_value::object& left_object =
                json_object(*left, "PCM module");
            const json_value::object& right_object =
                json_object(*right, "PCM module");
            return json_string(
                       json_field(
                           left_object, "name", "PCM module"),
                       "PCM module.name")
                < json_string(
                       json_field(
                           right_object, "name", "PCM module"),
                       "PCM module.name");
        });
    for (const json_value* module : sorted_modules) {
        modules.push_back(*module);
    }
    json_value::array postgresql_roots;
    for (const std::string& root :
         postgresql_authority.authenticated_roots) {
        postgresql_roots.emplace_back(root);
    }
    json_value::array rejected_postgresql_roots;
    for (const std::string& root :
         postgresql_authority.rejected_roots) {
        rejected_postgresql_roots.emplace_back(root);
    }
    json_value::array postgresql_headers;
    for (const auto& [path, sha256] :
         postgresql_authority.authenticated_headers) {
        postgresql_headers.emplace_back(json_value::object{
            {"path", path},
            {"sha256", sha256},
        });
    }

    return render_json(json_value::object{
        {"clang_version", std::string{CLANG_VERSION_STRING}},
        {"compilation_signature", identity.compilation_signature},
        {"compile_database",
         canonical_path(request.compile_database)},
        {"compile_database_sha256",
         bioseq::common::sha256_hex(compile_database_text)},
        {"modules", json_value{std::move(modules)}},
        {"pcm_manifest", canonical_path(request.pcm_manifest)},
        {"pcm_manifest_sha256",
         bioseq::common::sha256_hex(pcm_manifest_text)},
        {"pcm_root", canonical_path(request.pcm_root)},
        {"profile", profile_name(request.profile)},
        {"postgresql_authenticated_headers",
         json_value{std::move(postgresql_headers)}},
        {"postgresql_header_roots",
         json_value{std::move(postgresql_roots)}},
        {"postgresql_rejected_header_roots",
         json_value{std::move(rejected_postgresql_roots)}},
        {"schema", "bioseq_safety_compile_graph"},
        {"schema_revision", json_value{std::size_t{1}}},
        {"source_list", canonical_path(request.source_list)},
        {"source_list_sha256",
         bioseq::common::sha256_hex(source_list_text)},
        {"translation_units", json_value{std::move(units)}},
    });
}

[[nodiscard]] std::string render_provenance_candidates(
    compilation_profile profile,
    std::string_view authority,
    const std::map<
        std::string,
        analysis_environment::provenance_candidate,
        std::less<>>& candidates) {
    json_value::array values;
    values.reserve(candidates.size());
    for (const auto& [key, candidate] : candidates) {
        static_cast<void>(key);
        values.emplace_back(json_value::object{
            {"authority_id", candidate.authority_id},
            {"body_available", candidate.body_available},
            {"fragment_sha256", candidate.fragment_sha256},
            {"kind", provenance_kind_name(candidate.kind)},
            {"library_version", candidate.library_version},
            {"required_review",
             candidate.kind
                     == provenance_kind::pg_no_longjmp_leaf
                 ? "author only after proving the exact PostgreSQL "
                   "declaration/body cannot raise ERROR"
                 : "set user_code_callback_free only after reviewing "
                   "the exact declaration/body and its transitive "
                   "callback behavior"},
            {"target_qualified_name",
             candidate.target_qualified_name},
            {"target_usr", candidate.target_usr},
            {"upstream_file", candidate.upstream_file},
            {"upstream_file_sha256",
             candidate.upstream_file_sha256},
            {"upstream_span",
             json_value{serialization_detail::range_json(
                 candidate.upstream_span)}},
        });
    }
    return render_json(json_value::object{
        {"candidates", json_value{std::move(values)}},
        {"compiler_authority", std::string{authority}},
        {"profile", profile_name(profile)},
        {"review_status", "unreviewed"},
        {"scope",
         "registered-source scan; author only records "
         "also named by a reachable-closure failure"},
        {"schema", "bioseq_safety_provenance_candidates"},
        {"schema_revision", json_value{std::size_t{1}}},
    });
}

[[nodiscard]] std::string canonical_function_fact(
    const function_flow& function,
    const std::vector<runtime_probe_fact>& runtime_probes) {
    compiler_analysis_facts facts{
        .profile = compilation_profile::production,
        .compiler_profile = "canonical",
        .clang_version = "canonical",
        .source_list_sha256 = std::string(64, '1'),
        .compile_database_sha256 = std::string(64, '2'),
        .producer_executable_sha256 = std::string(64, '3'),
        .compile_graph_sha256 = std::string(64, '4'),
        .pcm_manifest_sha256 = std::string(64, '5'),
        .sql_manifest_sha256 = std::string(64, '6'),
        .root_usrs = {"canonical"},
        .functions = {function},
        .runtime_probes = runtime_probes,
    };
    return render_compiler_analysis_facts_json(facts);
}

[[nodiscard]] bool same_runtime_probe_fact(
    const runtime_probe_fact& left,
    const runtime_probe_fact& right) noexcept {
    return left.fault_key == right.fault_key
        && left.relation == right.relation
        && left.site_call_stable_id
            == right.site_call_stable_id
        && left.visit_call_stable_id
            == right.visit_call_stable_id
        && left.trigger_call_stable_id
            == right.trigger_call_stable_id;
}

[[nodiscard]] isolated_compiler_fact_scan merge_function_facts(
    std::vector<translation_function_facts> functions,
    std::vector<postgres_callback_root> callback_roots = {}) {
    std::map<
        std::string,
        translation_function_facts,
        std::less<>>
        merged;
    for (translation_function_facts& unit : functions) {
        const std::string usr = unit.function.stable_usr;
        const auto existing = merged.find(usr);
        if (existing == merged.end()) {
            merged.emplace(
                usr, std::move(unit));
            continue;
        }
        if (canonical_function_fact(
                existing->second.function,
                {})
            != canonical_function_fact(
                unit.function,
                {})) {
            fact_failure(
                unit.function.qualified_name,
                "one Clang USR produced conflicting instantiated "
                "function facts");
        }
        if (existing->second.runtime_probes.empty()) {
            existing->second.runtime_probes =
                std::move(unit.runtime_probes);
        } else if (
            !unit.runtime_probes.empty()
            && canonical_function_fact(
                   existing->second.function,
                   existing->second.runtime_probes)
                != canonical_function_fact(
                    unit.function,
                    unit.runtime_probes)) {
            fact_failure(
                unit.function.qualified_name,
                "one SQL boundary specialization produced "
                "conflicting runtime-probe facts");
        }
    }
    isolated_compiler_fact_scan result;
    result.functions.reserve(merged.size());
    for (auto& [usr, unit] : merged) {
        static_cast<void>(usr);
        for (runtime_probe_fact& probe :
             unit.runtime_probes) {
            result.runtime_probes.push_back(
                std::move(probe));
        }
        result.functions.push_back(
            std::move(unit.function));
    }

    std::map<std::string, std::string, std::less<>>
        call_owners;
    for (const function_flow& function : result.functions) {
        for (const flow_call_site& call : function.calls) {
            if (!call_owners.emplace(
                    call.stable_site_id,
                    function.stable_usr).second) {
                fact_failure(
                    call.stable_site_id,
                    "one stable call id belongs to multiple "
                    "function facts");
            }
        }
    }
    std::map<
        std::pair<std::string, std::string>,
        runtime_probe_fact>
        normalized;
    for (runtime_probe_fact& probe : result.runtime_probes) {
        std::set<std::string, std::less<>> owners;
        for (const std::string* id : {
                 std::addressof(probe.site_call_stable_id),
                 std::addressof(probe.visit_call_stable_id)}) {
            const auto owner = call_owners.find(*id);
            if (owner == call_owners.end()) {
                fact_failure(
                    *id,
                    "runtime probe references an unknown stable "
                    "call id");
            }
            owners.insert(owner->second);
        }
        if (probe.trigger_call_stable_id) {
            const auto owner = call_owners.find(
                *probe.trigger_call_stable_id);
            if (owner == call_owners.end()) {
                fact_failure(
                    *probe.trigger_call_stable_id,
                    "runtime probe trigger references an unknown "
                    "stable call id");
            }
            owners.insert(owner->second);
        }
        if (owners.size() != 1) {
            fact_failure(
                probe.fault_key,
                "runtime probe stable call ids span multiple "
                "functions");
        }
        const std::pair key{
            *owners.begin(), probe.fault_key};
        const auto [existing, inserted] =
            normalized.emplace(key, probe);
        if (!inserted
            && !same_runtime_probe_fact(
                existing->second, probe)) {
            fact_failure(
                probe.fault_key,
                "one function/fault key produced conflicting "
                "runtime-probe facts");
        }
    }
    result.runtime_probes.clear();
    result.runtime_probes.reserve(normalized.size());
    for (auto& [key, probe] : normalized) {
        static_cast<void>(key);
        result.runtime_probes.push_back(std::move(probe));
    }
    std::map<
        std::string,
        postgres_callback_root,
        std::less<>>
        normalized_callbacks;
    for (postgres_callback_root& root : callback_roots) {
        const auto [existing, inserted] =
            normalized_callbacks.emplace(
                root.stable_callback_id, root);
        if (!inserted
            && (existing->second.family != root.family
                || existing->second.slot != root.slot
                || existing->second.owner_usr != root.owner_usr
                || existing->second.owner_qualified_name
                    != root.owner_qualified_name
                || existing->second.registration.source_file
                    != root.registration.source_file
                || !same_source_range(
                    existing->second.registration.span,
                    root.registration.span)
                || existing->second.target_usr != root.target_usr
                || existing->second.target_qualified_name
                    != root.target_qualified_name
                || existing->second.target_definition.source_file
                    != root.target_definition.source_file
                || !same_source_range(
                    existing->second.target_definition.span,
                    root.target_definition.span))) {
            fact_failure(
                root.stable_callback_id,
                "one PostgreSQL callback root produced "
                "conflicting provenance");
        }
    }
    result.callback_roots.reserve(
        normalized_callbacks.size());
    for (auto& [id, root] : normalized_callbacks) {
        static_cast<void>(id);
        result.callback_roots.push_back(std::move(root));
    }
    return result;
}

struct reachable_compiler_facts {
    std::vector<std::string> roots;
    std::vector<postgres_callback_root> callback_roots;
    std::vector<function_flow> functions;
    std::vector<runtime_probe_fact> runtime_probes;
};

[[nodiscard]] reachable_compiler_facts
reachable_fact_closure(
    const safety_manifest& manifest,
    isolated_compiler_fact_scan facts) {
    std::map<std::string, const function_flow*, std::less<>>
        by_usr;
    std::multimap<std::string, const function_flow*, std::less<>>
        by_name;
    std::map<std::string, std::string, std::less<>>
        call_owners;
    for (const function_flow& function : facts.functions) {
        by_usr.emplace(
            function.stable_usr, std::addressof(function));
        by_name.emplace(
            function.qualified_name, std::addressof(function));
        for (const flow_call_site& call : function.calls) {
            call_owners.emplace(
                call.stable_site_id, function.stable_usr);
        }
    }

    std::vector<std::string> roots;
    std::deque<const function_flow*> pending;
    for (const safety_manifest_owner& owner : manifest.owners) {
        const auto [begin, end] =
            by_name.equal_range(owner.public_symbol);
        const function_flow* match = nullptr;
        for (auto candidate = begin; candidate != end; ++candidate) {
            if (candidate->second->module_name
                != owner.generated_wrapper_module) {
                continue;
            }
            if (match != nullptr
                && match->stable_usr
                    != candidate->second->stable_usr) {
                fact_failure(
                    owner.public_symbol,
                    "generated wrapper resolves multiple Clang "
                    "definitions");
            }
            match = candidate->second;
        }
        if (match == nullptr) {
            fact_failure(
                owner.public_symbol,
                "generated wrapper definition/module is absent "
                "from compiler facts");
        }
        roots.push_back(match->stable_usr);
        pending.push_back(match);
    }
    std::ranges::sort(roots);
    roots.erase(
        std::unique(roots.begin(), roots.end()), roots.end());

    for (const postgres_callback_root& callback :
         facts.callback_roots) {
        const auto definition = by_usr.find(
            callback.target_usr);
        if (definition == by_usr.end()
            || definition->second->qualified_name
                != callback.target_qualified_name
            || definition->second->source_file
                != callback.target_definition.source_file
            || !same_source_range(
                definition->second->declaration_span,
                callback.target_definition.span)) {
            fact_failure(
                callback.stable_callback_id,
                "PostgreSQL callback root does not resolve to its "
                "exact owned definition");
        }
        pending.push_back(definition->second);
    }

    std::set<std::string, std::less<>> reachable;
    while (!pending.empty()) {
        const function_flow* function = pending.front();
        pending.pop_front();
        if (!reachable.insert(function->stable_usr).second) continue;
        if (!function->body_available
            || !function->template_instantiation_complete
            || !function->macro_expansion_complete
            || !function->cfg_complete
            || !function->lifetime_analysis_complete
            || !function->implicit_call_edges_complete
            || function->cfg_block_count == 0
            || function->emitted_callsite_count
                != function->calls.size()
            || function->emitted_callsite_count
                != function->ast_callsite_count
                    + function->cfg_implicit_callsite_count
            || !std::ranges::is_sorted(
                function->lifetime_object_ids)
            || std::adjacent_find(
                   function->lifetime_object_ids.begin(),
                   function->lifetime_object_ids.end())
                != function->lifetime_object_ids.end()
            || !function->unresolved_indirect_targets.empty()
            || !function->unavailable_bodies.empty()
            || !function->control_flow_assembly_sites.empty()) {
            fact_failure(
                function->qualified_name,
                "reachable AST/CFG/lifetime/call-target closure "
                "is incomplete");
        }
        for (const flow_call_site& call : function->calls) {
            if (!call.target_set_complete
                || (call.resolution
                        != resolution_status::direct
                    && call.resolution
                        != resolution_status::resolved_indirect)
                || call.targets.empty()) {
                fact_failure(
                    call.stable_site_id,
                    "reachable call target set is incomplete");
            }
            if (call.bridge_transition != bridge_kind::none
                && (!call.bridge_primitive_evidence
                    || call.targets.size() != 1)) {
                fact_failure(
                    call.stable_site_id,
                    "reachable bridge lacks exact pinned evidence");
            }
            for (const flow_call_target& target : call.targets) {
                if (target.domain
                        == callee_domain::postgresql_server) {
                    continue;
                }
                if (target.domain
                        == callee_domain::standard_library
                    || target.domain
                        == callee_domain::system_library
                    || target.domain
                        == callee_domain::compiler_builtin) {
                    if (!target.external_provenance
                        || target.external_mode
                            == external_call_mode::unknown) {
                        fact_failure(
                            target.target_qualified_name,
                            "reachable external declaration lacks "
                            "pinned provenance");
                    }
                    if (target.external_mode
                        == external_call_mode::
                            audited_no_user_code) {
                        if (!target.external_provenance
                                 ->user_code_callback_free) {
                            fact_failure(
                                target.target_qualified_name,
                                "external stop may invoke user code");
                        }
                        continue;
                    }
                }
                if (target.domain == callee_domain::unknown) {
                    fact_failure(
                        target.target_qualified_name,
                        "reachable target domain is unknown");
                }
                const auto definition =
                    by_usr.find(target.target_usr);
                if (definition == by_usr.end()) {
                    fact_failure(
                        target.target_qualified_name,
                        "reachable project/callback target body is "
                        "unavailable");
                }
                pending.push_back(definition->second);
            }
        }
    }

    std::vector<function_flow> filtered;
    filtered.reserve(reachable.size());
    for (function_flow& function : facts.functions) {
        if (reachable.contains(function.stable_usr)) {
            filtered.push_back(std::move(function));
        }
    }
    std::vector<runtime_probe_fact> filtered_probes;
    for (runtime_probe_fact& probe : facts.runtime_probes) {
        const auto owner =
            call_owners.find(probe.site_call_stable_id);
        if (owner == call_owners.end()) {
            fact_failure(
                probe.site_call_stable_id,
                "runtime probe site call owner disappeared before "
                "reachability filtering");
        }
        if (reachable.contains(owner->second)) {
            filtered_probes.push_back(std::move(probe));
        }
    }
    return {
        .roots = std::move(roots),
        .callback_roots =
            std::move(facts.callback_roots),
        .functions = std::move(filtered),
        .runtime_probes = std::move(filtered_probes),
    };
}

[[nodiscard]] isolated_compiler_fact_scan run_database_pass(
    const std::vector<fs::path>& sources,
    const fs::path& compile_database,
    analysis_environment environment) {
    std::unique_ptr<clang::tooling::CompilationDatabase> database =
        load_compile_database(compile_database);
    std::vector<std::string> files;
    files.reserve(sources.size());
    for (const fs::path& source : sources) {
        files.push_back(source.string());
    }
    translation_output output;
    clang::tooling::ClangTool tool{*database, files};
    tool.appendArgumentsAdjuster(fact_arguments_adjuster());
    fact_factory factory{output, std::move(environment)};
    if (tool.run(std::addressof(factory)) != 0) {
        fact_failure(
            compile_database.string(),
            "Clang AST/CFG compiler-fact pass reported errors");
    }
    return merge_function_facts(
        std::move(output.functions),
        std::move(output.callback_roots));
}

}  // namespace fact_detail

compiler_fact_product produce_compiler_analysis_facts(
    const compiler_fact_request& request) {
    if (request.schema_version != 2) {
        fact_detail::fact_failure(
            "schema", "only --schema-version 2 is supported");
    }
    if (request.source_list.empty()
        || request.compile_database.empty()
        || request.pcm_root.empty()
        || request.pcm_manifest.empty()
        || request.sql_manifest.empty()
        || request.provenance.empty()
        || request.provenance_candidates_output.empty()
        || request.producer_executable.empty()) {
        fact_detail::fact_failure(
            "inputs", "one or more required paths are empty");
    }
    if (!std::filesystem::is_directory(request.pcm_root)) {
        fact_detail::fact_failure(
            request.pcm_root.string(), "PCM root is not a directory");
    }
    for (const std::filesystem::path& file : {
             request.source_list,
             request.compile_database,
             request.pcm_manifest,
             request.sql_manifest,
             request.provenance,
             request.producer_executable}) {
        if (!std::filesystem::is_regular_file(file)) {
            fact_detail::fact_failure(
                file.string(), "required input is not a regular file");
        }
    }

    static int executable_anchor;
    const std::filesystem::path actual_executable{
        llvm::sys::fs::getMainExecutable(
            request.producer_executable.c_str(),
            std::addressof(executable_anchor))};
    std::error_code equivalent_error;
    if (actual_executable.empty()
        || !std::filesystem::equivalent(
            actual_executable,
            request.producer_executable,
            equivalent_error)
        || equivalent_error) {
        fact_detail::fact_failure(
            request.producer_executable.string(),
            "--producer-executable is not the running compiler-fact "
            "producer");
    }

    const std::string source_list_text =
        bioseq::codegen::read_artifact_bytes(request.source_list);
    const std::string compile_database_text =
        bioseq::codegen::read_artifact_bytes(
            request.compile_database);
    const std::string pcm_manifest_text =
        bioseq::codegen::read_artifact_bytes(request.pcm_manifest);
    const std::string sql_manifest_text =
        bioseq::codegen::read_artifact_bytes(request.sql_manifest);
    const std::string provenance_text =
        bioseq::codegen::read_artifact_bytes(request.provenance);
    const std::string producer_bytes =
        bioseq::codegen::read_artifact_bytes(
            request.producer_executable);

    const std::vector<std::filesystem::path> sources =
        fact_detail::parse_source_list(request.source_list);
    const safety_manifest sql_manifest =
        parse_safety_manifest(sql_manifest_text);
    const provenance_bundle provenance =
        parse_provenance_bundle_json(provenance_text);
    validate_provenance_bundle(provenance);
    const fact_detail::prepared_identity identity =
        fact_detail::parse_prepared_identity(pcm_manifest_text);
    const auto prepared =
        bioseq::codegen::structural_analysis::
            validate_dependency_compilation_environment(
                sources,
                request.compile_database,
                request.pcm_root,
                request.pcm_manifest,
                identity.clang_binary,
                identity.clang_scan_deps,
                identity.llvm_config,
                identity.compiler_module_inventory,
                identity.clang_sha256,
                identity.clang_scan_deps_sha256,
                identity.llvm_config_sha256,
                identity.compiler_module_inventory_sha256,
                profile_name(request.profile),
                "safety");
    if (prepared.compilation_signature
            != identity.compilation_signature
        || prepared.manifest_sha256
            != bioseq::common::sha256_hex(
                pcm_manifest_text)) {
        fact_detail::fact_failure(
            request.pcm_manifest.string(),
            "prepared compilation validation returned a different "
            "signature or manifest hash");
    }

    std::unique_ptr<clang::tooling::CompilationDatabase> database =
        fact_detail::load_compile_database(
            request.compile_database);
    const std::vector<clang::tooling::CompileCommand> commands =
        fact_detail::validate_compile_commands(
            *database, sources, request.pcm_root);
    const fact_detail::postgresql_header_authority
        postgresql_authority =
            fact_detail::derive_postgresql_header_authority(
                commands, provenance);
    const std::string compile_graph =
        fact_detail::render_compile_graph(
            request,
            sources,
            commands,
            source_list_text,
            compile_database_text,
            pcm_manifest_text,
            identity,
            postgresql_authority);

    auto provenance_candidates = std::make_shared<std::map<
        std::string,
        fact_detail::analysis_environment::provenance_candidate,
        std::less<>>>();
    fact_detail::analysis_environment environment{
        .postgresql_header_roots =
            postgresql_authority.authenticated_roots,
        .rejected_postgresql_header_roots =
            postgresql_authority.rejected_roots,
        .provenance = std::addressof(provenance),
        .compiler_profile =
            std::string{profile_name(request.profile)},
        .provenance_authority =
            "clang:" + std::string{CLANG_VERSION_STRING},
        .provenance_candidates = provenance_candidates,
    };
    for (const std::filesystem::path& source : sources) {
        environment.source_paths.insert(
            fact_detail::canonical_path(source));
    }
    isolated_compiler_fact_scan scanned =
        fact_detail::run_database_pass(
            sources,
            request.compile_database,
            std::move(environment));
    if (scanned.callback_roots.empty()) {
        fact_detail::fact_failure(
            profile_name(request.profile),
            "no PostgreSQL callback roots were discovered in the "
            "profile source closure");
    }
    const std::string candidate_inventory =
        fact_detail::render_provenance_candidates(
            request.profile,
            "clang:" + std::string{CLANG_VERSION_STRING},
            *provenance_candidates);
    bioseq::codegen::publish_artifact_if_changed(
        request.provenance_candidates_output,
        candidate_inventory);
    if (bioseq::codegen::read_artifact_bytes(
            request.provenance_candidates_output)
        != candidate_inventory) {
        fact_detail::fact_failure(
            request.provenance_candidates_output.string(),
            "persisted provenance candidate inventory differs "
            "from analyzed candidates");
    }
    fact_detail::reachable_compiler_facts reachable =
        fact_detail::reachable_fact_closure(
            sql_manifest, std::move(scanned));

    compiler_analysis_facts facts{
        .schema_revision = 2,
        .profile = request.profile,
        .compiler_profile =
            std::string{profile_name(request.profile)},
        .clang_version = CLANG_VERSION_STRING,
        .source_list_sha256 =
            bioseq::common::sha256_hex(source_list_text),
        .compile_database_sha256 =
            bioseq::common::sha256_hex(compile_database_text),
        .producer_executable_sha256 =
            bioseq::common::sha256_hex(producer_bytes),
        .compile_graph_sha256 =
            bioseq::common::sha256_hex(compile_graph),
        .pcm_manifest_sha256 =
            bioseq::common::sha256_hex(pcm_manifest_text),
        .sql_manifest_sha256 =
            bioseq::common::sha256_hex(sql_manifest_text),
        .root_usrs = std::move(reachable.roots),
        .callback_roots =
            std::move(reachable.callback_roots),
        .functions = std::move(reachable.functions),
        .runtime_probes =
            std::move(reachable.runtime_probes),
    };
    return {
        .facts = std::move(facts),
        .compile_graph_json = compile_graph,
    };
}

isolated_compiler_fact_scan scan_safety_facts_from_code(
    const std::filesystem::path& source_path,
    std::string_view source,
    const std::vector<std::string>& compiler_arguments,
    std::string_view compiler_profile,
    const provenance_bundle* provenance) {
    if (source.empty() || source_path.empty()) {
        fact_detail::fact_failure(
            "isolated pass", "source path/text cannot be empty");
    }
    fact_detail::translation_output output;
    auto provenance_candidates = std::make_shared<std::map<
        std::string,
        fact_detail::analysis_environment::provenance_candidate,
        std::less<>>>();
    fact_detail::analysis_environment environment{
        .provenance = provenance,
        .compiler_profile = std::string{compiler_profile},
        .provenance_authority =
            "clang:" + std::string{CLANG_VERSION_STRING},
        .provenance_candidates =
            std::move(provenance_candidates),
    };
    environment.source_paths.insert(
        fact_detail::canonical_path(source_path));
    std::vector<std::string> arguments = compiler_arguments;
    if (std::ranges::none_of(
            arguments,
            [](std::string_view argument) {
                return argument.starts_with("-std=");
            })) {
        arguments.emplace_back("-std=c++26");
    }
    arguments.emplace_back("-Wno-unknown-attributes");
    arguments.emplace_back("-Wno-ignored-attributes");
    if (provenance != nullptr) {
        std::vector<std::string> command_line{"clang++"};
        command_line.insert(
            command_line.end(),
            arguments.begin(),
            arguments.end());
        const std::vector<clang::tooling::CompileCommand>
            commands{clang::tooling::CompileCommand{
                std::filesystem::current_path().string(),
                source_path.string(),
                std::move(command_line),
                ""}};
        const fact_detail::postgresql_header_authority authority =
            fact_detail::derive_postgresql_header_authority(
                commands, *provenance);
        environment.postgresql_header_roots =
            authority.authenticated_roots;
        environment.rejected_postgresql_header_roots =
            authority.rejected_roots;
    }
    const bool success = clang::tooling::runToolOnCodeWithArgs(
        std::make_unique<fact_detail::fact_action>(
            output, std::move(environment)),
        llvm::StringRef{source.data(), source.size()},
        arguments,
        source_path.string());
    if (!success) {
        fact_detail::fact_failure(
            source_path.string(),
            "isolated Clang AST/CFG fact pass reported errors");
    }
    return fact_detail::merge_function_facts(
        std::move(output.functions),
        std::move(output.callback_roots));
}

std::vector<function_flow> scan_safety_flow_from_code(
    const std::filesystem::path& source_path,
    std::string_view source,
    const std::vector<std::string>& compiler_arguments,
    std::string_view compiler_profile,
    const provenance_bundle* provenance) {
    return scan_safety_facts_from_code(
               source_path,
               source,
               compiler_arguments,
               compiler_profile,
               provenance)
        .functions;
}

}  // namespace bioseq::codegen::safety
