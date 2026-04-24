package assembly;

import generator.Fragmentizer;

import java.util.ArrayList;
import java.util.EnumMap;
import java.util.List;
import java.util.Map;

public class Fragment implements Cloneable
{
	public static final int MAXIMUM_BASE_QUALITY = 40;

	protected final CharSequence sequence;
	protected final Map<FragmentPositionSource, Integer> positions;
	/**
	 * Array of Phred-scaled read quality values
	 */
	protected int[] readQuality;
	/**
	 * {@link #sequence} might be longer than this fragment if it was read with
	 * some {@link Fragmentizer.Options#overage} -- this is the actual length
	 * that we're interested in. This should be preserved when introducing
	 * indels.
	 */
	protected int length;

	public Fragment(CharSequence sequence_)
	{
		sequence = sequence_;
		length = sequence.length();
		positions = new EnumMap<FragmentPositionSource, Integer>(FragmentPositionSource.class);
		readQuality = new int[sequence.length()];
		for (int i = 0; i < sequence.length(); i++)
		{
			readQuality[i] = MAXIMUM_BASE_QUALITY;
		}
	}

	public Integer getPosition(FragmentPositionSource source)
	{
		return positions.get(source);
	}

	public void setPosition(FragmentPositionSource source, Integer value)
	{
		positions.put(source, value);
	}

	public int getLength()
	{
		return length;
	}

	public void setLength(int length_)
	{
		if (length_ >= 0 && length <= sequence.length())
		{
			length = length_;
		}
	}

	@Override
	public int hashCode()
	{
		return getSequence().hashCode();
	}

	@Override
	public boolean equals(Object o)
	{
		if (!(o instanceof Fragment) || o == null)
		{
			return false;
		}
		Fragment that = (Fragment) o;
		if (getSequence() == null)
		{
			return that.getSequence() == null;
		}
		return getSequence().equals(that.getSequence());
	}

	public boolean equalsWithPositions(Fragment that)
	{
		if (!getSequence().equals(that.getSequence()))
		{
			return false;
		}
		for (FragmentPositionSource source : FragmentPositionSource.values())
		{
			Integer thisPosition = this.getPosition(source);
			Integer thatPosition = that.getPosition(source);
			/*
			 * Only compare positions that are present in both fragments. If a
			 * position has not been assigned in one of the fragments, it
			 * doesn't count toward equality.
			 */
			if (thisPosition != null && thatPosition != null && !thisPosition.equals(thatPosition))
			{
				return false;
			}
		}
		return true;
	}

	public int getReadQuality(int index)
	{
		if (index >= readQuality.length)
		{
			index = readQuality.length - 1;
		}
		return readQuality[index];
	}

	public void setReadQuality(int index, int quality)
	{
		readQuality[index] = quality;
	}

	public void setReadQuality(int[] qualities)
	{
		readQuality = qualities;
	}

	/**
	 * @param length The length of the paired end reads on either side of the
	 *               actual fragment
	 * @return
	 */
	public List<? extends Fragment> pairedEndClone(int length)
	{
		List<PairedEndFragment> list = new ArrayList<PairedEndFragment>(2);
		PairedEndFragment one = new PairedEndFragment(sequence, true, length);
		one.clonePositionsAndReadQuality(this);
		PairedEndFragment two = new PairedEndFragment(sequence, false, length);
		two.clonePositionsAndReadQuality(this);
		list.add(one);
		list.add(two);
		return list;
	}

	public static List<List<? extends Fragment>> pairedEndClone(List<? extends Fragment> list,
			int length)
	{
		List<PairedEndFragment> one = new ArrayList<PairedEndFragment>(list.size());
		List<PairedEndFragment> two = new ArrayList<PairedEndFragment>(list.size());
		for (Fragment f : list)
		{
			List<? extends Fragment> clones = f.pairedEndClone(length);
			one.add((PairedEndFragment) clones.get(0));
			two.add((PairedEndFragment) clones.get(1));
		}
		List<List<? extends Fragment>> both = new ArrayList<List<? extends Fragment>>(2);
		both.add(one);
		both.add(two);
		return both;
	}

	/**
	 * Use {@link Fragment#getSequence()} if you can. In the event that the
	 * sequence is actually a {@link util.ropes.Rope} or something like that,
	 * don't force conversion to a String unless it's necessary.
	 */
	@Override
	public String toString()
	{
		return getSequence().toString();
	}

	public CharSequence getSequence()
	{
		return sequence.subSequence(0, length);
	}

	/**
	 * Copies positions and read quality data from the input fragment into this
	 * one
	 *
	 * @param that
	 */
	public void clonePositionsAndReadQuality(Fragment that)
	{
		for (FragmentPositionSource source : FragmentPositionSource.values())
		{
			this.setPosition(source, that.getPosition(source));
		}
		if (this.getSequence().length() == that.getSequence().length())
		{
			for (int i = 0; i < this.getSequence().length(); i++)
			{
				this.setReadQuality(i, that.getReadQuality(i));
			}
		}
	}
}
