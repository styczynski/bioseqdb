package generator;

import generator.errors.FragmentErrorGenerator;
import generator.errors.UniformErrorGenerator;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Random;

/**
 * TODO: Make this class implement {@link SequenceFilter} instead of extending
 * {@link SequenceGenerator}, remove the generation functions and change the
 * 'insert' method to match {@link SequenceFilter#filter(CharSequence)}
 * 
 * @author mruffalo
 */
public class SeqFilterTandemRepeats implements SequenceFilter
{
	public static class Options
	{
		/**
		 * Used for possible character substitutions in the repeats.
		 */
		public String characters = SequenceGenerator.NUCLEOTIDES;
		public int repeatCount;
		public int repeatLength;
		public double repeatErrorProbability;
	}

	private boolean verbose;

	private Options o;
	private List<TandemRepeatDescriptor> repeats;

	public static class TandemRepeatDescriptor
	{
		public final int position;
		public final int length;

		public TandemRepeatDescriptor(int position_, int length_)
		{
			position = position_;
			length = length_;
		}
	}

	Random random = new Random();

	public SeqFilterTandemRepeats(Options o_)
	{
		o = o_;
		repeats = new ArrayList<TandemRepeatDescriptor>(o.repeatCount);
	}

	public void setVerbose(boolean verbose_)
	{
		verbose = verbose_;
	}

	@Override
	public CharSequence filter(CharSequence s)
	{
		final FragmentErrorGenerator eg = new UniformErrorGenerator(o.characters,
			o.repeatErrorProbability);
		StringBuilder sb = new StringBuilder(s);
		int[] repeatedSequenceIndices = new int[o.repeatCount];
		int nonRepeatedLength = s.length() - o.repeatCount * o.repeatLength;
		if (verbose)
		{
			System.out.printf("Non-repeated sequence length: %d%n", nonRepeatedLength);
		}
		if (nonRepeatedLength > 0)
		{
			for (int i = 0; i < o.repeatCount; i++)
			{
				repeatedSequenceIndices[i] = random.nextInt(nonRepeatedLength - o.repeatLength)
						+ o.repeatLength;
			}
			Arrays.sort(repeatedSequenceIndices);
			if (verbose)
			{
				System.out.print("Raw repeat positions: ");
				for (int position : repeatedSequenceIndices)
				{
					System.out.print(position);
					System.out.print(' ');
				}
				System.out.println();
			}
		}
		if (verbose)
		{
			int newLength = sb.length() + o.repeatCount * o.repeatLength;
			for (double l = Math.log10(newLength); l > 0; l--)
			{
				int p = (int) Math.pow(10, (int) l);
				for (int j = 0; j < newLength; j++)
				{
					if (j % p == 0)
					{
						System.out.print((j / p) % 10);
					}
					else
					{
						System.out.print(' ');
					}
				}
				System.out.println();
			}
		}
		int repeatStart = 0;
		for (int i = 0; i < o.repeatCount; i++)
		{
			int begin = repeatedSequenceIndices[i] + (i - 1) * o.repeatLength;
			int end = repeatedSequenceIndices[i] + i * o.repeatLength;
			repeats.add(new TandemRepeatDescriptor(begin, o.repeatLength - 1));
			CharSequence repeatedSequence = sb.subSequence(begin, end);
			if (o.repeatErrorProbability > 0.0)
			{
				repeatedSequence = eg.generateErrors(repeatedSequence);
			}
			if (verbose)
			{
				for (int j = 0; j < repeatedSequenceIndices[i] - repeatStart - o.repeatLength; j++)
				{
					System.out.print(" ");
				}
				for (int j = 0; j < o.repeatLength; j++)
				{
					System.out.print("-");
				}
				System.out.print(repeatedSequence);
				repeatStart = repeatedSequenceIndices[i];
			}
			sb.insert(i * o.repeatLength + repeatedSequenceIndices[i], repeatedSequence);
		}
		String string = sb.toString();
		if (verbose)
		{
			System.out.println();
			System.out.println(string);
		}
		return string;
	}

	public List<TandemRepeatDescriptor> getRepeats()
	{
		return repeats;
	}

	@Override
	public String getDescription()
	{
		// TODO Auto-generated method stub
		return null;
	}

	/**
	 * Standalone debug method
	 * 
	 * @param args
	 */
	@SuppressWarnings("unused")
	public static void main(String[] args)
	{
		Options o = new Options();
		o.repeatCount = 4;
		o.repeatLength = 5;
		SeqFilterTandemRepeats generator = new SeqFilterTandemRepeats(o);
		generator.setVerbose(true);
		CharSequence s = SequenceGenerator.generateSequence(SequenceGenerator.NUCLEOTIDES, 100);
		System.out.println("Original sequence:");
		System.out.println(s);
		CharSequence generated = generator.filter(s);
		for (TandemRepeatDescriptor repeat : generator.getRepeats())
		{
			System.out.printf("Repeat: %04d - %04d%n", repeat.position, repeat.position
					+ repeat.length);
		}
	}
}
