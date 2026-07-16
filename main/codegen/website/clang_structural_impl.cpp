// Clang 22 structural/export index implementation (§11.3), compiled as an
// ordinary (non-module) TU so Clang/LLVM headers never enter the C++26 module
// graph. Exposes only the POD clang_shim interface. Clang owns C++ semantics;
// it never reads raw comment text and never emits object/dependency/BMI output.

#include "clang_structural_shim.h"

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/PrettyPrinter.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Basic/Module.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Basic/Specifiers.h>
#include <clang/Basic/Version.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Lex/Lexer.h>
#include <clang/Tooling/ArgumentsAdjusters.h>
#include <clang/Tooling/CompilationDatabase.h>
#include <clang/Tooling/JSONCompilationDatabase.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>

#include <cstddef>
#include <fstream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>

namespace bioseq::codegen::website::clang_shim {
namespace {

// A fixed, fully-qualified printing policy so signatures are deterministic and
// independent of the parsed translation unit's using-directives.
clang::PrintingPolicy make_policy(const clang::ASTContext& context) {
    clang::PrintingPolicy policy(context.getLangOpts());
    policy.FullyQualifiedName = true;
    policy.SuppressTagKeyword = true;
    policy.SuppressScope = false;
    policy.Bool = true;
    policy.TerseOutput = false;
    policy.PolishForDeclaration = true;
    return policy;
}

const char* classify_record(const clang::CXXRecordDecl* record) {
    if (record->isUnion()) {
        return "union";
    }
    if (record->isStruct()) {
        return "struct";
    }
    return "class";
}

const char* member_kind_of(const clang::FunctionDecl* function) {
    if (llvm::isa<clang::CXXConstructorDecl>(function)) {
        return "constructor";
    }
    if (llvm::isa<clang::CXXDestructorDecl>(function)) {
        return "destructor";
    }
    if (llvm::isa<clang::CXXConversionDecl>(function)) {
        return "conversion";
    }
    if (function->isOverloadedOperator()) {
        return "operator";
    }
    if (llvm::isa<clang::CXXMethodDecl>(function)) {
        return "method";
    }
    return "free";
}

struct structural_visitor : clang::RecursiveASTVisitor<structural_visitor> {
    clang::ASTContext* context{};
    const clang::SourceManager* sources{};
    clang::PrintingPolicy policy;
    std::vector<entity>* out{};
    const std::set<std::string>* files{};

    explicit structural_visitor(clang::ASTContext& ctx)
        : context(&ctx), sources(&ctx.getSourceManager()), policy(make_policy(ctx)) {}

    bool allows(llvm::StringRef path) const {
        return files == nullptr || files->empty()
            || files->count(std::string(path)) != 0;
    }

    // A decl is a candidate page-owner when it is exported by a named module,
    // written in one of the reachable public files, and not compiler-implicit.
    bool candidate(const clang::Decl* decl) const {
        if (decl->isImplicit() || !decl->isInExportDeclContext()) {
            return false;
        }
        const clang::SourceLocation location = decl->getLocation();
        if (location.isInvalid() || !sources->isInMainFile(location)) {
            return false;
        }
        return allows(sources->getFilename(location));
    }

    void fill_location(const clang::Decl* decl, entity& record) const {
        const clang::SourceLocation location = decl->getLocation();
        if (location.isInvalid()) {
            return;
        }
        record.path = std::string(sources->getFilename(location));
        record.line = sources->getSpellingLineNumber(location);
        record.column = sources->getSpellingColumnNumber(location);
        record.has_location = true;
    }

    void fill_module(const clang::Decl* decl, entity& record) const {
        if (const clang::Module* owner = decl->getOwningModule()) {
            record.module_name = owner->getFullModuleName();
            record.has_module = true;
        }
    }

    void emit(const clang::NamedDecl* decl, const char* kind) {
        entity record;
        record.kind = kind;
        record.qualified_name = decl->getQualifiedNameAsString();
        record.display_name = decl->getNameAsString();
        record.normalized_signature = record.qualified_name;
        fill_module(decl, record);
        fill_location(decl, record);
        out->push_back(std::move(record));
    }

