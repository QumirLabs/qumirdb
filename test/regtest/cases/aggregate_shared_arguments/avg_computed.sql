-- avg(strlen(s)) lowers to sum and count over one projected strlen(s); the NULL
-- rows of s are left out of both, and group 3 has only NULLs.
SELECT k, avg(strlen(s)) AS a, sum(strlen(s)) AS l, count(strlen(s)) AS c
FROM t GROUP BY k;
