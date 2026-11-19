"""Reads the version the project declares, which is the version of every package of a release.

The version of the library and of the packages released with it is stated once, in the project()
call of the top level CMakeLists.txt. The declaration lint compares the version of each package
against it and the packager names the release after it, so both read the value here rather than
keeping an expression of their own. A second expression is a second answer, and the two copies
drift until a package ships a version nobody thinks to question.
"""

from __future__ import annotations

import re
from pathlib import Path

# The project() call that states the version. CMake accepts the command in any case, whitespace
# between the command and its parenthesis, and a project name that is an identifier or a quoted
# string, so each of those forms is read here. The command is anchored to the start of a line,
# because a mention of it inside another command is not a declaration.
PROJECT_VERSION = re.compile(
    r'^\s*project\s*\(\s*(?:"[^"]*"|[A-Za-z_][A-Za-z0-9_+-]*)\s+VERSION\s+([0-9][0-9.]*)',
    re.IGNORECASE | re.MULTILINE)

# A bracketed comment. It is removed before the line comments, because its opening line would
# otherwise be consumed as a line comment and leave its body as code.
BLOCK_COMMENT = re.compile(r"#\[\[.*?\]\]", re.DOTALL)
LINE_COMMENT = re.compile(r"#[^\n]*")


def declare(text: str) -> str | None:
    """Returns the version a CMake source declares, or None when it declares none.

    Comments are removed first, because a project() call that is commented out is not a
    declaration: a commented example above the real call would otherwise decide the version.
    """
    body = LINE_COMMENT.sub("", BLOCK_COMMENT.sub("", text))
    match = PROJECT_VERSION.search(body)
    return match.group(1) if match else None


def find(start: Path) -> tuple[Path, str] | None:
    """Returns the nearest directory at or above \\a start that declares a project version.

    The nearest one wins, because a nested project states the version of what is built inside it
    and the directory of a package is the innermost statement of the tree it belongs to. A
    CMakeLists.txt that declares no version, and one that cannot be read, are passed over for the
    next directory above them.

    :returns: the directory and the version it declares, or None when no directory at or above
              \\a start declares one.
    """
    directory = Path(start)
    for candidate in [directory, *directory.parents]:
        manifest = candidate / "CMakeLists.txt"
        if not manifest.is_file():
            continue
        try:
            # A manifest that cannot be decoded is not a reason to fail the whole check: the next
            # directory above may state the version instead.
            text = manifest.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        version = declare(text)
        if version is not None:
            return candidate, version
    return None


def release_tag(version: str) -> str:
    """Returns the release tag of a version, which drops its trailing zero components.

    The tag names the release, and zero components at the end of a version add nothing to the name:
    0.1.0.0 is published as models-v0.1, which is the form the documentation and the release notes
    use. A version of nothing but zeros keeps one component, because a tag without a number would
    name no version at all.
    """
    parts = version.split(".")
    while len(parts) > 1 and not parts[-1].strip("0"):
        parts.pop()
    return "models-v" + ".".join(parts)
