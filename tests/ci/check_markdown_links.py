#!/usr/bin/env python3
"""Validate repository-local links in Markdown without network access.

The checker deliberately resolves every path component with exact casing, even
on case-insensitive developer machines. It also validates fragments against
explicit HTML IDs and GitHub-style heading slugs in the destination document.
"""

from __future__ import annotations

import argparse
import html
import os
import re
import sys
import unicodedata
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from urllib.parse import unquote, urlsplit


FENCE_RE = re.compile(r"^\s*(`{3,}|~{3,})")
ATX_HEADING_RE = re.compile(r"^\s{0,3}(#{1,6})\s+(.+?)\s*$")
SETEXT_RE = re.compile(r"^\s{0,3}(?:=+|-+)\s*$")
EXPLICIT_ID_RE = re.compile(
    r"<(?:a|[A-Za-z][\w:-]*)\b[^>]*\b(?:id|name)\s*=\s*"
    r"(?:\"([^\"]+)\"|'([^']+)'|([^\s>]+))",
    re.IGNORECASE,
)
HTML_LINK_RE = re.compile(
    r"<(?:a|img)\b[^>]*\b(?:href|src)\s*=\s*"
    r"(?:\"([^\"]+)\"|'([^']+)'|([^\s>]+))",
    re.IGNORECASE,
)
REFERENCE_RE = re.compile(r"^\s{0,3}\[[^]]+\]:\s*(.+?)\s*$")
INLINE_CODE_RE = re.compile(r"(`+)(.*?)\1")
MARKDOWN_LINK_LABEL_RE = re.compile(r"!?\[([^]]*)\]\([^)]*\)")
HTML_TAG_RE = re.compile(r"<[^>]+>")
LINE_FRAGMENT_RE = re.compile(r"L\d+(?:-L\d+)?")


@dataclass(frozen=True)
class Link:
    source: Path
    line: int
    destination: str


@dataclass(frozen=True)
class Issue:
    source: Path
    line: int
    message: str


def repository_root(start: Path) -> Path:
    current = start.resolve()
    if current.is_file():
        current = current.parent
    while current != current.parent:
        if (current / ".git").exists():
            return current
        current = current.parent
    raise RuntimeError("not inside a Git working tree")


def markdown_files(root: Path) -> list[Path]:
    result: list[Path] = []
    for directory, names, files in os.walk(root):
        names[:] = sorted(name for name in names if name != ".git")
        for name in sorted(files):
            if Path(name).suffix.lower() in {".md", ".markdown"}:
                result.append(Path(directory) / name)
    return result


def active_lines(text: str) -> list[tuple[int, str]]:
    """Return lines outside fenced code blocks."""

    result: list[tuple[int, str]] = []
    fence_character: str | None = None
    fence_length = 0
    for line_number, line in enumerate(text.splitlines(), start=1):
        match = FENCE_RE.match(line)
        if match:
            marker = match.group(1)
            if fence_character is None:
                fence_character = marker[0]
                fence_length = len(marker)
            elif marker[0] == fence_character and len(marker) >= fence_length:
                fence_character = None
                fence_length = 0
            continue
        if fence_character is None:
            result.append((line_number, line))
    return result


def destination_token(raw: str) -> str:
    value = raw.strip()
    if not value:
        return ""
    if value.startswith("<"):
        closing = value.find(">", 1)
        return value[1:closing] if closing >= 0 else value[1:]

    escaped = False
    token: list[str] = []
    for character in value:
        if escaped:
            token.append(character)
            escaped = False
        elif character == "\\":
            escaped = True
        elif character.isspace():
            break
        else:
            token.append(character)
    if escaped:
        token.append("\\")
    return "".join(token)


def inline_destinations(line: str) -> list[str]:
    """Extract inline link destinations while allowing balanced parentheses."""

    masked = INLINE_CODE_RE.sub("", line)
    destinations: list[str] = []
    search_from = 0
    while True:
        marker = masked.find("](", search_from)
        if marker < 0:
            break
        cursor = marker + 2
        depth = 1
        escaped = False
        content: list[str] = []
        while cursor < len(masked):
            character = masked[cursor]
            if escaped:
                content.append(character)
                escaped = False
            elif character == "\\":
                content.append(character)
                escaped = True
            elif character == "(":
                depth += 1
                content.append(character)
            elif character == ")":
                depth -= 1
                if depth == 0:
                    break
                content.append(character)
            else:
                content.append(character)
            cursor += 1

        if depth != 0:
            search_from = marker + 2
            continue
        destination = destination_token("".join(content))
        if destination:
            destinations.append(destination)
        search_from = cursor + 1
    return destinations


def links_in(path: Path) -> list[Link]:
    text = path.read_text(encoding="utf-8")
    links: list[Link] = []
    for line_number, line in active_lines(text):
        for destination in inline_destinations(line):
            links.append(Link(path, line_number, destination))

        reference = REFERENCE_RE.match(INLINE_CODE_RE.sub("", line))
        if reference:
            destination = destination_token(reference.group(1))
            if destination:
                links.append(Link(path, line_number, destination))

        for match in HTML_LINK_RE.finditer(line):
            destination = next(group for group in match.groups() if group is not None)
            links.append(Link(path, line_number, html.unescape(destination)))
    return links


