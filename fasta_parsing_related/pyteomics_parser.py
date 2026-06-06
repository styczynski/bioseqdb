Source code for pyteomics.fasta
"""
fasta - manipulations with FASTA databases
==========================================

FASTA is a simple file format for protein sequence databases. Please refer to
`the NCBI website <http://www.ncbi.nlm.nih.gov/blast/fasta.shtml>`_
for the most detailed information on the format.

Data manipulation
-----------------

Classes
.......

Several classes of FASTA parsers are available. All of them have common features:

 - context manager support;

 - header parsing;

 - direct iteration.

Available classes:

  :py:class:`FASTABase` - common ancestor, suitable for type checking.
  Abstract class.

  :py:class:`FASTA` - text-mode, sequential parser.
  Good for iteration over database entries.

  :py:class:`IndexedFASTA` - binary-mode, indexing parser.
  Supports direct indexing by header string.

  :py:class:`TwoLayerIndexedFASTA` - additionally supports
  indexing by extracted header fields.

  :py:class:`UniProt` and :py:class:`IndexedUniProt`,
  :py:class:`UniParc` and :py:class:`IndexedUniParc`,
  :py:class:`UniMes` and :py:class:`IndexedUniMes`,
  :py:class:`UniRef` and :py:class:`IndexedUniRef`,
  :py:class:`SPD` and :py:class:`IndexedSPD`,
  :py:class:`NCBI` and :py:class:`IndexedNCBI`,
  :py:class:`RefSeq` and :py:class:`IndexedRefSeq`, - format-specific parsers.

Functions
.........

  :py:func:`read` - returns an instance of the appropriate reader class,
  for sequential iteration or random access.

  :py:func:`chain` - read multiple files at once.

  :py:func:`chain.from_iterable` - read multiple files at once, using an
  iterable of files.

  :py:func:`write` - write entries to a FASTA database.

  :py:func:`parse` - parse a FASTA header.

Decoy sequence generation
-------------------------

:py:func:`decoy_sequence` - generate a decoy sequence from a given sequence, using
one of the other functions listed in this section or any other callable.

:py:func:`reverse` - generate a reversed decoy sequence.

:py:func:`shuffle` - generate a shuffled decoy sequence.

:py:func:`fused_decoy` - generate a "fused" decoy sequence.


Decoy database generation
-------------------------

  :py:func:`write_decoy_db` - generate a decoy database and write it to a file.

  :py:func:`decoy_db` - generate entries for a decoy database from a given FASTA
  database.

  :py:func:`decoy_entries` - generate decoy entries for an iterator.

  :py:func:`decoy_chain` - a version of :py:func:`decoy_db` for multiple files.

  :py:func:`decoy_chain.from_iterable` - like :py:func:`decoy_chain`, but with
  an iterable of files.

Auxiliary
---------

  :py:data:`std_parsers` - a dictionary with parsers for known FASTA header
  formats.

-------------------------------------------------------------------------------
"""

#   Copyright 2012 Anton Goloborodko, Lev Levitsky
#
#   Licensed under the Apache License, Version 2.0 (the "License");
#   you may not use this file except in compliance with the License.
#   You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
#   Unless required by applicable law or agreed to in writing, software
#   distributed under the License is distributed on an "AS IS" BASIS,
#   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#   See the License for the specific language governing permissions and
#   limitations under the License.

import itertools
import random
from collections import namedtuple
import re
import abc
from . import auxiliary as aux
from .auxiliary.utils import add_metaclass


Protein = namedtuple('Protein', ('description', 'sequence'))
DECOY_PREFIX = 'DECOY_'
RAW_HEADER_KEY = '__raw__'


def _add_raw_field(parser):
    """
    Add :py:const:`RAW_HEADER_KEY` field to the parsed dictinary.

    Parameters
    ----------
    parser : func
        parser function.

    Returns
    -------
    None.

    """
    def _new_parser(instance, descr):
        parsed = parser(instance, descr)
        if RAW_HEADER_KEY not in parsed:
            parsed[RAW_HEADER_KEY] = descr
        elif parsed[RAW_HEADER_KEY] != descr:
            raise aux.PyteomicsError('Cannot save raw protein header, since the corresponsing'
                                     'key ({}) already exists.'.format(RAW_HEADER_KEY))
        return parsed

    return _new_parser



[docs]
class FASTABase(object):
    """Abstract base class for FASTA file parsers.
    Can be used for type checking.
    """
    parser = None
    _ignore_comments = False
    _comments = set('>;')


