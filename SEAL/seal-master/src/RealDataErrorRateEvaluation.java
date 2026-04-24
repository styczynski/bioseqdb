import com.beust.jcommander.JCommander;
import com.beust.jcommander.Parameter;
import external.AccuracyEvaluationProgram;
import external.AlignmentResults;
import external.AlignmentToolService;
import generator.Fragmentizer;
import generator.SequenceGenerator;
import generator.errors.FragmentErrorGenerator;
import generator.errors.RealDataErrorGenerator;

import java.io.File;
import java.util.*;

public class RealDataErrorRateEvaluation extends AccuracyEvaluationProgram
{
	@Parameter(names = "--read-file")
	protected String realReadFilename = "error_reads.fastq";

	public void errorRateEvaluation()
	{
		List<Double> errorRateValues = Arrays.asList(0.0);
		final String testDescription = "error_rate";

		Map<Double, List<FragmentErrorGenerator>> fegs = new TreeMap<Double, List<FragmentErrorGenerator>>();
		File realReadFile = new File(realReadFilename);
		int limit = 100000;
		FragmentErrorGenerator realErrorGenerator = new RealDataErrorGenerator(SequenceGenerator.NUCLEOTIDES,
				realReadFile, limit);
		List<FragmentErrorGenerator> generatorList = new ArrayList<FragmentErrorGenerator>();
		generatorList.add(realErrorGenerator);
		fegs.put(0.0, generatorList);

		AlignmentToolService ats = new AlignmentToolService(getToolNames());
		Fragmentizer.Options fo = new Fragmentizer.Options();
		fo.fragmentLength = fragmentLength;
		fo.fragmentCount = fragmentCount;
		fo.fragmentLengthSd = fragmentLengthSd;
		AlignmentToolService.ProcessedGenome pg = ats.getGenomeAndFragmentFiles(genome, generatedGenomeLength, fo, fegs,
				testDescription,
				String.format("Introducing fragment read errors from %s ... ", realReadFile.getAbsolutePath()));
		AlignmentToolService.SimulationParameters pa =
				new AlignmentToolService.SimulationParameters(errorRateValues, false,
						testDescription, genome, pg.file, pg.fragmentsByParameter);
		Map<Double, Map<String, AlignmentResults>> m = ats.runAccuracySimulation(pa);
		ats.writeAccuracyResults(pa, m, "ErrorRate");
	}

	public static void main(String[] args)
	{
		RealDataErrorRateEvaluation rdere = new RealDataErrorRateEvaluation();
		JCommander jc = new JCommander(rdere, args);
		if (rdere.showHelp)
		{
			jc.usage();
		}
		else
		{
			rdere.errorRateEvaluation();
		}
	}
}
