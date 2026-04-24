import com.beust.jcommander.JCommander;
import com.beust.jcommander.Parameter;
import external.AlignmentResults;
import external.AlignmentToolService;
import external.AccuracyEvaluationProgram;
import generator.Fragmentizer;
import generator.SequenceGenerator;
import generator.errors.FragmentErrorGenerator;
import generator.errors.IndelGenerator;
import util.DoubleCsvConverter;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;

public class IndelFrequencyEvaluation extends AccuracyEvaluationProgram
{
	@Parameter(names = "--indel-frequencies")
	/**
	 * This is a String because JCommander keeps wanting to <b>append</b> to a collection
	 * instead of replacing it with command line arguments.
	 */
	protected String indelFrequencies = "1e-5,3e-5,1e-4,3e-4,1e-3,3e-3,1e-2";

	public void indelFrequencyEvaluation()
	{
		List<Double> indelFrequencyValues = new DoubleCsvConverter().convert(indelFrequencies);
		final String testDescription = "indel_freq";

		final int indelLengthMean = 2;
		final double indelLengthStdDev = 0.2;
		Map<Double, List<FragmentErrorGenerator>> fegs = new TreeMap<Double, List<FragmentErrorGenerator>>();
		for (double indelFrequency : indelFrequencyValues)
		{
			IndelGenerator.Options igo = new IndelGenerator.Options();
			igo.deleteLengthMean = indelLengthMean;
			igo.deleteLengthStdDev = indelLengthStdDev;
			igo.deleteProbability = indelFrequency;
			igo.insertLengthMean = indelLengthMean;
			igo.insertLengthStdDev = indelLengthStdDev;
			igo.insertProbability = indelFrequency;
			FragmentErrorGenerator indel_eg = new IndelGenerator(SequenceGenerator.NUCLEOTIDES, igo);
			List<FragmentErrorGenerator> generatorList = new ArrayList<FragmentErrorGenerator>();
			generatorList.add(indel_eg);
			fegs.put(indelFrequency, generatorList);
		}

		AlignmentToolService ats = new AlignmentToolService(getToolNames());
		Fragmentizer.Options fo = new Fragmentizer.Options();
		fo.fragmentLength = fragmentLength;
		fo.fragmentCount = fragmentCount;
		fo.fragmentLengthSd = fragmentLengthSd;
		AlignmentToolService.ProcessedGenome pg = ats.getGenomeAndFragmentFiles(genome, generatedGenomeLength, fo, fegs,
				testDescription, "Introducing fragment read errors for indel frequency %.0f ... ");
		AlignmentToolService.SimulationParameters pa =
				new AlignmentToolService.SimulationParameters(indelFrequencyValues, false,
						testDescription, genome, pg.file, pg.fragmentsByParameter);
		Map<Double, Map<String, AlignmentResults>> m = ats.runAccuracySimulation(pa);
		ats.writeAccuracyResults(pa, m, "IndelFrequency");
	}

	public static void main(String[] args)
	{
		IndelFrequencyEvaluation ife = new IndelFrequencyEvaluation();
		JCommander jc = new JCommander(ife, args);
		if (ife.showHelp)
		{
			jc.usage();
		}
		else
		{
			ife.indelFrequencyEvaluation();
		}
	}
}
