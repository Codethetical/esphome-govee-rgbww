#!/bin/sh
# Verify that a release tag name agrees with the content of the commit it
# points at.
#
# Usage: scripts/check-release-version.sh vX.Y.Z <commit-ish>
#
# Two callers share this, deliberately: .githooks/pre-push runs it before a
# tag is published, and .github/workflows/release-check.yml runs it after,
# covering tags cut from a clone without core.hooksPath set or made through
# the GitHub web UI. Keeping the rules in one file means the guard cannot
# drift from itself - which is the same class of bug it exists to catch.
#
# Content is read with `git show <commit>:<path>` rather than from the
# working tree, because a tag can point at any commit, including one that
# predates whatever is currently checked out.
#
# Every mismatch is reported, not just the first, so one run tells you the
# whole list to fix.

set -u

HEADER='components/govee_rgbww/light/govee_rgbww_light.h'
CHANGELOG='CHANGELOG.md'
PINNED='README.md examples/h7039.yaml'

if [ $# -ne 2 ]; then
  echo "usage: $0 <tag> <commit-ish>" >&2
  exit 2
fi

tag=$1
commit=$2
version=${tag#v}

fail=0
note() { echo "$tag: $1" >&2; }

at() { git show "$commit:$1" 2>/dev/null; }

header_version=$(at "$HEADER" |
  sed -n 's/^#define GOVEE_RGBWW_VERSION "\(.*\)".*/\1/p')
if [ "$header_version" != "$version" ]; then
  note "$HEADER defines \"${header_version:-<not found>}\", expected \"$version\""
  fail=1
fi

if ! at "$CHANGELOG" | grep -q "^## \[$version\] - "; then
  note "$CHANGELOG has no '## [$version] - <date>' heading"
  fail=1
fi

if ! at "$CHANGELOG" | grep -q "^\[$version\]: "; then
  note "$CHANGELOG has no '[$version]:' link reference"
  fail=1
fi

# Every copy-pasteable github:// pin must name this tag, or the release ships
# instructions pointing at a tag that does not exist. @main is not a pin and
# is deliberately not matched.
for f in $PINNED; do
  stale=$(at "$f" |
    grep -o '@v[0-9][0-9A-Za-z.-]*' | grep -vx "@$tag" | sort -u | tr '\n' ' ')
  if [ -n "$stale" ]; then
    note "$f pins ${stale}- expected @$tag"
    fail=1
  fi
done

if [ "$fail" -ne 0 ]; then
  echo '' >&2
  echo "Tagged commit does not match the tag name. Bump the version references," >&2
  echo "commit, then move the tag onto the new commit." >&2
  exit 1
fi

echo "$tag: version references agree with the tag."
exit 0
