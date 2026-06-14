#!/usr/bin/env bash
# deployment_audit.sh — BLOCKING deployment-guardrail audit (issue #60).
#
# The previous CI step set FOUND_VIOLATION=true but never `exit 1` ("Do not fail
# build"), so a committed gcloud/aws/GCP_SA_KEY reference passed CI silently — the
# gate gave false assurance. This script makes the guardrail real: a match on any
# forbidden deployment pattern EXITS NON-ZERO, failing CI.
#
# To keep it from misfiring on legitimate documentary mentions, matches are
# filtered through an allowlist file (tests/ci/deployment_audit_allow.txt): each
# non-comment line is an extended-regex; any matching "path:line:text" hit is
# excused. The allowlist is the human-in-the-loop escape hatch — adding a line is
# the explicit "yes, this reference is intentional" sign-off the old comment
# described, but now enforced instead of assumed.
#
# Usage:
#   deployment_audit.sh [scan_root]   # default scan_root = repo root
#
# Exit: 0 = clean (or every hit allowlisted); 1 = at least one un-allowlisted
# forbidden reference.

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
SCAN_ROOT="${1:-$(cd "$HERE/../.." && pwd)}"
ALLOWLIST="$HERE/deployment_audit_allow.txt"

# The same set the old inline step used. These are upstream CD / cloud-deploy
# footguns this on-box firmware repo must never carry.
FORBIDDEN_PATTERNS=("gcloud" "google-cloud-sdk" "firebase" "aws" "heroku" "terraform" "deployment_key" "GCP_SA_KEY")

# Build the allowlisted-regex set (skip comments/blank lines). Missing file = no
# allowances, which is the strict default.
ALLOW_REGEXES=()
if [ -f "$ALLOWLIST" ]; then
    while IFS= read -r line; do
        case "$line" in
            ''|\#*) continue ;;
        esac
        ALLOW_REGEXES+=("$line")
    done < "$ALLOWLIST"
fi

is_allowlisted() {
    local hit="$1"
    local re
    for re in "${ALLOW_REGEXES[@]:-}"; do
        [ -z "$re" ] && continue
        if printf '%s' "$hit" | grep -Eq "$re"; then
            return 0
        fi
    done
    return 1
}

echo "Running strict deployment audit guardrail (BLOCKING) over: $SCAN_ROOT"

VIOLATIONS=0
for pattern in "${FORBIDDEN_PATTERNS[@]}"; do
    # -rnwi: recursive, line numbers, whole-word, case-insensitive. Exclude VCS,
    # build output, this audit's own files (which legitimately name the patterns),
    # and ci.yml (it references them in comments) — same exclusions as the old step.
    while IFS= read -r hit; do
        [ -z "$hit" ] && continue
        if is_allowlisted "$hit"; then
            echo "  [allowlisted] $hit"
            continue
        fi
        echo "  [VIOLATION] forbidden pattern '$pattern': $hit"
        VIOLATIONS=$((VIOLATIONS+1))
    done < <(grep -rnwi "$SCAN_ROOT" -e "$pattern" \
                --exclude-dir=".git" \
                --exclude-dir="build" \
                --exclude-dir="build-verify" \
                --exclude-dir="node_modules" \
                --exclude="ci.yml" \
                --exclude="deployment_audit.sh" \
                --exclude="deployment_audit_allow.txt" \
                --exclude="test_deployment_audit.sh" \
                2>/dev/null)
done

if [ "$VIOLATIONS" -gt 0 ]; then
    echo "Audit Error: ${VIOLATIONS} unauthorized deployment reference(s) found."
    echo "Deployments must NEVER be automated without explicit human-in-the-loop"
    echo "approval. If a reference is intentional/documentary, add an allowlist"
    echo "regex to tests/ci/deployment_audit_allow.txt."
    exit 1
fi

echo "Deployment audit clean! No un-allowlisted GCP/cloud deployment pathways detected."
exit 0
