-- The input arg_0 is projected first; x * y must not reuse its name.
SELECT sum(arg_0) AS a, sum(x * y) AS xy FROM t;
