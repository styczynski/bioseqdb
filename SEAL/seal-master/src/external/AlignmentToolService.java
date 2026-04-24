package external;

import external.AlignmentToolInterface.AlignmentOperation;
import external.AlignmentToolInterface.Options;
import external.tool.*;
import generator.Fragmentizer;
import generator.SeqGenSingleSequenceMultipleRepeats;
import generator.SequenceGenerator;
import generator.errors.FragmentErrorGenerator;
import generator.errors.IndelGenerator;
import generator.errors.UniformErrorGenerator;
import io.FastaReader;
import io.FastaWriter;
import io.FastqWriter;

import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.util.*;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;

import assembly.Fragment;

import io.MultipartSequence;
import org.apache.log4j.Logger;
import util.Log4jConfig;

public class AlignmentToolService
{
	/**
	 * The number of CPUs in your system (maybe - 1) is a good value for this
	 */
	private static final int NUMBER_OF_CONCURRENT_THREADS = 2;
	private static final int EVAL_RUN_COUNT = 1;

	public static final int DEFAULT_GENERATED_GENOME_LENGTH = 500000000;
	public static final int DEFAULT_FRAGMENT_LENGTH_MEAN = 50;
	public static final double DEFAULT_FRAGMENT_LENGTH_SD = 1.0;
	public static final int DEFAULT_FRAGMENT_COUNT = 100000;

	protected static final List<Integer> PHRED_THRESHOLDS = Collections.unmodifiableList(Arrays.asList(
			0, 1, 2, 3, 4, 5, 7, 10, 14, 20, 25, 30, 35, 40));
	protected static final List<Integer> RUNTIME_THRESHOLDS = Collections.unmodifiableList(Arrays.asList(0));

	private static final File DATA_PATH = new File("data");

	private Logger log;

	private Set<String> toolNames;

	public AlignmentToolService(List<String> toolNames_)
	{
		Log4jConfig.initialConfig();
		log = Logger.getLogger(getClass());
		toolNames = Collections.unmodifiableSet(new HashSet<String>(toolNames_));
	}

	public static enum Genome
	{
		HUMAN_CHR22,
		HUMAN,
		RANDOM_EASY,
		RANDOM_HARD,
		RUNTIME_COV_RANDOM,
		RUNTIME_SIZE_RANDOM,
	}

	/**
	 * I <b>really</b> wish I could just return a tuple instead of making
	 * classes like this :(
	 *
	 * @author mruffalo
	 */
	public static class ProcessedGenome
	{
		public ProcessedGenome(File file_, Map<Double, File> fragmentsByParameter_)
		{
			file = file_;
			fragmentsByParameter = Collections.unmodifiableMap(fragmentsByParameter_);
		}

		/**
		 * The file in which this genome has been written to disk -- not where
		 * it was read from if applicable
		 */
		public final File file;
		public final Map<Double, File> fragmentsByParameter;
	}

	/**
	 * TODO: Figure out if I should refactor this
	 *
	 * @author mruffalo
	 */
	public static class SimulationParameters
	{
		public SimulationParameters(List<Double> errorRates_, boolean paired_end_,
				String testDescription_, Genome genome_, File genomeFile_,
				Map<Double, File> fragmentsByError_)
		{
			parameterList = Collections.unmodifiableList(errorRates_);
			paired_end = paired_end_;
			testDescription = testDescription_;
			genome = genome_;
			genomeFile = genomeFile_;
			fragmentsByError = Collections.unmodifiableMap(fragmentsByError_);
		}

		/**
		 * This could be:
		 * <ul>
		 * <li>base call error rate</li>
		 * <li>indel size</li>
		 * <li>indel frequency</li>
		 * <li>genome size</li>
		 * <li>read coverage</li>
		 * </ul>
		 */
		public final List<Double> parameterList;
		public final boolean paired_end;
		public final String testDescription;
		/**
		 * TODO: Refactor this for runtime genome size eval
		 */
		public final Genome genome;
		/**
		 * TODO: Refactor this for runtime genome size eval
		 */
		public final File genomeFile;
		/**
		 * TODO: Refactor this for runtime genome size eval
		 */
		public final Map<Double, File> fragmentsByError;
	}

