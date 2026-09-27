"""JSON Schemas + validation for ALL MCP requests.

Doc tools use strict schemas (additionalProperties:false). Build tools are validated against
their declared MCP inputSchema. Degrades to a minimal validator if jsonschema is unavailable."""
from __future__ import annotations

from .registry import VALID_TYPES, VALID_ORIGINS

try:
    import jsonschema
    _HAVE = True
except Exception:
    _HAVE = False

DOC_CREATE = {"type": "object", "additionalProperties": False, "required": ["type", "title", "topics"],
              "properties": {
                  "type": {"type": "string", "enum": sorted(VALID_TYPES)},
                  "title": {"type": "string", "minLength": 1},
                  "topics": {"type": "array", "items": {"type": "string", "minLength": 1}, "minItems": 1},
                  "summary": {"type": "string", "maxLength": 400},
                  "origin": {"type": "string", "enum": sorted(VALID_ORIGINS)},
                  "supersedes": {"type": "array", "items": {"type": "string", "pattern": "^DOC\\d{4}$"}},
                  "allow_new_topic": {"type": "boolean"}}}
DOC_ARCHIVE = {"type": "object", "additionalProperties": False, "required": ["doc"],
               "properties": {"doc": {"type": "string", "minLength": 1}}}
DOC_SEARCH = {"type": "object", "additionalProperties": False, "required": ["query"],
              "properties": {"query": {"type": "string", "minLength": 1},
                             "limit": {"type": "integer", "minimum": 1, "maximum": 50},
                             "min_score": {"type": "number", "minimum": 0, "maximum": 1},
                             "include_archived": {"type": "boolean"},
                             "intent": {"type": "string"}}}
DOC_READ = {"type": "object", "additionalProperties": False, "required": ["doc"],
            "properties": {"doc": {"type": "string", "minLength": 1}}}
SCHEMAS = {"doc_create": DOC_CREATE, "doc_archive": DOC_ARCHIVE, "doc_search": DOC_SEARCH,
           "doc_read": DOC_READ}


class ValidationError(Exception):
    pass


def _validate(schema: dict, args: dict) -> None:
    if _HAVE:
        try:
            jsonschema.validate(args, schema)
        except jsonschema.ValidationError as e:  # type: ignore
            raise ValidationError(e.message) from e
        return
    for req in schema.get("required", []):
        if req not in args:
            raise ValidationError(f"missing required field '{req}'")
    if schema.get("additionalProperties") is False:
        for k in args:
            if k not in schema.get("properties", {}):
                raise ValidationError(f"unknown field '{k}'")


def validate(tool: str, args: dict) -> None:
    s = SCHEMAS.get(tool)
    if s is None:
        raise ValidationError(f"no schema for '{tool}'")
    _validate(s, args)


def validate_input_schema(tool: str, args: dict, input_schema: dict) -> None:
    """Validate a build tool's args against its declared MCP inputSchema."""
    _validate(input_schema, args)