[docs]
    def __init__(self, source, **kwargs):
        self._ignore_comments = kwargs.pop('ignore_comments', False)
        parser = kwargs.pop('parser', None)
        if parser is not None:
            self.parser = parser
        super(FASTABase, self).__init__(source, **kwargs)



    def _is_comment(self, line):
        return line[0] in self._comments

    def get_entry(self, key):
        raise NotImplementedError





[docs]
class FASTA(FASTABase, aux.FileReader):
    """Text-mode, sequential FASTA parser.
    Suitable for iteration over the file to obtain all entries in order.
    """

[docs]
    def __init__(self, source, ignore_comments=False, parser=None, encoding=None):
        """Create a new FASTA parser object. Supports iteration,
        yields `(description, sequence)` tuples. Supports `with` syntax.

        Parameters
        ----------

        source : str or file-like
            File to read. If file object, it must be opened in *text* mode.
        ignore_comments : bool, optional
            If :py:const:`True` then ignore the second and subsequent lines of description.
            Default is :py:const:`False`, which concatenates multi-line descriptions into
            a single string.
        parser : function or None, optional
            Defines whether the FASTA descriptions should be parsed. If it is a
            function, that function will be given the description string, and
            the returned value will be yielded together with the sequence.
            The :py:data:`std_parsers` dict has parsers for several formats.
            Hint: specify :py:func:`parse` as the parser to apply automatic
            format recognition.
            Default is :py:const:`None`, which means return the header "as is".
        encoding : str or None, optional
            File encoding (if it is given by name).
        """
        super(FASTA, self).__init__(
            source, mode='r', parser_func=self._read, pass_file=False, args=(), kwargs={},
            encoding=encoding, ignore_comments=ignore_comments, parser=parser)



    def _read(self):
        accumulated_strings = []

        # Iterate through '>' after the file is over to retrieve the last entry.
        for string in itertools.chain(self._source, '>'):
            stripped_string = string.strip()

            # Skip empty lines.
            if not stripped_string:
                continue

            is_comment = self._is_comment(stripped_string)
            if is_comment:
                # If it is a continuing comment
                if len(accumulated_strings) == 1:
                    if not self._ignore_comments:
                        accumulated_strings[0] += (' ' + stripped_string[1:])
                    else:
                        continue

                elif accumulated_strings:
                    description = accumulated_strings[0]
                    sequence = ''.join(accumulated_strings[1:])

                    # Drop the translation stop sign.
                    if sequence and sequence[-1] == '*':
                        sequence = sequence[:-1]
                    if self.parser is not None:
                        description = self.parser(description)
                    yield Protein(description, sequence)
                    accumulated_strings = [stripped_string[1:]]
                else:
                    # accumulated_strings is empty; we're probably reading
                    # the very first line of the file
                    accumulated_strings.append(stripped_string[1:])
            else:
                accumulated_strings.append(stripped_string)

    def get_entry(self, key):
        raise aux.PyteomicsError('Direct indexing is not supported. '
            'Use IndexedFASTA and its subclasses')




def _reconstruct(cls, args, kwargs):
    kwargs['_skip_index'] = True
    return cls(*args, **kwargs)



[docs]
class IndexedFASTA(FASTABase, aux.TaskMappingMixin, aux.IndexedTextReader):
    """Indexed FASTA parser. Supports direct indexing by matched labels."""
    delimiter = '\n>'
    label = r'^[\n]?>(.*)\s*'