	/**
	 * TODO: Refactor this, probably by moving it into
	 * {@link SimulationParameters} or {@link ProcessedGenome}
	 *
	 * @author mruffalo
	 */
	public static class RuntimeGenomeData
	{
		public RuntimeGenomeData(Map<Double, File> genomesBySize_,
				Map<Double, Map<Double, File>> fragmentsByCoverage_)
		{
			genomesBySize = Collections.unmodifiableMap(genomesBySize_);
			fragmentsByReadCount = Collections.unmodifiableMap(fragmentsByCoverage_);
		}

		public final Map<Double, File> genomesBySize;
		/**
		 * First level: genome size, second level: read coverage
		 */
		public final Map<Double, Map<Double, File>> fragmentsByReadCount;
	}

	public ProcessedGenome getGenomeAndFragmentFiles(Genome genome, int generated_genome_length,
			Fragmentizer.Options fo,
			Map<Double, List<FragmentErrorGenerator>> fragmentErrorGenerators, String testDescription,
			String parameterMessage)
	{
		DATA_PATH.mkdirs();

		File genomeFile = null;
		CharSequence sequence = null;
		List<MultipartSequence> sequences = new ArrayList<MultipartSequence>();
		SequenceGenerator g = null;
		SequenceGenerator.Options sgo = null;
		log.info("Reading/creating genome");
		switch (genome)
		{
			case HUMAN_CHR22:
				final File chr22 = new File(DATA_PATH, "chr22.fa");
				try
				{
					/*
					 * Don't worry about casting file size to an int: we can't
					 * have strings longer than Integer.MAX_VALUE anyway
					 */
					sequence = FastaReader.getSequence(chr22, (int) chr22.length());
				}
				catch (IOException e)
				{
					e.printStackTrace();
				}
				genomeFile = new File(DATA_PATH, "chr22-contig.fa");
				break;
			case HUMAN:
				List<File> hg19_files = new ArrayList<File>(2);
				hg19_files.add(new File(DATA_PATH, "hg19-1_2.fa"));
				hg19_files.add(new File(DATA_PATH, "hg19-2_2.fa"));
				sequences = FastaReader.getSequences(hg19_files);
				genomeFile = new File(DATA_PATH, "hg19-contig.fa");
				break;
			case RANDOM_EASY:
				genomeFile = new File(DATA_PATH, "random_easy.fa");
				g = new SeqGenSingleSequenceMultipleRepeats();
				sgo = new SequenceGenerator.Options();
				sgo.length = generated_genome_length;
				sgo.repeatCount = 0;
				sequence = g.generateSequence(sgo);
				break;
			case RANDOM_HARD:
				genomeFile = new File(DATA_PATH, "random_hard.fa");
				g = new SeqGenSingleSequenceMultipleRepeats();
				sgo = new SequenceGenerator.Options();
				sgo.length = generated_genome_length;
				sgo.repeatCount = 100;
				sgo.repeatLength = 500;
				sgo.repeatErrorProbability = 0.03;
				sequence = g.generateSequence(sgo);
				break;
			default:
				break;
		}
		List<Fragment> list = null;
		// TODO refactor this
		if (genome.equals(Genome.HUMAN))
		{
			long genomeLength = 0L;
			for (MultipartSequence s : sequences)
			{
				genomeLength += s.sequence.length();
			}
			log.info(String.format("Genome length: %d", genomeLength));

			log.info(String.format("Writing genome to %s", genomeFile.getAbsolutePath()));
			try
			{
				FastaWriter.writeMultipartSequences(sequences, genomeFile);
			}
			catch (IOException e)
			{
				e.printStackTrace();
			}

			fo.fragmentLength = 50;
			fo.fragmentCount = 50000;
			fo.fragmentLengthSd = 1;

			log.info("Reading fragments");
			list = Fragmentizer.fragmentize(sequences, fo);
		}
		else
		{
			log.info(String.format("Genome length: %d", sequence.length()));

			log.info(String.format("Writing genome to %s", genomeFile.getAbsolutePath()));
			writeGenome(sequence, genomeFile);

			log.info("Reading fragments");
			list = Fragmentizer.fragmentize(sequence, fo);
		}

		Map<Double, File> fragmentsByError = new TreeMap<Double, File>();
		for (Map.Entry<Double, List<FragmentErrorGenerator>> e : fragmentErrorGenerators.entrySet())
		{
			double parameter = e.getKey();
			List<FragmentErrorGenerator> errorGenerators = e.getValue();

			log.info(String.format(parameterMessage, parameter));
			String error_identifier = Double.toString(parameter).replace('.', '_');
			String filename = String.format("fragments-%s-%s.fastq", testDescription,
					error_identifier);
			File fragmentFile = new File(DATA_PATH, filename);
			fragmentsByError.put(parameter, fragmentFile);
			FragmentErrorGenerator.generateErrorsToFile(errorGenerators, list, fragmentFile);
		}

		return new ProcessedGenome(genomeFile, fragmentsByError);
	}

