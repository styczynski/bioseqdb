import assembly.Fragment;
import assembly.FragmentPositionSource;
import generator.Fragmentizer;
import generator.SequenceGenerator;
import io.FastaReader;
import io.FastaWriter;
import io.FastqWriter;
import io.MultipartSequence;
import org.junit.Test;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;

public class TestMultipartHandling
{
	/*
	These values are meant to be small enough so that a user can
	manually inspect the output
	 */
	private static final int NUMBER_OF_SEQUENCES = 2;
	private static final int SEQUENCE_LENGTH = 50;
	private static final int FRAGMENT_COUNT_PER_SEQUENCE = 10;
	private static final int FRAGMENT_LENGTH = 25;

	/**
	 * This is more of an integration test than a unit test, so it doesn't
	 * really belong in any specific package
	 */
	@Test
	public void testMultipartHandling()
	{
		// 1. Generate some sequences directly to FASTA files
		List<File> files = new ArrayList<File>(NUMBER_OF_SEQUENCES);
		for (int i = 0; i < NUMBER_OF_SEQUENCES; i++)
		{
			String filename = String.format("m%d.fasta", i);
			File file = new File(filename);
			files.add(file);
			CharSequence sequence = SequenceGenerator.generateSequence(SequenceGenerator.NUCLEOTIDES, SEQUENCE_LENGTH);
			try
			{
				FastaWriter.writeSequence(sequence, file);
			}
			catch (IOException e)
			{
				e.printStackTrace();
			}
		}
		List<MultipartSequence> sequences = FastaReader.getSequences(files);
		try
		{
			FastaWriter.writeMultipartSequences(sequences, new File("m.fasta"));
		}
		catch (IOException e)
		{
			e.printStackTrace();
		}
		Fragmentizer.Options o = new Fragmentizer.Options();
		o.fragmentCount = FRAGMENT_COUNT_PER_SEQUENCE;
		o.fragmentLength = FRAGMENT_LENGTH;
		List<Fragment> fragments = Fragmentizer.fragmentize(sequences, o);
		for (Fragment fragment : fragments)
		{
			System.out.printf("%03d %s%n", fragment.getPosition(FragmentPositionSource.ORIGINAL_SEQUENCE),
					fragment.getSequence());
		}
		try
		{
			FastqWriter.writeFragments(fragments, new File("f.fastq"), 0);
		}
		catch (IOException e)
		{
			e.printStackTrace();
		}
	}
}
