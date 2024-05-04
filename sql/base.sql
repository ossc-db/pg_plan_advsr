LOAD 'pg_hint_plan';
LOAD 'pg_plan_advsr';
LOAD 'pg_store_plans';

-- Clean-up
truncate hint_plan.hints;
truncate plan_repo.norm_queries;
truncate plan_repo.raw_queries;
truncate plan_repo.plan_history;
select pg_store_plans_reset();

-- Enable auto-tuning
set max_parallel_workers to 0;
set max_parallel_workers_per_gather to 0;
set random_page_cost = 2;
set pg_plan_advsr.quieted to on;
select pg_plan_advsr_enable_feedback();

-- Execute the query 4 times
\o results/auto-tuning.tmpout
explain analyze 
select * 
from (select a.c1, a.c2 from table_a a, table_b b where a.c1 = b.c1 and a.c2 = b.c2) t1
join (select c.c1, c.c2 from table_c c where c.c1 > 1 and c.c2 >= 10) t2
on t1.c1 = t2.c1 and t1.c2 = t2.c2;

explain analyze 
select * 
from (select a.c1, a.c2 from table_a a, table_b b where a.c1 = b.c1 and a.c2 = b.c2) t1
join (select c.c1, c.c2 from table_c c where c.c1 > 1 and c.c2 >= 10) t2
on t1.c1 = t2.c1 and t1.c2 = t2.c2;

explain analyze 
select * 
from (select a.c1, a.c2 from table_a a, table_b b where a.c1 = b.c1 and a.c2 = b.c2) t1
join (select c.c1, c.c2 from table_c c where c.c1 > 1 and c.c2 >= 10) t2
on t1.c1 = t2.c1 and t1.c2 = t2.c2;

explain analyze 
select * 
from (select a.c1, a.c2 from table_a a, table_b b where a.c1 = b.c1 and a.c2 = b.c2) t1
join (select c.c1, c.c2 from table_c c where c.c1 > 1 and c.c2 >= 10) t2
on t1.c1 = t2.c1 and t1.c2 = t2.c2;
\o
\! sql/maskout.sh results/auto-tuning.tmpout

-- Check the result of auto-tuning
select rows_hint, join_rows_err, lead_hint, join_hint, scan_hint, join_cnt from plan_repo.plan_history order by id desc limit 4;
select norm_query_string, hints from hint_plan.hints;

-- Clean-up
\! rm -f results/auto-tuning.tmpout

