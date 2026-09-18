# sqlite-jev

Natural-language predicates, classification, and scoring for SQLite, powered by
[TypeSafe Jev](https://docs.typesafe.ai). It is inspired by
[`pg-jev`](https://github.com/realZachi/pg-jev), but shaped around SQLite's loadable-extension
and virtual-table APIs.

```sql
.load ./build/jev

SELECT t.id, t.subject, round(j.probability, 3) AS urgency
FROM jev_rows(
  'tickets',
  'The customer explicitly expresses urgency or says work is blocked',
  'noul',
  NULL,
  json_array('subject', 'message')
) AS j
JOIN tickets AS t ON t.rowid = j.source_rowid
WHERE j.probability >= 0.6;
```

`jev_rows` reads the selected columns, puts up to 40 rows in one shared state, asks one
question per row, and returns a virtual table that can be joined to the source by `rowid`.
Results are cached for the lifetime of the SQLite connection.

## Build

Requirements:

- SQLite 3.45 or newer, built with JSON support and loadable extensions
- A C11 compiler and `make`
- The `libcurl` runtime (`libcurl.so.4` on Linux or `libcurl.4.dylib` on macOS)
- A TypeSafe API key

The current development machine already has all of these; no extra system package is needed.

```bash
make
export TYPESAFE_API_KEY=...
sqlite3 my.db
```

Inside SQLite:

```sql
.load ./build/jev
SELECT jev_version();
```

`TYPESAFE_API_KEY` is the only environment variable used for the API key.

## Batched table queries

The signature is:

```sql
jev_rows(table_name, question [, kind [, criteria [, columns]]])
```

It returns `source_rowid`, `answer`, `probability`, `choice`, `score`, and `confidence`.
Columns that do not apply to the chosen primitive are `NULL`.

### Boolean judgment (Noul)

```sql
SELECT
  t.id,
  j.probability AS yes_probability,
  1.0 - j.probability AS no_probability,
  CASE WHEN j.probability >= 0.7 THEN 'yes' ELSE 'no' END AS answer
FROM jev_rows(
  'tickets',
  'The ticket explicitly asks for a refund',
  'noul',
  NULL,
  json_array('subject', 'message')
) AS j
JOIN tickets AS t ON t.rowid = j.source_rowid
ORDER BY t.id;
```

A Noul returns the probability of `yes`; `no` is its complement. Choosing the larger one is
equivalent to a `0.5` threshold. Keep the threshold in SQL so it can rise with the cost of a
false positive. The scalar `jev(state, condition)` uses `0.5` unless given a third argument.

### Classification (Choice)

```sql
SELECT t.id, j.choice AS team, j.confidence
FROM jev_rows(
  'tickets',
  'Which team should handle the main request?',
  'choice',
  json_object(
    'billing', 'Charges, invoices, payment methods, or refunds',
    'technical', 'Bugs, failures, or integrations',
    'sales', 'Pricing, plans, demos, or new purchases',
    'other', 'Anything outside those teams'
  ),
  json_array('subject', 'message')
) AS j
JOIN tickets AS t ON t.rowid = j.source_rowid;
```

### Ordered rating (Score)

```sql
SELECT t.id, j.score, j.confidence
FROM jev_rows(
  'tickets',
  'How frustrated is the customer?',
  'score',
  json_array('Calm and factual', 'Frustrated but civil', 'Very angry or abusive'),
  json_array('message')
) AS j
JOIN tickets AS t ON t.rowid = j.source_rowid;
```

For accuracy and cost, include only the columns the judgment needs. `jev_rows` requires a
rowid table. To prefilter a large data set, materialize the filtered rows into a temporary
table and evaluate that table.

## Scalar functions

Scalar functions are convenient for one record. When scanning a table, prefer `jev_rows` so
Jev can evaluate many questions in a single request.

| Function | Result |
| --- | --- |
| `jev(state, condition [, threshold])` | Boolean Noul predicate; default threshold is `0.5` |
| `jev_prob(state, condition)` | Noul probability from 0 to 1 |
| `jev_choice(state, question, criteria_json)` | Most likely Choice key |
| `jev_score(state, question, levels_json)` | Probability-weighted Score level |
| `jev_score_norm(state, question, levels_json)` | Score normalized to 0 through 1 |
| `jev_confidence(state, question, kind, criteria_json)` | Choice or Score confidence |
| `jev_eval(state, question [, kind [, criteria_json]])` | Full answer JSON |
| `jev_stats()` | Connection-local usage and cache statistics |
| `jev_cache_clear()` | Clears the connection-local answer cache |
| `jev_version()` | Extension version |

Pass structured state with SQLite JSON functions:

```sql
SELECT jev_prob(
  json_object('subject', subject, 'message', message),
  'The customer explicitly expresses urgency'
)
FROM tickets
WHERE id = 42;
```

## Configuration

Configuration is connection-local:

```sql
SELECT jev_config('model', 'jev-latest');
SELECT jev_config('batch_size', 40);
SELECT jev_config('max_rows', 500);
SELECT jev_config('timeout', 90);
SELECT jev_config('api_url', 'https://api.typesafe.ai/v1/systemone');
SELECT jev_config('api_key', '...');
```

`max_rows` is a spend guard. A `jev_rows` scan above the limit fails before sending any data.
The API key is never returned by `jev_config`; it reports only `set` or `unset`.

## Python

Add the platform package to a project and load it with the Python wrapper:

```bash
uv add sqlite-jev
```

```python
import sqlite3
import sqlite_jev

connection = sqlite_jev.load(sqlite3.connect(":memory:"))
```

The package contains the same native extension as the standalone release, has no runtime
Python dependencies, and supports maintained Python versions starting with Python 3.10. The
interpreter's `sqlite3` module must have loadable-extension support enabled.

To load a standalone build manually instead:

```python
import sqlite3

connection = sqlite3.connect(":memory:")
connection.enable_load_extension(True)
connection.load_extension("/path/to/jev.so")  # use jev.dylib on macOS
connection.enable_load_extension(False)

version = connection.execute("select jev_version()").fetchone()[0]
```

## Demo and tests

The deterministic suite uses a local mock server and never calls TypeSafe:

```bash
make test
```

The ticket-triage demo makes two live API requests, one for urgency and one for routing:

```bash
make live-test
```

See [`examples/ticket_triage.sql`](examples/ticket_triage.sql) for the complete query.

`make integration-test` runs a smaller live smoke test. In GitHub Actions it runs once every
two months and on manual dispatch using the `TYPESAFE_API_KEY` repository secret. It checks the API contract,
batching, and answer shapes; semantic expectations remain in the deterministic mock suite so
normal model variation cannot make pull requests flaky.

## Releases

Tags named `vX.Y.Z` build and publish four archives through GitHub Actions:

- Linux x86_64 and arm64
- macOS Intel and Apple Silicon

Each archive contains the native extension, this README, and its `pyproject.toml`. The release
also includes a `py3-none-<platform>` Python wheel and `SHA256SUMS`. CI installs each wheel
with `cibuildwheel` and verifies that `sqlite_jev.load()` enables the SQL API. The same tag
publishes the wheels and source distribution to PyPI through trusted publishing.

## Important limits

- Row contents are sent to TypeSafe. Do not evaluate data you are not allowed to share.
- This is a semantic full scan, not an index. Apply deterministic SQLite filters first and
  materialize a small candidate table.
- Cache entries live only for the current database connection and are keyed by row content,
  model, question, primitive, and criteria.
- Jev should make narrow judgments. Keep counting, arithmetic, and date comparison in SQL.
- Text stored in a row can steer model behavior. Test adversarial content and use conservative
  probability or confidence thresholds before automating consequential actions.
- `libcurl` is loaded dynamically so building does not require the curl development headers.
  Linux and macOS are the currently tested platforms.
