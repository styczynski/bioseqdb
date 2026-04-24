package generator;

import generator.errors.FragmentErrorGenerator;
import io.FastqWriter;

import java.io.BufferedWriter;
import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.Iterator;
import java.util.LinkedList;
import java.util.List;
import java.util.Random;
import java.util.Set;

import assembly.Fragment;
import assembly.FragmentPositionSource;
import io.MultipartSequence;

public class Fragmentizer
{
	public static class Options
	{
		/**
		 * Number of fragments to read
		 */
		public int fragmentCount;
		/**
		 * Approximate length of each fragment
		 */
		public int fragmentLength;
		/**
		 * Standard deviation of normally-distributed fragment size
		 */
		public double fragmentLengthSd;
		/**
		 * Only used for paired-end.
		 */
		public int readLength;
		/**
		 * Only used for paired-end.
		 */
		public double readLengthSd;
		/**
		 * If a simulated read error occurs, another character will be randomly
		 * sampled from this String
		 */
		public String allowedErrorCharacters;
		/**
		 * If this isn't null, the list will be passed to it in order to
		 * introduce read errors
		 */
		public List<FragmentErrorGenerator> errorGenerators;
		/**
		 * If this is true, the <code>readLength</code> and
		 * <code>readLengthSd</code> parameters will be used.
		 */
		public boolean pairedEnd;
		/**
		 * This is how far to read past each end, for the purposes of keeping a
		 * consistent read length when introducing deletions. This should
		 * probably be equal to the maximum indel size. TODO: Rename this
		 */
		public int overage;
	}

	/**
	 * @param string String to fragmentize
	 * @param o	  Options
	 * @return A list of fragments that were randomly read from the provided
	 *         string.
	 */
	public static List<Fragment> fragmentize(CharSequence string, Options o, int offset)
	{
		Random random = new Random();
		List<Fragment> list = new ArrayList<Fragment>(o.fragmentCount);
		for (int i = 0; i < o.fragmentCount; i++)
		{
			int sizeAddition = (int) (random.nextGaussian() * o.fragmentLengthSd);
			int fragmentLength = o.fragmentLength + sizeAddition;
			int index = random.nextInt(string.length() - fragmentLength);
			Fragment f = new Fragment(string.subSequence(index, index + fragmentLength));
			f.setPosition(FragmentPositionSource.ORIGINAL_SEQUENCE, index + offset);
			list.add(f);
		}
		return list;
	}

	/**
	 * Why can't I use default arguments? :(
	 */
	public static List<Fragment> fragmentize(CharSequence string, Options o)
	{
		return fragmentize(string, o, 0);
	}

	/**
	 * Produces two fragment lists, one for each end (forward, reverse)
	 *
	 * @param sequence
	 * @param o		Options. You probably want to set
	 *                 <code>readLengthSd = 0</code> to accurately represent
	 *                 real-world data.
	 * @return
	 */
	@SuppressWarnings("unchecked")
	public static List<List<? extends Fragment>> fragmentizePairedEnd(CharSequence sequence,
			Options o)
	{
		Random random = new Random();
		final int paired = 2;
		List<List<? extends Fragment>> list = new ArrayList<List<? extends Fragment>>(paired);
		List<Fragment> orig = fragmentize(sequence, o);
		for (int i = 0; i < paired; i++)
		{
			list.add(new ArrayList<Fragment>(o.fragmentCount));
		}
		List firstList = list.get(0);
		List secondList = list.get(1);
		for (Fragment f : orig)
		{
			int readLengthAddition = (int) (random.nextGaussian() * o.readLengthSd);
			int readLength = o.readLength + readLengthAddition;
			List<? extends Fragment> ends = f.pairedEndClone(readLength);
			Fragment first = ends.get(0);
			Fragment second = ends.get(1);
			firstList.add(first);
			secondList.add(second);
		}
		return list;
	}

