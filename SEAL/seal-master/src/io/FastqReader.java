package io;

import java.io.BufferedReader;
import java.io.Reader;

/**
 * TODO: Determine how these classes should work at a high level. Perhaps
 * similar to a BufferedReader: instantiate with another Reader and provide a
 * getFragment method? It probably makes sense for this class to use a
 * BufferedReader; its {@link java.io.BufferedReader#readLine()} method isn't
 * worth reinventing.
 * 
 * @author mruffalo
 */
public class FastqReader
{
	private static enum State
	{
		READ_HEADER,
		READ_DATA,
		QUALITY_HEADER,
		QUALITY_DATA;
	}

	private final BufferedReader source;

	public FastqReader(Reader in)
	{
		source = new BufferedReader(in);
	}
}
