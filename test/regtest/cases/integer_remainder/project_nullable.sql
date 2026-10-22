SELECT id, a % b AS ab, b % a AS ba, a % 2 AS a2, 7 % a AS sa, b % -3 AS bn, id % a AS ia
FROM t ORDER BY id;
