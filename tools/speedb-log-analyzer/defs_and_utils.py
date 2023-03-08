import datetime
from enum import Enum, auto
import re
import time
from calendar import timegm
from dataclasses import dataclass
import regexes


NO_COL_FAMILY = 'DB_WIDE'

OPTIONS_FILE_FOLDER = "options_files"

LOGGER_NAME = "log-analyzer-logger"

g_parsing_warnings = []


class ParsingError(Exception):
    def __init__(self, msg, file_path=None, line_idx=None):
        self.file_path = file_path
        self.line_idx = line_idx
        self.msg = msg

    def set_context(self, file_path, line_idx):
        self.file_path = file_path
        self.line_idx = line_idx

    def __str__(self):
        file_path = self.file_path if self.file_path is not None else ""
        line_num = self.line_idx+1 if self.line_idx is not None else -1
        return f"[{file_path} (line:{line_num})] - {self.msg}"


class LogFileNotFoundError(Exception):
    def __init__(self, file_path):
        self.msg = f"{file_path} Not Found"


class ParsingAssertion(ParsingError):
    def __init__(self, msg, file_path=None, line_idx=None):
        super().__init__(msg, file_path, line_idx)


class PointerResult(Enum):
    POINTER = auto()
    NULL_POINTER = auto()
    NOT_A_POINTER = auto()


class WarningType(str, Enum):
    WARN = "WARN"
    ERROR = "ERROR"
    FATAL = "FATAL"


def get_type(warning_type_str):
    return WarningType(warning_type_str)


class ConsoleOutputType(str, Enum):
    SHORT = "short"
    FULL = "full"


def try_parse_pointer(value_str):
    value_str = value_str.strip()
    if value_str == "(nil)":
        return PointerResult.NULL_POINTER
    else:
        match = re.findall(r'0x[\dA-Fa-f]+', value_str)
        return PointerResult.POINTER if len(match) == 1 else \
            PointerResult.NOT_A_POINTER


def get_gmt_timestamp(time_str):
    # example: '2018/07/25-11:25:45.782710' will be converted to the GMT
    # Unix timestamp 1532517945 (note: this method assumes that self.time
    # is in GMT)
    hr_time = time_str + 'GMT'
    return timegm(time.strptime(hr_time, "%Y/%m/%d-%H:%M:%S.%f%Z"))


def parse_date_time(date_time_str):
    try:
        return datetime.datetime.strptime(date_time_str,
                                          '%Y/%m/%d-%H:%M:%S.%f')
    except ValueError:
        return None


def get_value_by_unit(size_str, size_units_str):
    size_units_str = size_units_str.strip()

    multiplier = 1
    if size_units_str == "KB" or size_units_str == "K":
        multiplier = 2 ** 10
    elif size_units_str == "MB" or size_units_str == "M":
        multiplier = 2 ** 20
    elif size_units_str == "GB" or size_units_str == "G":
        multiplier = 2 ** 30
    elif size_units_str == "TB" or size_units_str == "T":
        multiplier = 2 ** 40
    elif size_units_str != '':
        assert False, f"Unexpected size units ({size_units_str}"

    result = float(size_str) * multiplier
    return int(result)


def get_size_for_display(size_in_bytes):
    if size_in_bytes < 2 ** 10:
        return str(size_in_bytes) + " B"
    elif size_in_bytes < 2 ** 20:
        size_units_str = "KB"
        divider = 2 ** 10
    elif size_in_bytes < 2 ** 30:
        size_units_str = "MB"
        divider = 2 ** 20
    elif size_in_bytes < 2 ** 40:
        size_units_str = "GB"
        divider = 2 ** 30
    else:
        size_units_str = "TB"
        divider = 2 ** 40

    return f"{float(size_in_bytes) / divider:.1f} {size_units_str}"


class ProductName(str, Enum):
    ROCKSDB = "RocksDB"
    SPEEDB = "Speedb"

    def __eq__(self, other):
        return self.lower() == other.lower()


@dataclass
class Version:
    major: int
    minor: int
    patch: int

    def __init__(self, version_str):
        version_parts = re.findall(regexes.VERSION_REGEX, version_str)
        assert len(version_parts) == 1 and len(version_parts[0]) == 3
        self.major = int(version_parts[0][0])
        self.minor = int(version_parts[0][1])
        self.patch = int(version_parts[0][2]) if version_parts[0][2] else None

    def get_patch_for_comparison(self):
        if self.patch is None:
            return -1
        return self.patch

    def __eq__(self, other):
        return self.major == other.major and \
               self.minor == other.minor and \
               self.get_patch_for_comparison() == \
               other.get_patch_for_comparison()

    def __lt__(self, other):
        if self.major != other.major:
            return self.major < other.major
        elif self.minor != other.minor:
            return self.minor < other.minor
        else:
            return self.get_patch_for_comparison() < \
                   other.get_patch_for_comparison()

    def __repr__(self):
        if self.patch is not None:
            patch = f".{self.patch}"
        else:
            patch = ""

        return f"{self.major}.{self.minor}{patch}"


@dataclass
class OptionsFileInfo:
    file_name: str
    version: Version

    def __lt__(self, other):
        return self.version < other.version