package assembly;

import static org.junit.Assert.*;
import java.util.List;
import generator.SeqGenSingleSequenceMultipleRepeats;
import generator.SequenceGenerator;
import org.junit.Test;

public class FragmentTest
{
	private static final int PAIRED_END_LENGTH = 100;

	@Test
	public void testPairedEndClone()
	{
		SequenceGenerator g = new SeqGenSingleSequenceMultipleRepeats();
		SequenceGenerator.Options o = new SequenceGenerator.Options();
		final int fragmentLength = 500;
		o.length = fragmentLength;
		CharSequence s = g.generateSequence(o);
		Fragment f = new Fragment(s);
		List<? extends Fragment> l = f.pairedEndClone(PAIRED_END_LENGTH);
		assertEquals(2, l.size());
		assertEquals(s.subSequence(0, PAIRED_END_LENGTH), l.get(0).getSequence());
		assertEquals(s.subSequence(fragmentLength - PAIRED_END_LENGTH, fragmentLength),
			l.get(1).getSequence());
	}
}
