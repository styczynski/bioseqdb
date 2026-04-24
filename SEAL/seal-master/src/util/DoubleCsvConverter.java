package util;

import com.beust.jcommander.IStringConverter;

import java.util.ArrayList;
import java.util.List;
import java.util.regex.Pattern;

public class DoubleCsvConverter implements IStringConverter<List<Double>>
{
	private static final Pattern COMMA = Pattern.compile(",");

	@Override
	public List<Double> convert(String s)
	{
		String[] pieces = COMMA.split(s);
		List<Double> list = new ArrayList<Double>(pieces.length);
		for (String piece: pieces)
		{
			list.add(Double.parseDouble(piece));
		}
		return list;
	}
}