    void emit_function(const clang::FunctionDecl* function) {
        entity record;
        record.kind = "function";
        record.qualified_name = function->getQualifiedNameAsString();
        record.display_name = function->getNameAsString();
        record.return_type = function->getReturnType().getAsString(policy);
        record.has_return_type = true;
        std::string signature = record.qualified_name + "(";
        bool first = true;
        for (const clang::ParmVarDecl* param : function->parameters()) {
            parameter info;
            info.name = param->getNameAsString();
            info.type = param->getType().getAsString(policy);
            if (param->hasDefaultArg()) {
                if (const clang::Expr* value = param->getDefaultArg()) {
                    const clang::CharSourceRange range =
                        clang::CharSourceRange::getTokenRange(value->getSourceRange());
                    info.default_value = std::string(clang::Lexer::getSourceText(
                        range, *sources, context->getLangOpts()));
                    info.has_default = true;
                }
            }
            if (!first) {
                signature += ", ";
            }
            signature += info.type;
            first = false;
            record.parameters.push_back(std::move(info));
        }
        signature += ")";
        // Preserve the cv- and ref-qualifiers that distinguish member-function
        // overloads (§12.1), in declaration order `const volatile &`/`&&`.
        if (const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(function)) {
            if (method->isConst()) {
                signature += " const";
            }
            if (method->isVolatile()) {
                signature += " volatile";
            }
            if (method->getRefQualifier() == clang::RQ_LValue) {
                signature += " &";
            } else if (method->getRefQualifier() == clang::RQ_RValue) {
                signature += " &&";
            }
        }
        record.normalized_signature = std::move(signature);
        record.member_kind = member_kind_of(function);
        fill_module(function, record);
        fill_location(function, record);
        out->push_back(std::move(record));
    }

