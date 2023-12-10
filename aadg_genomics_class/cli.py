from aadg_genomics_class import click_utils as click
from aadg_genomics_class.pipeline import run_aligner_pipeline

@click.command()
@click.argument('target-fasta', help="Target sequence FASTA file path")
@click.argument('query-fasta', help="Query sequences FASTA file path")
@click.argument('output', default="output.txt", help="Output file path")
@click.option('--kmer-len', default=15, show_default=True)
@click.option('--window-len', default=5, show_default=True)
def run_alignment_cli(target_fasta, query_fasta, output, kmer_len, window_len):
    run_aligner_pipeline(
        reference_file_path=target_fasta,
        reads_file_path=query_fasta,
        output_file_path=output,
        kmer_len=kmer_len,
        window_len=window_len,
    )
    

if __name__ == '__main__':
    run_alignment_cli()