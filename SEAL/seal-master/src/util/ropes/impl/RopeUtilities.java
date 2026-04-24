/*
 *  RopeUtilities.java
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
package util.ropes.impl;

import java.io.PrintStream;
import java.util.ArrayDeque;
import java.util.ArrayList;
import util.ropes.Rope;

/**
 * Contains utlities for manipulating ropes.
 * 
 * @author aahmad
 */
class RopeUtilities
{
	private static final long[] FIBONACCI = { 0l, 1l, 1l, 2l, 3l, 5l, 8l, 13l, 21l, 34l, 55l, 89l,
			144l, 233l, 377l, 610l, 987l, 1597l, 2584l, 4181l, 6765l, 10946l, 17711l, 28657l,
			46368l, 75025l, 121393l, 196418l, 317811l, 514229l, 832040l, 1346269l, 2178309l,
			3524578l, 5702887l, 9227465l, 14930352l, 24157817l, 39088169l, 63245986l, 102334155l,
			165580141l, 267914296l, 433494437l, 701408733l, 1134903170l, 1836311903l, 2971215073l,
			4807526976l, 7778742049l, 12586269025l, 20365011074l, 32951280099l, 53316291173l,
			86267571272l, 139583862445l, 225851433717l, 365435296162l, 591286729879l,
			956722026041l, 1548008755920l, 2504730781961l, 4052739537881l, 6557470319842l,
			10610209857723l, 17167680177565l, 27777890035288l, 44945570212853l, 72723460248141l,
			117669030460994l, 190392490709135l, 308061521170129l, 498454011879264l,
			806515533049393l, 1304969544928657l, 2111485077978050l, 3416454622906707l,
			5527939700884757l, 8944394323791464l, 14472334024676221l, 23416728348467685l,
			37889062373143906l, 61305790721611591l, 99194853094755497l, 160500643816367088l,
			259695496911122585l, 420196140727489673l, 679891637638612258l, 1100087778366101931l,
			1779979416004714189l, 2880067194370816120l, 4660046610375530309l, 7540113804746346429l };
	private static final short MAX_ROPE_DEPTH = 96;
	private static final String SPACES = "                                                                                                                                                                                                        ";

	public static RopeUtilities INSTANCE = new RopeUtilities();

	/**
	 * Rebalance a rope if the depth has exceeded MAX_ROPE_DEPTH. If the rope
	 * depth is less than MAX_ROPE_DEPTH or if the rope is of unknown type, no
	 * rebalancing will occur.
	 * 
	 * @param r
	 *            the rope to rebalance.
	 * @return a rebalanced copy of the specified rope.
	 */
	public Rope autoRebalance(final Rope r)
	{
		if (r instanceof AbstractRope && ((AbstractRope) r).depth() > RopeUtilities.MAX_ROPE_DEPTH)
		{
			return this.rebalance(r);
		}
		else
		{
			return r;
		}
	}

	/**
	 * Concatenate two ropes. Implements all recommended optimizations in
	 * "Ropes: an Alternative to Strings".
	 * 
	 * @param left
	 *            the first rope.
	 * @param right
	 *            the second rope.
	 * @return the concatenation of the specified ropes.
	 */
	Rope concatenate(final Rope left, final Rope right)
	{
		if (left.length() == 0)
			return right;
		if (right.length() == 0)
			return left;
		if ((long) left.length() + right.length() > Integer.MAX_VALUE)
			throw new IllegalArgumentException("Left length=" + left.length() + ", right length="
					+ right.length() + ". Concatenation would overflow length field.");
		final int combineLength = 17;
		if (left.length() + right.length() < combineLength)
		{
			return new FlatCharSequenceRope(left.toString() + right.toString());
		}
		if (!(left instanceof ConcatenationRope))
		{
			if (right instanceof ConcatenationRope)
			{
				final ConcatenationRope cRight = (ConcatenationRope) right;
				if (left.length() + cRight.getLeft().length() < combineLength)
					return this.autoRebalance(new ConcatenationRope(new FlatCharSequenceRope(
						left.toString() + cRight.getLeft().toString()), cRight.getRight()));
			}
		}
		if (!(right instanceof ConcatenationRope))
		{
			if (left instanceof ConcatenationRope)
			{
				final ConcatenationRope cLeft = (ConcatenationRope) left;
				if (right.length() + cLeft.getRight().length() < combineLength)
					return this.autoRebalance(new ConcatenationRope(cLeft.getLeft(),
						new FlatCharSequenceRope(cLeft.getRight().toString() + right.toString())));
			}
		}

		return this.autoRebalance(new ConcatenationRope(left, right));
	}

