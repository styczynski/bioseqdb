package io;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileNotFoundException;
import java.io.FileReader;
import java.io.IOException;
import java.io.Reader;
import java.util.HashSet;
import java.util.Set;
import java.util.regex.Matcher;
import external.AlignmentResults;
import external.AlignmentToolInterface.Options;
import org.apache.log4j.Logger;

public class SamReader
{
	/**
	 * TODO: Use this
	 */
	private final BufferedReader source;

	public SamReader(Reader in)
	{
		source = new BufferedReader(in);
	}

	public static Set<String> readMappedFragmentSet(File sam_output, int size)
	{
		Set<String> correctlyMappedFragments = new HashSet<String>(size);
		try
		{
			BufferedReader r = new BufferedReader(new FileReader(sam_output));
			String line = null;
			while ((line = r.readLine()) != null)
			{
				if (line.startsWith("@") || line.startsWith("#"))
				{
					continue;
				}
				String[] pieces = line.split("\\s+");
				if (pieces.length <= 3)
				{
					continue;
				}
				String fragmentIdentifier = pieces[0];
				int readPosition = -1;
				Matcher m = Constants.READ_POSITION_HEADER.matcher(pieces[0]);
				if (m.matches())
				{
					readPosition = Integer.parseInt(m.group(2));
				}
				int alignedPosition = Integer.parseInt(pieces[3]) - 1;
				if (readPosition == alignedPosition)
				{
					correctlyMappedFragments.add(fragmentIdentifier);
				}
			}
		}
		catch (FileNotFoundException e)
		{
			e.printStackTrace();
		}
		catch (IOException e)
		{
			e.printStackTrace();
		}
		return correctlyMappedFragments;
	}

	public static AlignmentResults readAlignment(Logger log, Options o, int fragmentCount,
		Set<String> correctlyMappedFragments)
	{
		log.info("Reading alignment");
		Set<String> totalMappedFragments = new HashSet<String>(fragmentCount);
		AlignmentResults rs = new AlignmentResults();
		try
		{
			BufferedReader r = new BufferedReader(new FileReader(o.sam_output));
			String line = null;
			while ((line = r.readLine()) != null)
			{
				if (line.startsWith("@") || line.startsWith("#"))
				{
					continue;
				}
				String[] pieces = line.split("\\s+");
				if (pieces.length <= 3)
				{
					continue;
				}
				String fragmentIdentifier = pieces[0];
				int readPosition = -1;
				Matcher m = Constants.READ_POSITION_HEADER.matcher(fragmentIdentifier);
				if (m.matches())
				{
					readPosition = Integer.parseInt(m.group(2));
				}
				int alignedPosition = Integer.parseInt(pieces[3]) - 1;
				int mappingScore = Integer.parseInt(pieces[4]);
				if (!rs.positives.containsKey(mappingScore))
				{
					rs.positives.put(mappingScore, 0);
				}
				if (!rs.negatives.containsKey(mappingScore))
				{
					rs.negatives.put(mappingScore, 0);
				}
				if (readPosition == alignedPosition)
				{
					rs.positives.put(mappingScore, rs.positives.get(mappingScore) + 1);
				}
				else if (o.penalize_duplicate_mappings
						|| (!o.penalize_duplicate_mappings && !correctlyMappedFragments.contains(fragmentIdentifier)))
				{
					rs.negatives.put(mappingScore, rs.negatives.get(mappingScore) + 1);
				}
				totalMappedFragments.add(fragmentIdentifier);
			}
			/*
			 * If a fragment didn't appear in the output at all, count it as a
			 * false negative
			 */
			if (fragmentCount >= totalMappedFragments.size())
			{
				rs.missingFragments += (fragmentCount - totalMappedFragments.size());
			}
		}
		catch (FileNotFoundException e)
		{
			e.printStackTrace();
		}
		catch (IOException e)
		{
			e.printStackTrace();
		}
		return rs;
	}
}
