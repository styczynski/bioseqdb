package generator;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;
import generator.errors.FragmentErrorGenerator;
import generator.errors.UniformErrorGenerator;
import java.io.File;
import java.util.*;

import org.junit.Before;
import org.junit.Test;
import assembly.Fragment;
import assembly.FragmentPositionSource;

public class FragmentizerTest
{
	private final String[] stringsWithSubstrings = { "ABCDEFG", "BCDEF", "DEFGH", "EFG", "ABCDEFG",
			"FGHIJK", "K" };
	private Set<String> stringsWithoutSubstrings;
	private List<Fragment> listWithSubstrings;

	private final String duplicateString = "TCTCACTAAGACGGACAACC";
	private final int origPos = 148;
	private final int assembledPos = 247;
	private List<Fragment> duplicatesWithPositions;

	@Before
	public void setUp() throws Exception
	{
		List<Fragment> temp = new LinkedList<Fragment>();
		for (String string : stringsWithSubstrings)
		{
			temp.add(new Fragment(string));
		}
		listWithSubstrings = Collections.unmodifiableList(temp);

		Set<String> tempSet = new HashSet<String>();
		for (String string : new String[] { "ABCDEFG", "DEFGH", "FGHIJK" })
		{
			tempSet.add(string);
		}
		stringsWithoutSubstrings = Collections.unmodifiableSet(tempSet);

		temp = new LinkedList<Fragment>();
		Fragment f = new Fragment(duplicateString);
		f.setPosition(FragmentPositionSource.ORIGINAL_SEQUENCE, origPos);
		f.setPosition(FragmentPositionSource.ASSEMBLED_SEQUENCE, origPos);
		temp.add(f);
		f = new Fragment(duplicateString);
		f.setPosition(FragmentPositionSource.ORIGINAL_SEQUENCE, origPos);
		f.setPosition(FragmentPositionSource.ASSEMBLED_SEQUENCE, assembledPos);
		temp.add(f);
		duplicatesWithPositions = Collections.unmodifiableList(temp);
	}

	/**
	 * TODO
	 */
	@Test
	public void testFragmentizeForShotgun()
	{
		CharSequence string = SequenceGenerator.generateSequence(SequenceGenerator.NUCLEOTIDES, 100);
		System.out.println(string);
		Fragmentizer.Options o = new Fragmentizer.Options();
		o.fragmentCount = 50;
		o.fragmentLength = 10;
		o.fragmentLengthSd = 2;
		List<Fragment> list = Fragmentizer.fragmentizeForShotgun(string, o);
		assertTrue(o.fragmentCount >= list.size());
		for (Fragment fragment : list)
		{
			System.out.println(fragment);
			assertTrue(string.toString().contains(fragment.getSequence()));
		}
		o.fragmentCount = 20;
		o.fragmentLength = 20;
		o.fragmentLengthSd = 4;
		list = Fragmentizer.fragmentizeForShotgun(string, o);
		assertTrue(o.fragmentCount >= list.size());
		for (Fragment fragment : list)
		{
			System.out.println(fragment);
			/*
			 * TODO: Figure out how to test this after changing uniform
			 * distribution to normal distribution
			 */
			// assertTrue(Math.abs(fragment.getString().length() - o.k) <=
			// o.ksd);
			assertTrue(string.toString().contains(fragment.getSequence()));
		}
	}

	@Test
	public void testFragmentizeForHybridization()
	{
		CharSequence string = SequenceGenerator.generateSequence(SequenceGenerator.NUCLEOTIDES, 100);
		int k = 5;
		List<Fragment> list = Fragmentizer.fragmentizeForHybridization(string, k);
		assertEquals(string.length() - k + 1, list.size());
		for (Fragment fragment : list)
		{
			assertEquals(k, fragment.getSequence().length());
			assertTrue(string.toString().contains(fragment.getSequence()));
		}
	}

	@Test
	public void testRemoveSubstrings()
	{
		Set<String> strings = new HashSet<String>();
		List<Fragment> fragments = Fragmentizer.removeSubstrings(listWithSubstrings);
		for (Fragment f : fragments)
		{
			strings.add(f.getSequence().toString());
		}
		// Sanity check: make sure that we don't get any duplicate strings
		assertEquals(strings.size(), fragments.size());
		for (String s : strings)
		{
			assertTrue(stringsWithoutSubstrings.contains(s));
		}
	}

	@Test
	public void testRemoveSubstringsWithPositions()
	{
		List<Fragment> fragments = Fragmentizer.removeSubstrings(duplicatesWithPositions);
		assertEquals(1, fragments.size());
	}

	/**
	 * TODO
	 */
	@Test
	public void testGroupByLine()
	{
		// TODO this
		// fail("Not yet implemented");
	}

	@Test
	public void testFragmentizeToFiles()
	{
		final int fileCount = 5;
		SequenceGenerator g = new SeqGenSingleSequenceMultipleRepeats();
		SequenceGenerator.Options sgo = new SequenceGenerator.Options();
		sgo.length = 10000;
		CharSequence s = g.generateSequence(sgo);
		File[] files = new File[fileCount];
		for (int i = 0; i < fileCount; i++)
		{
			files[i] = new File(String.format("%d.fasta", i));
		}
		Fragmentizer.Options fo = new Fragmentizer.Options();
		fo.fragmentCount = 500;
		fo.fragmentLength = 60;
		fo.fragmentLengthSd = 1.0;
		fo.errorGenerators = new ArrayList<FragmentErrorGenerator>(1);
		fo.errorGenerators.add(new UniformErrorGenerator(SequenceGenerator.NUCLEOTIDES, 0.01));
		Fragmentizer.fragmentizeToFiles(s, fo, files);
	}

	@Test
	public void testFragmentizePairedEnd()
	{
		SequenceGenerator g = new SeqGenSingleSequenceMultipleRepeats();
		SequenceGenerator.Options sgo = new SequenceGenerator.Options();
		sgo.length = 10000;
		CharSequence s = g.generateSequence(sgo);
		Fragmentizer.Options fo = new Fragmentizer.Options();
		fo.fragmentCount = 500;
		fo.fragmentLength = 200;
		fo.fragmentLengthSd = 4.0;
		fo.readLength = 50;
		List<List<? extends Fragment>> list = Fragmentizer.fragmentizePairedEnd(s, fo);
		assertEquals(2, list.size());
		for (List<? extends Fragment> l : list)
		{
			for (Fragment f : l)
			{
				assertTrue("Fragment was empty", !f.getSequence().equals(""));
			}
		}
	}
}
