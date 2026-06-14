#!/usr/bin/env bash
# test_deployment_audit.sh — self-test for deployment_audit.sh (issue #60).
#
# The whole point of #60 is that the guardrail must ACTUALLY exit non-zero on a
# violation (the old step never did). This proves it:
#   1. a clean tree audits clean (exit 0),
#   2. a planted gcloud/GCP_SA_KEY reference FAILS the audit (exit 1) — the real
#      failing-case test the audit previously lacked,
#   3. an allowlist regex excuses an intentional reference (exit 0 again),
#   4. the actual repo is clean under the blocking audit.
#
# No hardware/network. Operates entirely on throwaway temp trees.

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$HERE/../.." && pwd)"
AUDIT="$HERE/deployment_audit.sh"

PASS=0
FAIL=0
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

ok()  { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad() { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }

echo "== deployment_audit self-test =="

# 1. Clean tree -> exit 0.
mkdir -p "$TMP/clean"
echo "// just some firmware source" > "$TMP/clean/main.c"
bash "$AUDIT" "$TMP/clean" >/dev/null 2>&1
if [ $? -eq 0 ]; then ok "clean tree audits clean (exit 0)"; else bad "clean tree audits clean"; fi

# 2. Planted violation -> exit 1 (THE regression guard for #60).
mkdir -p "$TMP/dirty"
echo "// some source" > "$TMP/dirty/app.c"
# A committed CD key reference of exactly the kind the guardrail must block.
echo 'GCP_SA_KEY=super-secret-service-account' > "$TMP/dirty/deploy.env"
echo 'run: gcloud app deploy' > "$TMP/dirty/pipeline.yaml"
bash "$AUDIT" "$TMP/dirty" >/dev/null 2>&1
if [ $? -ne 0 ]; then ok "planted gcloud/GCP_SA_KEY reference FAILS audit (exit non-zero)"; \
                 else bad "planted violation should have failed the audit"; fi

# 3. Allowlist excuses an intentional reference -> back to exit 0.
#    Point the audit's allowlist at a temp file via a copied script dir so we
#    don't mutate the committed allowlist. Simplest: run the audit with an
#    allowlist that matches the planted line, by symlinking a temp script home.
mkdir -p "$TMP/allowtest"
cp "$AUDIT" "$TMP/allowtest/deployment_audit.sh"
# Allow exactly the deploy.env GCP_SA_KEY line; the gcloud pipeline line stays a
# violation, so this still fails — instead, allowlist BOTH planted patterns.
cat > "$TMP/allowtest/deployment_audit_allow.txt" <<'ALLOW'
GCP_SA_KEY=super-secret-service-account
gcloud app deploy
ALLOW
bash "$TMP/allowtest/deployment_audit.sh" "$TMP/dirty" >/dev/null 2>&1
if [ $? -eq 0 ]; then ok "allowlist excuses intentional references (exit 0)"; \
                 else bad "allowlisted references should pass"; fi

# 4. The REAL repo must be clean under the blocking audit.
bash "$AUDIT" "$REPO_ROOT" >/dev/null 2>&1
if [ $? -eq 0 ]; then ok "actual repository is clean under blocking audit"; \
                 else bad "actual repository tripped the blocking audit (unexpected)"; fi

echo "== self-test: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
