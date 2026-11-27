#!/usr/bin/env bash
# The integration suite's Qdrant: v1.20.2 over TLS with a throwaway CA and api-key, seeded with one collection.
#
#   scripts/qdrant-it.sh up     start it on 127.0.0.1:16333, seed it, write .cache/qdrant-it/env
#   scripts/qdrant-it.sh down   stop it and delete .cache/qdrant-it
#
# .cache/qdrant-it/env exports QOD_QDRANT_IT_ENDPOINT, QOD_QDRANT_IT_KEY and QOD_QDRANT_CA_FILE for the tests
# (test/integration, run with `source .cache/qdrant-it/env && make test_release`). Every key is generated here.
set -euo pipefail
cd "$(dirname "$0")/.."
state=.cache/qdrant-it
name=qod-qdrant-it
image=qdrant/qdrant:v1.20.2@sha256:d277592721d2e3b73f9b797b53cf05e6a5bb2134f6d13fdbda4ca51782afbcac
port=16333

case "${1:-}" in
up)
  mkdir -p "$state/tls"
  openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj /CN=qod-qdrant-it-ca \
    -keyout "$state/tls/ca.key" -out "$state/tls/ca.pem" 2>/dev/null
  openssl req -newkey rsa:2048 -nodes -subj /CN=localhost -keyout "$state/tls/server.key" -out "$state/tls/server.csr" 2>/dev/null
  printf 'subjectAltName=DNS:localhost,IP:127.0.0.1\n' > "$state/tls/san.ext"
  openssl x509 -req -in "$state/tls/server.csr" -CA "$state/tls/ca.pem" -CAkey "$state/tls/ca.key" -CAcreateserial \
    -days 2 -extfile "$state/tls/san.ext" -out "$state/tls/server.pem" 2>/dev/null
  chmod 644 "$state/tls/server.key"
  key="$(openssl rand -hex 24)"
  docker rm -f "$name" >/dev/null 2>&1 || true
  docker run -d --rm --name "$name" -p "127.0.0.1:$port:6333" \
    -e QDRANT__SERVICE__ENABLE_TLS=true -e QDRANT__TLS__CERT=/tls/server.pem -e QDRANT__TLS__KEY=/tls/server.key \
    -e QDRANT__SERVICE__API_KEY="$key" -e QDRANT__TELEMETRY_DISABLED=true \
    -v "$PWD/$state/tls:/tls:ro" "$image" >/dev/null
  base="https://localhost:$port"
  q() { curl -sf --cacert "$state/tls/ca.pem" -H "api-key: $key" -H 'Content-Type: application/json' -X "$1" "$base$2" ${3:+-d "$3"}; }
  for _ in $(seq 1 60); do q GET / >/dev/null && break; sleep 0.5; done
  q PUT /collections/it_docs '{"vectors":{"dense":{"size":2,"distance":"Cosine"}},"sparse_vectors":{"body_bm25":{"modifier":"idf"}}}' >/dev/null
  q PUT /collections/it_docs/index '{"field_name":"status","field_schema":"keyword"}' >/dev/null
  doc() { printf '{"text":"%s","model":"qdrant/bm25"}' "$1"; }
  q PUT '/collections/it_docs/points?wait=true' "{\"points\":[
    {\"id\":1,\"vector\":{\"dense\":[1,0],\"body_bm25\":$(doc 'merger agreement draft')},\"payload\":{\"title\":\"Merger memo\",\"body\":\"merger agreement draft\",\"status\":\"active\"}},
    {\"id\":2,\"vector\":{\"dense\":[0,1],\"body_bm25\":$(doc 'invoice terms')},\"payload\":{\"title\":\"Invoice\",\"body\":\"invoice terms\",\"status\":\"active\"}},
    {\"id\":3,\"vector\":{\"dense\":[0.7,0.7],\"body_bm25\":$(doc 'renewal of the lease')},\"payload\":{\"title\":\"Lease renewal\",\"body\":\"renewal of the lease\",\"status\":\"archived\"}}]}" >/dev/null
  {
    echo "export QOD_QDRANT_IT_ENDPOINT=$base"
    echo "export QOD_QDRANT_IT_KEY=$key"
    echo "export QOD_QDRANT_CA_FILE=$PWD/$state/tls/ca.pem"
  } > "$state/env"
  echo "qdrant-it: up at $base, seeded it_docs (3 points); source $state/env"
  ;;
down)
  docker rm -f "$name" >/dev/null 2>&1 || true
  rm -rf "$state"
  echo "qdrant-it: down"
  ;;
*)
  echo "usage: $0 up|down" >&2
  exit 2
  ;;
esac
