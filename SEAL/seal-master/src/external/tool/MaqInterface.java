package external.tool;

import io.Constants;
import io.SamReader;

import java.io.BufferedReader;
import java.io.File;
import java.io.IOException;
import java.io.InputStreamReader;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.regex.Matcher;

import external.AlignmentResults;
import external.AlignmentToolInterface;
import org.apache.log4j.NDC;
import util.ProcessRunner;

/**
 * TODO: Clean up local variables vs. method parameters
 *
 * @author mruffalo
 */
public class MaqInterface extends AlignmentToolInterface
{
	public static final String MAQ_COMMAND = "maq";
	public static final String FASTQ_TO_BFQ_COMMAND = "fastq2bfq";
	public static final String FASTA_TO_BFA_COMMAND = "fasta2bfa";
	public static final String ALIGN_COMMAND = "map";
	public static final String ASSEMBLE_COMMAND = "assemble";
	public static final String VIEW_ALIGNMENT_COMMAND = "mapview";

	public MaqInterface(int index_, String description_, List<Integer> thresholds_, Options o_,
		Map<String, AlignmentResults> m_)
	{
		super(index_, description_, thresholds_, o_, m_);
	}

	@Override
	public void align()
	{
		log.info("Aligning reads");
		List<String> commands = new ArrayList<String>();
		commands.add(MAQ_COMMAND);
		commands.add(ALIGN_COMMAND);
		commands.add(o.raw_output.getAbsolutePath());
		commands.add(o.binary_genome.getAbsolutePath());
		for (Options.Reads r : o.reads)
		{
			commands.add(r.binary_reads.getAbsolutePath());
		}
		ProcessRunner.run(log, commands, o.genome.getParentFile());
	}

	/**
	 * TODO: Duplicate much less code here
	 */
	public void convertGenomeToBfa()
	{
		List<String> commands = new ArrayList<String>();
		commands.add(MAQ_COMMAND);
		commands.add(FASTA_TO_BFA_COMMAND);
		commands.add(o.genome.getAbsolutePath());
		commands.add(o.binary_genome.getAbsolutePath());
		ProcessRunner.run(log, commands, o.genome.getParentFile());
	}

	/**
	 * TODO: Duplicate much less code here
	 */
	public void convertReadsToBfq()
	{
		for (Options.Reads r : o.reads)
		{
			List<String> commands = new ArrayList<String>();
			commands.add(MAQ_COMMAND);
			commands.add(FASTQ_TO_BFQ_COMMAND);
			commands.add(r.reads.getAbsolutePath());
			commands.add(r.binary_reads.getAbsolutePath());
			ProcessRunner.run(log, commands, r.reads.getParentFile());
		}
	}

	/**
	 * Can't use {@link SamReader} since MAQ doesn't output SAM files.
	 *
	 * TODO: Use ProcessRunner for this
	 */
	@Override
	public AlignmentResults readAlignment()
	{
		System.out.printf("%03d: Reading alignment...%n", index);
		AlignmentResults rs = new AlignmentResults();
		List<String> commands = new ArrayList<String>();
		commands.add(MAQ_COMMAND);
		commands.add(VIEW_ALIGNMENT_COMMAND);
		commands.add(o.raw_output.getAbsolutePath());
		for (String arg : commands)
		{
			System.err.printf("%03d: %s%n", index, arg);
		}
		ProcessBuilder pb = new ProcessBuilder(commands);
		pb.directory(o.genome.getParentFile());
		try
		{
			Process p = pb.start();
			BufferedReader stdout = new BufferedReader(new InputStreamReader(p.getInputStream()));
			BufferedReader stderr = new BufferedReader(new InputStreamReader(p.getErrorStream()));
			String line = null;
			while ((line = stdout.readLine()) != null)
			{
				if (line.startsWith("@"))
				{
					continue;
				}
				String[] pieces = line.split("\\s+");
				if (pieces.length <= 3)
				{
					continue;
				}
				int readPosition = -1;
				String fragmentIdentifier = pieces[0];
				Matcher m = Constants.READ_POSITION_HEADER.matcher(fragmentIdentifier);
				if (m.matches())
				{
					readPosition = Integer.parseInt(m.group(2));
				}
				int alignedPosition = Integer.parseInt(pieces[2]) - 1;
				int mappingScore = Integer.parseInt(pieces[6]);
				if (!rs.positives.containsKey(mappingScore))
				{
					rs.positives.put(mappingScore, 0);
				}
				if (!rs.negatives.containsKey(mappingScore))
				{
					rs.negatives.put(mappingScore, 0);
				}
				if (readPosition == alignedPosition)
				{
					rs.positives.put(mappingScore, rs.positives.get(mappingScore) + 1);
				}
				else
				{
					rs.negatives.put(mappingScore, rs.negatives.get(mappingScore) + 1);
				}
				totalMappedFragments.add(fragmentIdentifier);
			}
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
		return rs;
	}

	@Override
	public void preAlignmentProcessing()
	{
		convertReadsToBfq();
		convertGenomeToBfa();
	}

	@Override
	public void postAlignmentProcessing()
	{
	}
}
