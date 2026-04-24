/*
 *  RopeBuilder.java
 *  Copyright (C) 2007 Amin Ahmad.
 *
 *  This file is part of Java Ropes.
 *
 *  Java Ropes is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Java Ropes is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Java Ropes.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  Amin Ahmad can be contacted at amin.ahmad@gmail.com or on the web at
 *  www.ahmadsoft.org.
 */
package util.ropes;

import util.ropes.impl.FlatCharArrayRope;
import util.ropes.impl.FlatCharSequenceRope;

/**
 * A factory for building ropes.
 * 
 * @author Amin Ahmad
 */
public final class RopeBuilder
{
	/**
	 * Construct a rope from a character array.
	 * 
	 * @param sequence
	 *            a character array
	 * @return a rope representing the underlying character array.
	 */
	public static Rope build(final char[] sequence)
	{
		return new FlatCharArrayRope(sequence);
	}

	/**
	 * Construct a rope from an underlying character sequence.
	 * 
	 * @param sequence
	 *            the underlying character sequence.
	 * @return a rope representing the underlying character sequnce.
	 */
	public static Rope build(final CharSequence sequence)
	{
		if (sequence instanceof Rope)
			return (Rope) sequence;
		return new FlatCharSequenceRope(sequence);
	}
}
