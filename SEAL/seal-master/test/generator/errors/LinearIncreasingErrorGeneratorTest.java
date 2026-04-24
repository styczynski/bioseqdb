package generator.errors;

import generator.SeqGenSingleSequenceMultipleRepeats;
import generator.SequenceGenerator;
import generator.SequenceGenerator.Options;
import generator.errors.FragmentErrorGenerator;
import generator.errors.LinearIncreasingErrorGenerator;
import org.junit.Test;

public class LinearIncreasingErrorGeneratorTest
{
	@Test
	public void testGenerateErrors()
	{
		SeqGenSingleSequenceMultipleRepeats sg = new SeqGenSingleSequenceMultipleRepeats();
		SequenceGenerator.Options sgo = new SequenceGenerator.Options();
		sgo.length = 100;
		CharSequence orig = sg.generateSequence(sgo);
		FragmentErrorGenerator eg;

		eg = new LinearIncreasingErrorGenerator(SequenceGenerator.NUCLEOTIDES, 0.0, 1.0);
		eg.setVerboseOutput(true);
		eg.generateErrors(orig);

		eg = new LinearIncreasingErrorGenerator(SequenceGenerator.NUCLEOTIDES, 0.5, 1.0);
		eg.setVerboseOutput(true);
		eg.generateErrors(orig);

		eg = new LinearIncreasingErrorGenerator(SequenceGenerator.NUCLEOTIDES, 1.0, 0.0);
		eg.setVerboseOutput(true);
		eg.generateErrors(orig);
	}
}
