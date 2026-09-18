"""Load sqlite-jev into a Python SQLite connection."""

from importlib.metadata import version
from importlib.resources import as_file, files
import sys


__version__ = version("sqlite-jev")


def load(connection):
    """Load sqlite-jev into a stdlib sqlite3 connection and return it."""
    suffix = ".dylib" if sys.platform == "darwin" else ".so"
    extension = files(__package__).joinpath(f"jev{suffix}")

    with as_file(extension) as path:
        connection.enable_load_extension(True)
        try:
            connection.load_extension(str(path))
        finally:
            connection.enable_load_extension(False)

    return connection


__all__ = ["__version__", "load"]
