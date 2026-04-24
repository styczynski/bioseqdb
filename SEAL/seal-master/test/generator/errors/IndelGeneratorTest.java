package generator.errors;

import generator.SeqGenSingleSequenceMultipleRepeats;
import generator.SequenceGenerator;
import generator.SequenceGenerator.Options;
import generator.errors.FragmentErrorGenerator;
import generator.errors.IndelGenerator;
import org.junit.Test;

/**
 * Not really a unit test. Used for visually inspecting IndelGenerator's output.
 * 
 * @author mruffalo
 */
public class IndelGeneratorTest
{
	@Test
	public void testGenerateErrorsCharSequence()
	{
		SeqGenSingleSequenceMultipleRepeats sg = new SeqGenSingleSequenceMultipleRepeats();
		SequenceGenerator.Options sgo = new SequenceGenerator.Options();
		sgo.length = 100;
		CharSequence orig = sg.generateSequence(sgo);
		FragmentErrorGenerator eg;

		IndelGenerator.Options o;

		o = new IndelGenerator.Options();
		o.deleteLengthMean = 4;
		o.deleteLengthStdDev = 0.5;
		o.deleteProbability = 0.1;
		eg = new IndelGenerator(SequenceGenerator.NUCLEOTIDES, o);
		eg.setVerboseOutput(true);
		eg.generateErrors(orig);

		o = new IndelGenerator.Options();
		o.insertLengthMean = 4;
		o.insertLengthStdDev = 0.5;
		o.insertProbability = 0.1;
		eg = new IndelGenerator(SequenceGenerator.NUCLEOTIDES, o);
		eg.setVerboseOutput(true);
		eg.generateErrors(orig);

		o = new IndelGenerator.Options();
		o.deleteLengthMean = 4;
		o.deleteLengthStdDev = 0.5;
		o.deleteProbability = 0.1;
		o.insertLengthMean = 4;
		o.insertLengthStdDev = 0.5;
		o.insertProbability = 0.1;
		eg = new IndelGenerator(SequenceGenerator.NUCLEOTIDES, o);
		eg.setVerboseOutput(true);
		eg.generateErrors(orig);
	}
}