	public RuntimeGenomeData getRuntimeGenomeData(List<Double> genomeSizes,
			List<Double> fragmentCounts)
	{
		DATA_PATH.mkdirs();
		final double errorProbability = 0.01;
		Map<Double, File> genomeFilesBySize = new TreeMap<Double, File>();
		Map<Double, Map<Double, File>> fragmentsByCoverage = new TreeMap<Double, Map<Double, File>>();
		for (double genomeSize : genomeSizes)
		{
			String filename = String.format("runtime_genome_%.0f.fa", genomeSize);
			File genomeFile = new File(DATA_PATH, filename);
			genomeFilesBySize.put(genomeSize, genomeFile);
			SequenceGenerator g = new SeqGenSingleSequenceMultipleRepeats();
			SequenceGenerator.Options sgo = new SequenceGenerator.Options();
			sgo.length = (int) genomeSize;
			log.info(String.format("Generating sequence of length %d ... ", sgo.length));
			/*
			 * Can't generate sequence directly to a file; need to keep it in
			 * memory in order to read from it
			 */
			CharSequence sequence = g.generateSequence(sgo);
			log.info(String.format("Writing genome to %s ... ", genomeFile.getAbsolutePath()));
			writeGenome(sequence, genomeFile);
			System.out.println("done.");
			Map<Double, File> fragmentsForThisGenome = new TreeMap<Double, File>();
			fragmentsByCoverage.put(genomeSize, fragmentsForThisGenome);
			for (double fragmentCount : fragmentCounts)
			{
				Fragmentizer.Options fo = new Fragmentizer.Options();
				fo.fragmentLength = 50;
				/*
				 * Integer truncation is exactly what we want here
				 */
				fo.fragmentCount = (int) fragmentCount;
				fo.fragmentLengthSd = 1;

				// TODO: write fragments with fragmentizeToFile
				log.info(String.format("Reading %d fragments...", fo.fragmentCount));
				List<? extends Fragment> list = Fragmentizer.fragmentize(sequence, fo);

				String fragmentFilename = String.format("runtime_fragments_%.0f_%.0f.fastq",
						genomeSize, fragmentCount);
				File fragmentFile = new File(DATA_PATH, fragmentFilename);
				fragmentsForThisGenome.put(fragmentCount, fragmentFile);

				log.info("Introducing fragment read errors...");
				UniformErrorGenerator eg = new UniformErrorGenerator(SequenceGenerator.NUCLEOTIDES,
						errorProbability);
				List<FragmentErrorGenerator> fegs = new ArrayList<FragmentErrorGenerator>();
				fegs.add(eg);
				FragmentErrorGenerator.generateErrorsToFile(fegs, list, fragmentFile);
			}
		}

		return new RuntimeGenomeData(genomeFilesBySize, fragmentsByCoverage);
	}

