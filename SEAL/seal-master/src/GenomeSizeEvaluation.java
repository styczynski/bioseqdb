import com.beust.jcommander.JCommander;
import com.beust.jcommander.Parameter;
import external.AlignmentResults;
import external.AlignmentToolService;
import external.EvaluationProgram;
import util.DoubleCsvConverter;

import java.io.File;
import java.util.*;

/**
 * TODO: Clean up parameter duplication between these standalone files
 */
public class GenomeSizeEvaluation extends EvaluationProgram
{
	@Parameter(names = "--genome-sizes")
	/**
	 * This is a String because JCommander keeps wanting to <b>append</b> to a collection
	 * instead of replacing it with command line arguments.
	 */
	protected String genomeSizes = "1e6,3e6,1e7,3e7,1e8,3e8,5e8";

	@Parameter(names = "--fragment-count", description = "Fragment count")
	protected int fragmentCount = AlignmentToolService.DEFAULT_FRAGMENT_COUNT;

	public void genomeSizeEvaluation()
	{
		List<Double> genomeSizeValues = new DoubleCsvConverter().convert(genomeSizes);
		final String testDescription = "runtime_genome_size";

		final double readCount = fragmentCount;
		List<Double> readCounts = Arrays.asList(readCount);

		AlignmentToolService ats = new AlignmentToolService(getToolNames());
		AlignmentToolService.RuntimeGenomeData rgd = ats.getRuntimeGenomeData(genomeSizeValues, readCounts);
		AlignmentToolService.SimulationParameters pa = new AlignmentToolService.SimulationParameters(genomeSizeValues, false,
			testDescription, AlignmentToolService.Genome.RUNTIME_SIZE_RANDOM, null, new TreeMap<Double, File>());
		List<Map<Double, Map<Double, Map<String, AlignmentResults>>>> l = ats.runRuntimeSimulation(pa,
			rgd);
		ats.writeRuntimeResults(pa, l, "GenomeSize");
	}

	public static void main(String[] args)
	{
		GenomeSizeEvaluation gse = new GenomeSizeEvaluation();
		JCommander jc = new JCommander(gse, args);
		if (gse.showHelp)
		{
			jc.usage();
		}
		else
		{
			gse.genomeSizeEvaluation();
		}
	}
}
