"""Compares the Python decoder against the C encoder's expectations.

The C++ decoder is checked against the same files by the test binary; this
script closes the triangle, so that all three implementations are pinned to one
set of bytes in CI rather than to each other in review.

Usage:
    python tools/trace-golden/compare.py [directory]

Exit code 0 when every vector matches, 1 when one does not, 2 when the vectors
are missing (the generator has not been run).
"""

import json
import sys
from pathlib import Path

from decode import decode


def main(argv):
    directory = Path(argv[1]) if len(argv) > 1 else Path(__file__).resolve().parent.parent.parent / "test" / "trace-golden"

    if not directory.is_dir():
        print(f"no such directory: {directory}", file=sys.stderr)
        return 2

    vectors = sorted(directory.glob("*.bin"))

    if not vectors:
        print(f"no golden vectors in {directory}", file=sys.stderr)
        return 2

    failures = 0

    for vector in vectors:
        expected_path = vector.with_suffix(".expected.json")

        if not expected_path.exists():
            print(f"{vector.name}: missing expected json")
            failures += 1
            continue

        expected = json.loads(expected_path.read_text(encoding="utf-8"))
        actual = decode(vector.read_bytes(), vector.stem)

        if actual != expected:
            failures += 1
            print(f"{vector.name}: MISMATCH")
            for key in ("unknownTypes", "truncatedBytes"):
                if actual.get(key) != expected.get(key):
                    print(f"  {key}: expected {expected.get(key)}, got {actual.get(key)}")

            for left, right in zip(expected["records"], actual["records"]):
                if left != right:
                    print(f"  record {left.get('index')}: expected {left}")
                    print(f"  record {right.get('index')}:      got {right}")
                    break

            if len(expected["records"]) != len(actual["records"]):
                print(f"  record count: expected {len(expected['records'])}, got {len(actual['records'])}")
        else:
            print(f"{vector.name}: ok")

    print(f"\n{len(vectors) - failures}/{len(vectors)} vectors match")

    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
