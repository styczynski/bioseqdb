package generator.errors;

public class SnpGenerator extends SubstitutionErrorGenerator
{
	public SnpGenerator(String allowedCharacters_)
	{
		super(allowedCharacters_);
	}

	/**
	 * TODO this
	 */
	@Override
	protected double getSubstitutionProbability(int position, int length)
	{
		return 0.0;
	}

	/**
	 * 
	 */
	@Override
	public CharSequence generateErrors(CharSequence sequence)
	{
		if (verbose)
		{
			System.err.println();
			System.err.printf("Original sequence: %s%n", sequence);
			System.err.print("                   ");
		}
		StringBuilder sb = new StringBuilder(sequence.length());
		StringBuilder errorIndicator = new StringBuilder(sequence.length());
		for (int i = 0; i < sequence.length(); i++)
		{
			char orig = sequence.charAt(i);
			if (random.nextDouble() < getSubstitutionProbability(i, sequence.length()))
			{
				sb.append(chooseRandomReplacementCharacter(orig));
				if (verbose)
				{
					errorIndicator.append("X");
				}
			}
			else
			{
				sb.append(orig);
				if (verbose)
				{
					errorIndicator.append(" ");
				}
			}
		}
		if (verbose)
		{
			System.err.println(errorIndicator.toString());
			System.err.printf("New sequence:      %s%n%n", sb.toString());
		}
		return sb;
	}
}
