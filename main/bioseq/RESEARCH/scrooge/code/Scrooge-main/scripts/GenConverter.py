import urllib.request
import argparse

def download_reads_file(url):
    """
    url: link to read file
    returns: bytes string containing reads file
    """
    contents = urllib.request.urlopen(url).read()
    return contents

def import_reads(reads_file_contents, source_format):
    """
    reads_file_contents: bytes string containing reads file
    source_format: one of "FASTA", "FASTQ" or "VGSIM"
    returns: List of (name, bases) pairs
    """
    if source_format not in ["FASTA", "FASTQ", "HGA", "VGSIM"]:
        raise Exception(f"import_reads() received invalid source_format '{source_format}'")

    if source_format == "FASTA" or source_format == "HGA":
        res = []
        for read_section in reads_file_contents.split(b"\n>"):
            lines = read_section.split(b"\n")
            title = lines[0]
            bases = b"".join(lines[1:])
            res.append((title, bases))
        res[0] = (res[0][0][1:], res[0][1]) #remove leading > not removed by split
        return res

    if source_format == "FASTQ":
        res = []
        lines = reads_file_contents.split(b"\n")
        if lines[-1] == b"":
            lines = lines[:-1]
        for first_line_idx in range(0, len(lines), 4):
            read_lines = lines[first_line_idx:first_line_idx+4]
            title = read_lines[0][1:]
            bases = read_lines[1]
            res.append((title, bases))
        return res

    if source_format == "VGSIM":
        res = []
        for i, line in enumerate(reads_file_contents.split(b"\n")):
            title = f"read_{i:06d}".encode("ascii")
            bases = line
            res.append((title, bases))
        return res

    return []

def export_reads(reads, target_format, fasta_line_size=80):
    """
    reads: List of (name, bases) pairs
    target_format: one of "FASTA", "FASTQ" or "HGA"
    returns: bytes string containing reads file

    FASTQ format: todo
    HGA format:
        FASTA style
        all bases of a read are on a single line
        no spaces in title
    """
    if target_format not in ["FASTA", "FASTQ", "HGA"]:
        raise Exception(f"export_reads() received invalid target_format '{target_format}'")
    
    read_sections = []
    if target_format == "HGA":
        for title, bases in reads:
            title = title.replace(b" ", b"_").replace(b"\t", b"_")
            read_sections += [b">", title, b"\n"]
            read_sections += [bases, b"\n"]
        return b"".join(read_sections)

    if target_format == "FASTA":
        for title, bases in reads:
            read_sections += [b">", title, b"\n"]
            for line_start in range(0, len(bases), fasta_line_size):
                line = bases[line_start:line_start + fasta_line_size]
                read_sections += [line, b"\n"]
        return b"".join(read_sections)

    if target_format == "FASTQ":
        for title, bases in reads:
            read_sections += [b"@", title, b"\n"]
            read_sections += [bases, b"\n"]
            read_sections += [b"+", title, b"\n"]
            read_sections += [bytes([93]*len(bases)), b"\n"] #fill in dummy values for qualities
        return b"".join(read_sections)

def dataset_name(url):
    """
    url: link to read file
    returns: last component from url, with file ending removed
    """
    return url.split("/")[-1].split(".")[0]

def sanitize_reads(reads, upper=False, lower=False, prune_titles=False, replace_spaces=None, character_restriction=None, remove_unmapped_reads=None):
    """
    reads: List of (name, bases) pairs, bases are byte strings
    upper: if True, convert all bases to uppercase
    lower: if True, convert all bases to lowercase
    prune_title: if True, prune title at first space
    replace_spaces: if not None, replace all spaces by the given character
    character_restriction: bytestring or None. if set, filter out any characters not occuring in character_restriction
    returns: List of (name, bases) pairs, santized according to parameters
    """
    res = []
    if character_restriction:
        character_restriction = character_restriction.lower() + character_restriction.upper()
    if remove_unmapped_reads:
        mapped_read_ids = set([alignment[0] for alignment in remove_unmapped_reads])
    for title, basepairs in reads:
        if prune_titles:
            title = title.split(b' ')[0]
        if replace_spaces:
            title = title.replace(b' ', replace_spaces)
        if remove_unmapped_reads and title not in mapped_read_ids:
            continue
        if upper:
            basepairs = basepairs.upper()
        if lower:
            basepairs = basepairs.lower()
        if character_restriction:
            filtered_char_codes = filter(lambda bp: bp in character_restriction, basepairs)
            basepairs = bytes(filtered_char_codes)
        res.append((title, basepairs))
    
    return res