[docs]
    def __init__(self, source, ignore_comments=False, parser=None, **kwargs):
        """Create an indexed FASTA parser object.

        Parameters
        ----------
        source : str or file-like
            File to read. If file object, it must be opened in *binary* mode.
        ignore_comments : bool, optional
            If :py:const:`True` then ignore the second and subsequent lines of description.
            Default is :py:const:`False`, which concatenates multi-line descriptions into
            a single string.
        parser : function or None, optional
            Defines whether the FASTA descriptions should be parsed. If it is a
            function, that function will be given the description string, and
            the returned value will be yielded together with the sequence.
            The :py:data:`std_parsers` dict has parsers for several formats.
            Hint: specify :py:func:`parse` as the parser to apply automatic
            format recognition.
            Default is :py:const:`None`, which means return the header "as is".
        encoding : str or None, optional, keyword only
            File encoding. Default is UTF-8.
        block_size : int or None, optional, keyword only
            Number of bytes to consume at once.
        delimiter : str or None, optional, keyword only
            Overrides the FASTA record delimiter (default is ``'\\n>'``).
        label : str or None, optional, keyword only
            Overrides the FASTA record label pattern. Default is ``'^[\\n]?>(.*)'``.
        label_group : int or str, optional, keyword only
            Overrides the matched group used as key in the byte offset index.
            This in combination with `label` can be used to extract fields from headers.
            However, consider using :py:class:`TwoLayerIndexedFASTA` for this purpose.
        """
        super(IndexedFASTA, self).__init__(
            source, ignore_comments=ignore_comments, parser=parser,
            parser_func=self._read, pass_file=False, args=(), kwargs={}, **kwargs)
        self._init_args = (source, ignore_comments, parser)
        self._init_kwargs = kwargs



    def __reduce_ex__(self, protocol):
        return (_reconstruct,
            (self.__class__, self._init_args, self._init_kwargs),
            self.__getstate__())

    def _read_protein_lines(self, lines):
        description = []
        sequence = []

        for string in lines:
            stripped_string = string.strip()
            if not stripped_string:
                continue

            is_comment = self._is_comment(stripped_string)
            if is_comment:
                if not description or not self._ignore_comments:
                    description.append(stripped_string[1:])
            else:
                sequence.append(stripped_string)

        description = ' '.join(description)
        sequence = ''.join(sequence)
        # Drop the translation stop sign.
        if sequence and sequence[-1] == '*':
            sequence = sequence[:-1]
        if self.parser is not None:
            description = self.parser(description)
        return Protein(description, sequence)

    def _item_from_offsets(self, offsets):
        start, end = offsets
        lines = self._read_lines_from_offsets(start, end)
        return self._read_protein_lines(lines)

    def _read(self, **kwargs):
        for key, offsets in self._offset_index.items():
            yield self._item_from_offsets(offsets)

    def get_entry(self, key):
        return self.get_by_id(key)





[docs]
class TwoLayerIndexedFASTA(IndexedFASTA):
    """Parser with two-layer index. Extracted groups are mapped to full headers (where possible),
    full headers are mapped to byte offsets.

    When indexed, the key is looked up in both indexes, allowing access by meaningful IDs
    (like UniProt accession) and by full header string.
    """
    header_group = 1
    header_pattern = None


[docs]
    def __init__(self, source, header_pattern=None, header_group=None,
                 ignore_comments=False, parser=None, **kwargs):
        """Open `source` and create a two-layer index for convenient random access
        both by full header strings and extracted fields.

        Parameters
        ----------
        source : str or file-like
            File to read. If file object, it must be opened in *binary* mode.
        header_pattern : str or RE or None, optional
            Pattern to match the header string. Must capture the group used
            for the second index. If :py:const:`None` (default), second-level index is not created.
        header_group : int or str or None, optional
            Defines which group is used as key in the second-level index.
            Default is 1.
        ignore_comments : bool, optional
            If :py:const:`True` then ignore the second and subsequent lines of description.
            Default is :py:const:`False`, which concatenates multi-line descriptions into
            a single string.
        parser : function or None, optional
            Defines whether the FASTA descriptions should be parsed. If it is a
            function, that function will be given the description string, and
            the returned value will be yielded together with the sequence.
            The :py:data:`std_parsers` dict has parsers for several formats.
            Hint: specify :py:func:`parse` as the parser to apply automatic
            format recognition.
            Default is :py:const:`None`, which means return the header "as is".

        Other arguments : the same as for :py:class:`IndexedFASTA`.
        """
        super(TwoLayerIndexedFASTA, self).__init__(source, ignore_comments, parser, **kwargs)
        if header_group is not None:
            self.header_group = header_group
        if header_pattern is not None:
            self.header_pattern = header_pattern
        if not kwargs.get('_skip_index', False):
            self.build_second_index()
        self._init_args = (source, header_pattern, header_group, ignore_comments, parser)
        self._init_kwargs = kwargs




[docs]
    def build_second_index(self):
        """Create the mapping from extracted field to whole header string."""
        if self.header_pattern is None:
            self._id2header = None
        else:
            index = {}
            for key in self._offset_index:
                match = re.match(self.header_pattern, key)
                if match:
                    index[match.group(self.header_group)] = key
            self._id2header = index



    def __getstate__(self):
        state = super(TwoLayerIndexedFASTA, self).__getstate__()
        state['id2header'] = self._id2header
        return state

    def __setstate__(self, state):
        super(TwoLayerIndexedFASTA, self).__setstate__(state)
        self._id2header = state['id2header']


[docs]
    def get_by_id(self, key):
        """Get the entry by value of header string or extracted field."""
        try:
            return super(TwoLayerIndexedFASTA, self).get_by_id(key)
        except KeyError:
            if self._id2header:
                header = self._id2header.get(key)
                if header is not None:
                    return super(TwoLayerIndexedFASTA, self).get_entry(header)
        raise KeyError(key)



    def get_header(self, key):
        if key in self._id2header:
            return self._id2header[key]
        raise KeyError(key)

    def __contains__(self, key):
        return super(TwoLayerIndexedFASTA, self).__contains__(key) or key in self._id2header




