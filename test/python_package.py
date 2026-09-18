import sqlite3

import sqlite_jev


connection = sqlite3.connect(":memory:")
assert sqlite_jev.load(connection) is connection
assert connection.execute("select jev_version()").fetchone() == (sqlite_jev.__version__,)

try:
    connection.load_extension("does-not-exist")
except sqlite3.OperationalError as error:
    assert "not authorized" in str(error).lower()
else:
    raise AssertionError("sqlite_jev.load() did not disable extension loading")

print(f"sqlite-jev Python wheel {sqlite_jev.__version__}: ok")
