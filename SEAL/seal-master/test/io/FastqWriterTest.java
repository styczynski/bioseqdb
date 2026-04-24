package io;

import static org.junit.Assert.*;
import java.io.File;
import java.io.IOException;
import java.util.List;
import generator.Fragmentizer;
import generator.SequenceGenerator;
import org.junit.Test;
import assembly.Fragment;

public class FastqWriterTest
{
	/**
	 * Doesn't test anything automatically; writes stuff to a file for manual
	 * inspection
	 */
	@Test
	public void testWriteFragments()
	{
		CharSequence s = SequenceGenerator.generateSequence(SequenceGenerator.NUCLEOTIDES, 10000);
		Fragmentizer.Options o = new Fragmentizer.Options();
		o.fragmentLength = 200;
		o.fragmentLengthSd = 10;
		o.fragmentCount = 5;
		o.readLength = 50;
		o.readLengthSd = 1;
		List<? extends Fragment> l = Fragmentizer.fragmentize(s, o);
		File f = new File("test.fastq");
		try
		{
			System.out.printf("Writing to %s%n", f.getAbsolutePath());
			FastqWriter.writeFragments(l, f, 0);
		}
		catch (IOException e)
		{
			throw new RuntimeException(e);
		}
	}
}
