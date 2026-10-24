SELECT t.x FROM t JOIN u ON t.x = u.x AND row_number() OVER () = 1;
