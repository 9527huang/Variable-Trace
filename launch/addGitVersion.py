import subprocess
from pathlib import Path

# Run git log command to get the hash and format it into a Python file
git_hash = subprocess.check_output(
    [
        "git",
        "log",
        '--pretty=format:#define GIT_INFO_PRESENT%n static constexpr const char* GIT_HASH = "%H";',
        "-n",
        "1",
    ]
).decode()

# Preserve the timestamp when the commit did not change so incremental builds
# do not recompile the two translation units that include this generated file.
file_path = Path("./src/gitversion.hpp")
content = git_hash + "\n"
if not file_path.exists() or file_path.read_text(encoding="utf-8") != content:
    file_path.write_text(content, encoding="utf-8", newline="\n")
