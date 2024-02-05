create extension pg_hint_plan;
create extension pg_store_plans;
create extension pg_plan_advsr;

CREATE TABLE table_a (c1, c2) as (SELECT i, i FROM generate_series(1, 10000) as s(i));
CREATE TABLE table_b as SELECT * from table_a;
CREATE TABLE table_c as SELECT * from table_a;
CREATE INDEX ind_a_c1 on table_a (c1);
CREATE INDEX ind_a_c2 on table_a (c2);
CREATE INDEX ind_b_c1 on table_b (c1);
CREATE INDEX ind_b_c2 on table_b (c2);
CREATE INDEX ind_c_c1 on table_c (c1);
CREATE INDEX ind_c_c2 on table_c (c2);
ANALYZE;
