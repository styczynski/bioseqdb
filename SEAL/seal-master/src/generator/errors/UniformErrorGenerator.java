package generator.errors;

public class UniformErrorGenerator extends SubstitutionErrorGenerator
{
	private double errorProbability;

	public UniformErrorGenerator(String allowedCharacters_, double errorProbability_)
	{
		super(allowedCharacters_);
		setErrorProbability(errorProbability_);
	}

	@Override
	protected double getSubstitutionProbability(int position, int length)
	{
		return errorProbability;
	}

	public void setErrorProbability(double errorProbability_)
	{
		if (errorProbability_ <= 1.0 && errorProbability_ >= 0.0)
		{
			errorProbability = errorProbability_;
		}
		else
		{
			// TODO: be nicer about this :)
			throw new IllegalArgumentException("error probability must be >= 0.0 and <= 1.0");
		}
	}

	public double getErrorProbability()
	{
		return errorProbability;
	}

	/**
	 * Don't need to use the nice {@link #getErrorProbability} method,
	 * since we know that it's the same for every character position in this
	 * class.
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
