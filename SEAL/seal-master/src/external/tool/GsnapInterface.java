package external.tool;

import external.AlignmentResults;
import external.AlignmentToolInterface;
import org.apache.log4j.NDC;
import util.ProcessRunner;

import java.io.BufferedReader;
import java.io.FileWriter;
import java.io.IOException;
import java.io.InputStreamReader;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;

public class GsnapInterface extends AlignmentToolInterface
{
	private static final String INDEX_COMMAND = "gmap_build";
	private static final String ARGUMENT_GENOME_NAME = "-d";
	private static final String ALIGN_COMMAND = "gsnap";
	private static final String GENOME_NAME = "genome";
	private static final String OUTPUT_TYPE_OPTION = "-A";
	private static final String OUTPUT_TYPE_SAM = "sam";

	public GsnapInterface(int index_, String description_, List<Integer> thresholds_,
			Options o_, Map<String, AlignmentResults> m_)
	{
		super(index_, description_, thresholds_, o_, m_);
	}

	@Override
	public void preAlignmentProcessing()
	{
		List<String> commands = new ArrayList<String>();
		commands.add(INDEX_COMMAND);
		commands.add(ARGUMENT_GENOME_NAME);
		commands.add(GENOME_NAME);
		commands.add(o.genome.getAbsolutePath());
		ProcessRunner.run(log, commands, o.genome.getParentFile());
	}

	@Override
	public void align()
	{
		List<String> commands = new ArrayList<String>();
		commands.add(ALIGN_COMMAND);
		commands.add(ARGUMENT_GENOME_NAME);
		commands.add(GENOME_NAME);
		for (Options.Reads r : o.reads)
		{
			commands.add(r.reads.getAbsolutePath());
		}
		commands.add(OUTPUT_TYPE_OPTION);
		commands.add(OUTPUT_TYPE_SAM);
		ProcessRunner.run(log, commands, o.genome.getParentFile());
	}

	@Override
	public void postAlignmentProcessing()
	{
	}
}