    bool VisitNamespaceDecl(clang::NamespaceDecl* decl) {
        if (candidate(decl) && !decl->isAnonymousNamespace()) {
            emit(decl, "namespace");
        }
        return true;
    }
    bool VisitCXXRecordDecl(clang::CXXRecordDecl* decl) {
        if (candidate(decl) && decl->isThisDeclarationADefinition()
            && !decl->isInjectedClassName()) {
            emit(decl, classify_record(decl));
        }
        return true;
    }
    bool VisitFunctionDecl(clang::FunctionDecl* decl) {
        if (const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(decl)) {
            const clang::AccessSpecifier access = method->getAccess();
            if (access == clang::AS_private || access == clang::AS_protected) {
                return true;
            }
        }
        if (candidate(decl)) {
            emit_function(decl);
        }
        return true;
    }
    bool VisitEnumDecl(clang::EnumDecl* decl) {
        if (candidate(decl)) {
            emit(decl, "enum");
        }
        return true;
    }
    bool VisitEnumConstantDecl(clang::EnumConstantDecl* decl) {
        if (candidate(decl)) {
            emit(decl, "enum_value");
        }
        return true;
    }
    bool VisitTypeAliasDecl(clang::TypeAliasDecl* decl) {
        if (candidate(decl)) {
            emit(decl, "type_alias");
        }
        return true;
    }
    bool VisitTypedefDecl(clang::TypedefDecl* decl) {
        if (candidate(decl)) {
            emit(decl, "type_alias");
        }
        return true;
    }
    bool VisitVarDecl(clang::VarDecl* decl) {
        if (decl->isLocalVarDeclOrParm()) {
            return true;
        }
        if (candidate(decl)) {
            emit(decl, "variable");
        }
        return true;
    }
    bool VisitConceptDecl(clang::ConceptDecl* decl) {
        if (candidate(decl)) {
            emit(decl, "concept");
        }
        return true;
    }
};

struct structural_consumer : clang::ASTConsumer {
    std::vector<entity>* out;
    const std::set<std::string>* files;
    structural_consumer(std::vector<entity>* target, const std::set<std::string>* reachable)
        : out(target), files(reachable) {}
    void HandleTranslationUnit(clang::ASTContext& context) override {
        structural_visitor visitor(context);
        visitor.out = out;
        visitor.files = files;
        visitor.TraverseDecl(context.getTranslationUnitDecl());
    }
};

struct structural_action : clang::ASTFrontendAction {
    std::vector<entity>* out;
    const std::set<std::string>* files;
    structural_action(std::vector<entity>* target, const std::set<std::string>* reachable)
        : out(target), files(reachable) {}
    std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
        clang::CompilerInstance&, llvm::StringRef) override {
        return std::make_unique<structural_consumer>(out, files);
    }
};

struct structural_factory : clang::tooling::FrontendActionFactory {
    std::vector<entity>* out;
    const std::set<std::string>* files;
    structural_factory(std::vector<entity>* target, const std::set<std::string>* reachable)
        : out(target), files(reachable) {}
    std::unique_ptr<clang::FrontendAction> create() override {
        return std::make_unique<structural_action>(out, files);
    }
};

// Deduplicates redeclarations by (qualified name + normalized signature),
// keeping the first occurrence for deterministic order.
void deduplicate(std::vector<entity>& entities) {
    std::set<std::string> seen;
    std::vector<entity> unique;
    unique.reserve(entities.size());
    for (entity& record : entities) {
        std::string key = record.qualified_name + '\x1f' + record.normalized_signature;
        if (seen.insert(std::move(key)).second) {
            unique.push_back(std::move(record));
        }
    }
    entities = std::move(unique);
}

// Inlines an `@response-file` argument, or passes a plain argument through. clang
// tooling feeds compile-command arguments straight to CompilerInvocation, which -
// unlike the clang driver - does NOT expand `@file` arguments, so CMake's per-module
// import response file (`@.../foo.cppm.o.modmap`) would otherwise be misread as an
// input file named `@...`. The module map lists one argument per line; it is read
// relative to the current directory, which the scan pins to the compile db root so
// the map and the relative `-fmodule-file=` paths inside it resolve.
void append_expanded(const std::string& arg,
                     clang::tooling::CommandLineArguments& out) {
    if (arg.empty() || arg.front() != '@') {
        out.push_back(arg);
        return;
    }
    std::ifstream stream(arg.substr(1));
    if (!stream) {
        out.push_back(arg);  // leave it to clang to report the missing file
        return;
    }
    // The module map is whitespace-separated (e.g. `-x c++-module` is two tokens and
    // every module file its own token); no path in the build tree contains a space,
    // so a plain whitespace split reproduces the driver's tokenisation.
    std::string token;
    while (stream >> token) {
        out.push_back(token);
    }
}

// Expands response files, strips object/dependency/scan/diagnostic/BMI-output flags,
// and adds -fsyntax-only (§11.3). Retains -fmodule-file/import PCMs, standard,
// include, define, target, and ABI flags.
clang::tooling::ArgumentsAdjuster allowlist_adjuster() {
    return [](const clang::tooling::CommandLineArguments& args,
              llvm::StringRef) -> clang::tooling::CommandLineArguments {
        static const std::set<std::string> drop_with_value = {
            "-o", "-MF", "-MT", "-MQ", "-MJ", "--serialize-diagnostics"};
        static const std::set<std::string> drop_flag = {
            "-c", "-MD", "-MMD", "-MG", "-MP", "-M", "-MM"};
        clang::tooling::CommandLineArguments expanded;
        expanded.reserve(args.size() + 16);
        for (const std::string& arg : args) {
            append_expanded(arg, expanded);
        }
        clang::tooling::CommandLineArguments result;
        result.reserve(expanded.size() + 1);
        for (std::size_t index = 0; index < expanded.size(); ++index) {
            const std::string& arg = expanded[index];
            if (drop_flag.count(arg) != 0) {
                continue;
            }
            if (drop_with_value.count(arg) != 0) {
                ++index;
                continue;
            }
            if (arg.rfind("-fmodule-output", 0) == 0) {
                continue;
            }
            if (arg.rfind("-MF", 0) == 0 || arg.rfind("-MT", 0) == 0
                || arg.rfind("-MQ", 0) == 0 || arg.rfind("-MJ", 0) == 0) {
                continue;
            }
            result.push_back(arg);
        }
        result.push_back("-fsyntax-only");
        return result;
    };
}

// Records the provided module and each import edge, classifying an import as a
// public edge only when the ImportDecl sits in an export declaration context
// (§11.3): `export import M;` is a re-export, plain `import M;` never is.
struct edges_visitor : clang::RecursiveASTVisitor<edges_visitor> {
    module_edges* out{};
    bool VisitImportDecl(clang::ImportDecl* decl) {
        const clang::Module* imported = decl->getImportedModule();
        if (imported == nullptr) {
            return true;
        }
        std::string name = imported->getFullModuleName();
        if (decl->isInExportDeclContext()) {
            out->exported_imports.push_back(std::move(name));
        } else {
            out->plain_imports.push_back(std::move(name));
        }
        return true;
    }
};

struct edges_consumer : clang::ASTConsumer {
    std::vector<module_edges>* out;
    explicit edges_consumer(std::vector<module_edges>* target) : out(target) {}
    void HandleTranslationUnit(clang::ASTContext& context) override {
        module_edges unit;
        const clang::SourceManager& sources = context.getSourceManager();
        if (const auto file = sources.getFileEntryRefForID(sources.getMainFileID())) {
            unit.source_path = std::string(file->getName());
        }
        if (const clang::Module* current = context.getCurrentNamedModule()) {
            unit.provided_module = current->getFullModuleName();
            unit.has_provided = true;
        }
        edges_visitor visitor;
        visitor.out = &unit;
        visitor.TraverseDecl(context.getTranslationUnitDecl());
        out->push_back(std::move(unit));
    }
};

struct edges_action : clang::ASTFrontendAction {
    std::vector<module_edges>* out;
    explicit edges_action(std::vector<module_edges>* target) : out(target) {}
    std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
        clang::CompilerInstance&, llvm::StringRef) override {
        return std::make_unique<edges_consumer>(out);
    }
};

