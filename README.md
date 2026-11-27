# qod_qdrant

**Search your Qdrant collections from DuckDB, with plain SQL.**

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![DuckDB 2.0.1](https://img.shields.io/badge/DuckDB-2.0.1-yellow.svg)
![Qdrant 1.20.2](https://img.shields.io/badge/Qdrant-1.20.2-red.svg)

You keep your documents and embeddings in Qdrant, and the rest of your data in Parquet files, CSV or a database. qod_qdrant lets one DuckDB query do both: find the most relevant documents, then join, filter and aggregate them like any other table. You don't need a separate search script or any glue code.

```sql
-- the five most relevant documents about refunds, with who owns each one
SELECT d.title, d._qod_score, o.team
FROM qod_search('kb.main.docs', query := qod_bm25('body_en', 'refund policy'), k := 5) AS d
JOIN read_parquet('owners.parquet') AS o USING (doc_id)
ORDER BY d._qod_score DESC;
```

## What you can do

- Search by meaning, by keywords, or both: pass a query vector, a few words, or combine the two and let qod_qdrant fuse the rankings.
- Mix search with the rest of your data. Search results are rows, so you can join them with Parquet, CSV, Postgres or anything else DuckDB reads.
- Filter before you rank: `filter_sql := 'status = ''published'''` runs inside Qdrant, so the top 10 are the top 10 published documents.
- Search in your language. Keyword search can use the language your collection was indexed in, French as easily as English, and you can search both at once.
- Read a collection like a table: `SELECT status, count(*) FROM kb.main.docs GROUP BY status` works as you'd expect.
- Stay safe by default. It only reads from Qdrant, always verifies TLS, and keeps API keys in DuckDB secrets rather than in your configs.

## Get started

**1. Get the extension.** It isn't in DuckDB's community extensions yet, so for now you build it once from source. You need CMake, Ninja, a recent C++ compiler and OpenSSL 3:

```sh
git clone --recurse-submodules https://github.com/starlake-ai/qod_qdrant
cd qod_qdrant
GEN=ninja make release
./build/release/duckdb -unsigned
```

```sql
LOAD 'build/release/extension/qod_qdrant/qod_qdrant.duckdb_extension';
```

**2. Tell DuckDB your Qdrant key.**

```sql
CREATE SECRET qdrant (TYPE qdrant, API_KEY 'your-qdrant-api-key');
```

**3. Attach a collection.** Describe which payload fields you want as columns, and the vectors you search with:

```sql
ATTACH 'qdrant:docs' AS kb (TYPE qdrant, SECRET qdrant, READ_ONLY, CONFIG '{
  "endpoint": "https://your-qdrant-host:6333",
  "apiKeySecret": "QDRANT_API_KEY",
  "guarantee": "filtered_retrieval",
  "collections": [{
    "schema": "main", "table": "docs", "collection": "my_documents",
    "generation": 1, "schemaVersion": 1,
    "fields": [
      {"name": "doc_id", "path": "doc_id", "type": "VARCHAR", "required": true},
      {"name": "title",  "path": "title",  "type": "VARCHAR", "required": false},
      {"name": "body",   "path": "body",   "type": "VARCHAR", "required": false},
      {"name": "status", "path": "status", "type": "VARCHAR", "required": true}
    ],
    "vectors": [],
    "sparseVectors": [{"name": "body_en", "model": "qdrant/bm25", "dependsOn": ["body"]}]
  }]
}');
```

**4. Search.**

```sql
SELECT title, _qod_score
FROM qod_search('kb.main.docs', query := qod_bm25('body_en', 'refund policy'), k := 10);
```

## Recipes

Search by meaning, with a query vector from your embedding model. This needs a dense vector in the config (`title_dense` below, see the [reference](docs/REFERENCE.md#config-reference)), and `$query_vector` is your embedding, bound as a prepared-statement parameter:

```sql
SELECT title FROM qod_search('kb.main.docs', query := qod_dense('title_dense', $query_vector::FLOAT[]), k := 5);
```

Search by keywords and by meaning at once, one ranking:

```sql
SELECT title FROM qod_search('kb.main.docs',
  query := qod_rrf([qod_bm25('body_en', 'refund policy'), qod_dense('title_dense', $query_vector::FLOAT[])]),
  k := 10);
```

Search French and English documents together:

```sql
-- declare body_fr with "options": {"language": "french"} next to body_en
SELECT title FROM qod_search('kb.main.docs',
  query := qod_rrf([qod_bm25('body_fr', 'politique de remboursement'), qod_bm25('body_en', 'refund policy')]),
  k := 10);
```

Count what's in a collection:

```sql
SELECT status, count(*) FROM kb.main.docs GROUP BY status;
```

Every config key, every function and the limits are in the [reference](docs/REFERENCE.md).

## Where it stands

qod_qdrant is a preview. It builds and passes its tests on Linux (x86_64 and arm64), macOS (Apple silicon) and Windows (x86_64), an integration suite checks it against Qdrant 1.20.2 on every push, and it also runs inside [QoD (Quack on Demand)](https://github.com/starlake-ai/qod). Still to come:

- `INSTALL qod_qdrant FROM community`, so you can skip building it
- plain HTTP to a Qdrant on your own machine, for local development

## Contributing

Issues and pull requests are welcome. `make test_release` runs the SQL tests; the [reference](docs/REFERENCE.md#tests) explains how to run the integration suite against a local Qdrant.

## License

MIT. Copyright (c) 2026 Starlake AI. Includes [cpp-httplib](https://github.com/yhirose/cpp-httplib) (MIT).
