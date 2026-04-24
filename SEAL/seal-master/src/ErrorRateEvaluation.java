import com.beust.jcommander.JCommander;
import com.beust.jcommander.Parameter;
import external.AccuracyEvaluationProgram;
import external.AlignmentResults;
import external.AlignmentToolService;
import generator.Fragmentizer;
import generator.SequenceGenerator;
import generator.errors.FragmentErrorGenerator;
import generator.errors.IndelGenerator;
import generator.errors.LinearIncreasingErrorGenerator;
import util.DoubleCsvConverter;
import util.GenomeConverter;

import java.util.*;

public class ErrorRateEvaluation extends AccuracyEvaluationProgram
{
	@Parameter(names = "--error-rates")
	/**
	 * This is a String because JCommander keeps wanting to <b>append</b> to a collection
	 * instead of replacing it with command line arguments.
	 */
	protected String errorRates = "0.0,0.001,0.004,0.01,0.025,0.05,0.1";

	public void errorRateEvaluation()
	{
		List<Double> errorRateValues = new DoubleCsvConverter().convert(this.errorRates);
		final String testDescription = "error_rate";

		Map<Double, List<FragmentErrorGenerator>> fegs = new TreeMap<Double, List<FragmentErrorGenerator>>();
		for (double errorProbability : errorRateValues)
		{
			double errorBeginMin = Math.max(0, errorProbability * 0.35);
			double errorBeginMax = Math.min(errorProbability * 0.7, 1.0);
			double errorEndMin = Math.max(0, errorBeginMin * 0.7);
			double errorEndMax = Math.min(errorProbability * 1.3, 1.0);
			FragmentErrorGenerator base_call_eg = new LinearIncreasingErrorGenerator(
					SequenceGenerator.NUCLEOTIDES, errorBeginMin, errorBeginMax, errorEndMin, errorEndMax, 0.04);
			List<FragmentErrorGenerator> generatorList = new ArrayList<FragmentErrorGenerator>();
			generatorList.add(base_call_eg);
			fegs.put(errorProbability, generatorList);
		}

		AlignmentToolService ats = new AlignmentToolService(getToolNames());
		Fragmentizer.Options fo = new Fragmentizer.Options();
		fo.fragmentLength = fragmentLength;
		fo.fragmentCount = fragmentCount;
		fo.fragmentLengthSd = fragmentLengthSd;
		AlignmentToolService.ProcessedGenome pg = ats.getGenomeAndFragmentFiles(genome, generatedGenomeLength, fo, fegs,
				testDescription, "Introducing fragment read errors for error rate %f ... ");
		AlignmentToolService.SimulationParameters pa =
				new AlignmentToolService.SimulationParameters(errorRateValues, false,
						testDescription, genome, pg.file, pg.fragmentsByParameter);
		Map<Double, Map<String, AlignmentResults>> m = ats.runAccuracySimulation(pa);
		ats.writeAccuracyResults(pa, m, "ErrorRate");
	}

	public static void main(String[] args)
	{
		ErrorRateEvaluation ere = new ErrorRateEvaluation();
		JCommander jc = new JCommander(ere, args);
		if (ere.showHelp)
		{
			jc.usage();
		}
		else
		{
			ere.errorRateEvaluation();
		}
	}
}
