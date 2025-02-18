#!/usr/bin/env bash

set -e

script_path=$(cd -P -- "$(dirname -- "$0")" && pwd -P)
cd "${script_path}/.." || exit 1

ports=true
if [ "$1" == "--no-ports" ]; then
  ports=false
  shift
fi

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m' # No Color

FAILURES=0

set +e

for cmd in \
        Meta/check-ak-test-files.sh \
        Meta/check-debug-flags.sh \
        Meta/check-emoji.py \
        Meta/check-markdown.sh \
        Meta/check-newlines-at-eof.py \
        Meta/check-png-sizes.sh \
        Meta/check-style.py \
        Meta/lint-executable-resources.sh \
        Meta/lint-gml-format.sh \
        Meta/lint-gn.sh \
        Meta/lint-keymaps.py \
        Meta/lint-prettier.sh \
        Meta/lint-python.sh \
        Meta/lint-shell-scripts.sh; do
    echo "Running ${cmd}"
    if time "${cmd}" "$@"; then
        echo -e "[${GREEN}OK${NC}]: ${cmd}"
    else
        echo -e "[${RED}FAIL${NC}]: ${cmd}"
        ((FAILURES+=1))
    fi
done

if [ -x ./Build/lagom/bin/IPCMagicLinter ]; then
    echo "Running IPCMagicLinter"
    if time { git ls-files '*.ipc' | xargs ./Build/lagom/bin/IPCMagicLinter; }; then
        echo -e "[${GREEN}OK${NC}]: IPCMagicLinter (in Meta/lint-ci.sh)"
    else
        echo -e "[${RED}FAIL${NC}]: IPCMagicLinter (in Meta/lint-ci.sh)"
        ((FAILURES+=1))
    fi
else
    echo -e "[${GREEN}SKIP${NC}]: IPCMagicLinter (in Meta/lint-ci.sh)"
fi

echo "Running Meta/lint-clang-format.sh"
if time Meta/lint-clang-format.sh --overwrite-inplace "$@" && git diff --exit-code; then
    echo -e "[${GREEN}OK${NC}]: Meta/lint-clang-format.sh"
else
    echo -e "[${RED}FAIL${NC}]: Meta/lint-clang-format.sh"
    ((FAILURES+=1))
fi

# lint-ports.py is handled separately as it scans all Ports/ all the time.
# This is fine when running lint-ci.sh from the PR validation workflow.
# However when running from the pre-commit workflow it takes an excessive
# amount of time. This condition allows the pre-commit program to detect
# when Ports/ files have changed and only invoke lint-ports.py when needed.
#
if [ "$ports" = true ]; then
    echo "Running Meta/lint-ports.py"
    if time Meta/lint-ports.py; then
        echo -e "[${GREEN}OK${NC}]: Meta/lint-ports.py"
    else
        echo -e "[${RED}FAIL${NC}]: Meta/lint-ports.py"
        ((FAILURES+=1))
    fi
fi

echo "(Also look out for check-symbols.sh, which can only be executed after the build!)"

exit "${FAILURES}"
