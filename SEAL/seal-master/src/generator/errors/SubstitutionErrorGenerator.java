package generator.errors;

import generator.SequenceGenerator;
import java.util.HashMap;
import java.util.Map;

public abstract class SubstitutionErrorGenerator extends FragmentErrorGenerator
{
	/**
	 * Used to ensure that characters are replaced with a different character;
	 * i.e. that the same character is not randomly chosen. This requires n^2
	 * space with respect to the set of allowed characters, but these alphabets
	 * are usually very small anyway (the largest considered here,
	 * {@link SequenceGenerator#AMINO_ACID_ALLOWED_CHARACTERS}, is 27 characters
	 * long).
	 */
	protected final Map<Character, String> replacements;

	public SubstitutionErrorGenerator(String allowedCharacters)
	{
		super(allowedCharacters);
		replacements = new HashMap<Character, String>(allowedCharacters.length());
		for (int i = 0; i < allowedCharacters.length(); i++)
		{
			StringBuilder sb = new StringBuilder(allowedCharacters.length() - 1);
			for (int j = 0; j < i; j++)
			{
				sb.append(allowedCharacters.charAt(j));
			}
			for (int j = i + 1; j < allowedCharacters.length(); j++)
			{
				sb.append(allowedCharacters.charAt(j));
			}
			replacements.put(allowedCharacters.charAt(i), sb.toString());
		}
	}

	protected abstract double getSubstitutionProbability(int position, int length);

	@Override
	public int getQuality(int position, int length)
	{
		return phredScaleProbability(getSubstitutionProbability(position, length));
	}

	public char chooseRandomReplacementCharacter(char originalCharacter)
	{
		String possibilities = replacements.get(Character.toUpperCase(originalCharacter));
		if (possibilities == null)
		{
			possibilities = allowedCharacters;
		}
		return possibilities.charAt(characterChoiceRandomizer.nextInt(possibilities.length()));
	}
}