	/**
	 * Reads this string into fragments and directly writes them to the
	 * specified Files.
	 *
	 * @param sequence
	 * @param o
	 * @param files
	 */
	public static void fragmentizeToFiles(CharSequence sequence, Options o, File... files)
	{
		List<FastqWriter> fastqWriters = new ArrayList<FastqWriter>(files.length);
		for (File file : files)
		{
			try
			{
				BufferedWriter bw = new BufferedWriter(new FileWriter(file));
				fastqWriters.add(new FastqWriter(bw));
			}
			catch (IOException e)
			{
				e.printStackTrace();
			}
		}
		Random random = new Random();
		try
		{
			for (int i = 0; i < o.fragmentCount; i++)
			{
				int sizeAddition = (int) (random.nextGaussian() * o.fragmentLengthSd);
				int fragmentLength = o.fragmentLength + o.overage + sizeAddition;
				int startPosition = random.nextInt(sequence.length() - fragmentLength);
				Fragment f = new Fragment(sequence.subSequence(startPosition, startPosition
						+ fragmentLength));
				for (FragmentErrorGenerator eg : o.errorGenerators)
				{
					f = eg.generateErrors(f);
				}
				f.setPosition(FragmentPositionSource.ORIGINAL_SEQUENCE, startPosition);
				for (FastqWriter fw : fastqWriters)
				{
					fw.writeFragment(f, 0, i);
				}
			}
		}
		catch (IOException e)
		{
			e.printStackTrace();
		}
		for (FastqWriter fw : fastqWriters)
		{
			try
			{
				fw.close();
			}
			catch (IOException e)
			{
				e.printStackTrace();
			}
		}
	}

	/**
	 * XXX: o.fragmentCount is interpreted *per sequence*, so read coverage
	 * is a function of the sequence length (human genome case: 2GB + 1GB with
	 * same number of reads from each = 1GB part gets denser coverage for same
	 * number of reads)
	 *
	 * @param sequences
	 * @param o
	 * @return
	 */
	public static List<Fragment> fragmentize(List<MultipartSequence>
			sequences, Options o)
	{
		List<Fragment> list = new ArrayList<Fragment>(o.fragmentCount);
		for (MultipartSequence ms : sequences)
		{
			list.addAll(fragmentize(ms.sequence, o, ms.offset));
		}
		return list;
	}

	/**
	 * @param string String to fragmentize
	 * @param o	  Options
	 * @return A list of fragments that were randomly read from the provided
	 *         String. Fragments that are entirely contained in another fragment
	 *         <b>have already been filtered</b>. This means that you will
	 *         probably get less than <code>n</code> fragments back.
	 */
	public static List<Fragment> fragmentizeForShotgun(CharSequence string, Options o)
	{
		return removeSubstrings(fragmentize(string, o));
	}

	/**
	 * @param fragments A list of Fragments
	 * @return A filtered list -- if a Fragment is entirely contained in another
	 *         Fragment, it has been removed.
	 */
	public static List<Fragment> removeSubstrings(List<Fragment> fragments)
	{
		List<Fragment> fixed = new ArrayList<Fragment>(fragments.size());
		Set<Fragment> nonDuplicates = new HashSet<Fragment>(fragments);
		Set<Fragment> substrings = new HashSet<Fragment>();
		for (Fragment first : nonDuplicates)
		{
			for (Fragment second : nonDuplicates)
			{
				// Filter out substrings
				if (first.getSequence().toString().contains(second.getSequence().toString())
						&& !first.getSequence().equals(second.getSequence()))
				{
					substrings.add(second);
				}
			}
		}
		for (Fragment fragment : nonDuplicates)
		{
			if (!substrings.contains(fragment))
			{
				fixed.add(fragment);
			}
		}
		return fixed;
	}