	public Map<Double, Map<String, AlignmentResults>> runAccuracySimulation(SimulationParameters p)
	{
		ExecutorService pool = Executors.newFixedThreadPool(NUMBER_OF_CONCURRENT_THREADS);
		List<AlignmentToolInterface> atiList = new ArrayList<AlignmentToolInterface>();

		Map<Double, Map<String, AlignmentResults>> m =
				Collections.synchronizedMap(new TreeMap<Double, Map<String, AlignmentResults>>());
		List<Future<AlignmentResults>> futureList = new ArrayList<Future<AlignmentResults>>();

		int index = 0;
		for (double errorProbability : p.parameterList)
		{
			Map<String, AlignmentResults> m_ep = Collections.synchronizedMap(new TreeMap<String, AlignmentResults>());
			m.put(errorProbability, m_ep);

			List<AlignmentToolInterface> alignmentInterfaceList = new ArrayList<AlignmentToolInterface>();

			Options o;
			// TODO make this smarter and don't hardcode tool names to check
			if (toolNames.contains("mrfast"))
			{
				o = new Options(p.paired_end, errorProbability);
				o.penalize_duplicate_mappings = false;
				alignmentInterfaceList.add(new MrFastInterface(++index, "MrFast-R", PHRED_THRESHOLDS,
						o, m_ep));
				alignmentInterfaceList.add(new MrFastInterface(++index, "MrFast-S", PHRED_THRESHOLDS,
						new Options(p.paired_end, errorProbability), m_ep));
			}
			if (toolNames.contains("mrsfast"))
			{
				o = new Options(p.paired_end, errorProbability);
				o.penalize_duplicate_mappings = false;
				alignmentInterfaceList.add(new MrsFastInterface(++index, "MrsFast-R", PHRED_THRESHOLDS,
						o, m_ep));
				alignmentInterfaceList.add(new MrsFastInterface(++index, "MrsFast-S", PHRED_THRESHOLDS,
						new Options(p.paired_end, errorProbability), m_ep));
			}
			if (toolNames.contains("shrimp"))
			{
				o = new Options(p.paired_end, errorProbability);
				o.penalize_duplicate_mappings = false;
				alignmentInterfaceList.add(new ShrimpInterface(++index, "SHRiMP-R", PHRED_THRESHOLDS,
						o, m_ep));
				alignmentInterfaceList.add(new ShrimpInterface(++index, "SHRiMP-S", PHRED_THRESHOLDS,
						new Options(p.paired_end, errorProbability), m_ep));
			}
			if (toolNames.contains("soap"))
			{
				alignmentInterfaceList.add(new SoapInterface(++index, "SOAP", PHRED_THRESHOLDS,
						new Options(p.paired_end, errorProbability), m_ep));
			}
			if (toolNames.contains("bwa"))
			{
				alignmentInterfaceList.add(new BwaInterface(++index, "BWA", PHRED_THRESHOLDS,
						new Options(p.paired_end, errorProbability), m_ep));
			}
			if (toolNames.contains("bowtie"))
			{
				alignmentInterfaceList.add(new BowtieInterface(++index, "Bowtie", PHRED_THRESHOLDS,
						new Options(p.paired_end, errorProbability), m_ep));
			}
			if (toolNames.contains("novoalign"))
			{
				alignmentInterfaceList.add(new NovoalignInterface(++index, "Novoalign",
						PHRED_THRESHOLDS, new Options(p.paired_end, errorProbability), m_ep));
			}
			if (toolNames.contains("gsnap"))
			{
				alignmentInterfaceList.add(new GsnapInterface(++index, "GSNAP",
						PHRED_THRESHOLDS, new Options(p.paired_end, errorProbability), m_ep));
			}

			for (AlignmentToolInterface ati : alignmentInterfaceList)
			{
				ati.o.tool_path = new File(DATA_PATH, String.format("%03d-%s-%s-%s", ati.index,
						ati.description, p.testDescription, p.genome.toString().toLowerCase()));
				ati.o.tool_path.mkdirs();
				ati.o.genome = new File(ati.o.tool_path, "genome.fasta");
				ati.o.binary_genome = new File(ati.o.tool_path, "genome.bfa");
				ati.o.orig_genome = p.genomeFile;

				int read_count = p.paired_end ? 2 : 1;
				for (int i = 1; i <= read_count; i++)
				{
					Options.Reads r = new Options.Reads(i);
					r.reads = new File(ati.o.tool_path, String.format("fragments%d.fastq", i));
					r.binary_reads = new File(ati.o.tool_path, String.format("fragments%d.bfq", i));
					r.aligned_reads = new File(ati.o.tool_path, String.format("alignment%d.sai", i));
					r.orig_reads = p.fragmentsByError.get(errorProbability);
					ati.o.reads.add(r);
				}
				ati.o.raw_output = new File(ati.o.tool_path, "out.raw");
				ati.o.sam_output = new File(ati.o.tool_path, "alignment.sam");
				ati.o.converted_output = new File(ati.o.tool_path, "out.txt");
				ati.o.roc_output = new File(ati.o.tool_path, "roc.csv");

				ati.initLogger();
				log.info(String.format("%03d %s: %f", ati.index, ati.description, ati.o.error_rate));

				atiList.add(ati);
			}
		}
		for (AlignmentToolInterface ati : atiList)
		{
			futureList.add(pool.submit(ati));
		}

		pool.shutdown();
		for (Future<AlignmentResults> f : futureList)
		{
			try
			{
				f.get();
			}
			catch (InterruptedException e)
			{
				e.printStackTrace();
			}
			catch (ExecutionException e)
			{
				e.printStackTrace();
			}
		}
		return m;
	}

