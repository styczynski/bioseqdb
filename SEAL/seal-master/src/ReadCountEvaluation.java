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
public class ReadCountEvaluation extends EvaluationProgram
{
	@Parameter(names = "--read-counts")
	/**
	 * This is a String because JCommander keeps wanting to <b>append</b> to a collection
	 * instead of replacing it with command line arguments.
	 */
	protected String readCounts = "1e4,3e4,1e5,3e5,1e6,3e6,1e7,3e7,1e8";

	@Parameter(names = "--genome-size", description = "Genome size")
	/**
	 * Specified as a double for parameter type consistency
	 */
	protected double genomeSize = 500000000.0;

	public void readCountEvaluation()
	{
		List<Double> readCountValues = new DoubleCsvConverter().convert(readCounts);
		final String testDescription = "runtime_read_count";

		List<Double> genomeSizes = Arrays.asList(genomeSize);

		AlignmentToolService ats = new AlignmentToolService(getToolNames());
		AlignmentToolService.RuntimeGenomeData rgd = ats.getRuntimeGenomeData(genomeSizes, readCountValues);
		AlignmentToolService.SimulationParameters pa = new AlignmentToolService.SimulationParameters(readCountValues, false,
			testDescription, AlignmentToolService.Genome.RUNTIME_COV_RANDOM, rgd.genomesBySize.get(genomeSize),
			new TreeMap<Double, File>());
		List<Map<Double, Map<Double, Map<String, AlignmentResults>>>> l = ats.runRuntimeSimulation(pa,
			rgd);
		ats.writeRuntimeResults(pa, l, "ReadCount");
	}

	public static void main(String[] args)
	{
		ReadCountEvaluation rce = new ReadCountEvaluation();
		JCommander jc = new JCommander(rce, args);
		if (rce.showHelp)
		{
			jc.usage();
		}
		else
		{
			rce.readCountEvaluation();
		}
	}
}
