"""Compare quoted CSV results without assuming a stable row order."""

from collections import defaultdict
import csv
import math
from pathlib import Path
import re


FLOAT = re.compile(r"[+-]?(?:(?:[0-9]+\.[0-9]*|\.[0-9]+)(?:[eE][+-]?[0-9]+)?|[0-9]+[eE][+-]?[0-9]+)\Z")
INTEGER = re.compile(r"-?(?:0|[1-9][0-9]*)\Z")


def rows_match(left, right, rel_tol, abs_tol):
    return all(math.isclose(a, b, rel_tol=rel_tol, abs_tol=abs_tol) for a, b in zip(left, right))


def match_group(left, right, rel_tol, abs_tol):
    if len(left) != len(right):
        return False
    left, right = sorted(left), sorted(right)
    if all(rows_match(a, b, rel_tol, abs_tol) for a, b in zip(left, right)):
        return True
    # A tolerant match is not transitive. A greedy match can consume the only
    # partner of a later row, so use augmenting paths for ambiguous groups.
    edges = [[j for j, b in enumerate(right) if rows_match(a, b, rel_tol, abs_tol)] for a in left]
    left_match = [-1] * len(left)
    right_match = [-1] * len(right)
    for start in range(len(left)):
        queue, seen, parents = [start], {start}, {}
        free = None
        for i in queue:
            for j in edges[i]:
                if j in parents:
                    continue
                parents[j] = i
                if right_match[j] == -1:
                    free = j
                    break
                other = right_match[j]
                if other not in seen:
                    seen.add(other)
                    queue.append(other)
            if free is not None:
                break
        if free is None:
            return False
        while free != -1:
            i = parents[free]
            previous = left_match[i]
            left_match[i], right_match[free] = free, i
            free = previous
    return True


def compare_csv(left_path, right_path, sql="", rel_tol=1e-9, abs_tol=1e-6):
    def load(path):
        with Path(path).open(newline="") as source:
            return list(csv.reader(source))

    left, right = load(left_path), load(right_path)
    if not left and not right:
        return {"status": "equal", "rows": 0}
    if not left or not right or left[0] != right[0]:
        return {"status": "different", "reason": "column headers differ"}
    width = len(left[0])
    left, right = left[1:], right[1:]
    if any(len(row) != width for row in left + right):
        return {"status": "different", "reason": "malformed CSV row"}
    floating = []
    for column in range(width):
        cells = [row[column] for row in left + right]
        if cells and all(FLOAT.fullmatch(cell) or INTEGER.fullmatch(cell) for cell in cells):
            if any(FLOAT.fullmatch(cell) for cell in cells) and all(math.isfinite(float(cell)) for cell in cells):
                floating.append(column)

    def groups(rows):
        result = defaultdict(list)
        for row in rows:
            key = tuple(value for i, value in enumerate(row) if i not in floating)
            result[key].append(tuple(float(row[i]) for i in floating))
        return result

    a, b = groups(left), groups(right)
    equal = a.keys() == b.keys() and all(match_group(a[key], b[key], rel_tol, abs_tol) for key in a)
    if equal:
        return {"status": "equal", "rows": len(left)}
    # A limited result can select different tied rows. Never approve a changed
    # subset merely because LIMIT occurs: retain both results for an unbounded
    # check or a check against the complete ordering keys.
    limited = bool(re.search(r"\blimit\s+[0-9]+|\btop\s+[0-9]+", sql, re.IGNORECASE))
    return {
        "status": "requires_review" if limited else "different",
        "reason": "limited result differs; boundary ties are not proven" if limited else "row multisets differ",
        "left_rows": len(left),
        "right_rows": len(right),
    }