	/**
	 * TODO: Don't duplicate code in
	 * {@link #runAccuracySimulation(SimulationParameters)}
	 *
	 * @param p
	 * @return
	 */
	public List<Map<Double, Map<Double, Map<String, AlignmentResults>>>> runRuntimeSimulation(
			SimulationParameters p, RuntimeGenomeData rgd)
	{
		ExecutorService pool = Executors.newFixedThreadPool(NUMBER_OF_CONCURRENT_THREADS);
		List<AlignmentToolInterface> atiList = new ArrayList<AlignmentToolInterface>();

		List<Map<Double, Map<Double, Map<String, AlignmentResults>>>> l =
				new ArrayList<Map<Double, Map<Double, Map<String, AlignmentResults>>>>(
						EVAL_RUN_COUNT);
		List<Future<AlignmentResults>> futureList = new ArrayList<Future<AlignmentResults>>(
				rgd.fragmentsByReadCount.size() * EVAL_RUN_COUNT * 7);
		int index = 0;
		for (int which_run = 0; which_run < EVAL_RUN_COUNT; which_run++)
		{
			Map<Double, Map<Double, Map<String, AlignmentResults>>> m =
					Collections.synchronizedMap(new TreeMap<Double, Map<Double, Map<String, AlignmentResults>>>());
			l.add(m);

			for (double genomeSize : rgd.fragmentsByReadCount.keySet())
			{
				Map<Double, Map<String, AlignmentResults>> m_gs =
						Collections.synchronizedMap(new TreeMap<Double, Map<String, AlignmentResults>>());
				m.put(genomeSize, m_gs);
				for (double readCount : rgd.fragmentsByReadCount.get(genomeSize).keySet())
				{
					Map<String, AlignmentResults> m_c =
							Collections.synchronizedMap(new TreeMap<String, AlignmentResults>());
					m_gs.put(readCount, m_c);

					List<AlignmentToolInterface> alignmentInterfaceList = new ArrayList<AlignmentToolInterface>();

					// TODO make this smarter and don't hardcode tool names to check
					if (toolNames.contains("mrfast"))
					{
						alignmentInterfaceList.add(new MrFastInterface(++index, "MrFast",
								RUNTIME_THRESHOLDS, new Options(p.paired_end, readCount), m_c));
					}
					if (toolNames.contains("mrsfast"))
					{
						alignmentInterfaceList.add(new MrsFastInterface(++index, "MrsFast",
								RUNTIME_THRESHOLDS, new Options(p.paired_end, readCount), m_c));
					}
					if (toolNames.contains("soap"))
					{
						alignmentInterfaceList.add(new SoapInterface(++index, "SOAP",
								RUNTIME_THRESHOLDS, new Options(p.paired_end, readCount), m_c));
					}
					if (toolNames.contains("bwa"))
					{
						alignmentInterfaceList.add(new BwaInterface(++index, "BWA", RUNTIME_THRESHOLDS,
								new Options(p.paired_end, readCount), m_c));
					}
					if (toolNames.contains("shrimp"))
					{
						alignmentInterfaceList.add(new ShrimpInterface(++index, "SHRiMP",
								RUNTIME_THRESHOLDS, new Options(p.paired_end, readCount), m_c));
					}
					if (toolNames.contains("bowtie"))
					{
						alignmentInterfaceList.add(new BowtieInterface(++index, "Bowtie",
								RUNTIME_THRESHOLDS, new Options(p.paired_end, readCount), m_c));
					}
					if (toolNames.contains("novoalign"))
					{
						alignmentInterfaceList.add(new NovoalignInterface(++index, "Novoalign",
								RUNTIME_THRESHOLDS, new Options(p.paired_end, readCount), m_c));
					}
					if (toolNames.contains("gsnap"))
					{
						alignmentInterfaceList.add(new GsnapInterface(++index, "GSNAP",
								RUNTIME_THRESHOLDS, new Options(p.paired_end, readCount), m_c));
					}

					for (AlignmentToolInterface ati : alignmentInterfaceList)
					{
						ati.o.tool_path = new File(DATA_PATH, String.format("%03d-%s-%s-%s",
								ati.index, ati.description, p.testDescription,
								p.genome.toString().toLowerCase()));
						ati.o.tool_path.mkdirs();
						ati.o.orig_genome = rgd.genomesBySize.get(genomeSize);
						ati.o.genome = new File(ati.o.tool_path, "genome.fasta");
						ati.o.binary_genome = new File(ati.o.tool_path, "genome.bfa");

						Options.Reads r = new Options.Reads(1);
						r.reads = new File(ati.o.tool_path, String.format("fragments%d.fastq", 1));
						r.binary_reads = new File(ati.o.tool_path, String.format("fragments%d.bfq", 1));
						r.aligned_reads = new File(ati.o.tool_path, String.format("alignment%d.sai", 1));
						r.orig_reads = rgd.fragmentsByReadCount.get(genomeSize).get(readCount);
						ati.o.reads.add(r);

						ati.o.raw_output = new File(ati.o.tool_path, "out.raw");
						ati.o.sam_output = new File(ati.o.tool_path, "alignment.sam");
						ati.o.converted_output = new File(ati.o.tool_path, "out.txt");
						ati.o.roc_output = new File(ati.o.tool_path, "roc.csv");

						ati.initLogger();
						log.info(String.format("%03d %s: %.0f%n", ati.index, ati.description, readCount));

						atiList.add(ati);
					}
				}
			}
		}
		log.info(String.format("Running %d tool evaluations.", atiList.size()));
		for (AlignmentToolInterface ati : atiList)
		{
			futureList.add(pool.submit(ati));
		}
		pool.shutdown();
		for (Future<AlignmentResults> f : futureList)
		{
			try
			{
				f.get();
			}
			catch (InterruptedException e)
			{
				e.printStackTrace();
			}
			catch (ExecutionException e)
			{
				e.printStackTrace();
			}
		}
		return l;
	}

