package io;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.IOException;
import java.io.Reader;
import java.util.ArrayList;
import java.util.LinkedList;
import java.util.List;
import java.util.Scanner;
import java.util.regex.Matcher;

import assembly.Fragment;
import assembly.FragmentPositionSource;
import util.ropes.*;

/**
 * TODO: Clean up IOExceptions
 */
public class FastaReader
{
	private final BufferedReader source;
	private Integer prevReadPosition = null;
	private Integer prevAssembledPosition = null;

	public FastaReader(Reader in)
	{
		source = new BufferedReader(in);
	}

	/**
	 * TODO: Finish this
	 *
	 * @return
	 */
	public Fragment readFragment()
	{
		Fragment f = null;
		try
		{
			StringBuilder sb = new StringBuilder();
			String line = null;
			Integer readPosition = null;
			Integer assembledPosition = null;
			while ((line = source.readLine()) != null)
			{
				Matcher m = Constants.READ_POSITION_HEADER.matcher(line);
				if (m.matches())
				{
					//
				}
				if (line.startsWith(">"))
				{
					break;
				}
				sb.append(line.trim());
			}
			f = new Fragment(sb.toString());
		}
		catch (IOException e)
		{
			System.err.println("Caught IOException:");
			e.printStackTrace();
		}
		return f;
	}

	public static String getSequence(File file, int expectedSize) throws IOException
	{
		StringBuilder sb = new StringBuilder(expectedSize);
		String line = null;
		BufferedReader input = null;
		try
		{
			input = new BufferedReader(new FileReader(file));
			while ((line = input.readLine()) != null)
			{
				if (line.startsWith(">"))
				{
					continue;
				}
				sb.append(line.trim());
			}
		}
		finally
		{
			if (input != null)
			{
				input.close();
			}
		}
		return sb.toString();
	}

	public static String getSequence(File file) throws IOException
	{
		StringBuilder sb = new StringBuilder();
		String temp = null;
		Scanner input = null;
		try
		{
			input = new Scanner(new BufferedReader(new FileReader(file)));
			while (input.hasNext())
			{
				temp = input.next();
				if (temp.startsWith(">"))
				{
					continue;
				}
				sb.append(temp);
			}
		}
		finally
		{
			if (input != null)
			{
				input.close();
			}
		}
		return sb.toString();
	}

	public static CharSequence getLargeSequence(File file) throws IOException
	{
		RopeBuilder rb = new RopeBuilder();
		Rope sequence = rb.build("");
		Scanner input = null;
		try
		{
			input = new Scanner(new BufferedReader(new FileReader(file)));
			while (input.hasNext())
			{
				String temp = input.next();
				if (temp.startsWith(">"))
				{
					continue;
				}
				sequence = sequence.append(rb.build(temp));
			}
		}
		finally
		{
			if (input != null)
			{
				input.close();
			}
		}
		return sequence;
	}

	public static List<Fragment> getFragments(File file) throws IOException
	{
		BufferedReader input = null;
		List<Fragment> list = new LinkedList<Fragment>();
		try
		{
			String line;
			StringBuilder sb = null;
			input = new BufferedReader(new FileReader(file));
			Integer prevOrigPosition = null;
			Integer prevAssembledPosition = null;
			while ((line = input.readLine()) != null)
			{
				if (line.startsWith(">"))
				{
					if (sb != null)
					{
						Fragment f = new Fragment(sb.toString());
						f.setPosition(FragmentPositionSource.ORIGINAL_SEQUENCE, prevOrigPosition);
						f.setPosition(FragmentPositionSource.ASSEMBLED_SEQUENCE,
								prevAssembledPosition);
						list.add(new Fragment(sb.toString()));
						prevOrigPosition = prevAssembledPosition = null;
					}
					Matcher m = Constants.READ_POSITION_HEADER.matcher(line);
					if (m.matches())
					{
						prevOrigPosition = Integer.parseInt(m.group(2));
						prevAssembledPosition = Integer.parseInt(m.group(4));
					}
					sb = new StringBuilder();
				}
				else
				{
					sb.append(line.trim());
				}
			}
		}
		finally
		{
			if (input != null)
			{
				input.close();
			}
		}
		return list;
	}

	public static List<MultipartSequence> getSequences(List<File> files)
	{
		int lastOffset = 0;
		List<MultipartSequence> list = new ArrayList<MultipartSequence>(files.size());
		try
		{
			for (File file : files)
			{
				CharSequence sequence = FastaReader.getSequence(file);
				list.add(new MultipartSequence(sequence, lastOffset));
				lastOffset += sequence.length();
			}
		}
		catch (IOException e)
		{
			System.err.println("Caught IOException:");
			e.printStackTrace();
		}
		return list;
	}
}
