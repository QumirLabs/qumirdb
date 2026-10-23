WITH j AS (SELECT jt.y, ju.z FROM jt JOIN ju ON jt.x = ju.x) SELECT y FROM j UNION ALL SELECT z FROM j;
