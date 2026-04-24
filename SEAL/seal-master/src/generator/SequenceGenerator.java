package generator;

import java.util.*;

public abstract class SequenceGenerator
{
	public static final String NUCLEOTIDES = "ACGT";
	public static final String NUCLEIC_ACID_ALLOWED_CHARACTERS = "ACGTURYKMSWBDHVNX-";
	public static final String AMINO_ACID_ALLOWED_CHARACTERS = "ABCDEFGHIKLMNOPQRSTUVWYZX*-";

	protected boolean verbose;

	public static class Options
	{
		/**
		 * Characters in the generated sequence come from here
		 */
		public String characters = NUCLEOTIDES;
		public int length;
		public int repeatCount;
		public int repeatLength;
		/**
		 * Each character in each repeat will be substituted with a random
		 * choice from {@link #characters} at this probability
		 */
		public double repeatErrorProbability;
	}

	/**
	 * Generates a single sequence with the provided length from the given
	 * characters. XXX Make this protected again after fixing all of the
	 * horrible design decisions I've recently made
	 * 
	 * @param sample
	 * @param length
	 * @return
	 */
	public static CharSequence generateSequence(String sample, int length)
	{
		Random random = new Random();
		StringBuilder sb = new StringBuilder(length);
		for (int i = 0; i < length; i++)
		{
			int index = random.nextInt(sample.length());
			sb.append(sample.substring(index, index + 1));
		}
		return sb;
	}

	/**
	 * Controls whether debugging information will be printed to
	 * <code>System.err</code>. TODO: Maybe allow a separate
	 * {@link java.io.OutputStream} to be specified.
	 * 
	 * @param verbose_
	 */
	public void setVerboseOutput(boolean verbose_)
	{
		verbose = verbose_;
	}

	/**
	 * Generates a sequence according to the provided options
	 * 
	 * @param o
	 *            Options specifying length, repeat count, repeat length,
	 *            characters to sample, error rate, etc.
	 * @return
	 */
	public abstract CharSequence generateSequence(Options o);
}
