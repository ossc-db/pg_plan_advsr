-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_plan_advsr" to load this file. \quit

SET search_path = public;
SET LOCAL client_min_messages = WARNING;

CREATE SCHEMA plan_repo;

-- Register tables
CREATE TABLE plan_repo.plan_history
(
	id					serial,
	norm_query_hash		text,
	pgsp_queryid		bigint,
	pgsp_planid			bigint,
	execution_time		double precision,
	rows_hint			text,
	scan_hint			text,
	join_hint			text,
	lead_hint			text,
	diff_of_scans		double precision,
	max_diff_ratio_scan		double precision,
	diff_of_joins		double precision,
	max_diff_ratio_join		double precision,
	join_cnt			int,
	application_name	text,
	timestamp			timestamp
);

CREATE TABLE plan_repo.norm_queries
(
	norm_query_hash		text,
	norm_query_string	text
);

CREATE TABLE plan_repo.raw_queries
(
	norm_query_hash		text,
	raw_query_id		serial,
	raw_query_string	text,
	timestamp			timestamp
);


-- Register functions
CREATE FUNCTION pg_plan_advsr_enable_feedback()
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION pg_plan_advsr_disable_feedback()
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE OR REPLACE FUNCTION plan_repo.get_hint(bigint)
RETURNS text
	AS 'select ''/*+'' || chr(10) || '
	   'lead_hint || chr(10) || '
	   'join_hint || chr(10) || '
	   'scan_hint || chr(10) || '
	   '''*/'' || chr(10) || '
	   '''--'' || pgsp_planid '
	   'from plan_repo.plan_history '
	   'where pgsp_planid = $1 '
	   'order by id desc '
	   'limit 1;'
LANGUAGE SQL
IMMUTABLE
RETURNS NULL ON NULL INPUT;
