"""
  Main entrypoint to the aligner

  @Piotr Styczyński 2023 <piotr@styczynski.in>
  MIT LICENSE
  Algorithms for genomic data analysis | AADG | MIM UW | Bioinformatyka
"""
import sys
from Bio import SeqIO

from aadg_genomics_class.monitoring.logs import LOGS
from aadg_genomics_class.minimizer.minimizer import get_minimizers, MAX_KMER_SIZE
from aadg_genomics_class.minimizer.extender import extend
from aadg_genomics_class.minimizer.prefilter import cleanup_kmer_index
from aadg_genomics_class.sequences import format_sequences, iter_sequences
from aadg_genomics_class.aligner.aligner import align
from aadg_genomics_class.monitoring.task_reporter import TaskReporter


def run_aligner_pipeline(
    reference_file_path: str,
    reads_file_path: str,
    output_file_path: str,
    kmer_len: int,
    window_len: int,
    kmers_cutoff_f: float,
    score_match: int,
    score_mismatch: int,
    score_gap: int,
):
    LOGS.cli.info(f"Invoked CLI with the following args: {' '.join(sys.argv)}")
    
    with TaskReporter("Sequence read alignnment") as reporter:

        with reporter.task("Load target sequence"):
            reference_records, reference_ids = format_sequences(SeqIO.parse(reference_file_path, "fasta"))

        if kmer_len > MAX_KMER_SIZE:
            kmer_len = MAX_KMER_SIZE

        with reporter.task("Create minimizer target index") as target_index_task:
            min_index_target = get_minimizers(
                reference_records[reference_ids[0]],
                kmer_len=kmer_len,
                window_len=window_len,
            )
            try:
                with target_index_task.task("Prefilter target index"):
                    cleanup_kmer_index(
                        index=min_index_target,
                        kmers_cutoff_f=kmers_cutoff_f,
                    )
            except Exception as e:
                target_index_task.fail(e)

        with open(output_file_path, 'w') as output_file:
            for (query_id, query_seq) in iter_sequences(SeqIO.parse(reads_file_path, "fasta")):
                with reporter.task(f"Load query '{query_id}'") as query_task:
                    try:
                        
                        with query_task.task('Get minimizers'):
                            min_index_query = get_minimizers(
                                query_seq,
                                kmer_len=kmer_len,
                                window_len=window_len,
                            )

                        with query_task.task('Extend'):
                            region_match = extend(
                                target_minimizer_index=min_index_target,
                                query_minimizer_index=min_index_query,
                            )

                        with query_task.task('Align'):
                            t_begin, t_end = align(
                                region=region_match,
                                target_seq=reference_records[reference_ids[0]],
                                query_seq=query_seq,
                                kmer_len=kmer_len,
                                full_query_len=len(query_seq),
                                score_match=score_match,
                                score_mismatch=score_mismatch,
                                score_gap=score_gap,
                            )
                        
                        output_file.write(f"{query_id} {t_begin} {t_end}\n")
                    except Exception as e:
                        query_task.fail(e)
            LOGS.cli.info(f"Wrote records to {output_file_path}")
