// Search relevance is independent of presentation and browser locale.
import { normalizeSearchText } from "./search_tokens.mjs";

export const MAX_SEARCH_RESULTS = 40;

export function searchType(record) {
  const kind = String(record.kind || "").toLowerCase();
  const group = String(record.group || "").toLowerCase();
  if (kind === "setting") return "Setting";
  if (kind === "sql_object") return "SQL";
  if (group === "tutorials" || group === "pages" ||
      kind === "tutorial" || kind === "page") return "Tutorial";
  if (kind.includes("function") || kind === "overload_group") return "Function";
  if (kind.includes("type") || kind.includes("alias") ||
      kind.includes("enum") || kind === "sql_schema") return "Type";
  return "Class";
}

export function searchDomain(record) {
  const kind = String(record.kind || "").toLowerCase();
  const group = String(record.group || "").toLowerCase();
  if (group === "sql" || String(record.id || "").startsWith("sql:") ||
      kind.startsWith("sql_") || kind === "setting") return "SQL";
  if (group === "tutorials" || group === "pages" ||
      kind === "tutorial" || kind === "page") return "Documentation";
  return "C++";
}

const DOMAIN_PRIORITY = { SQL: 0, Documentation: 1, "C++": 2 };
const compareText = (left, right) => left < right ? -1 : left > right ? 1 : 0;

export function rankedSearch(records, query, limit = MAX_SEARCH_RESULTS) {
  const normalized = normalizeSearchText(query);
  const tokens = normalized.split(" ").filter(Boolean);
  const scored = [];
  records.forEach((record) => {
    const title = normalizeSearchText(record.title || "");
    const qualified = normalizeSearchText(record.qualified_name || "");
    const names = String(record.name_tokens || `${title} ${qualified}`);
    const text = String(record.text_tokens || normalizeSearchText(record.summary || ""));
    let tier = -1;
    if (!normalized) tier = 4;
    else if (title === normalized || qualified === normalized) tier = 0;
    else if (title.startsWith(normalized) || qualified.startsWith(normalized) ||
             names.split(" ").some((token) => token.startsWith(normalized))) tier = 1;
    else if (tokens.every((token) => names.includes(token))) tier = 2;
    else if (tokens.every((token) => `${names} ${text}`.includes(token))) tier = 3;
    if (tier >= 0) scored.push({
      record, tier,
      domain: DOMAIN_PRIORITY[searchDomain(record)],
      exactSetting: searchType(record) === "Setting" && tier === 0 ? 0 : 1,
    });
  });
  scored.sort((left, right) => left.domain - right.domain ||
    left.exactSetting - right.exactSetting || left.tier - right.tier ||
    compareText(String(left.record.title || ""), String(right.record.title || "")) ||
    compareText(String(left.record.qualified_name || ""), String(right.record.qualified_name || "")) ||
    compareText(String(left.record.id || ""), String(right.record.id || "")) ||
    compareText(String(left.record.route || ""), String(right.record.route || "")));
  return scored.slice(0, limit).map((entry) => entry.record);
}
