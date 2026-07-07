create function demo_len(text) returns int8 language c as 'MODULE_PATHNAME';
create function demo_srf(int8) returns setof int8
    strict
    language c as
'MODULE_PATHNAME';