struct edges_factory : clang::tooling::FrontendActionFactory {
    std::vector<module_edges>* out;
    explicit edges_factory(std::vector<module_edges>* target) : out(target) {}
    std::unique_ptr<clang::FrontendAction> create() override {
        return std::make_unique<edges_action>(out);
    }
};

}  // namespace

// Runs the process from the compile database's directory for the guard's lifetime
// and restores the previous directory on destruction. The compile commands are
// generated with a `directory` of the build root and reference their module maps
// (`@.../foo.cppm.o.modmap`) - and the relative `-fmodule-file=` BMI paths inside
// them - relative to it. ClangTool applies that directory only to its virtual file
// system, but response-file expansion resolves against the real working directory,
// so those paths must be reachable from the process CWD too (§11.3).
class scoped_compile_db_cwd {
  public:
    explicit scoped_compile_db_cwd(const std::string& compile_database) {
        if (llvm::sys::fs::current_path(previous_)) {
            return;  // cannot read CWD; leave it untouched
        }
        const llvm::StringRef directory =
            llvm::sys::path::parent_path(compile_database);
        if (!directory.empty()) {
            restore_ = !static_cast<bool>(llvm::sys::fs::set_current_path(directory));
        }
    }
    scoped_compile_db_cwd(const scoped_compile_db_cwd&) = delete;
    scoped_compile_db_cwd& operator=(const scoped_compile_db_cwd&) = delete;
    ~scoped_compile_db_cwd() {
        if (restore_) {
            static_cast<void>(llvm::sys::fs::set_current_path(previous_));
        }
    }

  private:
    llvm::SmallString<256> previous_;
    bool restore_ = false;
};

