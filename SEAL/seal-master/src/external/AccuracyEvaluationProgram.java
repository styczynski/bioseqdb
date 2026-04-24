package external;

import com.beust.jcommander.Parameter;
import util.DoubleConverter;
import util.GenomeConverter;

public class AccuracyEvaluationProgram extends EvaluationProgram
{
	@Parameter(names = "--length", description = "Length of genome (if generated)")
	protected int generatedGenomeLength = AlignmentToolService.DEFAULT_GENERATED_GENOME_LENGTH;

	@Parameter(names = "--genome", description = "Genome to use; HUMAN and HUMAN_CHR22 require FASTA files to read",
			converter = GenomeConverter.class)
	protected AlignmentToolService.Genome genome = AlignmentToolService.Genome.RANDOM_HARD;

	@Parameter(names = "--fragment-length", description = "Fragment length (mean)")
	protected int fragmentLength = AlignmentToolService.DEFAULT_FRAGMENT_LENGTH_MEAN;

	@Parameter(names = "--fragment-length-sd", description = "Fragment length (std.dev.)",
			converter = DoubleConverter.class)
	protected double fragmentLengthSd = AlignmentToolService.DEFAULT_FRAGMENT_LENGTH_SD;

	@Parameter(names = "--fragment-count", description = "Fragment count")
	protected int fragmentCount = AlignmentToolService.DEFAULT_FRAGMENT_COUNT;
}
