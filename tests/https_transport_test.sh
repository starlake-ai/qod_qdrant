#!/usr/bin/env bash
# The HTTPS transport against a local TLS stub: verified certificate, api-key header, redirect refused,
# response budget, deadline, untrusted certificate, hostname mismatch (IP and DNS name), IP SAN, closed
# port, cancellation and deadline while waiting for a response or for the TLS handshake, QOD_QDRANT_CA_FILE,
# every lease released. The CA and the server certificates are generated here, per run.
# usage: https_transport_test.sh <tests dir> <build dir>
set -euo pipefail
tests="${1:?usage: https_transport_test.sh <tests dir> <build dir>}"
build="${2:?usage: https_transport_test.sh <tests dir> <build dir>}"
work="$(mktemp -d)"
stubs=()
cleanup() {
  for pid in "${stubs[@]+"${stubs[@]}"}"; do
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
  done
  rm -rf "$work"
}
trap cleanup EXIT

# A throwaway CA and the server certificates it issues: localhost, another DNS name, the IP 127.0.0.1.
cat > "$work/openssl.cnf" <<'EOF'
[req]
distinguished_name = dn
prompt = no
[dn]
CN = qod-qdrant-test-ca
[ca]
basicConstraints = critical, CA:TRUE
keyUsage = critical, keyCertSign, cRLSign
subjectKeyIdentifier = hash
[localhost]
basicConstraints = critical, CA:FALSE
keyUsage = critical, digitalSignature, keyEncipherment
extendedKeyUsage = serverAuth
subjectAltName = DNS:localhost
[mismatch]
basicConstraints = critical, CA:FALSE
keyUsage = critical, digitalSignature, keyEncipherment
extendedKeyUsage = serverAuth
subjectAltName = DNS:qdrant.invalid
[ip]
basicConstraints = critical, CA:FALSE
keyUsage = critical, digitalSignature, keyEncipherment
extendedKeyUsage = serverAuth
subjectAltName = IP:127.0.0.1
EOF
openssl req -x509 -newkey rsa:2048 -nodes -days 2 -config "$work/openssl.cnf" -extensions ca \
  -keyout "$work/ca.key" -out "$work/ca.pem" 2>/dev/null
serial=1
for name in localhost mismatch ip; do
  openssl req -new -newkey rsa:2048 -nodes -subj "/CN=$name" -config "$work/openssl.cnf" \
    -keyout "$work/$name.key" -out "$work/$name.csr" 2>/dev/null
  openssl x509 -req -in "$work/$name.csr" -CA "$work/ca.pem" -CAkey "$work/ca.key" -set_serial "$serial" \
    -days 2 -extfile "$work/openssl.cnf" -extensions "$name" -out "$work/$name.pem" 2>/dev/null
  serial=$((serial + 1))
done

for name in localhost mismatch ip; do
  python3 -I "$tests/https_stub.py" --cert "$work/$name.pem" --key "$work/$name.key" --port-file "$work/$name.port" \
    --log "$work/requests.log" --api-key test-key --points "$tests/fixtures/stub_points.json" &
  stubs+=($!)
done
python3 -I "$tests/silent_stub.py" --port-file "$work/silent.port" &
stubs+=($!)
for name in localhost mismatch ip silent; do
  for _ in $(seq 1 100); do
    [[ -s "$work/$name.port" ]] && break
    sleep 0.1
  done
done
out="$("$build/https_transport_probe" "$(cat "$work/localhost.port")" "$work/ca.pem" "$(cat "$work/mismatch.port")" \
  "$(cat "$work/ip.port")" "$(cat "$work/silent.port")")"
expected="ok failure=none status=200
badkey failure=none status=401
redirect failure=redirect status=302
big failure=too_large status=0
slow failure=timeout status=0
untrusted failure=tls status=0
wronghost failure=tls status=0
wrongname failure=tls status=0
ipsan failure=none status=200
closed failure=connect status=0
cancelled failure=cancelled status=0
handshake_cancelled failure=cancelled status=0
handshake_deadline failure=timeout status=0
envca failure=none status=200
leases outstanding=0"
if [[ "$out" != "$expected" ]]; then
  echo "FAIL https transport"
  echo "expected:"; echo "$expected"
  echo "got:"; echo "$out"
  exit 1
fi
grep -q "^POST /collections/c/points/scroll$" "$work/requests.log" || { echo "FAIL stub saw no request"; exit 1; }
# Exiting with a request abandoned in its handshake: a normal exit (0, not SIGABRT or SIGSEGV) that waited
# for the worker, several times.
for _ in 1 2 3; do
  status=0
  exited="$("$build/https_transport_probe" exit "$(cat "$work/silent.port")" "$work/ca.pem")" || status=$?
  expected_exit="abandoned failure=cancelled status=0
exit in_flight=0"
  if [[ "$status" != 0 || "$exited" != "$expected_exit" ]]; then
    echo "FAIL exit with an abandoned request: status $status"
    echo "expected:"; echo "$expected_exit"
    echo "got:"; echo "$exited"
    exit 1
  fi
done
echo ok
