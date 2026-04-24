import com.beust.jcommander.JCommander;
import com.beust.jcommander.Parameter;
import external.AccuracyEvaluationProgram;
import external.AlignmentResults;
import external.AlignmentToolService;
import external.EvaluationProgram;
import generator.Fragmentizer;
import generator.SequenceGenerator;
import generator.errors.FragmentErrorGenerator;
import generator.errors.IndelGenerator;
import util.DoubleCsvConverter;
import util.GenomeConverter;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;

public class IndelSizeEvaluation extends AccuracyEvaluationProgram
{
	@Parameter(names = "--indel_sizes")
	/**
	 * This is a String because JCommander keeps wanting to <b>append</b> to a collection
	 * instead of replacing it with command line arguments.
	 */
	protected String indelSizes = "2,4,7,10,16";

	public void indelSizeEvaluation()
	{
		List<Double> indelSizeValues = new DoubleCsvConverter().convert(indelSizes);
		final String testDescription = "indel_size";

		final double indelLengthStdDev = 0.2;
		final double indelFrequency = 5e-2;
		Map<Double, List<FragmentErrorGenerator>> fegs = new TreeMap<Double, List<FragmentErrorGenerator>>();
		for (double indelSize : indelSizeValues)
		{
			IndelGenerator.Options igo = new IndelGenerator.Options();
			igo.deleteLengthMean = indelSize;
			igo.deleteLengthStdDev = indelLengthStdDev;
			igo.deleteProbability = indelFrequency;
			igo.insertLengthMean = indelSize;
			igo.insertLengthStdDev = indelLengthStdDev;
			igo.insertProbability = indelFrequency;
			FragmentErrorGenerator indel_eg = new IndelGenerator(SequenceGenerator.NUCLEOTIDES, igo);
			List<FragmentErrorGenerator> generatorList = new ArrayList<FragmentErrorGenerator>();
			generatorList.add(indel_eg);
			fegs.put(indelSize, generatorList);
		}

		AlignmentToolService ats = new AlignmentToolService(getToolNames());
		Fragmentizer.Options fo = new Fragmentizer.Options();
		fo.fragmentLength = fragmentLength;
		fo.fragmentCount = fragmentCount;
		fo.fragmentLengthSd = fragmentLengthSd;
		AlignmentToolService.ProcessedGenome pg = ats.getGenomeAndFragmentFiles(genome, generatedGenomeLength, fo, fegs,
				testDescription, "Introducing fragment read errors for indel size %.0f ... ");
		AlignmentToolService.SimulationParameters pa =
				new AlignmentToolService.SimulationParameters(indelSizeValues, false,
						testDescription, genome, pg.file, pg.fragmentsByParameter);
		Map<Double, Map<String, AlignmentResults>> m = ats.runAccuracySimulation(pa);
		ats.writeAccuracyResults(pa, m, "IndelSize");
	}

	public static void main(String[] args)
	{
		IndelSizeEvaluation ise = new IndelSizeEvaluation();
		JCommander jc = new JCommander(ise, args);
		if (ise.showHelp)
		{
			jc.usage();
		}
		else
		{
			ise.indelSizeEvaluation();
		}
	}
}
