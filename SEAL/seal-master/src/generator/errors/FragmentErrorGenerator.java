package generator.errors;

import assembly.Fragment;
import generator.Fragmentizer;
import io.FastqWriter;
import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.Random;

/**
 * A class that extends this really only needs to implement 'get the error
 * probability for this position' (which can be uniform, increase linearly,
 * increase exponentially, etc.) and 'mangle the input sequence given that error
 * probability'.
 * 
 * @author mruffalo
 */
public abstract class FragmentErrorGenerator
{
	/**
	 * Used to determine which character to randomly insert/switch
	 */
	protected final Random characterChoiceRandomizer;
	/**
	 * Intended for use by each generator to determine whether to introduce an
	 * error (e.g. substitute a character, start an indel)
	 */
	protected final Random random;
	protected final String allowedCharacters;
	protected boolean verbose;

	public FragmentErrorGenerator(String allowedCharacters_)
	{
		allowedCharacters = allowedCharacters_;
		characterChoiceRandomizer = new Random();
		random = new Random();
	}

	public abstract CharSequence generateErrors(CharSequence sequence);

	public Fragment generateErrors(Fragment fragment)
	{
		String s = fragment.toString();
		Fragment errored = new Fragment(generateErrors(s).toString());
		errored.clonePositionsAndReadQuality(fragment);
		for (int i = 0; i < errored.getSequence().length(); i++)
		{
			errored.setReadQuality(i, getQuality(i, errored.getSequence().length()));
		}
		return errored;
	}

	public List<? extends Fragment> generateErrors(List<? extends Fragment> fragments)
	{
		List<Fragment> list = new ArrayList<Fragment>(fragments.size());
		for (Fragment fragment : fragments)
		{
			list.add(generateErrors(fragment));
		}
		return list;
	}

	/**
	 * @deprecated You should probably use
	 *             {@link Fragmentizer#fragmentizeToFile(CharSequence, generator.Fragmentizer.Options, File)}
	 *             and set the {@link Fragmentizer.Options#errorGenerators}
	 *             field appropriately instead of using this.
	 * @param errorGenerators
	 * @param fragmentList
	 * @param fragmentFile
	 */
	@Deprecated
	public static void generateErrorsToFile(List<FragmentErrorGenerator> errorGenerators,
		List<? extends Fragment> fragmentList, File fragmentFile)
	{
		try
		{
			FastqWriter w = new FastqWriter(new FileWriter(fragmentFile));
			for (int i = 0; i < fragmentList.size(); i++)
			{
				Fragment f = fragmentList.get(i);
				for (FragmentErrorGenerator eg : errorGenerators)
				{
					f = eg.generateErrors(f);
				}
				w.writeFragment(f, 0, i);
			}
			w.close();
		}
		catch (IOException e)
		{
			e.printStackTrace();
		}
	}

	public abstract int getQuality(int position, int length);

	public static int phredScaleProbability(double errorProbability)
	{
		return (int) (-10 * Math.log10(errorProbability));
	}

	public void setVerboseOutput(boolean verbose_)
	{
		verbose = verbose_;
	}
}
