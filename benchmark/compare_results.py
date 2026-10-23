"""Compare quoted CSV results; row order is checked only where the query defines one."""

from collections import defaultdict
import csv
import math
from pathlib import Path
import re


FLOAT = re.compile(r"[+-]?(?:(?:[0-9]+\.[0-9]*|\.[0-9]+)(?:[eE][+-]?[0-9]+)?|[0-9]+[eE][+-]?[0-9]+)\Z")
INTEGER = re.compile(r"-?(?:0|[1-9][0-9]*)\Z")
NULL = "NULL"


def defines_order(sql):
    """True if the statement has an ORDER BY outside any parentheses."""
    sql = re.sub(r"'(?:[^']|'')*'|--[^\n]*|/\*.*?\*/", " ", sql, flags=re.DOTALL)
    depth = 0
    for token in re.finditer(r"[()]|\border\s+by\b", sql, re.IGNORECASE):
        if token.group() == "(":
            depth += 1
        elif token.group() == ")":
            depth -= 1
        elif depth == 0:
            return True
    return False


def values_match(a, b, rel_tol, abs_tol):
    # NULL (None) is an exact value: it equals only another NULL.
    if a is None or b is None:
        return a is b
    return math.isclose(a, b, rel_tol=rel_tol, abs_tol=abs_tol)


def rows_match(left, right, rel_tol, abs_tol):
    return all(values_match(a, b, rel_tol, abs_tol) for a, b in zip(left, right))


def match_group(left, right, rel_tol, abs_tol):
    if len(left) != len(right):
        return False
    null_last = lambda row: tuple((value is None, value or 0.0) for value in row)
    left, right = sorted(left, key=null_last), sorted(right, key=null_last)
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
        # NULL cells do not make a numeric column textual; they stay exact values.
        cells = [row[column] for row in left + right if row[column] != NULL]
        if cells and all(FLOAT.fullmatch(cell) or INTEGER.fullmatch(cell) for cell in cells):
            if any(FLOAT.fullmatch(cell) for cell in cells) and all(math.isfinite(float(cell)) for cell in cells):
                floating.append(column)

    def split(row):
        key = tuple(value for i, value in enumerate(row) if i not in floating)
        return key, tuple(None if row[i] == NULL else float(row[i]) for i in floating)

    def groups(rows):
        result = defaultdict(list)
        for key, numbers in map(split, rows):
            result[key].append(numbers)
        return result

    a, b = groups(left), groups(right)
    equal = a.keys() == b.keys() and all(match_group(a[key], b[key], rel_tol, abs_tol) for key in a)
    if equal:
        # The same rows in a different order are an error only where the query
        # fixes the order. Ties of a partial ORDER BY key are not exempt.
        if defines_order(sql) and not all(
                ka == kb and rows_match(na, nb, rel_tol, abs_tol)
                for (ka, na), (kb, nb) in zip(map(split, left), map(split, right))):
            return {"status": "different", "reason": "row order differs", "rows": len(left)}
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
