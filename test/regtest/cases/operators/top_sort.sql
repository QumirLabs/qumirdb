-- Several batches and a limit below the row count, so the top-sort kernel
-- merges a running top-K instead of sorting once.
SELECT k AS k, v AS v FROM t ORDER BY v DESC, k ASC LIMIT 3;
