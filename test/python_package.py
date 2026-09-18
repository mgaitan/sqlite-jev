import sqlite3
from importlib.resources import as_file, files
import subprocess
import sys

import sqlite_jev


connection = sqlite3.connect(":memory:")
if hasattr(connection, "enable_load_extension"):
    assert sqlite_jev.load(connection) is connection
    assert connection.execute("select jev_version()").fetchone() == (
        sqlite_jev.__version__,
    )

    try:
        connection.load_extension("does-not-exist")
    except sqlite3.OperationalError as error:
        assert "not authorized" in str(error).lower()
    else:
        raise AssertionError("sqlite_jev.load() did not disable extension loading")
else:
    suffix = ".dylib" if sys.platform == "darwin" else ".so"
    extension = files(sqlite_jev).joinpath(f"jev{suffix}")
    with as_file(extension) as path:
        result = subprocess.run(
            ["sqlite3", ":memory:", f".load {path}", "select jev_version();"],
            check=True,
            capture_output=True,
            text=True,
        )
    assert result.stdout.strip() == sqlite_jev.__version__

print(f"sqlite-jev Python wheel {sqlite_jev.__version__}: ok")
