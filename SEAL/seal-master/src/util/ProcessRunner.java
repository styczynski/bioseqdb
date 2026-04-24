package util;

import org.apache.log4j.Logger;
import org.apache.log4j.NDC;

import java.io.*;
import java.util.List;

/**
 * Encapsulates a lot of boilerplate code
 *
 * @author mruffalo
 */
public class ProcessRunner
{
	public static void run(Logger log, List<String> commands)
	{
		run(log, null);
	}

	/**
	 * Runs a command. Logs stdout and stderr of the subprocess.
	 *
	 * @param commands
	 */
	public static void run(Logger log, List<String> commands, File directory)
	{
		ProcessBuilder pb = new ProcessBuilder(commands);
		if (directory != null)
		{
			pb.directory(directory);
		}
		try
		{
			Process p = pb.start();
			BufferedReader stdout = new BufferedReader(
				new InputStreamReader(p.getInputStream()));
			BufferedReader stderr = new BufferedReader(
				new InputStreamReader(p.getErrorStream()));
			String line = null;
			NDC.push("stdout");
			while ((line = stdout.readLine()) != null)
			{
				log.info(line);
			}
			NDC.pop();
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
}