class _FastaParserFlavorMeta(abc.ABCMeta):
    def __new__(mcs, name, bases, namespace):
        if "parser" in namespace:
            namespace["parser"] = _add_raw_field(namespace["parser"])
        if name != 'FlavoredMixin':
            reader_type = None
            for t in (FASTA, IndexedFASTA, TwoLayerIndexedFASTA):
                if t in bases:
                    reader_type = t

            if reader_type is not None:
                # this is a "concrete" reader class
                # add a unified __init__ method for it
                for c in bases:
                    if issubclass(c, FlavoredMixin):
                        flavor = c
                        break
                else:
                    raise aux.PyteomicsError('Could not detect flavor of {}, not a subclass of `FlavoredMixin`.')

                def __init__(self, source, parse=True, **kwargs):
                    reader_type.__init__(self, source, **kwargs)
                    flavor.__init__(self, parse)
                    self._init_args = (source, parse)
                    self._init_kwargs = kwargs

                flavor_name = name[:-5]
                type_name = "Text-mode" if reader_type is FASTA else "Indexed"
                __init__.__doc__ = """Creates a :py:class:`{}` object.

                Parameters
                ----------
                source : str or file
                    The file to read. If a file object, it needs to be in *{}* mode.
                parse : bool, optional
                    Defines whether the descriptions should be parsed in the produced tuples.
                    Default is :py:const:`True`.
                kwargs : passed to the :py:class:`{}` constructor.
                """.format(name, 'text' if reader_type is FASTA else 'binary', reader_type.__name__)
                namespace['__init__'] = __init__
                namespace['__doc__'] = """{} parser for {} FASTA files.""".format(type_name, flavor_name)

        return super(_FastaParserFlavorMeta, mcs).__new__(mcs, name, bases, namespace)



[docs]
@add_metaclass(_FastaParserFlavorMeta)
class FlavoredMixin():
    """Parser aimed at a specific FASTA flavor.
    Subclasses should define `parser` and `header_pattern`.
    The `parse` argument in :py:meth:`__init__` defines whether description is
    parsed in output.
    """

[docs]
    def __init__(self, parse=True):
        if not parse:
            self.parser = None






[docs]
class UniProtMixin(FlavoredMixin):
    header_pattern = r'^(?P<db>\w+)\|(?P<id>[-\w]+)\|(?P<entry>\w+)\s+(?P<name>.*?)(?:(\s+OS=(?P<OS>[^=]+))|(\s+OX=(?P<OX>\d+))|(\s+GN=(?P<GN>[^=]+))|(\s+PE=(?P<PE>\d))|(\s+SV=(?P<SV>\d+)))*\s*$'
    header_group = 'id'

    def parser(self, header):
        info = re.match(self.header_pattern, header).groupdict()
        for key in ['OS', 'OX', 'GN', 'PE', 'SV']:
            if info[key] is None:
                del info[key]
        info['gene_id'], info['taxon'] = info['entry'].split('_')
        _intify(info, ('PE', 'SV', 'OX'))
        return info





[docs]
class UniProt(UniProtMixin, FASTA):
    pass





[docs]
class IndexedUniProt(UniProtMixin, TwoLayerIndexedFASTA):
    pass





[docs]
class UniRefMixin(FlavoredMixin):
    header_pattern = r'^(?P<id>\S+)\s+(?P<cluster>.*?)(?:(\s+n=(?P<n>\d+))|(\s+Tax=(?P<Tax>.+?))|(\s+TaxID=(?P<TaxID>\S+))|(\s+RepID=(?P<RepID>\S+)))*\s*$'
    header_group = 'id'

    def parser(self, header):
        assert 'Tax' in header
        info = re.match(self.header_pattern, header).groupdict()
        for key in ['TaxID', 'Tax', 'RepID', 'n']:
            if info[key] is None:
                del info[key]
        _intify(info, ('n',))
        return info





[docs]
class UniRef(UniRefMixin, FASTA):
    pass





[docs]
class IndexedUniRef(UniRefMixin, TwoLayerIndexedFASTA):
    pass





[docs]
class UniParcMixin(FlavoredMixin):
    header_pattern = r'(\S+)\s+status=(\w+)\s*$'

    def parser(self, header):
        ID, status = re.match(self.header_pattern, header).groups()
        return {'id': ID, 'status': status}