	public void writeAccuracyResults(SimulationParameters pa,
			Map<Double, Map<String, AlignmentResults>> m, String parameterName)
	{
		String filename = String.format("%s_%s.csv", pa.testDescription,
				pa.genome.toString().toLowerCase());
		String roc_filename = String.format("%s_%s_roc.csv", pa.testDescription,
				pa.genome.toString().toLowerCase());
		try
		{
			log.info(String.format("Writing results to %s", filename));
			FileWriter w = new FileWriter(new File(DATA_PATH, filename));
			w.write(String.format("%s,%s,%s,%s,%s,%s,%s%n", "Tool", parameterName, "Threshold",
					"Precision", "Recall", "Time", "UsedReadRatio"));
			for (Double d : m.keySet())
			{
				for (String s : m.get(d).keySet())
				{
					for (Integer i : PHRED_THRESHOLDS)
					{
						AlignmentResults ar = m.get(d).get(s);
						FilteredAlignmentResults r = ar.filter(i);
						w.write(String.format("%s,%f,%d,%f,%f,%d,%f%n", s, d, i, r.getPrecision(),
								r.getRecall(), ar.timeMap.get(AlignmentOperation.TOTAL),
								r.getUsedReadRatio()));
					}
				}
			}
			w.close();

			log.info(String.format("Writing overall ROC data to %s", roc_filename));
			w = new FileWriter(new File(DATA_PATH, roc_filename));
			w.write(String.format("%s,%s,%s,%s%n", "Tool", parameterName, "Score", "Label"));
			for (Double d : m.keySet())
			{
				for (String s : m.get(d).keySet())
				{
					// TODO: Don't duplicate code here
					AlignmentResults r = m.get(d).get(s);
					for (Map.Entry<Integer, Integer> p : r.positives.entrySet())
					{
						int score = p.getKey();
						int count = p.getValue();
						for (int i = 0; i < count; i++)
						{
							w.write(String.format("%s,%f,%d,%d%n", s, d, score, 1));
						}
					}
					for (Map.Entry<Integer, Integer> n : r.negatives.entrySet())
					{
						int score = n.getKey();
						int count = n.getValue();
						for (int i = 0; i < count; i++)
						{
							w.write(String.format("%s,%f,%d,%d%n", s, d, score, 0));
						}
					}
				}
			}
			w.close();
		}
		catch (IOException e)
		{
			e.printStackTrace();
		}
	}

