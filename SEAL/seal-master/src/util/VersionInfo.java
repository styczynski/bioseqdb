package util;

import java.io.IOException;
import java.io.InputStream;
import java.util.Properties;

public class VersionInfo
{
	public static final String SOFTWARE_VERSION;
	private static final String UNKNOWN_VERSION = "Unknown";

	static
	{
		InputStream istream = VersionInfo.class.getResourceAsStream("version.properties");
		Properties p = new Properties();
		String version = UNKNOWN_VERSION;
		if (istream != null)
		{
			try
			{
				p.load(istream);
				version = p.getProperty("software.version", version);
			}
			catch (IOException e)
			{
				e.printStackTrace();
			}
		}
		SOFTWARE_VERSION = version;
	}
}
