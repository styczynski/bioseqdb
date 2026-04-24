package io;

import java.io.BufferedWriter;
import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.io.Writer;
import java.util.List;

import assembly.Fragment;
import assembly.FragmentPositionSource;

public class FastaWriter
{
	private Writer output;

	public FastaWriter(Writer out_)
	{
		output = out_;
	}

	public static void writeSequence(CharSequence sequence, File file) throws IOException
	{
		BufferedWriter output = null;
		try
		{
			output = new BufferedWriter(new FileWriter(file));
			output.write(">COMPLETED_SEQUENCE");
			output.write(System.getProperty("line.separator"));
			// TODO: Improve this
			output.write(sequence.toString());
			output.write(System.getProperty("line.separator"));
		}
		finally
		{
			if (output != null)
			{
				output.close();
			}
		}
	}

	public static void writeMultipartSequences(List<MultipartSequence> sequences,
			File file) throws IOException
	{
		BufferedWriter output = null;
		try
		{
			output = new BufferedWriter(new FileWriter(file));
			for (int i = 0; i < sequences.size(); i++)
			{
				CharSequence sequence = sequences.get(i).sequence;
				String identifier = String.format(">SEQUENCE_PART_%d", i);
				output.write(identifier);
				output.write(System.getProperty("line.separator"));
				// TODO: Improve this
				output.write(sequence.toString());
				output.write(System.getProperty("line.separator"));
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

	public static void writeFragments(List<? extends Fragment> fragments, File file)
			throws IOException
	{
		BufferedWriter output = null;
		try
		{
			output = new BufferedWriter(new FileWriter(file));
			int i = 0;
			for (Fragment fragment : fragments)
			{
				output.write(String.format(">%d", i++));
				Integer originalPosition = fragment.getPosition(FragmentPositionSource.ORIGINAL_SEQUENCE);
				if (originalPosition != null)
				{
					output.write(String.format(":READ_POS=%d", originalPosition));
				}
				Integer assembledPosition = fragment.getPosition(FragmentPositionSource.ASSEMBLED_SEQUENCE);
				if (assembledPosition != null)
				{
					output.write(String.format(",%d", assembledPosition));
				}
				output.write(String.format("%n"));
				// TODO: Improve this to be Rope-smart
				output.write(fragment.getSequence().toString());
				output.write(String.format("%n"));
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
		output.write(String.format(">%s%n", fragmentIdentifier.toString()));
		// TODO: Improve this to be Rope-smart
		output.write(fragment.toString());
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
		BufferedWriter output = null;
		try
		{
			FastaWriter writer = new FastaWriter(new BufferedWriter(new FileWriter(file)));

			int i = 0;
			for (Fragment fragment : fragments)
			{
				writer.writeFragment(fragment, pairedIndex, i++);
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
