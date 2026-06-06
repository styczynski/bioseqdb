"""This module manages parsers and writer for the headers of FASTA records.

This module provides classes for parsing the headers of FASTA records into a structured
format and writing the structured format back to a header string. The default FASTA
header parsers and writers are registered in a global registry, which can be accessed
via the `get_parser` and `get_writer` functions and the name of the parser or writer.
New parsers and writers must be registered via the `register_parser` and
`register_writer` functions before they become available in the other modules.

Classes:
    AbstractParsedHeader (Protocol): Interface for representing a parsed FASTA header.
    AbstractHeaderParser (Protocol): Interface for a FASTA header parser.
    AbstractHeaderWriter (Protocol): Interface for a FASTA header writer.
    ParsedHeader: Representation of a parsed FASTA header.
    DefaultParser: Default FASTA header parser.
    UniprotParser: Parser for Uniprot FASTA headers.
    UniprotLikeParser: Parser for less strict Uniprot like FASTA headers.
    DefaultWriter: Default FASTA header writer.
    UniprotWriter: Writer for Uniprot FASTA headers.
    UniprotLikeWriter: Writer for less strict Uniprot like FASTA headers.

Functions:
    register_parser: Register a custom FASTA header parser by name.
    get_parser: Get a registered FASTA header parser by name.
    replace_parser: Replace a previously registered non-built-in parser.
    list_parsers: Return the names of all currently registered parsers.
    register_writer: Register a custom FASTA header writer by name.
    get_writer: Get a registered FASTA header writer by name.
    replace_writer: Replace a previously registered non-built-in writer.
    list_writers: Return the names of all currently registered writers.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from typing import Protocol


class AbstractParsedHeader(Protocol):
    """Abstract parsed FASTA header.

    Attributes:
        identifier: The unique identifier of the FASTA entry.
        header: The FASTA header, not containing the starting ">" character.
        header_fields: The parsed header fields as a dictionary.
    """

    identifier: str
    header: str
    header_fields: dict[str, str]


class AbstractHeaderParser(Protocol):
    """Abstract header parser."""

    @classmethod
    def parse(cls, header: str) -> AbstractParsedHeader:
        """Parse a FASTA header string into a ParsedHeader object.

        Raises:
            ValueError: If the header could not be parsed.
        """
        ...


class AbstractHeaderWriter(Protocol):
    """Abstract header writer."""

    @classmethod
    def write(cls, parsed_header: AbstractParsedHeader) -> str:
        """Write a FASTA header string from a ParsedHeader object."""
        ...


@dataclass
class ParsedHeader:
    """Parsed FASTA header.

    Attributes:
        identifier: The unique identifier of the FASTA entry.
        header: The FASTA header, not containing the starting ">" character.
        header_fields: The parsed header fields as a dictionary.
    """

    identifier: str
    header: str
    header_fields: dict[str, str] = field(default_factory=dict)


class DefaultParser:
    """Default FASTA header parser.

    The `parse` method returns a ParsedHeader object with the identifier being the
    first whitespace-separated word of the header. The rest of the header is stored
    in the "description" field of the `header_fields` dictionary. If there is no
    description, the "description" key is absent from `header_fields`. This parser
    is guaranteed to work for any FASTA header string and never fail.
    """

    @classmethod
    def parse(cls, header: str) -> ParsedHeader:
        """Parse a FASTA header string into a ParsedHeader object."""
        split_header = header.split(maxsplit=1)
        _id = split_header[0]
        fields = {"description": split_header[1]} if len(split_header) > 1 else {}
        return ParsedHeader(_id, header, fields)


class DefaultWriter:
    """Default FASTA header writer.

    Writes the original `header` string from the parsed header directly, without
    inspecting or requiring any `header_fields`. Compatible with any parsed header.
    """

    @classmethod
    def write(cls, parsed_header: AbstractParsedHeader) -> str:
        """Write a FASTA header string from a ParsedHeader object."""
        return parsed_header.header


class DecoyWriter:
    """A FASTA header writer for decoy entries.

    Prepends a decoy tag to the original `header` string directly, without inspecting
    or requiring any `header_fields`. Compatible with any parsed header.

    This class can be used directly with the default "rev_" tag, or specialized for
    custom tags using the `with_tag` factory method.

    Example:
        # Default usage ("rev_")
        profasta.parser.DecoyWriter.write(parsed_header)

        # Custom usage and registration of the custom writer in the writer registry
        CustomDecoy = profasta.parser.DecoyWriter.with_tag("decoy_")
        profasta.parser.register_writer("decoy", CustomDecoy)
    """

    decoy_tag: str = "rev_"

    @classmethod
    def write(cls, parsed_header: AbstractParsedHeader) -> str:
        """Write a FASTA header string from a ParsedHeader object."""
        return f"{cls.decoy_tag}{parsed_header.header}"

    @classmethod
    def with_tag(cls, decoy_tag: str = "rev_"):
        """Create a specialized DecoyWriter class with a custom tag."""
        new_class_name = f"{cls.__name__}_{decoy_tag.rstrip('_')}"
        return type(new_class_name, (cls,), {"decoy_tag": decoy_tag})


class UniprotParser:
    """Parser for UniProt FASTA headers.

    Parses the standard UniProt header format and stores all fields as strings:
    `db|identifier|entry_name protein_name OS=... OX=... GN=... PE=... SV=...`

    The following `header_fields` keys are always present after parsing:
        - `db`: Source database (e.g. "sp" or "tr").
        - `identifier`: UniProt accession number (e.g. "P12345").
        - `entry_name`: UniProt entry name (e.g. "INS_HUMAN").
        - `protein_name`: Full protein name.

    The following `header_fields` keys are optional (only present if in the header):
        - `organism_name`: Organism name (OS field).
        - `organism_identifier`: NCBI taxonomy ID (OX field).
        - `gene_name`: Gene name (GN field).
        - `protein_existence`: Protein existence level (PE field).
        - `sequence_version`: Sequence version (SV field).
    """

    header_pattern = re.compile(
        r"^(?P<db>\w+)\|(?P<id>[-\w]+)\|(?P<entry>\w+)\s+(?P<name>.*?)"
        r"(?:(\s+OS=(?P<OS>[^=]+))|"
        r"(\s+OX=(?P<OX>\d+))|"
        r"(\s+GN=(?P<GN>\S+))|"
        r"(\s+PE=(?P<PE>\d))|"
        r"(\s+SV=(?P<SV>\d+)))*\s*$"
    )

    field_names = {
        "db": "db",
        "id": "identifier",
        "entry": "entry_name",
        "name": "protein_name",
        "OS": "organism_name",
        "OX": "organism_identifier",
        "GN": "gene_name",
        "PE": "protein_existence",
        "SV": "sequence_version",
    }

    @classmethod
    def parse(cls, header: str) -> ParsedHeader:
        """Parse a FASTA header string into a ParsedHeader object.

        Raises:
            ValueError: If the header could not be parsed.
        """
        match = cls.header_pattern.match(header)
        if match is None:
            raise ValueError(f"Header does not match the UniProt pattern: {header}")
        fields = match.groupdict()

        for key in ["OS", "OX", "GN", "PE", "SV"]:
            if fields[key] is None:
                del fields[key]
        fields = {cls.field_names[key]: value for key, value in fields.items()}

        return ParsedHeader(fields["identifier"], header, fields)


class UniprotWriter:
    """Writer for UniProt FASTA headers.

    Reconstructs a standard UniProt header string from `header_fields`.

    Required `header_fields` keys (see `UniprotWriter.required_fields`):
        - `db`: Source database.
        - `identifier`: UniProt accession number.
        - `entry_name`: UniProt entry name.
        - `protein_name`: Full protein name.

    Optional `header_fields` keys (included in the header if present):
        - `organism_name`: Written as `OS=...`.
        - `organism_identifier`: Written as `OX=...`.
        - `gene_name`: Written as `GN=...`.
        - `protein_existence`: Written as `PE=...`.
        - `sequence_version`: Written as `SV=...`.
    """

    required_fields: frozenset[str] = frozenset(
        {"db", "identifier", "entry_name", "protein_name"}
    )

    field_names = {
        "db": "db",
        "id": "identifier",
        "entry": "entry_name",
        "name": "protein_name",
        "OS": "organism_name",
        "OX": "organism_identifier",
        "GN": "gene_name",
        "PE": "protein_existence",
        "SV": "sequence_version",
    }

    @classmethod
    def write(cls, parsed_header: AbstractParsedHeader) -> str:
        """Write a FASTA header string from a ParsedHeader object.

        Raises:
            ValueError: If any required fields are missing from `header_fields`.
                See `UniprotWriter.required_fields` for the list of required fields.
        """
        fields = parsed_header.header_fields
        missing = cls.required_fields - fields.keys()
        if missing:
            raise ValueError(
                f"UniprotWriter is missing the following required header fields: "
                f"{sorted(missing)}. See UniprotWriter.required_fields."
            )
        header_entries = [
            f"{fields['db']}|{fields['identifier']}|{fields['entry_name']}",
            f"{fields['protein_name']}",
        ]
        for key in ["OS", "OX", "GN", "PE", "SV"]:
            field_name = cls.field_names[key]
            if field_name not in fields:
                continue
            header_entries.append(f"{key}={fields[field_name]}")
        return " ".join(header_entries)


class UniprotLikeParser:
    """A tolerant parser for UniProt-like FASTA headers.

    Parses headers that follow a relaxed UniProt-like format and stores all fields as
    strings. The first whitespace-separated token must be `db|identifier|entry_name`;
    the remainder of the description is parsed on a best-effort basis.

    The following `header_fields` keys are always present after parsing:
        - `db`: Source database identifier.
        - `identifier`: Accession or unique identifier.
        - `entry_name`: Entry name.

    The following `header_fields` keys are optional (only present if in the header):
        - `protein_name`: Protein description (text before the first tag field).
        - `organism_name`: Organism name (OS field).
        - `organism_identifier`: NCBI taxonomy ID (OX field).
        - `gene_name`: Gene name (GN field).
        - `protein_existence`: Protein existence level (PE field).
        - `sequence_version`: Sequence version (SV field).
    """

    field_pattern = re.compile(
        r"(?:(\s+OS=(?P<OS>[^=]+))|"
        r"(\s+OX=(?P<OX>\d+))|"
        r"(\s+GN=(?P<GN>\S+))|"
        r"(\s+PE=(?P<PE>\d))|"
        r"(\s+SV=(?P<SV>\d+)))*\s*$"
    )

    tag_names = {
        "OS": "organism_name",
        "OX": "organism_identifier",
        "GN": "gene_name",
        "PE": "protein_existence",
        "SV": "sequence_version",
    }

    @classmethod
    def parse(cls, header: str) -> ParsedHeader:
        """Parse a FASTA header string into a ParsedHeader object.

        Raises:
            ValueError: If the header could not be parsed.
        """
        split_header = header.split(maxsplit=1)
        try:
            db, _id, entry = split_header[0].split("|")
        except ValueError:
            raise ValueError(
                f"Header does not match the minimal UniProt like pattern: {header}"
            )
        fields = {"db": db, "identifier": _id, "entry_name": entry}

        if len(split_header) == 1:
            return ParsedHeader(fields["identifier"], header, fields)

        description = split_header[1]
        tag_positions = [description.find(f"{tag}=") for tag in cls.tag_names]
        matched_start = sorted([num for num in tag_positions if num >= 0])
        matched_end = matched_start[1:] + [len(description)]

        if not matched_start:  # Description contains only the protein name
            fields["protein_name"] = description
        elif matched_start[0] != 0:  # Description contains protein name and tag fields
            fields["protein_name"] = description[: matched_start[0]].rstrip()

        if matched_start:  # Header contains tag fields
            for start, end in zip(matched_start, matched_end):
                matched_field = description[start:end].rstrip().split("=", maxsplit=1)
                fields[matched_field[0]] = matched_field[1]

        for old_tag, new_tag in cls.tag_names.items():
            if old_tag in fields:
                fields[new_tag] = fields.pop(old_tag)

        return ParsedHeader(fields["identifier"], header, fields)


class UniprotLikeWriter:
    """Writer for UniProt-like FASTA headers.

    Reconstructs a UniProt-like header string from `header_fields`. More tolerant
    than `UniprotWriter`: only the three core identifier fields are required; all
    other fields are optional and are included in the header only if present.

    Required `header_fields` keys (see `UniprotLikeWriter.required_fields`):
        - `db`: Source database identifier.
        - `identifier`: Accession or unique identifier.
        - `entry_name`: Entry name.

    Optional `header_fields` keys (included in the header if present):
        - `protein_name`: Written after the identifier block.
        - `organism_name`: Written as `OS=...`.
        - `organism_identifier`: Written as `OX=...`.
        - `gene_name`: Written as `GN=...`.
        - `protein_existence`: Written as `PE=...`.
        - `sequence_version`: Written as `SV=...`.
    """

    required_fields: frozenset[str] = frozenset({"db", "identifier", "entry_name"})

    tag_names = {
        "OS": "organism_name",
        "OX": "organism_identifier",
        "GN": "gene_name",
        "PE": "protein_existence",
        "SV": "sequence_version",
    }

    @classmethod
    def write(cls, parsed_header: AbstractParsedHeader) -> str:
        """Write a FASTA header string from a ParsedHeader object.

        Raises:
            ValueError: If any required fields are missing from `header_fields`.
                See `UniprotLikeWriter.required_fields` for the list of required
                fields.
        """
        fields = parsed_header.header_fields
        missing = cls.required_fields - fields.keys()
        if missing:
            raise ValueError(
                f"UniprotLikeWriter is missing the following required header fields: "
                f"{sorted(missing)}. See UniprotLikeWriter.required_fields."
            )
        header_entries = [
            f"{fields['db']}|{fields['identifier']}|{fields['entry_name']}",
        ]
        if "protein_name" in fields:
            header_entries.append(f"{fields['protein_name']}")

        for key in ["OS", "OX", "GN", "PE", "SV"]:
            field_name = cls.tag_names[key]
            if field_name not in fields:
                continue
            header_entries.append(f"{key}={fields[field_name]}")
        return " ".join(header_entries)


_PARSER_REGISTRY: dict[str, type[AbstractHeaderParser]] = {
    "default": DefaultParser,
    "uniprot": UniprotParser,
    "uniprot_like": UniprotLikeParser,
}

_WRITER_REGISTRY: dict[str, type[AbstractHeaderWriter]] = {
    "default": DefaultWriter,
    "uniprot": UniprotWriter,
    "uniprot_like": UniprotLikeWriter,
}

_BUILTIN_PARSERS: frozenset[str] = frozenset(_PARSER_REGISTRY)
_BUILTIN_WRITERS: frozenset[str] = frozenset(_WRITER_REGISTRY)


def register_parser(name: str, parser: type[AbstractHeaderParser]) -> None:
    """Register a custom FASTA header parser by name.

    Args:
        name: The name to register the parser under. Must not conflict with a built-in
            parser name or an already registered custom parser name.
        parser: The parser class to register.

    Raises:
        ValueError: If `name` is a built-in parser name.
        ValueError: If a parser is already registered under `name`. Use `replace_parser`
            to overwrite an existing custom parser.
    """
    if name in _BUILTIN_PARSERS:
        raise ValueError(
            f"Cannot register '{name}': it is a built-in parser name and cannot be "
            f"overwritten. Built-in parsers are: {sorted(_BUILTIN_PARSERS)}."
        )
    if name in _PARSER_REGISTRY:
        raise ValueError(
            f"A parser named '{name}' is already registered. Use replace_parser() "
            f"to overwrite an existing custom parser."
        )
    _PARSER_REGISTRY[name] = parser


def get_parser(name: str) -> type[AbstractHeaderParser]:
    """Get a registered FASTA header parser by name.

    Args:
        name: The name of the parser to retrieve.

    Returns:
        The parser class registered under `name`.

    Raises:
        KeyError: If no parser is registered under `name`.
    """
    if name not in _PARSER_REGISTRY:
        raise KeyError(
            f"No parser registered under the name '{name}'. "
            f"Available parsers are: {list(_PARSER_REGISTRY)}."
        )
    return _PARSER_REGISTRY[name]


def replace_parser(name: str, parser: type[AbstractHeaderParser]) -> None:
    """Replace a previously registered non-built-in parser.

    Args:
        name: The name of the custom parser to replace.
        parser: The new parser class to register under `name`.

    Raises:
        ValueError: If `name` refers to a built-in parser, which cannot be replaced.
        ValueError: If no custom parser is registered under `name`. Use
            `register_parser` to add a new parser.
    """
    if name in _BUILTIN_PARSERS:
        raise ValueError(
            f"Cannot replace '{name}': built-in parsers cannot be overwritten. "
            f"Built-in parsers are: {sorted(_BUILTIN_PARSERS)}."
        )
    if name not in _PARSER_REGISTRY:
        raise ValueError(
            f"No custom parser registered under the name '{name}'. "
            f"Use register_parser() to add a new parser."
        )
    _PARSER_REGISTRY[name] = parser


def list_parsers() -> list[str]:
    """Return the names of all currently registered parsers.

    Returns:
        A list of parser names, including both built-in and custom parsers.
    """
    return sorted(_PARSER_REGISTRY)


def register_writer(name: str, writer: type[AbstractHeaderWriter]) -> None:
    """Register a custom FASTA header writer by name.

    Args:
        name: The name to register the writer under. Must not conflict with a built-in
            writer name or an already registered custom writer name.
        writer: The writer class to register.

    Raises:
        ValueError: If `name` is a built-in writer name.
        ValueError: If a writer is already registered under `name`. Use `replace_writer`
            to overwrite an existing custom writer.
    """
    if name in _BUILTIN_WRITERS:
        raise ValueError(
            f"Cannot register '{name}': it is a built-in writer name and cannot be "
            f"overwritten. Built-in writers are: {sorted(_BUILTIN_WRITERS)}."
        )
    if name in _WRITER_REGISTRY:
        raise ValueError(
            f"A writer named '{name}' is already registered. Use replace_writer() "
            f"to overwrite an existing custom writer."
        )
    _WRITER_REGISTRY[name] = writer


def get_writer(name: str) -> type[AbstractHeaderWriter]:
    """Get a registered FASTA header writer by name.

    Args:
        name: The name of the writer to retrieve.

    Returns:
        The writer class registered under `name`.

    Raises:
        KeyError: If no writer is registered under `name`.
    """
    if name not in _WRITER_REGISTRY:
        raise KeyError(
            f"No writer registered under the name '{name}'. "
            f"Available writers are: {list(_WRITER_REGISTRY)}."
        )
    return _WRITER_REGISTRY[name]


def replace_writer(name: str, writer: type[AbstractHeaderWriter]) -> None:
    """Replace a previously registered non-built-in writer.

    Args:
        name: The name of the custom writer to replace.
        writer: The new writer class to register under `name`.

    Raises:
        ValueError: If `name` refers to a built-in writer, which cannot be replaced.
        ValueError: If no custom writer is registered under `name`. Use
            `register_writer` to add a new writer.
    """
    if name in _BUILTIN_WRITERS:
        raise ValueError(
            f"Cannot replace '{name}': built-in writers cannot be overwritten. "
            f"Built-in writers are: {sorted(_BUILTIN_WRITERS)}."
        )
    if name not in _WRITER_REGISTRY:
        raise ValueError(
            f"No custom writer registered under the name '{name}'. "
            f"Use register_writer() to add a new writer."
        )
    _WRITER_REGISTRY[name] = writer


def list_writers() -> list[str]:
    """Return the names of all currently registered writers.

    Returns:
        A list of writer names, including both built-in and custom writers.
    """
    return sorted(_WRITER_REGISTRY)


def _reset_registries():
    """Internal helper to restore registries to built-in state."""
    for name in list(_PARSER_REGISTRY):
        if name not in _BUILTIN_PARSERS:
            del _PARSER_REGISTRY[name]

    for name in list(_WRITER_REGISTRY):
        if name not in _BUILTIN_WRITERS:
            del _WRITER_REGISTRY[name]