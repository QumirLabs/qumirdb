SELECT avg(d) OVER (PARTITION BY id) FROM date_values;
