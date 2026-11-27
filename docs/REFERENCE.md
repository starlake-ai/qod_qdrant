# qod_qdrant reference

The details behind the [README](../README.md): every config key, every SQL function, the limits, and how the tests run.

## Config reference

`CONFIG` is one JSON object. Unknown keys are refused.

| Key | What it holds |
|---|---|
| `endpoint` | The Qdrant origin, `https://host[:port]`, no path. |
| `apiKeySecret` | The name of the secret that holds the API key, `[A-Z][A-Z0-9_]{0,63}`. A literal key in the config is refused. |
| `guarantee` | `exact_isolated` (the default) or `filtered_retrieval`. A search with a keyword branch runs only under `filtered_retrieval`. |
| `collections[]` | 1 to 64 tables: `schema`, `table`, the Qdrant `collection`, `generation` and `schemaVersion` (integers, 1 or more). |
| `…fields[]` | Payload fields read as columns: `name`, `path`, `type`, `required`. |
| `…vectors[]` | Dense vectors: `name`, `dimensions`, `datatype` (`float32`), `dependsOn`, `modelVersion`. |
| `…sparseVectors[]` | 0 to 8 BM25 indexes: `name`, `model` (`qdrant/bm25`), `dependsOn`. The collection declares the vector with `modifier: idf`. |
| `…sparseVectors[].options` | Optional. The Qdrant BM25 options the index was built with (`language`, `stemmer`, `stopwords`, `tokenizer`, `ascii_folding`, `k`, `b`, `avg_len`), sent with every keyword query on it. Without it, Qdrant's defaults apply (English). |

Declare exactly the options your loader used to index the collection. Qdrant checks neither the keys nor the values, and a mismatch only shows as missing results.

## SQL reference

| Name | What it is |
|---|---|
| `qod_search(relation, query := …, filter_sql := …, k := …)` | The top k points of a declared table, best first, with every declared column, `_qod_id` and `_qod_score`. |
| `qod_search(relation, request := '<json>', k := …)` | The same with a raw Qdrant `points/query` body instead of `query`. |
| `qod_dense(vector_name, vector)` | A dense branch; the vector is checked against the declared dimensions. |
| `qod_bm25(vector_name, text)` | A keyword branch, tokenized by Qdrant with the index's declared options. |
| `qod_rrf([branches], prefetch_k := …)` | Reciprocal rank fusion of two or more branches. |
| `_qod_id`, `_qod_score` | The point id (decimal or UUID text) and Qdrant's score. Scores of dense, keyword and hybrid searches are not comparable with each other. |

## Good to know

- RRF uses Qdrant's default constant unless a raw request sets `rrf.k` or weights.
- Keyword statistics are computed per shard, so a document's score depends on its shard.
- Budgets cap k, request and response size, deadline, concurrency and prefetch depth; each is a DuckDB setting (`search_max_k`, `search_deadline_ms`, …).
- Endpoints must be HTTPS. Standalone, with `QOD_QDRANT_ALLOWED_ENDPOINTS` unset, any HTTPS origin is accepted; set, only its exact origins are. Plain HTTP on loopback hosts, for local development, is planned. `QOD_QDRANT_CA_FILE` adds a PEM bundle to the system CA store.
- Persistent secrets (`CREATE PERSISTENT SECRET`) are stored unencrypted on disk by DuckDB; prefer a temporary secret.

## Tests

`make test_release` runs the sqllogictests in `test/sql`, and `test/integration` when a Qdrant is up: `scripts/qdrant-it.sh up` starts Qdrant 1.20.2 with a throwaway TLS CA and api-key and seeds it, then `source .cache/qdrant-it/env && make test_release`; `scripts/qdrant-it.sh down` stops it. Inside qod-search, `scripts/build-node.sh` builds the QoD variant and its C++ unit tests (`tests/`).
