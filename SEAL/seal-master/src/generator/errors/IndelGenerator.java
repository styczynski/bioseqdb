package generator.errors;

import generator.SeqGenSingleSequenceMultipleRepeats;
import generator.SequenceGenerator;

/**
 * TODO: Examine whether this should really subclass
 * {@link FragmentErrorGenerator}
 * 
 * @author mruffalo
 */
public class IndelGenerator extends FragmentErrorGenerator
{
	private Options o;
	private SequenceGenerator sg;

	public static class Options
	{
		/**
		 * Probability of starting a random insert at any given base
		 */
		public double insertProbability;
		/**
		 * Average length of each insertion
		 */
		public double insertLengthMean;
		/**
		 * Standard deviation of insertion lengths
		 */
		public double insertLengthStdDev;
		/**
		 * Probability of starting a random deletion at any given base
		 */
		public double deleteProbability;
		/**
		 * Average length of each deletion
		 */
		public double deleteLengthMean;
		/**
		 * Standard deviation of deletion lengths
		 */
		public double deleteLengthStdDev;
	}

	public IndelGenerator(String allowedCharacters, Options o_)
	{
		super(allowedCharacters);
		o = o_;
		sg = new SeqGenSingleSequenceMultipleRepeats();
	}

	/**
	 * TODO: Test this
	 */
	@Override
	public CharSequence generateErrors(CharSequence sequence)
	{
		StringBuilder sb = new StringBuilder(sequence.length());
		StringBuilder adjustedOrigSequence = new StringBuilder(sb.length());
		StringBuilder errorIndicator = new StringBuilder(sequence.length());
		/*
		 * Used for printing the sequence in verbose mode. Contains spaces where
		 * a deletion occurred.
		 */
		StringBuilder adjustedNewSequence = new StringBuilder(sequence.length());
		for (int i = 0; i < sequence.length(); i++)
		{
			/*
			 * We insert this character whether this is a deletion, insertion,
			 * or (unlikely as that is) both. If this is a deletion, we'll skip
			 * after this character.
			 */
			sb.append(sequence.charAt(i));
			if (verbose)
			{
				adjustedOrigSequence.append(sequence.charAt(i));
				errorIndicator.append(' ');
				adjustedNewSequence.append(sequence.charAt(i));
			}
			if (random.nextDouble() < o.insertProbability)
			{
				int insertLength = (int) (o.insertLengthMean + o.insertLengthStdDev
						* random.nextGaussian());
				if (insertLength < 0)
				{
					insertLength = 0;
				}
				SequenceGenerator.Options sgo = new SequenceGenerator.Options();
				sgo.length = insertLength;
				CharSequence insertedSequence = sg.generateSequence(sgo);
				sb.append(insertedSequence);
				if (verbose)
				{
					for (int j = 0; j < insertLength; j++)
					{
						errorIndicator.append('+');
						adjustedOrigSequence.append(' ');
					}
					adjustedNewSequence.append(insertedSequence);
				}
			}
			if (random.nextDouble() < o.deleteProbability)
			{
				int deleteLength = (int) (o.deleteLengthMean + o.deleteLengthStdDev
						* random.nextGaussian());
				if (verbose)
				{
					int subsequenceEnd = i + deleteLength;
					if (subsequenceEnd >= sequence.length())
					{
						subsequenceEnd = sequence.length() - 1;
					}
					adjustedOrigSequence.append(sequence.subSequence(i, subsequenceEnd));
					for (int j = 0; j < deleteLength; j++)
					{
						errorIndicator.append('-');
						adjustedNewSequence.append(' ');
					}
				}
				i += deleteLength;
			}
		}
		if (verbose)
		{
			System.err.println();
			System.err.printf("Original sequence: %s%n", adjustedOrigSequence.toString());
			System.err.print("                   ");
			System.err.println(errorIndicator.toString());
			System.err.printf("New sequence:      %s%n%n", adjustedNewSequence.toString());
		}
		return sb;
	}

	@Override
	public int getQuality(int position, int length)
	{
		return 40;
	}
}
