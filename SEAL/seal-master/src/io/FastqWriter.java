package io;

import java.io.BufferedWriter;
import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.io.Writer;
import java.util.List;
import assembly.Fragment;
import assembly.FragmentPositionSource;

public class FastqWriter
{
	private Writer output;

	public FastqWriter(Writer out_)
	{
		output = out_;
	}

	private static char getQualityCharacter(int offset, int value)
	{
		int adjustedValue = value;
		if (adjustedValue >= Constants.MAXIMUM_QUALITY_VALUE)
		{
			adjustedValue = Constants.MAXIMUM_QUALITY_VALUE;
		}
		return Constants.FASTQ_QUALITY_CHARACTERS.charAt(adjustedValue);
	}

	/**
	 * Writes a single Fragment to the internal Writer
	 * 
	 * @param fragment
	 * @param pairedIndex
	 * @param fragmentIndex
	 * @throws IOException
	 */
	public void writeFragment(Fragment fragment, int pairedIndex, int fragmentIndex)
		throws IOException
	{
		StringBuilder fragmentIdentifier = new StringBuilder();
		fragmentIdentifier.append(fragmentIndex);
		Integer originalPosition = fragment.getPosition(FragmentPositionSource.ORIGINAL_SEQUENCE);
		if (originalPosition != null)
		{
			fragmentIdentifier.append(":READ_POS=");
			fragmentIdentifier.append(originalPosition);
		}
		Integer assembledPosition = fragment.getPosition(FragmentPositionSource.ASSEMBLED_SEQUENCE);
		if (assembledPosition != null)
		{
			fragmentIdentifier.append(",");
			fragmentIdentifier.append(assembledPosition);
		}
		fragmentIdentifier.append("/");
		fragmentIdentifier.append(pairedIndex);
		output.write(String.format("@%s%n", fragmentIdentifier.toString()));
		// TODO: Improve this to be Rope-smart
		output.write(fragment.toString());
		output.write(String.format("%n"));
		output.write(String.format("+%s%n", fragmentIdentifier.toString()));
		StringBuilder sb = new StringBuilder();
		for (int j = 0; j < fragment.toString().length(); j++)
		{
			// TODO: Un-hardcode Illumina 1.3 settings
			sb.append(getQualityCharacter(Constants.ILLUMINA_1_3_OFFSET, fragment.getReadQuality(j)));
		}
		output.write(sb.toString());
		output.write(String.format("%n"));
	}

	public void close() throws IOException
	{
		output.close();
	}

	/**
	 * TODO: Improve this API in the same style as the unused methods in
	 * {@link FastaWriter}
	 * 
	 * @param fragments
	 * @param file
	 * @throws IOException
	 */
	public static void writeFragments(List<? extends Fragment> fragments, File file, int pairedIndex)
		throws IOException
	{
		FastqWriter output = null;
		try
		{
			output = new FastqWriter(new BufferedWriter(new FileWriter(file)));

			int i = 0;
			for (Fragment fragment : fragments)
			{
				output.writeFragment(fragment, pairedIndex, i++);
			}
		}
		finally
		{
			if (output != null)
			{
				output.close();
			}
		}
	}
}
