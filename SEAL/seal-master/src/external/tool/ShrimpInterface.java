package external.tool;

import io.SamReader;

import java.io.BufferedReader;
import java.io.FileWriter;
import java.io.IOException;
import java.io.InputStreamReader;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;

import external.AlignmentResults;
import external.AlignmentToolInterface;
import org.apache.log4j.NDC;

public class ShrimpInterface extends AlignmentToolInterface
{
	private static final String SHRIMP_ALIGN_COMMAND = "gmapper-ls";

	public ShrimpInterface(int index_, String description_, List<Integer> thresholds_, Options o_,
		Map<String, AlignmentResults> m_)
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
		final int i = 0;
		List<String> commands = new ArrayList<String>();
		commands.add(SHRIMP_ALIGN_COMMAND);
		commands.add(o.reads.get(i).reads.getAbsolutePath());
		commands.add(o.genome.getAbsolutePath());
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

	/**
	 * Overridden here since SHRiMP requires FASTA reads
	 */
	@Override
	public void linkFragments()
	{
		// XXX Move this to AlignmentToolInterface
	}

	@Override
	public void preAlignmentProcessing()
	{
	}

	@Override
	public void postAlignmentProcessing()
	{
		correctlyMappedFragments = SamReader.readMappedFragmentSet(o.sam_output, fragmentCount);
	}
}
