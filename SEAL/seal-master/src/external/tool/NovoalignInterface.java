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

public class NovoalignInterface extends AlignmentToolInterface
{
	public static final String NOVOINDEX_COMMAND = "novoindex";
	public static final String NOVOALIGN_COMMAND = "novoalign";
	public static final String NOVOALIGN_INDEX_PATH_OPTION = "-d";
	public static final String NOVOALIGN_SEQUENCE_PATH_OPTION = "-f";
	public static final String NOVOALIGN_OUTPUT_FORMAT_OPTION = "-o";
	public static final String NOVOALIGN_OUTPUT_FORMAT_SAM = "SAM";

	public NovoalignInterface(int index_, String description_, List<Integer> thresholds_,
		Options o_, Map<String, AlignmentResults> m_)
	{
		super(index_, description_, thresholds_, o_, m_);
	}

	/**
	 * TODO: Add ProcessRunner functionality for this
	 */
	@Override
	public void align()
	{
		log.info("Aligning reads");
		List<String> commands = new ArrayList<String>();
		commands.add(NOVOALIGN_COMMAND);
		commands.add(NOVOALIGN_OUTPUT_FORMAT_OPTION);
		commands.add(NOVOALIGN_OUTPUT_FORMAT_SAM);
		commands.add(NOVOALIGN_INDEX_PATH_OPTION);
		commands.add(o.index.getAbsolutePath());
		commands.add(NOVOALIGN_SEQUENCE_PATH_OPTION);
		for (Options.Reads r : o.reads)
		{
			commands.add(r.reads.getAbsolutePath());
		}
		ProcessBuilder pb = new ProcessBuilder(commands);
		pb.directory(o.genome.getParentFile());
		try
		{
			Process p = pb.start();
			BufferedReader stdout = new BufferedReader(new InputStreamReader(p.getInputStream()));
			BufferedReader stderr = new BufferedReader(new InputStreamReader(p.getErrorStream()));
			String line = null;
			FileWriter w = new FileWriter(o.sam_output);
			while ((line = stdout.readLine()) != null)
			{
				w.write(String.format("%s%n", line));
			}
			w.close();
			NDC.push("stderr");
			while ((line = stderr.readLine()) != null)
			{
				log.info(line);
			}
			NDC.pop();
			p.waitFor();
		}
		catch (IOException e)
		{
			e.printStackTrace();
		}
		catch (InterruptedException e)
		{
			e.printStackTrace();
		}
	}

	@Override
	public void postAlignmentProcessing()
	{
	}

	public void createIndex()
	{
		o.index = new File(o.genome.getParentFile(), o.genome.getName() + ".index");
		if (o.index.isFile())
		{
			log.debug("Index found; skipping");
		}
		else
		{
			log.info("Building index");
			List<String> commands = new ArrayList<String>();
			commands.add(NOVOINDEX_COMMAND);
			commands.add(o.index.getAbsolutePath());
			commands.add(o.genome.getAbsolutePath());
			ProcessRunner.run(log, commands, o.genome.getParentFile());
		}
	}

	@Override
	public void preAlignmentProcessing()
	{
		createIndex();
	}
}
