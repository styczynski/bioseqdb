package external;

import com.beust.jcommander.Parameter;

import java.util.ArrayList;
import java.util.List;

/**
 * Encapsulates common information in ErrorRateEvaluation, etc.
 */
public class EvaluationProgram
{
	@Parameter(names = {"-h", "--help"}, hidden = true)
	protected boolean showHelp = false;

	@Parameter(names = {"--tools"}, description = "Comma-separated list of which tools to run. Not case-sensitive.")
	protected String toolNames = "Bowtie,BWA,GSNAP,mrFAST,mrsFAST,Novoalign,SHRiMP,SOAP";

	protected List<String> getToolNames()
	{
		String[] tools = toolNames.split(",");
		List<String> list = new ArrayList<String>(tools.length);
		for (String tool : tools)
		{
			list.add(tool.toLowerCase());
		}
		return list;
	}
}