def github_slug(value: str) -> str:
    value = MARKDOWN_LINK_LABEL_RE.sub(lambda match: match.group(1), value)
    value = HTML_TAG_RE.sub("", value)
    value = INLINE_CODE_RE.sub(lambda match: match.group(2), value)
    value = html.unescape(value).strip().lower()
    value = re.sub(r"\s+#+\s*$", "", value)
    value = "".join(
        character
        for character in value
        if not (
            unicodedata.category(character).startswith(("P", "S"))
            and character not in {"-", "_"}
        )
    )
    return re.sub(r"\s", "-", value)


def anchors_in(path: Path) -> set[str]:
    text = path.read_text(encoding="utf-8")
    lines = active_lines(text)
    anchors: set[str] = set()
    slug_counts: dict[str, int] = {}

    for _, line in lines:
        for match in EXPLICIT_ID_RE.finditer(line):
            anchors.add(html.unescape(next(group for group in match.groups() if group)))

    previous: tuple[int, str] | None = None
    for _, line in lines:
        heading = ATX_HEADING_RE.match(line)
        heading_text: str | None = heading.group(2) if heading else None
        if heading_text is None and SETEXT_RE.match(line) and previous is not None:
            heading_text = previous[1].strip()
        if heading_text:
            base = github_slug(heading_text)
            if base:
                duplicate = slug_counts.get(base, 0)
                anchors.add(base if duplicate == 0 else f"{base}-{duplicate}")
                slug_counts[base] = duplicate + 1
        previous = (0, line) if line.strip() else None
    return anchors


def relative_parts(source: Path, destination: str, root: Path) -> tuple[list[str], str | None]:
    parsed = urlsplit(destination)
    decoded_path = unquote(parsed.path)
    if "\\" in decoded_path:
        raise ValueError("local links must use '/' instead of '\\'")
    if decoded_path.startswith("/"):
        raise ValueError("repository-local links must be relative, not root-relative")

    stack = list(source.relative_to(root).parent.parts)
    if decoded_path:
        for component in PurePosixPath(decoded_path).parts:
            if component in {"", "."}:
                continue
            if component == "..":
                if not stack:
                    raise ValueError("path escapes the repository")
                stack.pop()
            else:
                stack.append(component)
    elif not stack or source.name not in stack:
        stack.append(source.name)
    else:
        stack.append(source.name)
    return stack, unquote(parsed.fragment) if parsed.fragment else None


def resolve_exact(root: Path, components: list[str]) -> Path:
    current = root
    for component in components:
        if not current.is_dir():
            raise FileNotFoundError(f"'{current.relative_to(root).as_posix()}' is not a directory")
        names = {entry.name: entry for entry in current.iterdir()}
        if component in names:
            current = names[component]
            continue
        folded = sorted(name for name in names if name.casefold() == component.casefold())
        if folded:
            raise FileNotFoundError(
                f"case mismatch: requested '{component}', actual '{folded[0]}'"
            )
        raise FileNotFoundError(f"missing path component '{component}'")
    return current


def is_external(destination: str) -> bool:
    if destination.startswith("//"):
        return True
    parsed = urlsplit(destination)
    return bool(parsed.scheme or parsed.netloc)


def validate(root: Path) -> list[Issue]:
    issues: list[Issue] = []
    anchor_cache: dict[Path, set[str]] = {}
    files = markdown_files(root)
    for source in files:
        for link in links_in(source):
            destination = link.destination.strip()
            if not destination or is_external(destination):
                continue
            try:
                components, fragment = relative_parts(source, destination, root)
                target = resolve_exact(root, components)
                if target.is_dir() and fragment:
                    target = resolve_exact(target, ["README.md"])
            except (ValueError, FileNotFoundError) as error:
                issues.append(Issue(source, link.line, f"{destination}: {error}"))
                continue

            if not fragment:
                continue
            if target.suffix.lower() not in {".md", ".markdown"}:
                if not LINE_FRAGMENT_RE.fullmatch(fragment):
                    issues.append(
                        Issue(source, link.line, f"{destination}: cannot validate fragment on non-Markdown target")
                    )
                continue

            anchors = anchor_cache.setdefault(target, anchors_in(target))
            if fragment not in anchors:
                issues.append(Issue(source, link.line, f"{destination}: anchor '#{fragment}' not found"))
    return issues


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", nargs="?", default=".", help="path inside the repository")
    arguments = parser.parse_args()

    try:
        root = repository_root(Path(arguments.root))
        issues = validate(root)
    except (OSError, UnicodeError, RuntimeError) as error:
        print(f"Markdown link checker failed: {error}", file=sys.stderr)
        return 2

    if issues:
        for issue in issues:
            relative = issue.source.relative_to(root).as_posix()
            print(f"{relative}:{issue.line}: {issue.message}", file=sys.stderr)
        print(f"Markdown link check failed with {len(issues)} issue(s).", file=sys.stderr)
        return 1

    print(f"Markdown link check passed for {len(markdown_files(root))} file(s).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