	/**
	 * TODO: Improve runtime from O(n^2) and generally make this code less
	 * stupid
	 *
	 * @param fragments
	 * @return
	 */
	public static List<List<Fragment>> groupByLine(List<Fragment> fragments,
			FragmentPositionSource source)
	{
		List<List<Fragment>> groupedList = new LinkedList<List<Fragment>>();
		Set<Fragment> fragmentSet = new HashSet<Fragment>(fragments);
		/*
		 * The position of a fragment might be null if 'source' is
		 * ASSEMBLED_SEQUENCE and the fragment was not used (e.g. if it was
		 * entirely contained in another fragment). Remove all fragments that
		 * have null positions.
		 */
		Iterator<Fragment> removalIterator = fragmentSet.iterator();
		while (removalIterator.hasNext())
		{
			Fragment f = removalIterator.next();
			if (f.getPosition(source) == null)
			{
				removalIterator.remove();
			}
		}
		while (!fragmentSet.isEmpty())
		{
			Set<Fragment> possibleFragments = new HashSet<Fragment>(fragmentSet);
			List<Fragment> list = new LinkedList<Fragment>();
			int begin = 0;
			while (!possibleFragments.isEmpty())
			{
				Fragment earliestFinish = null;
				Iterator<Fragment> iterator = possibleFragments.iterator();
				while (iterator.hasNext())
				{
					Fragment fragment = iterator.next();
					if (fragment.getPosition(source) >= begin)
					{
						if (earliestFinish == null
								|| (fragment.getPosition(
								source) + fragment.getSequence().length()) < (earliestFinish.getPosition(
								source) + earliestFinish.getSequence().length()))
						{
							earliestFinish = fragment;
						}
					}
					else
					{
						iterator.remove();
					}
				}
				if (earliestFinish != null)
				{
					list.add(earliestFinish);
					/*
					 * +1 here ensures that successive fragments on one line are
					 * separated by a gap of at least one character.
					 */
					begin = earliestFinish.getPosition(source)
							+ earliestFinish.getSequence().length() + 1;
					fragmentSet.remove(earliestFinish);
				}
			}
			groupedList.add(list);
		}
		return groupedList;
	}

	/**
	 * @param string String to fragmentize
	 * @param k	  Length of each fragment
	 * @return A List of all substrings of length <code>k</code> contained in
	 *         <code>string</code>
	 */
	public static List<Fragment> fragmentizeForHybridization(CharSequence string, int k)
	{
		ArrayList<Fragment> list = new ArrayList<Fragment>(string.length() - k);
		for (int i = 0; i <= string.length() - k; i++)
		{
			list.add(new Fragment(string.subSequence(i, i + k)));
		}
		return list;
	}

	/**
	 * Output a <code>String</code> and its <code>List&lt;Fragment&gt;</code> to
	 * <code>System.out</code>.
	 *
	 * @param args
	 */
	public static void main(String[] args)
	{
		if (args.length < 4)
		{
			System.err.printf(
					"*** Usage: %s string n k kVariance pairedEnd readLength readLengthSd%n",
					Fragmentizer.class.getCanonicalName());
			System.exit(1);
		}
		String string = args[0];
		Options options = new Options();
		options.fragmentCount = Integer.parseInt(args[1]);
		options.fragmentLength = Integer.parseInt(args[2]);
		options.fragmentLengthSd = Integer.parseInt(args[3]);
		options.pairedEnd = Boolean.parseBoolean(args[4]);
		options.readLength = Integer.parseInt(args[5]);
		options.readLengthSd = Double.parseDouble(args[3]);
		FragmentPositionSource source = FragmentPositionSource.ORIGINAL_SEQUENCE;
		List<Fragment> fragments = fragmentizeForShotgun(string, options);
		for (Fragment fragment : fragments)
		{
			System.out.printf("%5d: %s%n", fragment.getPosition(source), fragment.getSequence());
		}
		System.out.println();
		System.out.println(string);
		List<List<Fragment>> grouped = groupByLine(fragments, source);
		for (List<Fragment> list : grouped)
		{
			int begin = 0;
			for (Fragment fragment : list)
			{
				for (int i = 0; i < fragment.getPosition(source) - begin; i++)
				{
					System.out.print(" ");
				}
				System.out.print(fragment.getSequence());
				begin = fragment.getPosition(source) + fragment.getSequence().length();
			}
			System.out.println();
		}
	}
}