[docs]
class UniParc(UniParcMixin, FASTA):
    pass





[docs]
class IndexedUniParc(UniParcMixin, TwoLayerIndexedFASTA):
    pass





[docs]
class UniMesMixin(FlavoredMixin):
    header_pattern = r'^(\S+)\s+([^=]*\S)((\s+\w+=[^=]+(?!\w*=))+)\s*$'

    def parser(self, header):
        assert 'OS=' in header and 'SV=' in header and 'PE=' not in header
        ID, name, pairs, _ = re.match(self.header_pattern, header).groups()
        info = {'id': ID, 'name': name}
        info.update(_split_pairs(pairs))
        _intify(info, ('SV',))
        return info





[docs]
class UniMes(UniMesMixin, FASTA):
    pass





[docs]
class IndexedUniMes(UniMesMixin, TwoLayerIndexedFASTA):
    pass





[docs]
class SPDMixin(FlavoredMixin):
    header_pattern = r'^([^|]+?)\s*\|\s*(([^|]+?)_([^|]+?))\s*\|\s*([^|]+?)\s*$'

    def parser(self, header):
        assert '=' not in header
        ID, gene, gid, taxon, d = re.match(self.header_pattern, header).groups()
        return {'id': ID, 'gene': gene, 'description': d,
                'taxon': taxon, 'gene_id': gid}





[docs]
class SPD(SPDMixin, FASTA):
    pass





[docs]
class IndexedSPD(SPDMixin, TwoLayerIndexedFASTA):
    pass





[docs]
class NCBIMixin(FlavoredMixin):
    header_pattern = r'^(\S+)\s+(.*\S)\s+\[(.*)\]'

    def parser(self, header):
        ID, description, organism = re.match(self.header_pattern, header).groups()
        return {'id': ID, 'description': description, 'taxon': organism}





[docs]
class NCBI(NCBIMixin, FASTA):
    pass





[docs]
class IndexedNCBI(NCBIMixin, TwoLayerIndexedFASTA):
    pass





[docs]
class RefSeqMixin(FlavoredMixin):
    header_pattern = r'^ref\|([^|]+)\|\s*([^\[]*\S)\s*\[(.*)\]'

    def parser(self, header):
        ID, description, organism = re.match(self.header_pattern, header).groups()
        return {'id': ID, 'description': description, 'taxon': organism}





[docs]
class RefSeq(RefSeqMixin, FASTA):
    pass





[docs]
class IndexedRefSeq(RefSeqMixin, TwoLayerIndexedFASTA):
    pass





[docs]
def read(source=None, use_index=None, flavor=None, **kwargs):
    """Parse a FASTA file. This function serves as a dispatcher between
    different parsers available in this module.

    Parameters
    ----------
    source : str or file or None, optional
        A file object (or file name) with a FASTA database. Default is
        :py:const:`None`, which means read standard input.
    use_index : bool, optional
        If :py:const:`True`, the created parser object will be an instance of
        :py:class:`IndexedFASTA`. If :py:const:`False` (default), it will be
        an instance of :py:class:`FASTA`.
    flavor : str or None, optional
        A supported FASTA header format. If specified, a format-specific
        parser instance is returned.

        .. note:: See :py:data:`std_parsers` for supported flavors.

    Returns
    -------
    out : iterator of tuples
        A named 2-tuple with FASTA header (str or dict) and sequence (str).
        Attributes 'description' and 'sequence' are also provided.
    """
    try:
        parser = std_parsers[flavor and flavor.lower()]
    except KeyError:
        raise aux.PyteomicsError('No parser for flavor: {}. Supported flavors: {}'.format(
            flavor, ', '.join(map(str, std_parsers))))
    use_index = aux._check_use_index(source, use_index, False)
    return parser[use_index](source, **kwargs)





[docs]
@aux._file_writer()
def write(entries, output=None):
    """
    Create a FASTA file with `entries`.

    Parameters
    ----------
    entries : iterable of (str/dict, str) tuples
        An iterable of 2-tuples in the form (description, sequence).
        If description is a dictionary, it must have a special key, whose value
        will be written as protein description. The special key is defined by the variable
        :py:const:`RAW_HEADER_KEY`.
    output : file-like or str, optional
        A file open for writing or a path to write to. If the file exists,
        it will be opened for writing. Default is :py:const:`None`, which
        means write to standard output.

        .. note::
            The default mode for output files specified by name has been changed
            from `a` to `w` in *pyteomics 4.6*. See `file_mode` to override the mode.

    file_mode : str, keyword only, optional
        If `output` is a file name, defines the mode the file will be opened in.
        Otherwise will be ignored. Default is `'w'`.

        .. note ::
            The default changed from `'a'` in *pyteomics 4.6*.

    Returns
    -------
    output_file : file object
        The file where the FASTA is written.
    """
    for descr, seq in entries:
        if isinstance(descr, str):
            output.write('>' + descr.replace('\n', '\n;') + '\n')
        elif isinstance(descr, dict) and RAW_HEADER_KEY in descr:
            output.write('>' + descr[RAW_HEADER_KEY].replace('\n', '\n;') + '\n')
        else:
            raise aux.PyteomicsError('Cannot use provided description: ' + repr(descr))
        output.write(''.join([('%s\n' % seq[i:i+70]) for i in range(0, len(seq), 70)]) + '\n')

    return output.file





