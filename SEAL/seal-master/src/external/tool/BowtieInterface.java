package external.tool;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.io.InputStreamReader;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;

import external.AlignmentResults;
import external.AlignmentToolInterface;
import org.apache.log4j.NDC;
import util.ProcessRunner;

public class BowtieInterface extends AlignmentToolInterface
{
	private static final String BOWTIE_COMMAND = "bowtie";
	private static final String BOWTIE_INDEX_COMMAND = "bowtie-build";
	private static final String FIRST_PAIR_OPTION = "-1";
	private static final String SECOND_PAIR_OPTION = "-2";
	private static final String SAM_OPTION = "--sam";

	public BowtieInterface(int index_, String description_, List<Integer> thresholds_, Options o_,
		Map<String, AlignmentResults> m_)
	{
		super(index_, description_, thresholds_, o_, m_);
	}

	private void createIndex()
	{
		String index_base_name = "index";
		o.index = new File(o.genome.getParentFile(), index_base_name);
		String index_addition = ".1.ebwt";
		File index_file_to_check = new File(o.genome.getParentFile(), index_base_name
			+ index_addition);
		if (index_file_to_check.isFile())
		{
			log.debug("Index found; skipping");
		}
		else
		{
			List<String> commands = new ArrayList<String>();
			commands.add(BOWTIE_INDEX_COMMAND);
			commands.add(o.genome.getAbsolutePath());
			commands.add(index_base_name);
			ProcessRunner.run(log, commands, o.index.getParentFile());
		}
	}

	@Override
	public void preAlignmentProcessing()
	{
		createIndex();
	}

	@Override
	public void align()
	{
		log.info("Aligning reads");
		List<String> commands = new ArrayList<String>();
		commands.add(BOWTIE_COMMAND);
		commands.add(SAM_OPTION);
		commands.add(o.index.getName());
		if (o.is_paired_end)
		{
			commands.add(FIRST_PAIR_OPTION);
			commands.add(o.reads.get(0).reads.getAbsolutePath());
			commands.add(SECOND_PAIR_OPTION);
			commands.add(o.reads.get(1).reads.getAbsolutePath());
		}
		else
		{
			commands.add(o.reads.get(0).reads.getAbsolutePath());
		}
		commands.add(o.sam_output.getAbsolutePath());
		ProcessRunner.run(log, commands, o.index.getParentFile());
	}

	@Override
	public void postAlignmentProcessing()
	{
	}
}
