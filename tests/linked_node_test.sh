#!/usr/bin/env bash
# The private node end to end at the native level. scripts/build-node.sh links libqod_qdrant.a into the
# public qod-node; the linked binary must carry the extension (marker, functions, settings), keep the
# search_* settings out of a tenant's initSql, and attach a source whose endpoint is a local HTTPS stub in
# trusted init while refusing an invalid config. Through the same core and extension, linked_node_probe
# then scans and searches that stub over verified TLS and refuses an inexact policy with zero requests.
# usage: linked_node_test.sh <tests dir> <build dir>
set -euo pipefail
tests="${1:?usage: linked_node_test.sh <tests dir> <build dir>}"
build="${2:?usage: linked_node_test.sh <tests dir> <build dir>}"
root="$(cd "$tests/../../.." && pwd)"
work="$(mktemp -d)"
stub=""
cleanup() {
  if [[ -n "$stub" ]]; then
    kill "$stub" 2>/dev/null || true
    wait "$stub" 2>/dev/null || true
  fi
  rm -rf "$work"
}
trap cleanup EXIT
fail() { echo "FAIL $*"; exit 1; }

# Core, DuckDB, OpenSSL and the build type come from the configured build (ctest's ENVIRONMENT); the rest is
# forced here so an outer QOD_QDRANT_TESTING, QOD_QDRANT_TESTS or QOD_NODE_RELATIVE_RPATH cannot leak in.
build_node() {
  QOD_QDRANT_TESTING="$1" QOD_QDRANT_TESTS=OFF QOD_NODE_RELATIVE_RPATH=OFF "$root/scripts/build-node.sh" "$2"
}
node="$(build_node 1 "$build/linked" 2>"$work/build.log" | tail -1)"
[[ "$node" == "$build/linked/node/qod-node" && -x "$node" ]] || { tail -50 "$work/build.log"; fail "no linked qod-node"; }
release="$(build_node 0 "$build/release" 2>"$work/release.log" | tail -1)"
[[ "$release" == "$build/release/node/qod-node" && -x "$release" ]] || { tail -50 "$work/release.log"; fail "no release qod-node"; }
[[ "$(grep -c QOD_QDRANT_TEST_REQUEST_LOG "$release" || true)" == 0 ]] || fail "the release node carries the request-log hook"
[[ "$(grep -c QOD_QDRANT_TEST_REQUEST_LOG "$node" || true)" != 0 ]] || fail "the testing node lacks the request-log hook"
openssl_symbols=' _?(SSL|EVP|OPENSSL|CRYPTO|X509|BIO)_'
if [[ "$(uname)" == Darwin ]]; then
  [[ "$(otool -L "$release" | grep -c 'homebrew\|/usr/local/opt' || true)" == 0 ]] || fail "OpenSSL linked dynamically"
  # Recorded, not asserted: a macOS executable exports its external symbols unless told otherwise.
  echo "macOS release node exports $(nm -gU "$release" | grep -cE "$openssl_symbols" || true) OpenSSL symbols"
else
  [[ "$(readelf -d "$release" | grep -c 'NEEDED.*lib\(ssl\|crypto\)' || true)" == 0 ]] || fail "OpenSSL linked dynamically"
  for n in "$release" "$node"; do
    exported="$(nm -D --defined-only "$n" | grep -cE "$openssl_symbols" || true)"
    [[ "$exported" == 0 ]] || fail "$n exports $exported OpenSSL symbols"
  done
fi
for n in "$release" "$node"; do
  [[ "$("$n" --build-info)" == *";extra=qod_qdrant" ]] || fail "$n: marker does not end with ;extra=qod_qdrant"
done
functions="$("$node" --list-functions)"
for f in qod_search qod_dense qod_bm25 qod_rrf; do
  case "$functions" in *"\"name\":\"$f\""*) ;; *) fail "$f not listed";; esac
