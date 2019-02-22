truncate hint_plan.hints;

truncate plan_repo.norm_queries;
truncate plan_repo.raw_queries;
truncate plan_repo.plan_history;

select pg_store_plans_reset();

