\set ON_ERROR_STOP on
\set QUIET 1
SET search_path = pg_catalog;
CREATE TEMPORARY TABLE doc0388_snapshot_session (
  marker boolean NOT NULL
);

CREATE OR REPLACE FUNCTION pg_temp.doc0388_extension_schema()
RETURNS text
LANGUAGE sql
STABLE
AS $function$
  SELECT n.nspname::text
    FROM pg_catalog.pg_extension AS e
    JOIN pg_catalog.pg_namespace AS n ON n.oid = e.extnamespace
   WHERE e.extname = 'bioseq'
$function$;

CREATE OR REPLACE FUNCTION pg_temp.doc0388_norm_schema(value text)
RETURNS text
LANGUAGE sql
STABLE
AS $function$
  SELECT CASE
           WHEN value = pg_temp.doc0388_extension_schema()
             THEN '$extension'
           ELSE value
         END
$function$;

CREATE OR REPLACE FUNCTION pg_temp.doc0388_norm_text(value text)
RETURNS text
LANGUAGE sql
STABLE
AS $function$
  SELECT CASE
           WHEN value IS NULL THEN NULL
           ELSE replace(
             replace(
               value,
               pg_catalog.quote_ident(
                 pg_temp.doc0388_extension_schema()) || '.',
               '$extension.'),
             pg_temp.doc0388_extension_schema() || '.',
             '$extension.')
         END
$function$;

CREATE OR REPLACE FUNCTION pg_temp.doc0388_norm_role(role_oid oid)
RETURNS text
LANGUAGE sql
STABLE
AS $function$
  SELECT CASE
           WHEN role_oid = 0 THEN '$public'
           WHEN role_oid = (
             SELECT oid FROM pg_catalog.pg_roles
              WHERE rolname = current_user)
             THEN '$owner'
           ELSE pg_catalog.pg_get_userbyid(role_oid)
         END
$function$;

CREATE TEMPORARY VIEW doc0388_extension_members AS
SELECT d.classid, d.objid, d.objsubid
  FROM pg_catalog.pg_depend AS d
  JOIN pg_catalog.pg_extension AS e
    ON d.refclassid = 'pg_catalog.pg_extension'::regclass
   AND d.refobjid = e.oid
 WHERE e.extname = 'bioseq'
   AND d.deptype = 'e';

