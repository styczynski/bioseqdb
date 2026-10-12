import assert from "node:assert/strict";
import test from "node:test";
import { rankedSearch, searchDomain, searchType } from "../../static/js/search-ranking.mjs";

const record = (id, kind, title, group, extra = {}) => ({
  id, kind, title, group, qualified_name: title, route: `/${id}/`, ...extra,
});
const setting = record("sql:guc:bioseq.index.insert_maintenance.enabled", "setting",
  "index.insert_maintenance.enabled", "SQL", {
    qualified_name: "bioseq.index.insert_maintenance.enabled",
    summary: "Update indexes during INSERT using the older index format.",
  });

test("blog posts have their own label and rank after SQL, documentation, and C++", () => {
  const blog = record("blog:sequences", "blog", "Sequences in SQL", "blog");
  const cpp = record("cpp:function:sequences", "function", "Sequences", "C++");
  const sql = record("sql:type:seq", "sql_type", "seq", "SQL", { summary: "Sequences" });
  assert.equal(searchType(blog), "Blog");
  assert.equal(searchDomain(blog), "Documentation");
  const tutorial = record("tutorial:sequences", "tutorial", "Sequences", "Tutorials");
  assert.deepEqual(rankedSearch([cpp, blog, sql, tutorial], "sequences"), [sql, tutorial, cpp, blog]);
  const exactBlog = record("blog:exact", "blog", "Sequences", "blog");
  const broadCpp = record("cpp:function:broad", "function", "other", "C++", { summary: "Sequences" });
  assert.deepEqual(rankedSearch([exactBlog, broadCpp], "sequences"), [broadCpp, exactBlog]);
  assert.deepEqual(rankedSearch([exactBlog, broadCpp], "sequences", 1), [broadCpp]);
});

test("GUC path fragments and full names return the Setting record", () => {
  assert.equal(searchType(setting), "Setting");
  assert.equal(searchDomain(setting), "SQL");
  for (const query of ["insert_maintenance", setting.title, setting.qualified_name]) {
    assert.deepEqual(rankedSearch([setting], query), [setting]);
  }
});

test("an exact setting beats an identically named SQL function and broad matches", () => {
  const sameNameFunction = record("sql:function:alias()", "sql_function", setting.title, "SQL");
  const broad = record("sql:function:helper()", "sql_function", "helper", "SQL", {
    summary: `Reads ${setting.title}.`,
  });
  const cppExact = record("cpp:variable:enabled", "variable", setting.title, "C++");
  const records = [cppExact, broad, sameNameFunction, setting];
  assert.deepEqual(rankedSearch(records, setting.title).map(x => x.id),
    [setting.id, sameNameFunction.id, broad.id, cppExact.id]);
});

test("all SQL kinds precede even exact C++ and tutorial matches", () => {
  const cpp = record("cpp:function:score()", "function", "score", "C++");
  const tutorial = record("tutorial:score", "tutorial", "score", "Tutorials");
  const sql = [
    record("sql:function:score(bioseq.seq)", "sql_function", "score", "SQL"),
    record("sql:type:seq", "sql_type", "seq", "SQL", { summary: "Contains an alignment score." }),
    record("sql:operator:>", "sql_object", ">", "SQL", { summary: "Compare score values." }),
    record("sql:guc:score", "setting", "aligner.score", "SQL"),
  ];
  const result = rankedSearch([cpp, tutorial, ...sql], "score");
  assert.ok(result.slice(0, sql.length).every(x => searchDomain(x) === "SQL"));
  assert.deepEqual(result.slice(sql.length).map(x => x.id), [tutorial.id, cpp.id]);
});

test("relevance within SQL stays exact, prefix, name, description", () => {
  const records = [
    record("sql:function:body", "sql_function", "other", "SQL", { summary: "score" }),
    record("sql:type:name", "sql_type", "target_score", "SQL"),
    record("sql:function:prefix", "sql_function", "score_value", "SQL"),
    record("sql:function:exact", "sql_function", "score", "SQL"),
  ];
  assert.deepEqual(rankedSearch(records, "score").map(x => x.id),
    ["sql:function:exact", "sql:function:prefix", "sql:type:name", "sql:function:body"]);
});

test("ties and the result cap are deterministic and applied after SQL priority", () => {
  const sql = record("sql:type:needle", "sql_type", "needle", "SQL");
  const cpp = Array.from({ length: 45 }, (_, i) => record(`cpp:${String(i).padStart(2, "0")}`,
    "function", "needle", "C++"));
  const records = [...cpp, sql];
  const forward = rankedSearch(records, "needle");
  assert.equal(forward.length, 40);
  assert.equal(forward[0].id, sql.id);
  assert.deepEqual(forward, rankedSearch([...records].reverse(), "needle"));
  assert.equal(rankedSearch(records, "")[0].id, sql.id);
  assert.deepEqual(rankedSearch(records, "missing"), []);
});
