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

public class SoapInterface extends AlignmentToolInterface
{
	public static final String SOAP_COMMAND = "soap";
	public static final String INDEX_COMMAND = "2bwt-builder";
	public static final String SOAP2SAM_COMMAND = "soap2sam";
	public static final String ALIGN_INDEX_OPTION = "-D";
	public static final String[] ALIGN_QUERY_OPTIONS = {"-a", "-b"};

	public SoapInterface(int index_, String description_, List<Integer> thresholds_, Options o_,
		Map<String, AlignmentResults> m_)
	{
		super(index_, description_, thresholds_, o_, m_);
	}

	public void createIndex()
	{
		String index_filename = o.genome.getName() + ".index";
		File file_to_check = new File(o.genome.getParentFile(), o.genome.getName() + ".index.bwt");
		o.index = new File(o.genome.getParentFile(), index_filename);
		if (file_to_check.isFile())
		{
			log.debug("Index found; skipping");
		}
		else
		{
			List<String> commands = new ArrayList<String>();
			commands.add(INDEX_COMMAND);
			commands.add(o.genome.getAbsolutePath());
			ProcessRunner.run(log, commands, o.genome.getParentFile());
		}
	}

	@Override
	public void align()
	{
		log.info("Aligning reads");
		List<String> commands = new ArrayList<String>();
		commands.add(SOAP_COMMAND);
		commands.add(ALIGN_INDEX_OPTION);
		commands.add(o.index.getAbsolutePath());
		for (int i = 0; i < o.reads.size(); i++)
		{
			commands.add(ALIGN_QUERY_OPTIONS[i]);
			commands.add(o.reads.get(i).reads.getAbsolutePath());
		}
		commands.add("-o");
		commands.add(o.raw_output.getAbsolutePath());
		ProcessRunner.run(log, commands, o.genome.getParentFile());
	}

	/**
	 * TODO: Add ProcessRunner functionality for this
	 */
	private void convertToSamFormat()
	{
		List<String> commands = new ArrayList<String>();
		commands.add(SOAP2SAM_COMMAND);
		commands.add(o.raw_output.getAbsolutePath());
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
	public void preAlignmentProcessing()
	{
		log.info("Indexing genome");
		createIndex();
	}

	@Override
	public void postAlignmentProcessing()
	{
		log.info("Converting output to SAM format");
		convertToSamFormat();
	}
}