def convert_file(input_path, output_path, source_format, target_format, upper=False, lower=False, prune_titles=False, replace_spaces=None, character_restriction=None, start=None, end=None, remove_unmapped_reads=None, fasta_line_size=80):
    with open(input_path, "rb") as f:
        rf = f.read()

    if remove_unmapped_reads:
        remove_unmapped_reads = read_paf(remove_unmapped_reads)

    reads = import_reads(rf, source_format)
    if end != None:
        reads = reads[:end]
    if start != None:
        reads = reads[start:]
    reads = sanitize_reads(reads, upper=upper, lower=lower, prune_titles=prune_titles, replace_spaces=replace_spaces, character_restriction=character_restriction, remove_unmapped_reads=remove_unmapped_reads)
    output_rf = export_reads(reads, target_format, fasta_line_size=fasta_line_size)

    with open(output_path, "wb+") as f:
        f.write(output_rf)

def print_info(input_path, source_format, start=None, end=None, titles=False, stats=False):
    with open(input_path, "rb") as f:
        rf = f.read()

    reads = import_reads(rf, source_format)

    if titles:
        for idx, read in enumerate(reads):
            if start != None and idx < start:
                continue
            if end != None and idx >= end:
                continue
            title, basepairs = read
            title = title.decode('ascii')
            print(f'idx={idx} \'{title}\'')
    
    if stats:
        analyze_reads(reads)

def download_as_hga_read(url, source_format):
    """
    url: link to read file
    downloads reads file from url, converts to HGA format, and saves it
    """
    rf = download_reads_file(url)
    reads = import_reads(rf, source_format)
    hga_rf = export_reads(reads, "HGA")

    file_name = dataset_name(url) + ".hga"
    with open(file_name, "wb+") as f:
        f.write(hga_rf)

def analyze_reads(reads):
    """
    reads: List of (name, bases) pairs, bases are byte strings
    prints statistics and metadata
    """
    lengths = [len(read[1]) for read in reads]
    min_, max_ = min(lengths), max(lengths)
    count = len(lengths)
    avg = sum(lengths) / count
    print(f"lengths min={min_} max={max_} avg={avg:.2f}")
    print(f"avg/max={avg/max_:.2f}")
    print(f"#reads={count}")

def detect_format(path):
    file_ending = path.split('.')[-1]
    for fmt in supported_formats:
        if fmt.startswith(file_ending.upper()):
            return fmt
    return None

def read_paf(file_path):
    with open(file_path, "rb") as f:
        raw_file = f.read()

    alignments = []
    for line in raw_file.split(b"\n"):
        if not line: continue

        fields = line.split(b"\t")[:12]
        for fid in [1, 2, 3, 6, 7, 8, 9, 10, 11]:
            fields[fid] = int(fields[fid])
        alignments.append(fields)

    return alignments

supported_formats = ["FASTA", "FASTQ", "HGA", "VGSIM"]
if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='Convert between genomic dataset file formats.')
    parser.add_argument('input_path', type=str, help='path to input file')
    parser.add_argument('output_path', nargs='?', type=str, help='path to input file', default='output.fa')
    parser.add_argument('--if', dest='input_format', type=str, default=None, choices=supported_formats)
    parser.add_argument('--of', dest='output_format', type=str, default=None, choices=supported_formats)
    parser.add_argument('--upper', action='store_true', help='convert base strings to uppercase')
    parser.add_argument('--lower', action='store_true', help='convert base strings to lowercase')
    parser.add_argument('--prune_titles', action='store_true', help='prune titles are first space')
    parser.add_argument('--replace_spaces', type=str, help='replace all spaces by the given character(s)')
    parser.add_argument('--restrict', type=str, default=None, help='allow only the passed in characters in reads, delete the rest')
    parser.add_argument('--print_titles', action='store_true', help='do not run conversion, print titles')
    parser.add_argument('--print_stats', action='store_true', help='do not run conversion, print stats')
    parser.add_argument('--start', type=int, help='limit to reads starting from given index (inclusive)', default=None)
    parser.add_argument('--end', type=int, help='limit to reads ending at given index (exclusive)', default=None)
    parser.add_argument('--remove_unmapped_reads', type=str, metavar="PAF file", help='remove reads missing in the supplied PAF file')
    parser.add_argument('--fasta_line_size', type=int, help='set line width, if output is in FASTA format', default=80)

    args = parser.parse_args()
    input_path = args.input_path
    output_path = args.output_path

    input_format = args.input_format or detect_format(input_path)
    if input_format == None:
        print("ERROR: could not detect input format")
        exit()
    output_format = args.output_format or detect_format(output_path)
    if output_format == None:
        print("ERROR: could not detect output format")
        exit()

    character_restriction = args.restrict and args.restrict.encode('ascii') 
    
    if args.print_titles or args.print_stats:
        print_info(input_path, input_format, start=args.start, end=args.end, titles=args.print_titles, stats=args.print_stats)
    else:
        convert_file(input_path, output_path, input_format, output_format,
                    upper=args.upper, lower=args.lower,
                    prune_titles=args.prune_titles, replace_spaces=args.replace_spaces.encode("ascii") if args.replace_spaces else None,
                    character_restriction=character_restriction,
                    start=args.start, end=args.end,
                    remove_unmapped_reads=args.remove_unmapped_reads,
                    fasta_line_size=args.fasta_line_size)
