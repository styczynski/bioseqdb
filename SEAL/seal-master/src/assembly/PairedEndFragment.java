package assembly;

import generator.SequenceGenerator;
import java.util.HashMap;
import java.util.Map;

public class PairedEndFragment extends Fragment
{
	/**
	 * TODO: Move this
	 */
	protected static final Map<Character, Character> NUCLEOTIDE_COMPLEMENTS = new HashMap<Character, Character>(
		SequenceGenerator.NUCLEOTIDES.length());
	static
	{
		NUCLEOTIDE_COMPLEMENTS.put('A', 'T');
		NUCLEOTIDE_COMPLEMENTS.put('T', 'A');
		NUCLEOTIDE_COMPLEMENTS.put('C', 'G');
		NUCLEOTIDE_COMPLEMENTS.put('G', 'C');
	}

	/**
	 * Whether this paired end read at the beginning of the original fragment
	 * (true) or at the end (false)
	 */
	protected boolean atBeginningOfFragment;
	protected int length;

	public PairedEndFragment(CharSequence sequence_, boolean atBeginningOfFragment_, int length_)
	{
		super(sequence_);
		atBeginningOfFragment = atBeginningOfFragment_;
		length = length_;
	}

	@Override
	public CharSequence getSequence()
	{
		if (atBeginningOfFragment)
		{
			return sequence.subSequence(0, length);
		}
		else
		{
			// TODO: Maybe memoize this
			StringBuilder reversed = new StringBuilder(sequence.subSequence(sequence.length()
					- length, sequence.length())).reverse();
			StringBuilder complemented = new StringBuilder(reversed.length());
			for (int i = 0; i < reversed.length(); i++)
			{
				complemented.append(NUCLEOTIDE_COMPLEMENTS.get(reversed.charAt(i)));
			}
			return complemented.toString();
		}
	}

	@Override
	public Integer getPosition(FragmentPositionSource source)
	{
		Integer rawPosition = positions.get(source);
		if (rawPosition == null)
		{
			return null;
		}
		if (atBeginningOfFragment)
		{
			return rawPosition;
		}
		else
		{
			return rawPosition + (sequence.length() - length);
		}
	}
}
