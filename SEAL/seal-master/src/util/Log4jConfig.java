package util;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.text.DateFormat;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Properties;

import org.apache.log4j.Logger;
import org.apache.log4j.PropertyConfigurator;

public class Log4jConfig
{
	private static final File LOG4J_PROPERTIES_FILE = new File("conf", "log4j.raw.properties");
	private static final DateFormat LOG_TIME_STAMP = new SimpleDateFormat("yyyyMMdd-HHmmss");
	private static final String LOG_TEMPLATE = "log_%s.txt";

	public static void initialConfig()
	{
		System.out.printf("Reading log4j config from %s%n", LOG4J_PROPERTIES_FILE.getAbsolutePath());
		Properties p = new Properties();
		try
		{
			p.load(new FileInputStream(LOG4J_PROPERTIES_FILE));
		}
		catch (IOException e)
		{
			e.printStackTrace();
		}
		String logFileName = String.format(LOG_TEMPLATE, LOG_TIME_STAMP.format(new Date()));
		File logFile = new File(logFileName);
		p.setProperty("log4j.appender.F1.File", logFile.getAbsolutePath());
		PropertyConfigurator.configure(p);
		Logger log = Logger.getLogger(Log4jConfig.class);
		log.info("log4j configuration initialized");
	}
}
