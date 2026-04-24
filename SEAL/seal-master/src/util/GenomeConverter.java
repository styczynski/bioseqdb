package util;

import com.beust.jcommander.IStringConverter;
import external.AlignmentToolService;

import java.util.ArrayList;
import java.util.List;
import java.util.regex.Pattern;

public class GenomeConverter implements IStringConverter<AlignmentToolService.Genome>
{
	@Override
	public AlignmentToolService.Genome convert(String s)
	{
		AlignmentToolService.Genome g = AlignmentToolService.Genome.valueOf(s);
		return g;
	}
}