// Keeps only the units that this configuration actually builds: a unit is buildable
// when at least one of its compile commands is satisfiable - every `@response` file
// it names (the module map) exists. A unit compiled solely by targets this build does
// not produce (test/bench-only helpers) has no satisfiable command and is not part of
// the shipped library's reachable universe, so it is dropped rather than failing the
// whole scan; clang tooling already skips units that have no compile command at all.
// Module-map paths resolve against the current directory, which scoped_compile_db_cwd
// pins to the compile db root before this runs.
std::vector<std::string> buildable_units(
    const clang::tooling::CompilationDatabase& database,
    const std::vector<std::string>& reachable_units) {
    std::vector<std::string> kept;
    kept.reserve(reachable_units.size());
    for (const std::string& unit : reachable_units) {
        for (const clang::tooling::CompileCommand& command :
             database.getCompileCommands(unit)) {
            bool satisfiable = true;
            for (const std::string& arg : command.CommandLine) {
                if (!arg.empty() && arg.front() == '@'
                    && !llvm::sys::fs::exists(arg.substr(1))) {
                    satisfiable = false;
                    break;
                }
            }
            if (satisfiable) {
                kept.push_back(unit);
                break;
            }
        }
    }
    return kept;
}

module_graph scan_module_edges_from_db(
    const std::vector<std::string>& reachable_units,
    const std::string& compile_database) {
    module_graph result;
    result.clang_version = CLANG_VERSION_STRING;
    std::string load_error;
    std::unique_ptr<clang::tooling::CompilationDatabase> database =
        clang::tooling::JSONCompilationDatabase::loadFromFile(
            compile_database, load_error,
            clang::tooling::JSONCommandLineSyntax::AutoDetect);
    if (!database) {
        throw std::runtime_error("cannot load compile database " + compile_database
                                 + ": " + load_error);
    }
    const scoped_compile_db_cwd cwd(compile_database);
    const std::vector<std::string> units = buildable_units(*database, reachable_units);
    clang::tooling::ClangTool tool(*database, units);
    tool.appendArgumentsAdjuster(allowlist_adjuster());
    edges_factory factory(&result.units);
    if (tool.run(&factory) != 0) {
        throw std::runtime_error("clang module-edge scan reported errors");
    }
    return result;
}

module_graph scan_module_edges_from_code(
    const std::string& code, const std::vector<std::string>& args) {
    module_graph result;
    result.clang_version = CLANG_VERSION_STRING;
    const bool ok = clang::tooling::runToolOnCodeWithArgs(
        std::make_unique<edges_action>(&result.units), code, args, "unit.cppm");
    if (!ok) {
        throw std::runtime_error("clang module-edge scan failed on in-memory unit");
    }
    return result;
}

index run_structural_from_code(
    const std::string& code, const std::vector<std::string>& args) {
    index result;
    result.clang_version = CLANG_VERSION_STRING;
    const std::set<std::string> files;  // empty => allow the main file
    const bool ok = clang::tooling::runToolOnCodeWithArgs(
        std::make_unique<structural_action>(&result.entities, &files),
        code, args, "unit.cppm");
    if (!ok) {
        throw std::runtime_error("clang structural pass failed on in-memory unit");
    }
    deduplicate(result.entities);
    return result;
}

index run_structural_from_db(
    const std::vector<std::string>& reachable_units,
    const std::string& compile_database) {
    index result;
    result.clang_version = CLANG_VERSION_STRING;
    std::string load_error;
    std::unique_ptr<clang::tooling::CompilationDatabase> database =
        clang::tooling::JSONCompilationDatabase::loadFromFile(
            compile_database, load_error,
            clang::tooling::JSONCommandLineSyntax::AutoDetect);
    if (!database) {
        throw std::runtime_error("cannot load compile database " + compile_database
                                 + ": " + load_error);
    }
    std::set<std::string> reachable_files(reachable_units.begin(), reachable_units.end());
    const scoped_compile_db_cwd cwd(compile_database);
    const std::vector<std::string> units = buildable_units(*database, reachable_units);
    clang::tooling::ClangTool tool(*database, units);
    tool.appendArgumentsAdjuster(allowlist_adjuster());
    structural_factory factory(&result.entities, &reachable_files);
    if (tool.run(&factory) != 0) {
        throw std::runtime_error("clang structural pass reported errors");
    }
    deduplicate(result.entities);
    return result;
}

std::string clang_version() {
    return CLANG_VERSION_STRING;
}

}  // namespace bioseq::codegen::website::clang_shim