[docs]
def reverse(sequence, keep_nterm=False, keep_cterm=False):
    """
    Create a decoy sequence by reversing the original one.

    Parameters
    ----------
    sequence : str
        The initial sequence string.
    keep_nterm : bool, optional
        If :py:const:`True`, then the N-terminal residue will be kept.
        Default is :py:const:`False`.
    keep_cterm : bool, optional
        If :py:const:`True`, then the C-terminal residue will be kept.
        Default is :py:const:`False`.

    Returns
    -------
    decoy_sequence : str
        The decoy sequence.
    """
    start = 1 if keep_nterm else 0
    end = len(sequence)-1 if keep_cterm else len(sequence)
    if start == end:
        return sequence
    return sequence[:start] + sequence[start:end][::-1] + sequence[end:]





[docs]
def shuffle(sequence, keep_nterm=False, keep_cterm=False, keep_nterm_M=False, fix_aa=''):
    """
    Create a decoy sequence by shuffling the original one.

    Parameters
    ----------
    sequence : str
        The initial sequence string.
    keep_nterm : bool, optional
        If :py:const:`True`, then the N-terminal residue will be kept.
        Default is :py:const:`False`.
    keep_cterm : bool, optional
        If :py:const:`True`, then the C-terminal residue will be kept.
        Default is :py:const:`False`.
    keep_nterm_M : bool, optional
        If :py:const:`True`, then the N-terminal methionine will be kept.
        Default is :py:const:`False`.
    fix_aa : iterable, optional
        Single letter codes for amino acids that should preserve their position
        during shuffling.
        Default is ''.

    Returns
    -------
    decoy_sequence : str
        The decoy sequence.
    """

    # empty sequence
    if len(sequence) == 0:
        return ''

    # presereve the first position
    if (keep_nterm_M and sequence[0] == 'M') or keep_nterm:
        return sequence[0] + shuffle(sequence[1:], keep_cterm=keep_cterm, fix_aa=fix_aa)

    # presereve the last position
    if keep_cterm:
        return shuffle(sequence[:-1], fix_aa=fix_aa) + sequence[-1]

    if not isinstance(fix_aa, str):
        fix_aa = ''.join(fix_aa)

    fixed = []
    position = 0
    if len(fix_aa) > 0:  # non-empty fixed list
        shuffled = []
        for match in re.finditer(r'[{}]'.format(fix_aa), sequence):
            fixed.append((match.start(), sequence[match.start()]))
            shuffled.extend(sequence[position:match.start()])
            position = match.end()
        shuffled.extend(sequence[position:])

    else:  # shuffle everything
        shuffled = list(sequence)

    random.shuffle(shuffled)

    for fix in fixed:
        shuffled.insert(fix[0], fix[1])

    return ''.join(shuffled)





[docs]
def fused_decoy(sequence, decoy_mode='reverse', sep='R', **kwargs):
    """
    Create a "fused" decoy sequence by concatenating a decoy sequence with the original one.
    The method and its use cases are described in:

    Ivanov, M. V., Levitsky, L. I., & Gorshkov, M. V. (2016).
    `Adaptation of Decoy Fusion Strategy for Existing Multi-Stage Search Workflows.
    <http://doi.org/10.1007/s13361-016-1436-7>`_
    Journal of The American Society for Mass Spectrometry, 27(9), 1579-1582.

    Parameters
    ----------
    sequence : str
        The initial sequence string.
    decoy_mode : str or callable, optional
        Type of decoy sequence to use. Should be one of the standard modes or any callable.
        Standard modes are:

        - 'reverse' for :py:func:`reverse`;
        - 'shuffle' for :py:func:`shuffle`;
        - 'fused' for :py:func:`fused_decoy` (if you love recursion).

        Default is 'reverse'.
    sep : str, optional
        Amino acid motif that separates the decoy sequence from the target one.
        This setting should reflect the enzyme specificity used in the search against the
        database being generated. Default is 'R', which is suitable for trypsin searches.
    **kwargs : given to the decoy generation function.

    Examples
    --------
    >>> fused_decoy('PEPT')
    'TPEPRPEPT'
    >>> fused_decoy('MPEPT', 'shuffle', 'K', keep_nterm=True)
    'MPPTEKMPEPT'
    """
    decoy = decoy_sequence(sequence, decoy_mode, **kwargs)
    return decoy + sep + sequence




