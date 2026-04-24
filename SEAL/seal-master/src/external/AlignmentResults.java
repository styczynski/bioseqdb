/**
 * 
 */
package external;

import java.util.Map;
import java.util.TreeMap;
import external.AlignmentToolInterface.AlignmentOperation;

public class AlignmentResults
{
	public AlignmentResults()
	{
		positives = new TreeMap<Integer, Integer>();
		negatives = new TreeMap<Integer, Integer>();
	}

	/**
	 * Quality values of alignments that are at the correct location in the
	 * genome
	 */
	public Map<Integer, Integer> positives;
	/**
	 * Quality values of alignments that are at an incorrect location in the
	 * genome.
	 */
	public Map<Integer, Integer> negatives;
	/**
	 * Number of fragments that were missing from the tool's output. Always
	 * added to false negative counts.
	 */
	public int missingFragments = 0;
	/**
	 * Stores time for each operation
	 */
	public Map<AlignmentOperation, Long> timeMap;

	public FilteredAlignmentResults filter(int threshold)
	{
		int tp = 0;
		int tn = 0;
		int fp = 0;
		int fn = missingFragments;
		for (Map.Entry<Integer, Integer> p : positives.entrySet())
		{
			if (p.getKey() >= threshold)
			{
				tp += p.getValue();
			}
			else
			{
				fn += p.getValue();
			}
		}
		for (Map.Entry<Integer, Integer> n : negatives.entrySet())
		{
			if (n.getKey() >= threshold)
			{
				fp += n.getValue();
			}
			else
			{
				tn += n.getValue();
			}
		}
		return new FilteredAlignmentResults(threshold, tp, tn, fp, fn);
	}

}
