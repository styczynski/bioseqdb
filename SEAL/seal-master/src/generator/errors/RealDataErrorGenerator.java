package generator.errors;

import assembly.Fragment;
import io.Constants;

import java.io.*;
import java.util.ArrayList;
import java.util.List;
import java.util.Random;

import org.apache.log4j.*;

/**
 * Reads a FASTQ file's base qualities as the error model
 */
public class RealDataErrorGenerator extends SubstitutionErrorGenerator
{
	/**
	 * This is read from a FASTQ file and used to modify reads
	 */
	private List<int[]> qualityScores;

	/**
	 * TODO move this to @{link SubstitutionErrorGenerator}
	 */
	private Logger log;
	private Random random = new Random();

	public RealDataErrorGenerator(String allowedCharacters)
	{
		super(allowedCharacters);
		log = Logger.getLogger(getClass());
	}

	/**
	 * @param allowedCharacters
	 * @param fastqFile		 FASTQ file to sample base qualities from
	 */
	public RealDataErrorGenerator(String allowedCharacters, File fastqFile)
	{
		this(allowedCharacters);
		qualityScores = readFastqFile(fastqFile, -1);
	}

	/**
	 * @param allowedCharacters
	 * @param fastqFile		 FASTQ file to sample base qualities from
	 * @param limit			 The maximum number of base quality strings to read from the FASTQ file
	 */
	public RealDataErrorGenerator(String allowedCharacters, File fastqFile, int limit)
	{
		this(allowedCharacters);
		qualityScores = readFastqFile(fastqFile, limit);
	}

	private static int[] decodeQualityString(String string, int offset)
	{
		int[] scores = new int[string.length()];
		for (int i = 0; i < string.length(); i++)
		{
			scores[i] = Character.codePointAt(string, i) - (offset + 33);
		}
		return scores;
	}

	/**
	 * TODO: refactor this; probably move it into FastqReader
	 *
	 * @param file
	 * @param limit How many records to read. If -1, read all
	 * @return
	 */
	private List<int[]> readFastqFile(File file, int limit)
	{
		List<int[]> list = new ArrayList<int[]>();
		int linesProcessed = 0;
		try
		{
			boolean isQualityString = false;
			BufferedReader r = new BufferedReader(new FileReader(file));
			String line;
			/*
			I'm checking for *equality* with 'limit' since I want to be able to use
			-1 to mean 'no limit'.
			 */
			while ((line = r.readLine()) != null && linesProcessed != limit)
			{
				// why oh why is "+" a valid FASTQ quality character
				if (isQualityString)
				{
					list.add(decodeQualityString(line.trim(), Constants.ILLUMINA_1_9_OFFSET));
					linesProcessed++;
					isQualityString = false;
				}
				else
				{
					if (line.startsWith(Constants.FASTQ_QUALITY_MARKER))
					{
						isQualityString = true;
					}
				}
			}
			r.close();
		}
		catch (FileNotFoundException e)
		{
			System.err.printf("File %s not found", file.getAbsolutePath());
			throw new RuntimeException(e);
		}
		catch (IOException e)
		{
			throw new RuntimeException(e);
		}
		log.info(String.format("Processed %d lines", linesProcessed));
		return list;
	}

	/**
	 * TODO figure out the best way to remove this
	 *
	 * @param position
	 * @param length
	 * @return
	 */
	@Override
	protected double getSubstitutionProbability(int position, int length)
	{
		return 0;
	}

	@Override
	public CharSequence generateErrors(CharSequence sequence)
	{
		return null;
	}

	private static double getErrorProbability(int[] qualities, int position)
	{
		if (position >= qualities.length)
		{
			position = qualities.length - 1;
		}
		return Math.pow(10, qualities[position] / (-10.0));
	}

	@Override
	public Fragment generateErrors(Fragment fragment)
	{
		if (verbose)
		{
			System.err.println();
			System.err.printf("Original sequence: %s%n", fragment.getSequence());
			System.err.print("                   ");
		}
		CharSequence sequence = fragment.getSequence();
		StringBuilder sb = new StringBuilder(sequence.length());
		StringBuilder errorIndicator = new StringBuilder(sequence.length());
		int whichQualityString = random.nextInt(qualityScores.size());
		int[] baseQualities = qualityScores.get(whichQualityString);
		for (int i = 0; i < sequence.length(); i++)
		{
			char orig = sequence.charAt(i);
			double substitutionProbability = getErrorProbability(baseQualities, i);
			if (random.nextDouble() <= substitutionProbability)
			{
				sb.append(chooseRandomReplacementCharacter(orig));
				errorIndicator.append("X");
			}
			else
			{
				sb.append(orig);
				errorIndicator.append(" ");
			}
		}
		if (verbose)
		{
			System.err.println(errorIndicator.toString());
			System.err.printf("New sequence:      %s%n%n", sb.toString());
		}
		Fragment f = new Fragment(sb.toString());
		f.clonePositionsAndReadQuality(fragment);
		f.setReadQuality(baseQualities);
		return f;
	}
}
