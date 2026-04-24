package generator;

import static org.junit.Assert.*;
import org.junit.Test;

public class SeqGenSingleSequenceMultipleRepeatsTest
{
	@Test
	public void testGenerateSequenceIntIntInt()
	{
		SequenceGenerator sg = new SeqGenSingleSequenceMultipleRepeats();
		SequenceGenerator.Options o = new SequenceGenerator.Options();
		o.length = 100;
		o.repeatCount = 4;
		o.repeatLength = 10;
		CharSequence string = sg.generateSequence(o);
		assertEquals(100, string.length());
		System.out.println(string);
		o = new SequenceGenerator.Options();
		o.length = 10;
		o.repeatCount = 2;
		o.repeatLength = 5;
		string = sg.generateSequence(o);
		assertEquals(10, string.length());
		System.out.println(string);
	}
}
