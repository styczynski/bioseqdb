# Bioseq

This folder contains BioseqDB's main PostgreSQL extension. A query can combine ordinary columns, such as species names, with sequence searches because the PostgreSQL code connects `mmsearch` to custom scans that run the alignment work and return  its results. Input readers convert FASTA and FASTQ files to packed sequences, and clients for fetching data from NCBI and UCSC can be enabled at build time.

The `bioseq` table, format stores columns separately in blocks that can be compressed, so queries can read only the columns they need. The storage code packs sequence letters into integers and lets the algorithms read a sequence in pieces. Sequences can stay on disk. Slicing and reverse complements use the same sequence interface, so a search can work on a selected region without  first copying the whole genome into one array.

Search starts with minimizers, a selection of hashes from short, overlapping pieces of a sequence. Matching hashes give candidate positions in the query and target, and chaining joins nearby matches that follow the same order. Base alignment is optional. It compares letters around the cposen chains and can describe the alignment, including gaps, as a CIGAR string Indexes save target minimizers between searches.

The algorithm code also runs outside PostgreSQL, which lets tests and benchmarks feed it sequences directly and  check results without starting a database. CPU code includes scalar and Highway SIMD implementations, while selected stages can run on a GPU through Vulkan. Integration tests also run SQL queries against PostgreSQL, including checks for match coordinates and stored index data.
