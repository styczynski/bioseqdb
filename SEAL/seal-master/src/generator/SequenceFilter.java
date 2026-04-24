package generator;

public interface SequenceFilter
{
	public CharSequence filter(CharSequence input);

	/**
	 * @return A free-form description of what this SequenceFilter did.
	 *         Hopefully machine-readable for another analysis step.
	 */
	public String getDescription();
}