	/**
	 * TODO: Don't duplicate code from {@link #writeAccuracyResults}
	 *
	 * @param pa
	 * @param l
	 * @param parameterName
	 */
	public void writeRuntimeResults(SimulationParameters pa,
			List<Map<Double, Map<Double, Map<String, AlignmentResults>>>> l, String parameterName)
	{
		String filename = pa.testDescription + "_data.csv";
		log.info(String.format("Writing time data to %s", filename));
		try
		{
			FileWriter w = new FileWriter(new File(DATA_PATH, filename));
			w.write(String.format(
					"Tool,GenomeSize,ReadCount,PreprocessingTime,AlignmentTime,PostprocessingTime,TotalTime%n"));
			for (Map<Double, Map<Double, Map<String, AlignmentResults>>> m : l)
			{
				for (Double gs : m.keySet())
				{
					for (Double c : m.get(gs).keySet())
					{
						for (String s : m.get(gs).get(c).keySet())
						{
							AlignmentResults r = m.get(gs).get(c).get(s);
							w.write(String.format("%s,%.0f,%f,%d,%d,%d,%d%n", s, gs, c,
									r.timeMap.get(AlignmentOperation.PREPROCESSING),
									r.timeMap.get(AlignmentOperation.ALIGNMENT),
									r.timeMap.get(AlignmentOperation.POSTPROCESSING),
									r.timeMap.get(AlignmentOperation.TOTAL)));
						}
					}
				}
			}
			w.close();
		}
		catch (IOException e)
		{
			e.printStackTrace();
		}
	}

	public static void writeGenome(CharSequence genome, File file)
	{
		try
		{
			FastaWriter.writeSequence(genome, file);
		}
		catch (IOException e)
		{
			e.printStackTrace();
		}
	}

	public static void writeFragments(File file, List<? extends Fragment> fragments)
	{
		try
		{
			FastqWriter.writeFragments(fragments, file, 0);
		}
		catch (IOException e)
		{
			e.printStackTrace();
		}
	}
}
