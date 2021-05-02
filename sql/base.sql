LOAD 'pg_hint_plan';
LOAD 'pg_plan_advsr';
set pg_plan_advsr.quieted to on;
select pg_plan_advsr_enable_feedback();

-- CTE (less than PG12)
-- First iteration
\o results/pg10cte.tmpout
explain analyze 
with x as (
	select * 
	from   t1 
	limit 201
) 
select * 
from (  select *
		from   t1 
		where  a in (select a from x)
		) tmp,
		  t2 
where tmp.a = t2.a;
\o
\! sql/maskout.sh results/pg10cte.tmpout

-- Hints
select scan_hint from plan_repo.plan_history;
select join_hint from plan_repo.plan_history;
select lead_hint from plan_repo.plan_history;
select rows_hint from plan_repo.plan_history;

-- Second iteration
\o results/pg10cte_fixed.tmpout
explain analyze 
with x as (
	select * 
	from   t1 
	limit 201
) 
select * 
from (  select *
		from   t1 
		where  a in (select a from x)
		) tmp,
		  t2 
where tmp.a = t2.a;
\o
\! sql/maskout.sh results/pg10cte_fixed.tmpout

-- Check Hints. Rows_hint must be NULL since estimation error was fixed.
select scan_hint from plan_repo.plan_history order by id desc limit 1;
select join_hint from plan_repo.plan_history order by id desc limit 1;
select lead_hint from plan_repo.plan_history order by id desc limit 1;
select rows_hint from plan_repo.plan_history order by id desc limit 1;

-- Clean-up
\! rm -f results/pg10cte.tmpout results/pg10cte_fixed.tmpout

