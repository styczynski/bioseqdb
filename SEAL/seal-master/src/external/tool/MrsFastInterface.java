package external.tool;

import io.SamReader;

import java.io.BufferedReader;
import java.io.File;
import java.io.IOException;
import java.io.InputStreamReader;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;

import external.AlignmentResults;
import external.AlignmentToolInterface;
import org.apache.log4j.NDC;
import util.ProcessRunner;

public class MrsFastInterface extends AlignmentToolInterface
{
	public static final String SEQ_OPTION = "--seq";
	public static final String MRSFAST_COMMAND = "mrsfast";
	public static final String INDEX_COMMAND = "--index";
	public static final String SEARCH_COMMAND = "--search";
	public static final String OUTPUT_FILE_OPTION = "-o";

	public MrsFastInterface(int index_, String description_, List<Integer> thresholds_, Options o_,
		Map<String, AlignmentResults> m_)
	{
		super(index_, description_, thresholds_, o_, m_);
	}

	public void createIndex()
	{
		String index_filename = o.genome.getName() + ".index";
		o.index = new File(o.genome.getParentFile(), index_filename);
		if (o.index.isFile())
		{
			log.debug("Index found; skipping");
		}
		else
		{
			List<String> commands = new ArrayList<String>();
			commands.add(MRSFAST_COMMAND);
			commands.add(INDEX_COMMAND);
			commands.add(o.genome.getAbsolutePath());
			ProcessRunner.run(log, commands, o.genome.getParentFile());
		}
	}

	@Override
	public void align()
	{
		log.info("Aligning reads...");
		List<String> commands = new ArrayList<String>();
		commands.add(MRSFAST_COMMAND);
		commands.add(SEARCH_COMMAND);
		commands.add(o.genome.getAbsolutePath());
		commands.add(SEQ_OPTION);
		commands.add(o.reads.get(0).reads.getAbsolutePath());
		commands.add(OUTPUT_FILE_OPTION);
		commands.add(o.sam_output.getAbsolutePath());
		ProcessRunner.run(log, commands, o.genome.getParentFile());
	}

	@Override
	public void preAlignmentProcessing()
	{
		log.info("Indexing genome");
		createIndex();
	}

	@Override
	public void postAlignmentProcessing()
	{
		correctlyMappedFragments = SamReader.readMappedFragmentSet(o.sam_output, fragmentCount);
	}
}
