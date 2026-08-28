#!/usr/bin/env bash
set -euo pipefail

readonly REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "$REPO_ROOT"

printf 'Linting PHP files\n'
php_count=0
while IFS= read -r -d '' file; do
  php -l "$file"
  php_count=$((php_count + 1))
done < <(find . -path ./.git -prune -o -type f -name '*.php' -print0)

if ((php_count == 0)); then
  printf 'No PHP files found.\n' >&2
  exit 1
fi

printf '\nRunning security protocol tests\n'
test_count=0
while IFS= read -r -d '' test_file; do
  php "$test_file"
  test_count=$((test_count + 1))
done < <(find tests/security_protocol -maxdepth 1 -type f -name '*_test.php' -print0)

if ((test_count == 0)); then
  printf 'No security protocol tests found.\n' >&2
  exit 1
fi
