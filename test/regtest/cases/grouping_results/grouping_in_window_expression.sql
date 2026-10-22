SELECT grouping(category) + grouping(class) AS lochierarchy,
       rank() OVER (PARTITION BY grouping(category) + grouping(class),
                    CASE WHEN grouping(class) = 0 THEN category END
                    ORDER BY sum(value) DESC) AS rank_within_parent
FROM w
GROUP BY ROLLUP(category, class);