	/**
	 * Returns the depth of the specified rope.
	 * 
	 * @param r
	 *            the rope.
	 * @return the depth of the specified rope.
	 */
	byte depth(final Rope r)
	{
		if (r instanceof AbstractRope)
		{
			return ((AbstractRope) r).depth();
		}
		else
		{
			return 0;
			// throw new IllegalArgumentException("Bad rope");
		}
	}

	boolean isBalanced(final Rope r)
	{
		final byte depth = this.depth(r);
		if (depth >= RopeUtilities.FIBONACCI.length - 2)
			return false;
		// TODO: not necessarily valid w/e.g. padding char sequences.
		return (RopeUtilities.FIBONACCI[depth + 2] <= r.length());
	}

	public Rope rebalance(final Rope r)
	{
		// get all the nodes into a list

		final ArrayList<Rope> leafNodes = new ArrayList<Rope>();
		final ArrayDeque<Rope> toExamine = new ArrayDeque<Rope>();
		// begin a depth first loop.
		toExamine.add(r);
		while (toExamine.size() > 0)
		{
			final Rope x = toExamine.pop();
			if (x instanceof ConcatenationRope)
			{
				toExamine.push(((ConcatenationRope) x).getRight());
				toExamine.push(((ConcatenationRope) x).getLeft());
				continue;
			}
			else
			{
				leafNodes.add(x);
			}
		}
		Rope result = merge(leafNodes, 0, leafNodes.size());
		return result;
	}

	private Rope merge(ArrayList<Rope> leafNodes, int start, int end)
	{
		int range = end - start;
		switch (range)
		{
			case 1:
				return leafNodes.get(start);
			case 2:
				return new ConcatenationRope(leafNodes.get(start), leafNodes.get(start + 1));
			default:
				int middle = start + (range / 2);
				return new ConcatenationRope(merge(leafNodes, start, middle), merge(leafNodes,
					middle, end));
		}
	}

	/**
	 * Visualize a rope.
	 * 
	 * @param r
	 * @param out
	 */
	void visualize(final Rope r, final PrintStream out)
	{
		this.visualize(r, out, (byte) 0);
	}

	public void visualize(final Rope r, final PrintStream out, final int depth)
	{
		if (r instanceof FlatRope)
		{
			out.print(RopeUtilities.SPACES.substring(0, depth * 2));
			out.println("\"" + r + "\"");
			// out.println(r.length());
		}
		if (r instanceof SubstringRope)
		{
			out.print(RopeUtilities.SPACES.substring(0, depth * 2));
			out.println("substring " + r.length() + " \"" + r + "\"");
			// this.visualize(((SubstringRope)r).getRope(), out, depth+1);
		}
		if (r instanceof ConcatenationRope)
		{
			out.print(RopeUtilities.SPACES.substring(0, depth * 2));
			out.println("concat[left]");
			this.visualize(((ConcatenationRope) r).getLeft(), out, depth + 1);
			out.print(RopeUtilities.SPACES.substring(0, depth * 2));
			out.println("concat[right]");
			this.visualize(((ConcatenationRope) r).getRight(), out, depth + 1);
		}
	}

	public void stats(final Rope r, final PrintStream out)
	{
		int nonLeaf = 0;
		final ArrayList<Rope> leafNodes = new ArrayList<Rope>();
		final ArrayDeque<Rope> toExamine = new ArrayDeque<Rope>();
		// begin a depth first loop.
		toExamine.add(r);
		while (toExamine.size() > 0)
		{
			final Rope x = toExamine.pop();
			if (x instanceof ConcatenationRope)
			{
				++nonLeaf;
				toExamine.push(((ConcatenationRope) x).getRight());
				toExamine.push(((ConcatenationRope) x).getLeft());
			}
			else
			{
				leafNodes.add(x);
			}
		}
		out.println("rope(length=" + r.length() + ", leaf nodes=" + leafNodes.size()
				+ ", non-leaf nodes=" + nonLeaf + ", depth=" + RopeUtilities.INSTANCE.depth(r)
				+ ")");
	}
}