_decoy_functions = {'reverse': reverse, 'shuffle': shuffle, 'fused': fused_decoy}



[docs]
def decoy_sequence(sequence, mode='reverse', **kwargs):
    """
    Create a decoy sequence out of a given sequence string.

    Parameters
    ----------
    sequence : str
        The initial sequence string.
    mode : str or callable, optional
        Type of decoy sequence. Should be one of the standard modes or any callable.
        Standard modes are:

        - 'reverse' for :py:func:`reverse`;
        - 'shuffle' for :py:func:`shuffle`;
        - 'fused' for :py:func:`fused_decoy`.

        Default is 'reverse'.
    **kwargs : given to the decoy function.

    Returns
    -------
    decoy_sequence : str
        The decoy sequence.
    """
    fmode = mode
    if isinstance(mode, str):
        fmode = _decoy_functions.get(mode)
        if fmode is None:
            raise aux.PyteomicsError('Unsupported decoy mode: {}'.format(mode))
    return fmode(sequence, **kwargs)





[docs]
def decoy_entries(entries, mode='reverse', prefix=DECOY_PREFIX, decoy_only=True, **kwargs):
    """Iterate over protein `entries` (tuples) and produce decoy entries.
    The `entries` are only iterated once.

    Parameters
    ----------
    entries : iterable of tuples
        Any iterable of (description, sequence) pairs.
    mode : str or callable, optional
        Algorithm of decoy sequence generation. 'reverse' by default.
        See :py:func:`decoy_sequence` for more information.
    prefix : str, optional
        A prefix to the protein descriptions of decoy entries. The default
        value is `'DECOY_'`.
    decoy_only : bool, optional
        If set to :py:const:`True`, only the decoy entries will be written to
        `output`. If :py:const:`False`, each consumed entry is yielded unchanged,
        followed by its decoy couterpart.
        :py:const:`True` by default.
    **kwargs : given to :py:func:`decoy_sequence`.

    Returns
    -------
    out : iterator
        An iterator over new entries.
    """
    for item in entries:
        if not decoy_only:
            yield item
        yield Protein(prefix + item[0], decoy_sequence(item[1], mode, **kwargs))





[docs]
@aux._file_reader()
def decoy_db(source=None, mode='reverse', prefix=DECOY_PREFIX, decoy_only=False,
             ignore_comments=False, parser=None, **kwargs):
    """Iterate over sequences for a decoy database out of a given ``source``.

    Parameters
    ----------
    source : file-like object or str or None, optional
        A path to a FASTA database or a file object itself. Default is
        :py:const:`None`, which means read standard input.
    mode : str or callable, optional
        Algorithm of decoy sequence generation. 'reverse' by default.
        See :py:func:`decoy_sequence` for more information.
    prefix : str, optional
        A prefix to the protein descriptions of decoy entries. The default
        value is `'DECOY_'`.
    decoy_only : bool, optional
        If set to :py:const:`True`, only the decoy entries will be written to
        `output`. If :py:const:`False`, the entries from `source` will be
        written first.
        :py:const:`False` by default.
    ignore_comments : bool, optional
        If True then ignore the second and subsequent lines of description.
        Default is :py:const:`False`.
    parser : function or None, optional
        Defines whether the fasta descriptions should be parsed. If it is a
        function, that function will be given the description string, and
        the returned value will be yielded together with the sequence.
        The :py:data:`std_parsers` dict has parsers for several formats.
        Hint: specify :py:func:`parse` as the parser to apply automatic
        format guessing.
        Default is :py:const:`None`, which means return the header "as is".
    **kwargs : given to :py:func:`decoy_sequence`.

    Returns
    -------
    out : iterator
        An iterator over entries of the new database.
    """

    # store the initial position
    pos = source.tell()
    if not decoy_only:
        with read(source, ignore_comments, parser) as f:
            for x in f:
                yield x
        # return to the initial position in the source file to read again
        source.seek(pos)

    parser = parser or (lambda x: x)
    with read(source, ignore_comments) as f:
        for descr, seq in f:
            yield Protein(parser(prefix + descr), decoy_sequence(seq, mode, **kwargs))





