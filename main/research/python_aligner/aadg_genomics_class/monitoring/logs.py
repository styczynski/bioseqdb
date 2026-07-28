"""
  Universal logging utilities.

  @Piotr Styczyński 2023 <piotr@styczynski.in>
  MIT LICENSE
  Algorithms for genomic data analysis | AADG | MIM UW | Bioinformatyka
"""
import inspect
import logging

BLACK, RED, GREEN, YELLOW, BLUE, MAGENTA, CYAN, WHITE = range(8)

RESET_SEQ = "\033[0m"
COLOR_SEQ = "\033[1;%dm"
BOLD_SEQ = "\033[1m"

COLORS = {
    "WARNING": YELLOW,
    "INFO": WHITE,
    "DEBUG": BLUE,
    "CRITICAL": YELLOW,
    "ERROR": RED,
}


def formatter_message(message: str, use_color: bool = True) -> str:
    """
    Format message string.
    :param message: Input message
    :param use_color: Use colouring for output?
    :return: Formatted message
    """
    if use_color:
        message = message.replace("$RESET", RESET_SEQ).replace(
            "$BOLD", BOLD_SEQ
        )
        for color_name in COLORS.keys():
            message = message.replace(
                f"${color_name}", COLOR_SEQ % (30 + COLORS[color_name])
            )
    else:
        message = message.replace("$RESET", "").replace("$BOLD", "")
    return message


class ColoredFormatter(logging.Formatter):
    """
    Log formatter that supports nice terminal output colouring.
    """

    def __init__(self, msg, use_color=True):
        logging.Formatter.__init__(self, msg)
        self.use_color = use_color

    def format(self, record) -> str:
        """
        Format logging record
        :param record: Logging record
        :return: Formatted log line
        """
        levelname = record.levelname
        if self.use_color and levelname in COLORS:
            levelname_color = (
                COLOR_SEQ % (30 + COLORS[levelname]) + levelname + RESET_SEQ
            )
            record.levelname = levelname_color
        return logging.Formatter.format(self, record)


class UniversalLoggerSet:
    """
    Collection of loggers.
    """

    def __init__(self):
        self._loggers = dict()

    def get_logger(self, name):
        """
        Get universal logger for a given name.
        If the logger does not exist yet, create it and add to the UniversalLoggerSet.
        If it exists already, just return it.
        :param name: Name of the loggger
        :return: Logger instance
        """
        if name in self._loggers:
            return self._loggers[name]
        logger = logging.getLogger(name)
        handler = logging.StreamHandler()
        logger.addHandler(handler)
        logger.setLevel(logging.DEBUG)

        universal_logger = UniversalLogger(logger, name)
        self._loggers[name] = universal_logger
        return universal_logger

    def __getattr__(self, attr):
        return self.get_logger(attr)


class UniversalLogger:
    """
    Wrapper for Python logging library logger and set of useful logging utilities.
    """

    def __init__(self, logger_instance, name):
        self._logger_instance = logger_instance
        self._enable_progress = True
        self._name = name

    def _prefromat_message(self, message, level):
        """
        Format message before passing to the inner logger.
        :param message: Input message string
        :param level: Level of logging for that message
        :return: Preformatted message
        """
        frame = inspect.currentframe()
        this_frame = frame  # Save current frame.

        frame_no = 0
        while frame.f_back:
            frame = frame.f_back
            if frame_no == 1:
                this_frame = frame
            frame_no = frame_no + 1

        this_frame_info = inspect.getframeinfo(this_frame)
        filename = "/".join(this_frame_info.filename.split("/")[-1:])
        lineno = this_frame_info.lineno

        level_block = "[$%s%-4s$RESET]" % (level, level.lower()[:4])
        name_block = "[$BOLD%-15s$RESET]" % self._name
        source_block = "%-20s |" % f"{filename}:{lineno}"
        return formatter_message(
            f"{level_block} {name_block} {source_block}  {message}"
        )

    def is_progress_enabled(self):
        """
        Check if displaying progress is enabled for this logger instance?
        :return: Is progress reporting enabled?
        """
        return self._enable_progress

    def error(self, message):
        """
        Log error message.
        :param message: Message to be logged
        """
        self._logger_instance.error(self._prefromat_message(message, "ERROR"))

    def info(self, message):
        """
        Log info message.
        :param message: Message to be logged
        """
        self._logger_instance.info(self._prefromat_message(message, "INFO"))

    def debug(self, message):
        """
        Log debug message.
        :param message: Message to be logged
        """
        self._logger_instance.debug(self._prefromat_message(message, "DEBUG"))


LOGS = UniversalLoggerSet()