COPY (
WITH
member_classes AS (
  SELECT c.*
    FROM pg_catalog.pg_class AS c
    JOIN pg_temp.doc0388_extension_members AS m
      ON m.classid = 'pg_catalog.pg_class'::regclass
     AND m.objid = c.oid
     AND m.objsubid = 0
),
member_routines AS (
  SELECT p.*
    FROM pg_catalog.pg_proc AS p
    JOIN pg_temp.doc0388_extension_members AS m
      ON m.classid = 'pg_catalog.pg_proc'::regclass
     AND m.objid = p.oid
     AND m.objsubid = 0
),
member_types AS (
  SELECT t.*
    FROM pg_catalog.pg_type AS t
    JOIN pg_temp.doc0388_extension_members AS m
      ON m.classid = 'pg_catalog.pg_type'::regclass
     AND m.objid = t.oid
     AND m.objsubid = 0
),
member_rules AS (
  SELECT r.*
    FROM pg_catalog.pg_rewrite AS r
    JOIN member_classes AS c ON c.oid = r.ev_class
),
rows AS (
  SELECT
    'pg_namespace'::text AS catalog,
    'schema:' || pg_temp.doc0388_norm_schema(n.nspname::text) AS identity,
    pg_catalog.jsonb_build_object(
      'owner', pg_temp.doc0388_norm_role(n.nspowner),
      'acl_is_null', n.nspacl IS NULL)::text AS value
  FROM pg_catalog.pg_namespace AS n
  WHERE n.oid IN (
    SELECT e.extnamespace
      FROM pg_catalog.pg_extension AS e
     WHERE e.extname = 'bioseq')
     OR n.nspname IN ('bioseq', 'bioseq_internal')

  UNION ALL

  SELECT
    'pg_class',
    'class:' ||
      pg_catalog.format(
        '%s.%I',
        pg_temp.doc0388_norm_schema(n.nspname::text),
        c.relname),
    pg_catalog.jsonb_build_object(
      'relkind', c.relkind,
      'persistence', c.relpersistence,
      'owner', pg_temp.doc0388_norm_role(c.relowner),
      'access_method', am.amname,
      'replica_identity', c.relreplident,
      'row_security', c.relrowsecurity,
      'force_row_security', c.relforcerowsecurity,
      'options', c.reloptions,
      'view_definition', CASE
        WHEN c.relkind IN ('v', 'm') THEN pg_temp.doc0388_norm_text(
          pg_catalog.pg_get_viewdef(c.oid, true))
        ELSE NULL
      END,
      'acl_is_null', c.relacl IS NULL)::text
  FROM member_classes AS c
  JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace
  LEFT JOIN pg_catalog.pg_am AS am ON am.oid = c.relam

  UNION ALL

  SELECT
    'pg_attribute',
    'attribute:' ||
      pg_catalog.format(
        '%s.%I.%I',
        pg_temp.doc0388_norm_schema(n.nspname::text),
        c.relname,
        a.attname),
    pg_catalog.jsonb_build_object(
      'ordinal', a.attnum,
      'type', pg_temp.doc0388_norm_text(
        pg_catalog.format_type(a.atttypid, a.atttypmod)),
      'not_null', a.attnotnull,
      'has_default', a.atthasdef,
      'default', pg_temp.doc0388_norm_text(
        pg_catalog.pg_get_expr(ad.adbin, ad.adrelid, true)),
      'identity', a.attidentity,
      'generated', a.attgenerated,
      'storage', a.attstorage,
      'compression', a.attcompression,
      'acl_is_null', a.attacl IS NULL,
      'collation', CASE
        WHEN a.attcollation = 0 THEN NULL
        ELSE pg_temp.doc0388_norm_text(a.attcollation::regcollation::text)
      END)::text
  FROM member_classes AS c
  JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace
  JOIN pg_catalog.pg_attribute AS a ON a.attrelid = c.oid
  LEFT JOIN pg_catalog.pg_attrdef AS ad
    ON ad.adrelid = a.attrelid AND ad.adnum = a.attnum
  WHERE a.attnum > 0
    AND NOT a.attisdropped

  UNION ALL

  SELECT
    'pg_constraint',
    'constraint:' ||
      pg_catalog.format(
        '%s.%I.%I',
        pg_temp.doc0388_norm_schema(n.nspname::text),
        c.relname,
        con.conname),
    pg_catalog.jsonb_build_object(
      'type', con.contype,
      'deferrable', con.condeferrable,
      'deferred', con.condeferred,
      'validated', con.convalidated,
      'definition', pg_temp.doc0388_norm_text(
        pg_catalog.pg_get_constraintdef(con.oid, true)))::text
  FROM pg_catalog.pg_constraint AS con
  JOIN member_classes AS c ON c.oid = con.conrelid
  JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace

  UNION ALL

  SELECT
    'pg_index',
    'index:' ||
      pg_catalog.format(
        '%s.%I',
        pg_temp.doc0388_norm_schema(n.nspname::text),
        idx.relname),
    pg_catalog.jsonb_build_object(
      'table', pg_temp.doc0388_norm_text(tbl.oid::regclass::text),
      'unique', i.indisunique,
      'primary', i.indisprimary,
      'exclusion', i.indisexclusion,
      'immediate', i.indimmediate,
      'valid', i.indisvalid,
      'ready', i.indisready,
      'live', i.indislive,
      'replica_identity', i.indisreplident,
      'definition', pg_temp.doc0388_norm_text(
        pg_catalog.pg_get_indexdef(i.indexrelid)))::text
  FROM pg_catalog.pg_index AS i
  JOIN member_classes AS idx ON idx.oid = i.indexrelid
  JOIN pg_catalog.pg_class AS tbl ON tbl.oid = i.indrelid
  JOIN pg_catalog.pg_namespace AS n ON n.oid = idx.relnamespace

  UNION ALL

  SELECT
    'pg_sequence',
    'sequence:' ||
      pg_catalog.format(
        '%s.%I',
        pg_temp.doc0388_norm_schema(n.nspname::text),
        c.relname),
    pg_catalog.jsonb_build_object(
      'type', pg_temp.doc0388_norm_text(
        pg_catalog.format_type(s.seqtypid, NULL)),
      'start', s.seqstart,
      'increment', s.seqincrement,
      'max', s.seqmax,
      'min', s.seqmin,
      'cache', s.seqcache,
      'cycle', s.seqcycle)::text
  FROM pg_catalog.pg_sequence AS s
  JOIN member_classes AS c ON c.oid = s.seqrelid
  JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace

  UNION ALL

  SELECT
    'pg_proc',
    CASE p.prokind
      WHEN 'p' THEN 'procedure:'
      WHEN 'a' THEN 'aggregate:'
      ELSE 'function:'
    END ||
      pg_catalog.format(
        '%s.%I(%s)',
        pg_temp.doc0388_norm_schema(n.nspname::text),
        p.proname,
        regexp_replace(
          pg_temp.doc0388_norm_text(
            pg_catalog.pg_get_function_identity_arguments(p.oid)),
          ',[[:space:]]+', ',', 'g')),
    pg_catalog.jsonb_build_object(
      'kind', p.prokind,
      'language', l.lanname,
      'owner', pg_temp.doc0388_norm_role(p.proowner),
      'result', pg_temp.doc0388_norm_text(
        pg_catalog.pg_get_function_result(p.oid)),
      'proargnames', p.proargnames,
      'proargmodes', p.proargmodes,
      'number_arguments', p.pronargs,
      'number_defaults', p.pronargdefaults,
      'argument_types', (
        SELECT pg_catalog.jsonb_agg(
          pg_temp.doc0388_norm_text(
            pg_catalog.format_type(argument_type, NULL))
          ORDER BY argument_ordinal)
          FROM pg_catalog.unnest(
            COALESCE(p.proallargtypes, p.proargtypes::oid[]))
            WITH ORDINALITY AS arguments(
              argument_type, argument_ordinal)),
      'default_expressions', pg_temp.doc0388_norm_text(
        pg_catalog.pg_get_expr(p.proargdefaults, 0, true)),
      'variadic_type', CASE
        WHEN p.provariadic = 0 THEN NULL
        ELSE pg_temp.doc0388_norm_text(
          pg_catalog.format_type(p.provariadic, NULL))
      END,
      'volatility', p.provolatile,
      'strict', p.proisstrict,
      'security_definer', p.prosecdef,
      'leakproof', p.proleakproof,
      'parallel', p.proparallel,
      'cost', p.procost,
      'rows', p.prorows,
      'support', CASE
        WHEN p.prosupport = 0 THEN NULL
        ELSE pg_temp.doc0388_norm_text(p.prosupport::regprocedure::text)
      END,
      'configuration', p.proconfig,
      'probin', p.probin,
      'prosrc', p.prosrc,
      'acl_is_null', p.proacl IS NULL)::text
  FROM member_routines AS p
  JOIN pg_catalog.pg_namespace AS n ON n.oid = p.pronamespace
  JOIN pg_catalog.pg_language AS l ON l.oid = p.prolang

  UNION ALL

  SELECT
    'pg_type',
    'type:' ||
      pg_catalog.format(
        '%s.%I',
        pg_temp.doc0388_norm_schema(n.nspname::text),
        t.typname),
    pg_catalog.jsonb_build_object(
      'owner', pg_temp.doc0388_norm_role(t.typowner),
      'kind', t.typtype,
      'category', t.typcategory,
      'preferred', t.typispreferred,
      'defined', t.typisdefined,
      'delimiter', t.typdelim,
      'length', t.typlen,
      'by_value', t.typbyval,
      'alignment', t.typalign,
      'storage', t.typstorage,
      'not_null', t.typnotnull,
      'input', pg_temp.doc0388_norm_text(t.typinput::regprocedure::text),
      'output', pg_temp.doc0388_norm_text(t.typoutput::regprocedure::text),
      'receive', CASE WHEN t.typreceive = 0 THEN NULL
        ELSE pg_temp.doc0388_norm_text(
          t.typreceive::regprocedure::text) END,
      'send', CASE WHEN t.typsend = 0 THEN NULL
        ELSE pg_temp.doc0388_norm_text(t.typsend::regprocedure::text) END,
      'typmod_in', CASE WHEN t.typmodin = 0 THEN NULL
        ELSE pg_temp.doc0388_norm_text(t.typmodin::regprocedure::text) END,
      'typmod_out', CASE WHEN t.typmodout = 0 THEN NULL
        ELSE pg_temp.doc0388_norm_text(t.typmodout::regprocedure::text) END,
      'analyze', CASE WHEN t.typanalyze = 0 THEN NULL
        ELSE pg_temp.doc0388_norm_text(t.typanalyze::regprocedure::text) END,
      'subscript', CASE WHEN t.typsubscript = 0 THEN NULL
        ELSE pg_temp.doc0388_norm_text(
          t.typsubscript::regprocedure::text) END,
      'element', CASE
        WHEN t.typelem = 0 THEN NULL
        ELSE pg_temp.doc0388_norm_text(
          pg_catalog.format_type(t.typelem, NULL))
      END,
      'array', CASE
        WHEN t.typarray = 0 THEN NULL
        ELSE pg_temp.doc0388_norm_text(
          pg_catalog.format_type(t.typarray, NULL))
      END,
      'base', CASE
        WHEN t.typbasetype = 0 THEN NULL
        ELSE pg_temp.doc0388_norm_text(
          pg_catalog.format_type(t.typbasetype, t.typtypmod))
      END,
      'collation', CASE
        WHEN t.typcollation = 0 THEN NULL
        ELSE pg_temp.doc0388_norm_text(t.typcollation::regcollation::text)
      END,
      'default', t.typdefault,
      'default_binary', pg_temp.doc0388_norm_text(
        pg_catalog.pg_get_expr(t.typdefaultbin, 0, true)),
      'acl_is_null', t.typacl IS NULL)::text
  FROM member_types AS t
  JOIN pg_catalog.pg_namespace AS n ON n.oid = t.typnamespace

  UNION ALL

  SELECT
    'pg_rewrite',
    'rule:' ||
      pg_temp.doc0388_norm_text(r.ev_class::regclass::text) || '.' ||
      r.rulename,
    pg_catalog.jsonb_build_object(
      'event', r.ev_type,
      'instead', r.is_instead,
      'definition', pg_temp.doc0388_norm_text(
        pg_catalog.pg_get_ruledef(r.oid, true)))::text
  FROM member_rules AS r

  UNION ALL

  SELECT
    'pg_cast',
    'cast:' ||
      pg_temp.doc0388_norm_text(
        pg_catalog.format_type(c.castsource, NULL)) || '->' ||
      pg_temp.doc0388_norm_text(
        pg_catalog.format_type(c.casttarget, NULL)),
    pg_catalog.jsonb_build_object(
      'context', c.castcontext,
      'method', c.castmethod,
      'function', CASE
        WHEN c.castfunc = 0 THEN NULL
        ELSE pg_temp.doc0388_norm_text(c.castfunc::regprocedure::text)
      END)::text
  FROM pg_catalog.pg_cast AS c
  JOIN pg_temp.doc0388_extension_members AS m
    ON m.classid = 'pg_catalog.pg_cast'::regclass
   AND m.objid = c.oid
   AND m.objsubid = 0

  UNION ALL

  SELECT
    'pg_operator',
    'operator:' ||
      pg_catalog.format(
        '%s.%I(%s,%s)',
        pg_temp.doc0388_norm_schema(n.nspname::text),
        o.oprname,
        CASE WHEN o.oprleft = 0 THEN 'NONE'
             ELSE pg_temp.doc0388_norm_text(
               pg_catalog.format_type(o.oprleft, NULL)) END,
        CASE WHEN o.oprright = 0 THEN 'NONE'
             ELSE pg_temp.doc0388_norm_text(
               pg_catalog.format_type(o.oprright, NULL)) END),
    pg_catalog.jsonb_build_object(
      'kind', o.oprkind,
      'can_merge', o.oprcanmerge,
      'can_hash', o.oprcanhash,
      'result', pg_temp.doc0388_norm_text(
        pg_catalog.format_type(o.oprresult, NULL)),
      'function', pg_temp.doc0388_norm_text(o.oprcode::regprocedure::text),
      'commutator', CASE
        WHEN o.oprcom = 0 THEN NULL
        ELSE pg_temp.doc0388_norm_text(o.oprcom::regoperator::text)
      END,
      'negator', CASE
        WHEN o.oprnegate = 0 THEN NULL
        ELSE pg_temp.doc0388_norm_text(o.oprnegate::regoperator::text)
      END,
      'restriction', CASE
        WHEN o.oprrest = 0 THEN NULL
        ELSE o.oprrest::regprocedure::text
      END,
      'join', CASE
        WHEN o.oprjoin = 0 THEN NULL
        ELSE o.oprjoin::regprocedure::text
      END)::text
  FROM pg_catalog.pg_operator AS o
  JOIN pg_catalog.pg_namespace AS n ON n.oid = o.oprnamespace
  JOIN pg_temp.doc0388_extension_members AS m
    ON m.classid = 'pg_catalog.pg_operator'::regclass
   AND m.objid = o.oid
   AND m.objsubid = 0

  UNION ALL

  SELECT
    'pg_opclass',
    'opclass:' ||
      pg_catalog.format(
        '%s.%I/%I',
        pg_temp.doc0388_norm_schema(n.nspname::text),
        oc.opcname,
        am.amname),
    pg_catalog.jsonb_build_object(
      'family', pg_temp.doc0388_norm_text(
        pg_catalog.format('%I.%I', fn.nspname, f.opfname)),
      'input_type', pg_temp.doc0388_norm_text(
        pg_catalog.format_type(oc.opcintype, NULL)),
      'default', oc.opcdefault,
      'key_type', CASE
        WHEN oc.opckeytype = 0 THEN NULL
        ELSE pg_temp.doc0388_norm_text(
          pg_catalog.format_type(oc.opckeytype, NULL))
      END,
      'operators', (
        SELECT pg_catalog.jsonb_agg(
          pg_catalog.jsonb_build_object(
            'strategy', ao.amopstrategy,
            'purpose', ao.amoppurpose,
            'left', pg_temp.doc0388_norm_text(
              pg_catalog.format_type(ao.amoplefttype, NULL)),
            'right', pg_temp.doc0388_norm_text(
              pg_catalog.format_type(ao.amoprighttype, NULL)),
            'operator', pg_temp.doc0388_norm_text(
              ao.amopopr::regoperator::text),
            'sort_family', CASE
              WHEN ao.amopsortfamily = 0 THEN NULL
              ELSE (
                SELECT pg_temp.doc0388_norm_text(
                  pg_catalog.format('%I.%I', sort_ns.nspname, sort_f.opfname))
                  FROM pg_catalog.pg_opfamily AS sort_f
                  JOIN pg_catalog.pg_namespace AS sort_ns
                    ON sort_ns.oid = sort_f.opfnamespace
                 WHERE sort_f.oid = ao.amopsortfamily)
            END)
          ORDER BY ao.amopstrategy, ao.amoppurpose,
                   ao.amoplefttype, ao.amoprighttype)
        FROM pg_catalog.pg_amop AS ao
        WHERE ao.amopfamily = oc.opcfamily),
      'functions', (
        SELECT pg_catalog.jsonb_agg(
          pg_catalog.jsonb_build_object(
            'number', ap.amprocnum,
            'left', pg_temp.doc0388_norm_text(
              pg_catalog.format_type(ap.amproclefttype, NULL)),
            'right', pg_temp.doc0388_norm_text(
              pg_catalog.format_type(ap.amprocrighttype, NULL)),
            'function', pg_temp.doc0388_norm_text(
              ap.amproc::regprocedure::text))
          ORDER BY ap.amprocnum, ap.amproclefttype, ap.amprocrighttype)
        FROM pg_catalog.pg_amproc AS ap
        WHERE ap.amprocfamily = oc.opcfamily))::text
  FROM pg_catalog.pg_opclass AS oc
  JOIN pg_catalog.pg_namespace AS n ON n.oid = oc.opcnamespace
  JOIN pg_catalog.pg_am AS am ON am.oid = oc.opcmethod
  JOIN pg_catalog.pg_opfamily AS f ON f.oid = oc.opcfamily
  JOIN pg_catalog.pg_namespace AS fn ON fn.oid = f.opfnamespace
  JOIN pg_temp.doc0388_extension_members AS m
    ON m.classid = 'pg_catalog.pg_opclass'::regclass
   AND m.objid = oc.oid
   AND m.objsubid = 0

  UNION ALL

  SELECT
    'pg_am',
    'access_method:' || am.amname,
    pg_catalog.jsonb_build_object(
      'type', am.amtype,
      'handler', pg_temp.doc0388_norm_text(
        am.amhandler::regprocedure::text))::text
  FROM pg_catalog.pg_am AS am
  JOIN pg_temp.doc0388_extension_members AS m
    ON m.classid = 'pg_catalog.pg_am'::regclass
   AND m.objid = am.oid
   AND m.objsubid = 0

  UNION ALL

  SELECT
    'pg_aggregate',
    'aggregate:' ||
      pg_temp.doc0388_norm_text(a.aggfnoid::regprocedure::text),
    pg_catalog.jsonb_build_object(
      'kind', a.aggkind,
      'arguments', a.aggnumdirectargs,
      'transition', pg_temp.doc0388_norm_text(
        a.aggtransfn::regprocedure::text),
      'final', CASE WHEN a.aggfinalfn = 0 THEN NULL
        ELSE pg_temp.doc0388_norm_text(a.aggfinalfn::regprocedure::text) END,
      'combine', CASE WHEN a.aggcombinefn = 0 THEN NULL
        ELSE pg_temp.doc0388_norm_text(a.aggcombinefn::regprocedure::text) END,
      'serial', CASE WHEN a.aggserialfn = 0 THEN NULL
        ELSE pg_temp.doc0388_norm_text(a.aggserialfn::regprocedure::text) END,
      'deserial', CASE WHEN a.aggdeserialfn = 0 THEN NULL
        ELSE pg_temp.doc0388_norm_text(a.aggdeserialfn::regprocedure::text) END,
      'moving_transition', CASE WHEN a.aggmtransfn = 0 THEN NULL
        ELSE pg_temp.doc0388_norm_text(a.aggmtransfn::regprocedure::text) END,
      'moving_inverse', CASE WHEN a.aggminvtransfn = 0 THEN NULL
        ELSE pg_temp.doc0388_norm_text(
          a.aggminvtransfn::regprocedure::text) END,
      'moving_final', CASE WHEN a.aggmfinalfn = 0 THEN NULL
        ELSE pg_temp.doc0388_norm_text(
          a.aggmfinalfn::regprocedure::text) END,
      'transition_type', pg_temp.doc0388_norm_text(
        pg_catalog.format_type(a.aggtranstype, NULL)),
      'transition_space', a.aggtransspace,
      'initial_condition', a.agginitval,
      'moving_transition_type', CASE WHEN a.aggmtranstype = 0 THEN NULL
        ELSE pg_temp.doc0388_norm_text(
          pg_catalog.format_type(a.aggmtranstype, NULL)) END,
      'moving_transition_space', a.aggmtransspace,
      'moving_initial_condition', a.aggminitval,
      'final_extra', a.aggfinalextra,
      'moving_final_extra', a.aggmfinalextra,
      'final_modify', a.aggfinalmodify,
      'moving_final_modify', a.aggmfinalmodify,
      'sort_operator', CASE WHEN a.aggsortop = 0 THEN NULL
        ELSE pg_temp.doc0388_norm_text(a.aggsortop::regoperator::text) END)::text
  FROM pg_catalog.pg_aggregate AS a
  JOIN member_routines AS p ON p.oid = a.aggfnoid

  UNION ALL

  SELECT
    'pg_extension',
    'extension:bioseq',
    pg_catalog.jsonb_build_object(
      'name', e.extname,
      'owner', pg_temp.doc0388_norm_role(e.extowner),
      'namespace', pg_temp.doc0388_norm_schema(n.nspname::text),
      'relocatable', e.extrelocatable,
      'version', e.extversion,
      'config', (
        SELECT pg_catalog.jsonb_agg(
          pg_temp.doc0388_norm_text(config_oid::regclass::text)
          ORDER BY config_ordinal)
          FROM pg_catalog.unnest(e.extconfig)
            WITH ORDINALITY AS configs(config_oid, config_ordinal)),
      'conditions', e.extcondition)::text
  FROM pg_catalog.pg_extension AS e
  JOIN pg_catalog.pg_namespace AS n ON n.oid = e.extnamespace
  WHERE e.extname = 'bioseq'

  UNION ALL

  SELECT
    'pg_depend',
    'dependency:' ||
      pg_temp.doc0388_norm_text(object_identity.identity) || '->' ||
    pg_temp.doc0388_norm_text(reference_identity.identity),
    pg_catalog.jsonb_build_object(
      'object_type', object_identity.type,
      'object_identity',
        pg_temp.doc0388_norm_text(object_identity.identity),
      'reference_type', reference_identity.type,
      'reference_identity',
        pg_temp.doc0388_norm_text(reference_identity.identity),
      'dependency_type', d.deptype)::text
  FROM pg_catalog.pg_depend AS d
  CROSS JOIN LATERAL pg_catalog.pg_identify_object(
    d.classid, d.objid, d.objsubid) AS object_identity
  CROSS JOIN LATERAL pg_catalog.pg_identify_object(
    d.refclassid, d.refobjid, d.refobjsubid) AS reference_identity
  WHERE EXISTS (
    SELECT 1
      FROM pg_temp.doc0388_extension_members AS m
     WHERE (m.classid, m.objid, m.objsubid) =
           (d.classid, d.objid, d.objsubid))
     OR EXISTS (
    SELECT 1
      FROM pg_temp.doc0388_extension_members AS m
     WHERE (m.classid, m.objid, m.objsubid) =
           (d.refclassid, d.refobjid, d.refobjsubid))

  UNION ALL

  SELECT
    'pg_description',
    'comment:' || pg_temp.doc0388_norm_text(object_identity.identity),
    pg_catalog.jsonb_build_object(
      'object_type', object_identity.type,
      'description', des.description)::text
  FROM pg_catalog.pg_description AS des
  CROSS JOIN LATERAL pg_catalog.pg_identify_object(
    des.classoid, des.objoid, des.objsubid) AS object_identity
  WHERE EXISTS (
    SELECT 1
      FROM pg_temp.doc0388_extension_members AS m
     WHERE m.classid = des.classoid
       AND m.objid = des.objoid
       AND (m.objsubid = des.objsubid OR m.objsubid = 0))

  UNION ALL

  SELECT
    'pg_shdescription',
    'shared-comment:' ||
      pg_temp.doc0388_norm_text(
        pg_catalog.pg_describe_object(
          des.classoid, des.objoid, 0)),
    pg_catalog.jsonb_build_object(
      'description', des.description)::text
  FROM pg_catalog.pg_shdescription AS des
  WHERE pg_catalog.pg_describe_object(
          des.classoid, des.objoid, 0) ~ '(^|[. ])bioseq([. (]|$)'

  UNION ALL

  SELECT
    'acl',
    'schema:' || pg_temp.doc0388_norm_schema(n.nspname::text) || ':' ||
      pg_temp.doc0388_norm_role(acl.grantee) || ':' ||
      acl.privilege_type,
    pg_catalog.jsonb_build_object(
      'grantor', pg_temp.doc0388_norm_role(acl.grantor),
      'grantable', acl.is_grantable)::text
  FROM pg_catalog.pg_namespace AS n
  CROSS JOIN LATERAL pg_catalog.aclexplode(
    COALESCE(n.nspacl, pg_catalog.acldefault('n', n.nspowner))) AS acl
  WHERE n.oid IN (
    SELECT e.extnamespace FROM pg_catalog.pg_extension AS e
     WHERE e.extname = 'bioseq')
     OR n.nspname IN ('bioseq', 'bioseq_internal')

  UNION ALL

  SELECT
    'acl',
    'class:' || pg_temp.doc0388_norm_text(c.oid::regclass::text) || ':' ||
      pg_temp.doc0388_norm_role(acl.grantee) || ':' ||
      acl.privilege_type,
    pg_catalog.jsonb_build_object(
      'grantor', pg_temp.doc0388_norm_role(acl.grantor),
      'grantable', acl.is_grantable)::text
  FROM member_classes AS c
  CROSS JOIN LATERAL pg_catalog.aclexplode(
    COALESCE(
      c.relacl,
      pg_catalog.acldefault(
        CASE WHEN c.relkind = 'S' THEN 'S'::"char" ELSE 'r'::"char" END,
        c.relowner))) AS acl
  WHERE c.relkind IN ('r', 'p', 'v', 'm', 'f', 'S')

  UNION ALL

  SELECT
    'acl',
    'column:' ||
      pg_temp.doc0388_norm_text(c.oid::regclass::text) || '.' ||
      pg_catalog.quote_ident(a.attname) || ':' ||
      pg_temp.doc0388_norm_role(acl.grantee) || ':' ||
      acl.privilege_type,
    pg_catalog.jsonb_build_object(
      'grantor', pg_temp.doc0388_norm_role(acl.grantor),
      'grantable', acl.is_grantable)::text
  FROM member_classes AS c
  JOIN pg_catalog.pg_attribute AS a
    ON a.attrelid = c.oid
   AND a.attnum > 0
   AND NOT a.attisdropped
  CROSS JOIN LATERAL pg_catalog.aclexplode(a.attacl) AS acl
  WHERE a.attacl IS NOT NULL

  UNION ALL

  SELECT
    'acl',
    'routine:' || pg_temp.doc0388_norm_text(p.oid::regprocedure::text) || ':' ||
      pg_temp.doc0388_norm_role(acl.grantee) || ':' ||
      acl.privilege_type,
    pg_catalog.jsonb_build_object(
      'grantor', pg_temp.doc0388_norm_role(acl.grantor),
      'grantable', acl.is_grantable)::text
  FROM member_routines AS p
  CROSS JOIN LATERAL pg_catalog.aclexplode(
    COALESCE(p.proacl, pg_catalog.acldefault('f', p.proowner))) AS acl

  UNION ALL

  SELECT
    'acl',
    'type:' || pg_temp.doc0388_norm_text(t.oid::regtype::text) || ':' ||
      pg_temp.doc0388_norm_role(acl.grantee) || ':' ||
      acl.privilege_type,
    pg_catalog.jsonb_build_object(
      'grantor', pg_temp.doc0388_norm_role(acl.grantor),
      'grantable', acl.is_grantable)::text
  FROM member_types AS t
  CROSS JOIN LATERAL pg_catalog.aclexplode(
    COALESCE(t.typacl, pg_catalog.acldefault('T', t.typowner))) AS acl

  UNION ALL

  SELECT
    'effective_privilege',
    'schema:' || pg_temp.doc0388_norm_schema(n.nspname::text) || ':' ||
      privilege.name,
    pg_catalog.jsonb_build_object(
      'probe_role', 'doc0388_catalog_probe',
      'granted', pg_catalog.has_schema_privilege(
        'doc0388_catalog_probe', n.oid, privilege.name))::text
  FROM pg_catalog.pg_namespace AS n
  CROSS JOIN (VALUES ('USAGE'), ('CREATE')) AS privilege(name)
  WHERE n.oid IN (
    SELECT e.extnamespace FROM pg_catalog.pg_extension AS e
     WHERE e.extname = 'bioseq')
     OR n.nspname IN ('bioseq', 'bioseq_internal')

  UNION ALL

  SELECT
    'effective_privilege',
    CASE WHEN c.relkind = 'S' THEN 'sequence:' ELSE 'table:' END ||
      pg_temp.doc0388_norm_text(c.oid::regclass::text) || ':' ||
      privilege.name,
    pg_catalog.jsonb_build_object(
      'probe_role', 'doc0388_catalog_probe',
      'granted', CASE
        WHEN c.relkind = 'S' THEN pg_catalog.has_sequence_privilege(
          'doc0388_catalog_probe', c.oid, privilege.name)
        ELSE pg_catalog.has_table_privilege(
          'doc0388_catalog_probe', c.oid, privilege.name)
      END)::text
  FROM member_classes AS c
  CROSS JOIN LATERAL (
    SELECT name
      FROM (VALUES
        ('SELECT'), ('INSERT'), ('UPDATE'), ('DELETE'),
        ('TRUNCATE'), ('REFERENCES'), ('TRIGGER')) AS table_privilege(name)
     WHERE c.relkind <> 'S'
    UNION ALL
    SELECT name
      FROM (VALUES ('USAGE'), ('SELECT'), ('UPDATE')) AS sequence_privilege(name)
     WHERE c.relkind = 'S'
  ) AS privilege
  WHERE c.relkind IN ('r', 'p', 'v', 'm', 'f', 'S')

  UNION ALL

  SELECT
    'effective_privilege',
    'column:' ||
      pg_temp.doc0388_norm_text(c.oid::regclass::text) || '.' ||
      pg_catalog.quote_ident(a.attname) || ':' ||
      privilege.name,
    pg_catalog.jsonb_build_object(
      'probe_role', 'doc0388_catalog_probe',
      'granted', pg_catalog.has_column_privilege(
        'doc0388_catalog_probe', c.oid, a.attnum, privilege.name))::text
  FROM member_classes AS c
  JOIN pg_catalog.pg_attribute AS a
    ON a.attrelid = c.oid
   AND a.attnum > 0
   AND NOT a.attisdropped
  CROSS JOIN (VALUES
    ('SELECT'), ('INSERT'), ('UPDATE'), ('REFERENCES')) AS privilege(name)
  WHERE c.relkind IN ('r', 'p', 'v', 'm', 'f')

  UNION ALL

  SELECT
    'effective_privilege',
    'routine:' || pg_temp.doc0388_norm_text(p.oid::regprocedure::text) ||
      ':EXECUTE',
    pg_catalog.jsonb_build_object(
      'probe_role', 'doc0388_catalog_probe',
      'granted', pg_catalog.has_function_privilege(
        'doc0388_catalog_probe', p.oid, 'EXECUTE'))::text
  FROM member_routines AS p

  UNION ALL

  SELECT
    'effective_privilege',
    'type:' || pg_temp.doc0388_norm_text(t.oid::regtype::text) || ':USAGE',
    pg_catalog.jsonb_build_object(
      'probe_role', 'doc0388_catalog_probe',
      'granted', pg_catalog.has_type_privilege(
        'doc0388_catalog_probe', t.oid, 'USAGE'))::text
  FROM member_types AS t

  UNION ALL

  SELECT
    'extension_membership',
    pg_temp.doc0388_norm_text(object_identity.identity),
    pg_catalog.jsonb_build_object(
      'object_type', object_identity.type,
      'dependency_type', 'e')::text
  FROM pg_temp.doc0388_extension_members AS m
  CROSS JOIN LATERAL pg_catalog.pg_identify_object(
    m.classid, m.objid, m.objsubid) AS object_identity

  UNION ALL

  SELECT
    'guc',
    'guc:' || s.name,
    pg_catalog.jsonb_build_object(
      'setting', s.setting,
      'unit', s.unit,
      'category', s.category,
      'short_description', s.short_desc,
      'extra_description', s.extra_desc,
      'context', s.context,
      'type', s.vartype,
      'source', s.source,
      'minimum', s.min_val,
      'maximum', s.max_val,
      'enum_values', s.enumvals,
      'boot_value', s.boot_val,
      'reset_value', s.reset_val,
      'pending_restart', s.pending_restart)::text
  FROM pg_catalog.pg_settings AS s
  WHERE s.name LIKE 'bioseq.%'

  UNION ALL

  SELECT
    'snapshot_scope',
    'catalog:' || scope.catalog,
    pg_catalog.jsonb_build_object(
      'queried', true,
      'fields', scope.fields)::text
  FROM (VALUES
    ('pg_namespace', ARRAY[
      'owner', 'acl_is_null']),
    ('pg_class', ARRAY[
      'relkind', 'persistence', 'owner', 'access_method',
      'replica_identity', 'row_security', 'force_row_security',
      'options', 'view_definition', 'acl_is_null']),
    ('pg_attribute', ARRAY[
      'ordinal', 'type', 'not_null', 'has_default', 'default',
      'identity', 'generated', 'storage', 'compression',
      'acl_is_null', 'collation']),
    ('pg_constraint', ARRAY[
      'type', 'deferrable', 'deferred', 'validated', 'definition']),
    ('pg_index', ARRAY[
      'table', 'unique', 'primary', 'exclusion', 'immediate',
      'valid', 'ready', 'live', 'replica_identity', 'definition']),
    ('pg_sequence', ARRAY[
      'type', 'start', 'increment', 'max', 'min', 'cache', 'cycle']),
    ('pg_proc', ARRAY[
      'kind', 'language', 'owner', 'result', 'proargnames',
      'proargmodes', 'number_arguments', 'number_defaults',
      'argument_types', 'default_expressions', 'variadic_type',
      'volatility', 'strict', 'security_definer', 'leakproof',
      'parallel', 'cost', 'rows', 'support', 'configuration',
      'probin', 'prosrc', 'acl_is_null']),
    ('pg_type', ARRAY[
      'owner', 'kind', 'category', 'preferred', 'defined',
      'delimiter', 'length', 'by_value', 'alignment', 'storage',
      'not_null', 'input', 'output', 'receive', 'send', 'typmod_in',
      'typmod_out', 'analyze', 'subscript', 'element', 'array',
      'base', 'collation', 'default', 'default_binary', 'acl_is_null']),
    ('pg_rewrite', ARRAY['event', 'instead', 'definition']),
    ('pg_cast', ARRAY['context', 'method', 'function']),
    ('pg_operator', ARRAY[
      'kind', 'can_merge', 'can_hash', 'result', 'function',
      'commutator', 'negator', 'restriction', 'join']),
    ('pg_opclass', ARRAY[
      'family', 'input_type', 'default', 'key_type',
      'operators', 'functions']),
    ('pg_am', ARRAY['type', 'handler']),
    ('pg_aggregate', ARRAY[
      'kind', 'arguments', 'transition', 'final', 'combine', 'serial',
      'deserial', 'moving_transition', 'moving_inverse',
      'moving_final', 'transition_type', 'transition_space',
      'initial_condition', 'moving_transition_type',
      'moving_transition_space', 'moving_initial_condition',
      'final_extra', 'moving_final_extra', 'final_modify',
      'moving_final_modify', 'sort_operator']),
    ('pg_extension', ARRAY[
      'name', 'owner', 'namespace', 'relocatable', 'version',
      'config', 'conditions']),
    ('pg_depend', ARRAY[
      'object_type', 'object_identity', 'reference_type',
      'reference_identity', 'dependency_type']),
    ('pg_description', ARRAY['object_type', 'description']),
    ('pg_shdescription', ARRAY['description']),
    ('acl', ARRAY['grantor', 'grantable']),
    ('effective_privilege', ARRAY['probe_role', 'granted']),
    ('extension_membership', ARRAY[
      'object_type', 'dependency_type']),
    ('guc', ARRAY[
      'setting', 'unit', 'category', 'short_description',
      'extra_description', 'context', 'type', 'source', 'minimum',
      'maximum', 'enum_values', 'boot_value', 'reset_value',
      'pending_restart'])
  ) AS scope(catalog, fields)
),
numbered AS (
  SELECT
    catalog,
    identity,
    row_number() OVER (
      PARTITION BY catalog, identity
      ORDER BY value)::integer AS ordinal,
    value
  FROM rows
)
SELECT catalog, identity, ordinal, value
  FROM numbered
 ORDER BY catalog, identity, ordinal
) TO STDOUT WITH (FORMAT csv, HEADER true, ENCODING 'UTF8');
