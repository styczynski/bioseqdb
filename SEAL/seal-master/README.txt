SEAL Sequencing Simulation/Evaluation Suite

http://compbio.case.edu/seal/

SEAL is a comprehensive sequencing simulation and alignment tool evaluation
suite. This software (implemented in Java) provides several utilities that can
be used to evaluate alignment algorithms, including:

 * Reading a pre-existing reference genome from one or more FASTA files.
 * Alternatively, generating an artificial reference genome based on input
   parameters (length, repeat count, repeat length, repeat variability rate).
 * Simulating reads from random locations in the genome based on input
   parameters of read length, coverage, sequencing error rate, and indel rate.
 * Applying alignment tools to the genome and the reads through a standardized
   interface.
 * Parsing the output of the alignment tool and calculating the number of reads
   that were correctly or incorrectly mapped.
 * Computing run times and measures of accuracy.

SEAL has interfaces to evaluate the following software packages:

 * Bowtie
 * BWA
 * MAQ
 * mrFAST
 * mrsFAST
 * Novoalign
 * SHRiMP
 * SOAPv2

== Prerequisites ==

 * Some type of Linux distribution. Cygwin may work but has not been tested.
 * Java 1.6. Sun Java is preferred; OpenJDK should work but has not been
   tested.
 * Each alignment tool that you'd like to evaluate must be in your PATH.
 * At least 8GB RAM depending on your usage (large numbers of reads and larger
   genomes may require more).

== Usage Instructions ==

1. Enter the 'build' directory, and append the required .jar files to your
   CLASSPATH environment variable:

   $ . classpath.sh

2. Run the appropriate evaluation program (one of the following:)

   $ java -Xmx16G ErrorRateEvaluation
   $ java -Xmx16G GenomeSizeEvaluation
   $ java -Xmx16G IndelFrequencyEvaluation
   $ java -Xmx16G IndelSizeEvaluation
   $ java -Xmx16G ReadCountEvaluation

== Notes ==

The "16G" memory usage is a suggestion that's based on the default genome
sizes and read counts; with a 500Mbp generated genome and 100,000 reads you
may need as little as 4GB. This memory usage is for SEAL itself, not each
alignment tool that is evaluated.

Each evaluation program accepts command-line options to specify which genome
to use, parameter values, etc. To view the options for each program, run e.g.

$ java ErrorRateEvaluation --help

Parameter values must be specified as a comma-separated list with no spaces
between elements, e.g.

$ java -Xmx16G ErrorRateEvaluation --error-rates 0.0,0.1,0.2

Defined genomes for accuracy simulations:

 * HUMAN_CHR22
 * HUMAN
 * RANDOM_EASY
 * RANDOM_HARD

RANDOM_EASY and RANDOM_HARD specify randomly generated genomes; "easy" signifies
no repeats, "hard" specifies 100 repeats of length 500bp each.

If you specify HUMAN_CHR22, this data (in FASTA format) will be read from the
file "build/data/chr22.fa". HUMAN requires *two* FASTA files:
"build/data/hg19-1_2.fa" and "build/data/hg19-2_2.fa". These files can be
obtained from the SEAL website, or can be produced by concatenating chromosome
FASTA files together. (The human genome is split into two pieces of length
~1.5Gbp to avoid a limitation in Java: Strings cannot have length greater than
Integer.MAX_VALUE = 2^31 - 1).

Results are written in CSV format to "build/data/[eval_type]_[genome].csv". The
accuracy format is:

   Tool,ErrorRate,Threshold,Precision,Recall,Time,UsedReadRatio

The runtime format is:

   Tool,GenomeSize,ReadCount,PreprocessingTime,AlignmentTime,PostprocessingTime,
      TotalTime
