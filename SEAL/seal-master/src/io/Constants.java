package io;

import java.util.regex.Pattern;

public class Constants
{
	public static final String FIELD_SEPARATOR = ",";
	public static final String FASTQ_QUALITY_MARKER = "+";
	public static final Pattern READ_POSITION_HEADER = Pattern.compile("[>@]?\\d+:READ_POS=((\\d+)(,(\\d+))?)(/\\d+)?");
	public static final String FASTQ_QUALITY_CHARACTERS = "!\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~";
	/*
	 * These offsets are not ASCII values; they are offsets
	 * into FASTQ_QUALITY_CHARACTERS.
	 *
	 * (Subtract 33 from each of the values shown at http://en.wikipedia.org/wiki/Fastq .)
	 */
	public static final int SANGER_OFFSET = 0;
	public static final int SOLEXA_OFFSET = 26;
	public static final int ILLUMINA_1_3_OFFSET = 33;
	public static final int ILLUMINA_1_5_OFFSET = 36;
	public static final int ILLUMINA_1_9_OFFSET = 0;
	/**
	 * From http://en.wikipedia.org/wiki/Fastq
	 */
	public static final int MAXIMUM_QUALITY_VALUE = 40;
}
