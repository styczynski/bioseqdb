// Register after the local Prism bundle and before main.js highlights code.
(function (Prism) {
  if (!Prism) return;
  const sql = Prism.languages.sql;
  if (sql) {
    Prism.languages.insertBefore('sql', 'string', {
      'dollar-string': { pattern: /(\$(?:[a-zA-Z_]\w*)?\$)[\s\S]*?\1/, greedy: true, alias: 'string' },
      'quoted-identifier': { pattern: /"(?:""|[^"\r\n])*"/, greedy: true, alias: 'symbol' },
    });
    Prism.languages.insertBefore('sql', 'function', {
      'sequence-type': { pattern: /\bbioseq\.(?:seq|letter|column_mapping)\b/, alias: 'type' },
      'qualified-function': { pattern: /\b(?:bioseq(?:_cache|_external)?|public|pg_catalog)\.[a-z_]\w*(?=\s*\()/i, alias: 'function' },
      'setting': { pattern: /\bbioseq(?:_cache|_external)?(?:\.[a-z_]\w*){2,}/i, alias: 'property' },
      'parameter': { pattern: /\$\d+\b/, alias: 'variable' },
    });
  }
  Prism.languages['pgsql-plan'] = {
    'comment': /^\s*(?:QUERY PLAN|-{4,}|\(\d+ rows?\))\s*$/m,
    'plan-node': { pattern: /(^[ \t]*(?:->\s*)?)(?:Custom Scan\s*\([^)]+\)|(?:Parallel )?(?:Seq Scan|Index(?: Only)? Scan|Bitmap(?: Heap| Index|And|Or)? Scan|Nested Loop|Hash(?: Join|Aggregate)?|Merge Join|GroupAggregate|Aggregate|Sort|Incremental Sort|Gather(?: Merge)?|Limit|Result|Materialize|Memoize|Append|CTE Scan|Subquery Scan|Unique|WindowAgg))(?=\s|$)/m, lookbehind: true, alias: 'function' },
    'plan-property': { pattern: /(^[ \t|*+\-]*)(?:[A-Z][\w ()\/.*-]*)(?=:\s)/m, lookbehind: true, alias: 'property' },
    'plan-cost': { pattern: /\b(?:cost|actual time|rows|width|loops|memory|Memory|Disk|time|batches|shared hit|read|written|temp read)(?==)/, alias: 'attr-name' },
    'string': /'(?:''|[^'])*'/,
    'boolean': /\b(?:true|false|enabled|disabled|absent|present)\b/i,
    'number': /\b\d+(?:\.\d+)?(?:ms|ns|us|kB|MB|GB|%)?\b/,
    'plan-tree': { pattern: /->|[│├└─]+|^\s*[|*+]/m, alias: 'punctuation' },
    'operator': /[=<>]+|\.\./,
    'punctuation': /[()[\],:]/,
  };
  Prism.languages.explain = Prism.languages['pgsql-plan'];
  Prism.languages.plan = Prism.languages['pgsql-plan'];
})(window.Prism);
