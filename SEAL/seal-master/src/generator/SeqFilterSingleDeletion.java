package generator;

import java.util.Random;

public class SeqFilterSingleDeletion implements SequenceFilter
{
	private Options o;
	/**
	 * This stores the position of the deletion after {@link #filter} has been
	 * called.
	 */
	private int deletePosition;

	public static class Options
	{
		public int length;
	}

	public SeqFilterSingleDeletion(Options o_)
	{
		o = o_;
	}

	/**
	 * @return the position of the deletion (after {@link #filter} has been
	 *         called)
	 */
	public int getDeletePosition()
	{
		return deletePosition;
	}

	@Override
	public CharSequence filter(CharSequence input)
	{
		int range = input.length() - o.length;
		StringBuilder sb = new StringBuilder(range);
		Random random = new Random();
		deletePosition = random.nextInt(range);
		sb.append(input.subSequence(0, deletePosition));
		sb.append(input.subSequence(deletePosition + o.length, input.length()));
		return sb.toString();
	}

	@Override
	public String getDescription()
	{
		// TODO: Figure out how useful this is
		return String.format("Delete position: %d", deletePosition);
	}
}