done
settings="$("$node" --list-settings)"
for s in search_max_k search_max_query_text_bytes search_max_prefetch_k search_max_fusion_branches; do
  case "$settings" in *"\"name\":\"$s\""*) ;; *) fail "$s not listed";; esac
done
for s in search_max_k search_max_query_text_bytes search_max_prefetch_k search_max_fusion_branches; do
  verdict="$(printf 'SET GLOBAL %s = 5;\n' "$s" | "$node" --check-init)"
  case "$verdict" in *'"refused":[{'*) ;; *) fail "a tenant initSql may set $s: $verdict";; esac
done

openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj /CN=localhost -addext "subjectAltName=DNS:localhost" \
  -keyout "$work/key.pem" -out "$work/cert.pem" 2>/dev/null
python3 -I "$tests/https_stub.py" --cert "$work/cert.pem" --key "$work/key.pem" --port-file "$work/port" \
  --log "$work/stub.log" --api-key test-key --points "$tests/fixtures/stub_points.json" 2>"$work/stub.err" &
stub=$!
for _ in $(seq 1 300); do
  [[ -s "$work/port" ]] && break
  sleep 0.1
done
port="$(cat "$work/port" 2>/dev/null || true)"
if ! [[ "$port" =~ ^[0-9]+$ ]]; then
  if kill -0 "$stub" 2>/dev/null; then echo "the stub is still running"; else echo "the stub exited"; fi
  cat "$work/stub.err"
  fail "https stub start: no port after 30 s"
fi
endpoint="https://localhost:$port"
config="$(sed "s#https://qdrant.internal:6333#$endpoint#" "$tests/fixtures/sample_canonical.json")"
attach() {
  printf "CREATE OR REPLACE SECRET qod_qdrant_search_docs (TYPE qdrant, API_KEY 'test-key');\nATTACH 'qdrant:fs-qdrant-1' AS \"search_docs\" (TYPE qdrant, SECRET qod_qdrant_search_docs, CONFIG '%s', READ_ONLY);\n" "$1"
}
attach "$config" > "$work/init.sql"
QOD_QDRANT_ALLOWED_ENDPOINTS="$endpoint" "$node" --list-functions --init "$work/init.sql" >/dev/null 2>"$work/init.err"
grep -q "2 of 2 statement(s) ran" "$work/init.err" || { cat "$work/init.err"; fail "trusted init did not attach"; }
# No I/O at ATTACH: an allowlisted endpoint nothing listens on still attaches.
attach "$(sed "s#https://qdrant.internal:6333#https://localhost:1#" "$tests/fixtures/sample_canonical.json")" > "$work/dead.sql"
QOD_QDRANT_ALLOWED_ENDPOINTS="https://localhost:1" "$node" --list-functions --init "$work/dead.sql" >/dev/null 2>"$work/dead.err"
grep -q "2 of 2 statement(s) ran" "$work/dead.err" || { cat "$work/dead.err"; fail "ATTACH contacted the endpoint"; }
attach "${config/\"generation\":1/\"generation\":0}" > "$work/bad.sql"
QOD_QDRANT_ALLOWED_ENDPOINTS="$endpoint" "$node" --list-functions --init "$work/bad.sql" >/dev/null 2>"$work/bad.err"
grep -q "search_config_invalid: " "$work/bad.err" || { cat "$work/bad.err"; fail "an invalid config attached"; }

out="$(QOD_QDRANT_TEST_REQUEST_LOG="$work/requests.log" \
  "$build/linked_node_probe" "$port" "$work/cert.pem" "$tests/fixtures/sample_canonical.json")"
expected="attach ok
scan d1,d2,d4
search d1|1|7|1.0,d4|18446744073709551615|9223372036854775807|0.5
policy d1
inexact refused
requests 3 then 3"
if [[ "$out" != "$expected" ]]; then
  echo "expected:"; echo "$expected"; echo "got:"; echo "$out"
  fail "linked node probe"
fi
[[ "$(grep -c '^POST /collections/contracts_v3/points/' "$work/stub.log")" == 3 ]] || fail "stub saw other than 3 requests"
echo ok
