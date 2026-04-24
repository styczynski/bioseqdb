package assembly;

/**
 * 
 */
public enum FragmentPositionSource
{
	ORIGINAL_SEQUENCE("Original Position"),
	ASSEMBLED_SEQUENCE("Assembled Position");

	public final String guiDescription;

	private FragmentPositionSource(String guiDescription_)
	{
		guiDescription = guiDescription_;
	}
}