[docs]
@aux._file_writer()
def write_decoy_db(source=None, output=None, mode='reverse', prefix=DECOY_PREFIX,
                   decoy_only=False, **kwargs):
    """Generate a decoy database out of a given ``source`` and write to file.

    If `output` is a path, the file will be open for appending, so no information
    will be lost if the file exists. Although, the user should be careful when
    providing open file streams as `source` and `output`. The reading and writing
    will start from the current position in the files, which is where the last I/O
    operation finished. One can use the :py:func:`file.seek` method to change it.

    Parameters
    ----------
    source : file-like object or str or None, optional
        A path to a FASTA database or a file object itself. Default is
        :py:const:`None`, which means read standard input.
    output : file-like object or str, optional
        A path to the output database or a file open for writing.
        Defaults to :py:const:`None`, the results go to the standard output.
    mode : str or callable, optional
        Algorithm of decoy sequence generation. 'reverse' by default.
        See :py:func:`decoy_sequence` for more details.
    prefix : str, optional
        A prefix to the protein descriptions of decoy entries. The default
        value is `'DECOY_'`
    decoy_only : bool, optional
        If set to :py:const:`True`, only the decoy entries will be written to
        `output`. If :py:const:`False`, the entries from `source` will be
        written as well.
        :py:const:`False` by default.
    file_mode : str, keyword only, optional
        If `output` is a file name, defines the mode the file will be opened in.
        Otherwise will be ignored. Default is 'a'.
    **kwargs : given to :py:func:`decoy_sequence`.

    Returns
    -------
    output : file
        A (closed) file object for the created file.
    """
    with decoy_db(source, mode, prefix, decoy_only, **kwargs) as entries:
        write(entries, output)
        return output.file




# auxiliary functions for parsing of FASTA headers
def _split_pairs(s):
    return dict(map(lambda x: x.strip(), x.split('=')) for x in re.split(r' (?=\w+=)', s.strip()))


def _intify(d, keys):
    for k in keys:
        if k in d:
            d[k] = int(d[k])


std_parsers = {
        'uniprot': (UniProt, IndexedUniProt), 'uniref': (UniRef, IndexedUniRef),
        'uniparc': (UniParc, IndexedUniParc), 'unimes': (UniMes, IndexedUniMes),
        'spd': (SPD, IndexedSPD), 'ncbi': (NCBI, IndexedNCBI),
        'refseq': (RefSeq, IndexedRefSeq),
        None: (FASTA, IndexedFASTA)}
"""A dictionary with parsers for known FASTA header formats. For now, supported
formats are those described at
`UniProt help page <http://www.uniprot.org/help/fasta-headers>`_."""


_std_mixins = {
        'uniprot': UniProtMixin, 'uniref': UniRefMixin,
        'uniparc': UniParcMixin, 'unimes': UniMesMixin, 'spd': SPDMixin,
        'ncbi': NCBIMixin, 'refseq': RefSeqMixin}



[docs]
def parse(header, flavor='auto', parsers=None):
    """Parse the FASTA header and return a nice dictionary.

    Parameters
    ----------

    header : str
        FASTA header to parse
    flavor : str, optional
        Short name of the header format (case-insensitive). Valid values are
        :py:const:`'auto'` and keys of the `parsers` dict. Default is
        :py:const:`'auto'`, which means try all formats in turn and return the
        first result that can be obtained without an exception.
    parsers : dict, optional
        A dict where keys are format names (lowercased) and values are functions
        that take a header string and return the parsed header.

    Returns
    -------

    out : dict
        A dictionary with the info from the header. The format depends on the
        flavor.
    """
    parser_function = lambda cls: cls().parser
    flavor = flavor.lower()
    # accept strings with and without leading '>'
    if header and header[0] == '>':
        header = header[1:]

    # choose the format
    known = parsers or _std_mixins

    if flavor == 'auto':
        for parser in known.values():
            try:
                return parser_function(parser)(header)
            except Exception:
                pass
        raise aux.PyteomicsError('Unknown FASTA header format: ' + header)
    elif flavor in known:
        try:
            return parser_function(known[flavor])(header)
        except Exception as e:
            raise aux.PyteomicsError(
                'Could not parse header as "{}". '
                'The error message was: {}: {}. Header: "{}"'.format(
                        flavor, type(e).__name__, e.args[0], header))
    raise aux.PyteomicsError('Unknown flavor: {}'.format(flavor))




chain = aux._make_chain(read, 'read')
decoy_chain = aux._make_chain(decoy_db, 'decoy_db')