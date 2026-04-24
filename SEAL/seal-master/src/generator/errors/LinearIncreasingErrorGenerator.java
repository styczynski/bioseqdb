package generator.errors;

import assembly.Fragment;

import java.util.Random;

public class LinearIncreasingErrorGenerator extends SubstitutionErrorGenerator
{
	private double beginErrorProbabilityMin;
	private double beginErrorProbabilityMax;
	private double endErrorProbabilityMin;
	private double endErrorProbabilityMax;
	private double errorProbabilityStdDev;
	private Random random = new Random();

	public LinearIncreasingErrorGenerator(String allowedCharacters_, double beginErrorProbabilityMin_,
			double beginErrorProbabilityMax_, double endErrorProbabilityMin_,
			double endErrorProbabilityMax_, double errorProbabilityStdDev_)
	{
		super(allowedCharacters_);
		setBeginErrorProbabilityMin(beginErrorProbabilityMin_);
		setBeginErrorProbabilityMax(beginErrorProbabilityMax_);
		setEndErrorProbabilityMin(endErrorProbabilityMin_);
		setEndErrorProbabilityMax(endErrorProbabilityMax_);
		setErrorProbabilityStdDev(errorProbabilityStdDev_);
	}

	public double getBeginErrorProbabilityMin()
	{
		return beginErrorProbabilityMin;
	}

	public void setBeginErrorProbabilityMin(double beginErrorProbability_)
	{
		if (beginErrorProbability_ <= 1.0 && beginErrorProbability_ >= 0.0)
		{
			beginErrorProbabilityMin = beginErrorProbability_;
		}
		else
		{
			// TODO: be nicer about this :)
			throw new IllegalArgumentException("error probability must be >= 0.0 and <= 1.0");
		}
	}

	public double getBeginErrorProbabilityMax()
	{
		return beginErrorProbabilityMax;
	}

	public void setBeginErrorProbabilityMax(double beginErrorProbabilityMax_)
	{
		if (beginErrorProbabilityMax_ <= 1.0 && beginErrorProbabilityMax_ >= 0.0)
		{
			beginErrorProbabilityMax = beginErrorProbabilityMax_;
		}
		else
		{
			// TODO: be nicer about this :)
			throw new IllegalArgumentException("error probability must be >= 0.0 and <= 1.0");
		}
	}

	public double getEndErrorProbabilityMin()
	{
		return endErrorProbabilityMin;
	}

	public void setEndErrorProbabilityMin(double endErrorProbabilityMin_)
	{
		if (endErrorProbabilityMin_ <= 1.0 && endErrorProbabilityMin_ >= 0.0)
		{
			endErrorProbabilityMin = endErrorProbabilityMin_;
		}
		else
		{
			// TODO: be nicer about this :)
			throw new IllegalArgumentException("error probability must be >= 0.0 and <= 1.0");
		}
	}

	public double getEndErrorProbabilityMax()
	{
		return endErrorProbabilityMax;
	}

	public void setEndErrorProbabilityMax(double endErrorProbability_)
	{
		if (endErrorProbability_ <= 1.0 && endErrorProbability_ >= 0.0)
		{
			endErrorProbabilityMax = endErrorProbability_;
		}
		else
		{
			// TODO: be nicer about this :)
			throw new IllegalArgumentException("error probability must be >= 0.0 and <= 1.0");
		}
	}

	public double getErrorProbabilityStdDev()
	{
		return errorProbabilityStdDev;
	}

	public void setErrorProbabilityStdDev(double errorProbabilityStdDev_)
	{
		if (errorProbabilityStdDev_ <= 1.0 && errorProbabilityStdDev_ >= 0.0)
		{
			errorProbabilityStdDev = errorProbabilityStdDev_;
		}
		else
		{
			// TODO: be nicer about this :)
			throw new IllegalArgumentException("error std.dev must be >= 0.0");
		}
	}

	@Override
	public CharSequence generateErrors(CharSequence sequence)
	{
		// TODO clean this up
		return null;
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
		double errorProbBegin = random.nextDouble() * (beginErrorProbabilityMax - beginErrorProbabilityMin) +
				beginErrorProbabilityMin;
		double errorProbEnd = random.nextDouble() * (endErrorProbabilityMax - endErrorProbabilityMin) +
				endErrorProbabilityMin;
		if (errorProbEnd < errorProbBegin)
		{
			errorProbEnd = errorProbBegin;
		}
		double[] qualities = new double[sequence.length()];
		for (int i = 0; i < sequence.length(); i++)
		{
			char orig = sequence.charAt(i);
			double substitutionProbability =
					getSubstitutionProbability(i, sequence.length(), errorProbBegin, errorProbEnd);
			qualities[i] = substitutionProbability;
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
		for (int i = 0; i < sequence.length(); i++)
		{
			f.setReadQuality(i, phredScaleProbability(qualities[i]));
		}
		return f;
	}

	protected double getSubstitutionProbability(int position, int length, double begin, double end)
	{
		double linear_combination = begin + (end - begin) * ((double) position / (double) length);
		double gaussian_noise = random.nextGaussian() * errorProbabilityStdDev;
		double sum = linear_combination + gaussian_noise;
		return Math.max(0.0, Math.min(sum, 1.0));
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
		return 0.0;
	}
}
