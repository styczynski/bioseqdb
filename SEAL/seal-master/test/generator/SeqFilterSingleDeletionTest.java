package generator;

import static org.junit.Assert.*;
import org.junit.Test;

public class SeqFilterSingleDeletionTest
{
	@Test
	public void testDeleteSubstring()
	{
		SeqFilterSingleDeletion.Options o = new SeqFilterSingleDeletion.Options();
		o.length = 10;
		CharSequence s = SequenceGenerator.generateSequence(SequenceGenerator.NUCLEOTIDES, 50);
		SeqFilterSingleDeletion f = new SeqFilterSingleDeletion(o);
		CharSequence d = f.filter(s);
		assertNotNull(d);
		int deletePosition = f.getDeletePosition();
		assertEquals(s.subSequence(0, deletePosition), d.subSequence(0, deletePosition));
		assertEquals(s.subSequence(deletePosition + o.length, s.length()), d.subSequence(
			deletePosition, d.length()));
	}
}
