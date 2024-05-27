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
	scan_rows_err		double precision,
	scan_err_ratio		double precision,
	join_rows_err		double precision,
	join_err_ratio		double precision,
	scan_cnt			int,
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

-- Register view
CREATE VIEW plan_repo.plan_history_pretty
AS
SELECT id,
	   norm_query_hash,
	   pgsp_queryid,
	   pgsp_planid,
	   execution_time::numeric(18, 3),
	   rows_hint,
	   scan_hint,
	   join_hint,
	   lead_hint,
	   scan_rows_err,
	   scan_err_ratio::numeric(18, 2),
	   join_rows_err,
	   join_err_ratio::numeric(18, 2),
	   scan_cnt,
	   join_cnt,
	   application_name,
	   timestamp
FROM plan_repo.plan_history
ORDER BY id;

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

-- This function can use on PG14 or above with pg_qualstats
CREATE OR REPLACE FUNCTION plan_repo.get_col_from_qualstats(bigint)
RETURNS TEXT
AS $$
	SELECT pg_catalog.quote_ident(a.attname)
	FROM pg_qualstats() q
	JOIN pg_catalog.pg_class c ON coalesce(q.lrelid, q.rrelid) = c.oid
	JOIN pg_catalog.pg_attribute a ON a.attrelid = c.oid
	 AND a.attnum = coalesce(q.lattnum, q.rattnum)
	JOIN pg_catalog.pg_operator op ON op.oid = q.opno
	WHERE q.qualnodeid = $1
	  AND q.qualid is not null;
$$ LANGUAGE sql;


CREATE OR REPLACE FUNCTION plan_repo.get_extstat(bigint)
RETURNS TABLE (suggest text) AS $$
	with all_quals as (
		--左
	    SELECT qualid,
			   lrelid as rel,
			   pg_catalog.quote_ident(n.nspname) || '.' ||
			   pg_catalog.quote_ident(c.relname) as relname,
			   plan_repo.get_col_from_qualstats(qualnodeid) as col
	    FROM pg_qualstats() q
	    JOIN pg_catalog.pg_class c ON q.lrelid = c.oid
	    JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace
		WHERE queryid = $1
		UNION
		--右
	    SELECT qualid,
			   rrelid as rel,
			   pg_catalog.quote_ident(n.nspname) || '.' ||
			   pg_catalog.quote_ident(c.relname) as relname,
			   plan_repo.get_col_from_qualstats(qualnodeid) as col
	    FROM pg_qualstats() q
	    JOIN pg_catalog.pg_class c ON q.rrelid = c.oid
	    JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace
		WHERE queryid = $1
	),
	data as (
		select relname,
			   col
		from all_quals
		where col is not null
		group by relname, col
		ORDER BY 1, 2
	),
	rels_cols as (
		select relname,
			   array_to_string(array_agg(col), ', ') as cols,
			   count(col) as col_num
		from data
		group by relname
	),
	nominated as (
		select relname,
			   cols
		from rels_cols
		where col_num > 1
	)
	select 'CREATE STATISTICS ON ' || array_to_string(array_agg(cols), ', ') || ' ' ||
		   'FROM '|| relname || ';' as suggest
	from nominated
	group by relname
	ORDER BY 1;
$$ LANGUAGE sql;


-- Grant
GRANT SELECT ON plan_repo.plan_history TO PUBLIC;
GRANT SELECT ON plan_repo.norm_queries TO PUBLIC;
GRANT SELECT ON plan_repo.raw_queries TO PUBLIC;
GRANT USAGE ON SCHEMA plan_repo TO PUBLIC;
