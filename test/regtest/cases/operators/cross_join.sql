-- A join without equi-keys: the nested-loop kernel and its residual filter.
SELECT l.k AS k, r.w AS w FROM l, r WHERE l.v < r.w;
