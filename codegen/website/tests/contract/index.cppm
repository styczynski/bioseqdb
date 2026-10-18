module;

#include <string>
#include <stdexcept>

module bioseq.codegen.website:tests.contract;

import :build_pipeline;
import :reference_index;
import :search_index;
import :html_renderer;
import bioseq.codegen.website.postgres;
import bioseq.codegen.website.model;
import bioseq.codegen.support;

// Import the website modules and call  their APIs so compilation checks
// the declared dependencies. The test entrypoint also runs the website
// pipeline checks.
namespace bioseq::codegen::website {

/// @brief Checks the imported website APIs and runs the pipeline tests.
void run_website_contract_self_tests() {
    // Only the sources actually used on a page belong in its compact footer.
    // Bibliography links remain local in the prose, but their cited URLs are
    // also exposed as ordinary, crawlable links at the end of the document.
    const std::string sources = render_external_sources_html(
        "<p><a href=\"../bibliography/#ref-used\">[1]</a> "
        "<a href=\"../bibliography/#ref-used\">[1]</a> "
        "<a href=\"https://example.org/paper?a=1&amp;b=2\">Repeated paper</a> "
        "<a href=\"https://other.org/guide\">A &amp; B guide</a> "
        "<a href=\"https://bioseqdb.com/blog/\">Our blog</a></p>"
        "<pre>&lt;a href=\"https://code.org/\"&gt;Code&lt;/a&gt;</pre>",
        "<p><a id=\"ref-used\"></a></p>"
        "<p><a href=\"https://example.org/paper?a=1&amp;b=2\">The &lt;paper&gt;</a></p>"
        "<a id=\"ref-unused\"></a><a href=\"https://unused.org/\">Unused paper</a>",
        "https://bioseqdb.com");
    const std::string first_source = "href=\"https://example.org/paper?a=1&amp;b=2\"";
    const auto first_source_at = sources.find(first_source);
    if (first_source_at == std::string::npos
        || sources.find(first_source, first_source_at + 1) != std::string::npos
        || !sources.contains("The &lt;paper&gt;")
        || !sources.contains("A &amp; B guide")
        || sources.contains("unused.org") || sources.contains("code.org")
        || sources.contains("bioseqdb.com/blog")) {
        throw std::runtime_error("External sources must resolve cited references, dedupe and escape links");
    }
    if (!render_external_sources_html("<p>No external links</p>", "", "https://bioseqdb.com").empty()) {
        throw std::runtime_error("Documents without external sources must not render an empty section");
    }
    const auto mounted_sources = render_external_sources_html(
        "<a href=\"/bioseqdb/v0.1/tutorials/bibliography/#ref-one\">1</a>",
        "<a id='ref-one'></a><a href='https://example.org/one'>One</a>",
        "https://bioseqdb.com");
    if (!mounted_sources.contains("https://example.org/one")) {
        throw std::runtime_error("Bibliography sources must resolve under a mounted base path");
    }
    const std::string download_body =
        "<a href=\"https://downloads.sourceforge.net/project/bioseqdb/build.deb\">Package</a>"
        "<a href=\"https://sourceforge.net/projects/bioseqdb/files/build.tar.gz/download?use_mirror=auto\">Archive</a>"
        "<a href=\"https://downloads.sourceforge.net/project/bioseqdb/sdk.json\">Manifest</a>"
        "<a href=\"https://example.org/paper.pdf\">Scientific paper</a>"
        "<a href=\"https://github.com/example/project/blob/main/README.txt\">Source documentation</a>"
        "<a href=\"https://sourceforge.net/projects/bioseqdb/\">Release page</a>";
    const auto reading_sources = render_external_sources_html(download_body, "", "https://bioseqdb.com");
    if (!download_body.contains("build.deb") || !download_body.contains("sdk.json")
        || reading_sources.contains("build.deb") || reading_sources.contains("build.tar.gz")
        || reading_sources.contains("sdk.json") || !reading_sources.contains("paper.pdf")
        || !reading_sources.contains("README.txt") || !reading_sources.contains("Release page")) {
        throw std::runtime_error("Source footers must omit package downloads but retain papers and source pages");
    }
    // Check that the PostgreSQL executor provides the required operations.
    static_assert(sql_example_executor<postgres_tutorial_executor>,
                  "postgres executor must model the tutorial executor concept");
    // Exercise the reference-ID API through its module import.
    const std::string probe = make_reference_entity_id(reference_entity{
        .domain = entity_domain::cpp, .qualified_name = "bioseq::probe"});
    (void)probe;
    // A GUC's leaf is ambiguous: preserve its full setting path in search.
    reference_entity setting;
    setting.domain = entity_domain::sql;
    setting.kind = entity_kind::sql_object;
    setting.canonical_id = "sql:guc:bioseq.index.insert_maintenance.enabled";
    setting.qualified_name = "bioseq.index.insert_maintenance.enabled";
    setting.display_name = "enabled";
    setting.attributes["sql_kind"] = "guc";
    const auto search_setting = build_entity_search_entry(setting);
    if (search_setting.kind != "setting"
        || search_setting.group != "SQL"
        || search_setting.title != "index.insert_maintenance.enabled"
        || search_setting.qualified_name != setting.qualified_name
        || search_setting.name_source.find("index.insert_maintenance.enabled")
            == std::string::npos) {
        throw std::runtime_error("GUC search must retain the full setting label and SQL identity");
    }
    setting.attributes.clear();
    if (build_entity_search_entry(setting).kind != "setting") {
        throw std::runtime_error("GUC canonical identities must retain the Setting search kind");
    }
    setting.canonical_id = "sql:guc:bioseq_cache.enabled";
    setting.qualified_name = "bioseq_cache.enabled";
    if (build_entity_search_entry(setting).title != "bioseq_cache.enabled") {
        throw std::runtime_error("Setting search may strip only the leading bioseq. prefix");
    }
    setting.canonical_id = "sql:operator:bioseq.=(bioseq.seq,bioseq.seq)";
    setting.qualified_name = "bioseq.=";
    setting.display_name = "=";
    if (build_entity_search_entry(setting).kind != "sql_object"
        || build_entity_search_entry(setting).title != "=") {
        throw std::runtime_error("Non-GUC SQL objects must not be relabeled as settings");
    }
    verify_website_pipeline();
}

}  // namespace bioseq::codegen::website
