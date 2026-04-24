package generator;

import static org.junit.Assert.*;
import org.junit.Test;

public class SequenceGeneratorTest
{
	@Test
	public void testGenerateSequenceStringInt()
	{
		CharSequence string = SequenceGenerator.generateSequence(SequenceGenerator.NUCLEOTIDES, 10);
		assertEquals(10, string.length());
	}
}
