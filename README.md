pg_plan_advsr
=============

pg_plan_advsr is a PostgreSQL extension that provides Automated execution plan tuning using feedback loop.
This extension might help you if you have an analytic query which has many joins and aggregates and you'd like to get an efficient plan to reduce execution time. This extension is intended to use in a plan tuning phase at the end of system development.

* Note: This extension is intended to be used in a validation environment, not a commercial environment.
* master branch for PostgreSQL 12 or above [![CI](https://github.com/ossc-db/pg_plan_advsr/workflows/CI/badge.svg)](https://github.com/ossc-db/pg_plan_advsr/actions?query=workflow%3ACI)

Contents
========

This README contains the following sections:

1. [Cookbook](#1-cookbook)
2. [Objects created by the extension](#2-objects-created-by-the-extension)
3. [Options](#3-options)
4. [Usage](#4-usage)
5. [Installation Requirements](#5-installation-requirements)
6. [Installation](#6-installation)
7. [Internals](#7-internals)
8. [Limitations](#8-limitations)
9. [Support](#9-support)
10. [Author](#10-author)
11. [Acknowledgments](#11-acknowledgments)

pg_plan_advsr was created by Tatsuro Yamada.


1 Cookbook
==========

This is a simple example of how to use pg_plan_advsr for auto execution plan tuning.
More detailed information will be provided in the sections
[Options](#3-options) and [Usage](#4-usage).

Try running regression tests inside the sql directory: pg_plan_advsr/sql.
First, create a test table, etc. to execute init.sql. After that, run base.sql and observe the results of automatic plan tuning.

As shown below, you can see that a Nested Loop join during the first run has changed to a Hash join after tuning.
The error in the estimated number of rows should be zero, and the query execution time was successfully reduced.

Note that ``base.sql`` will clear these tables and view such as hint_plan.hints, plan_repo.norm_queries, plan_repo.raw_queries, plan_repo.plan_history, and pg_store_plans view. So, please take care if you use it.

	psql
	\i init.sql
	\i base.sql

#### Before tuning (first run)

	                                                          QUERY PLAN
	-------------------------------------------------------------------------------------------------------------------------------
	 Nested Loop  (cost=295.29..515.35 rows=1 width=16) (actual time=24.591..164.421 rows=9991 loops=1)
	   ->  Hash Join  (cost=295.00..515.01 rows=1 width=16) (actual time=24.500..46.584 rows=10000 loops=1)
	         Hash Cond: ((a.c1 = b.c1) AND (a.c2 = b.c2))
	         ->  Seq Scan on table_a a  (cost=0.00..145.00 rows=10000 width=8) (actual time=0.032..3.899 rows=10000 loops=1)
	         ->  Hash  (cost=145.00..145.00 rows=10000 width=8) (actual time=24.359..24.359 rows=10000 loops=1)
	               Buckets: 16384  Batches: 1  Memory Usage: 519kB
	               ->  Seq Scan on table_b b  (cost=0.00..145.00 rows=10000 width=8) (actual time=0.026..5.771 rows=10000 loops=1)
	   ->  Index Scan using ind_c_c2 on table_c c  (cost=0.29..0.33 rows=1 width=8) (actual time=0.009..0.010 rows=1 loops=10000)
	         Index Cond: ((c2 = a.c2) AND (c2 >= 10))
	         Filter: ((c1 > 1) AND (c1 = a.c1))
	 Planning Time: 3.952 ms
	 Execution Time: 168.867 ms
	(12 rows)

#### After tuning (final run)
	                                                          QUERY PLAN
	-------------------------------------------------------------------------------------------------------------------------------
	 Hash Join  (cost=590.00..934.88 rows=9991 width=16) (actual time=37.030..74.904 rows=9991 loops=1)
	   Hash Cond: ((a.c1 = b.c1) AND (a.c2 = b.c2))
	   ->  Hash Join  (cost=295.00..564.93 rows=9991 width=16) (actual time=17.751..40.944 rows=9991 loops=1)
	         Hash Cond: ((c.c1 = a.c1) AND (c.c2 = a.c2))
	         ->  Seq Scan on table_c c  (cost=0.00..195.00 rows=9990 width=8) (actual time=0.060..7.318 rows=9991 loops=1)
	               Filter: ((c1 > 1) AND (c2 >= 10))
	               Rows Removed by Filter: 9
	         ->  Hash  (cost=145.00..145.00 rows=10000 width=8) (actual time=17.658..17.660 rows=10000 loops=1)
	               Buckets: 16384  Batches: 1  Memory Usage: 519kB
	               ->  Seq Scan on table_a a  (cost=0.00..145.00 rows=10000 width=8) (actual time=0.019..4.141 rows=10000 loops=1)
	   ->  Hash  (cost=145.00..145.00 rows=10000 width=8) (actual time=19.249..19.250 rows=10000 loops=1)
	         Buckets: 16384  Batches: 1  Memory Usage: 519kB
	         ->  Seq Scan on table_b b  (cost=0.00..145.00 rows=10000 width=8) (actual time=0.022..4.799 rows=10000 loops=1)
	 Planning Time: 3.961 ms
	 Execution Time: 78.294 ms
	(15 rows)


See: [Usage](#4-usage) for more details.

2 Objects created by the extension
==================================

Functions
---------
- FUNCTION ``pg_plan_advsr_enable_feedback()`` RETURNS void
- FUNCTION ``pg_plan_advsr_disable_feedback()`` RETURNS void
- FUNCTION ``plan_repo.get_hint(bigint)`` RETURNS text
	- If you give a pgsp_planid as an argument, it will return the hints to reproduce the plan based on pgsp_planid
- FUNCTION ``plan_repo.get_extstat(bigint)`` RETURNS text
	- If you give a queryid as an argument, it will return the syntax for generating extended statistics. This function supports PG14 or above since it uses compute_query_id.

Tables
------
- ``plan_repo.plan_history``
- ``plan_repo.norm_queries``
- ``plan_repo.raw_queries``

Table "plan_repo.plan_history"

	      Column         |            Type             | Description
	---------------------+-----------------------------+-------------------------------------------------------------------------------
	 id                  | integer                     | Sequence as a primary key: nextval('plan_repo.plan_history_id_seq'::regclass)
	 norm_query_hash     | text                        | MD5 based on normalized query text
	 pgsp_queryid        | bigint                      | Queryid of pg_store_plans
	 pgsp_planid         | bigint                      | Planid of pg_sotre_plans
	 execution_time      | numeric                     | Execution time (ms) of this planid
	 rows_hint           | text                        | Rows hint of this plan
	 scan_hint           | text                        | Scan hint of this plan
	 join_hint           | text                        | Join hint of this plan
	 lead_hint           | text                        | Leading hint of this plan
	 scan_rows_err       | numeric                     | Sum of estimation row error of scans
	 scan_err_ratio      | numeric                     | Maximum estimation row error ratio of scans
	 join_rows_err       | numeric                     | Sum of estimation row error of joins
	 join_err_ratio      | numeric                     | Maximum estimation row error ratio of joins
	 scan_cnt            | integer                     | Number of scan nodes in this plan
	 join_cnt            | integer                     | Number of Join nodes in this plan
	 application_name    | text                        | Application name of client tool such as "psql"
	 timestamp           | timestamp without time zone | Timestamp of this record inserted

Table "plan_repo.norm_queries"

	      Column       |           Type             | Description
	-------------------+----------------------------+-----------------------------------
	 norm_query_hash   | text                       | MD5 based on normalized query text
	 norm_query_string | text                       | Normalized query text

Table "plan_repo.raw_queries"

	      Column      |            Type             | Description
	------------------+-----------------------------+----------------------------------------------------------------------------------------
	 norm_query_hash  | text                        | MD5 based on normalized query text
	 raw_query_id     | integer                     | Sequence of raw query text: nextval('plan_repo.raw_queries_raw_query_id_seq'::regclass)
	 raw_query_string | text                        | Raw query text (not normalized)
	 timestamp        | timestamp without time zone | Timestamp of this record inserted


Views
-----
- ``plan_repo.plan_history_pretty``

	Columns are same as plan_history table, but number of decimal places are reduced for readability


3 Options
=========

- ``pg_plan_advsr.enabled``

	"ON": Enable pg_plan_advsr.
	It allows creating various hints for fixing row estimation errors and also for reproducing a plan.
	It also stores them in the plan_history table. If you want to use "auto plan tuning using feedback loop", you have to execute below function "pg_plan_advsr_enable_feedback().
	Default setting is "ON".

- ``pg_plan_advsr.quieted``

	"ON": Enable quiet mode.
	It allows to disable emmiting the following messages when your EXPLAIN ANALYZE commmand finished.

	    pgsp_queryid
	    pgsp_planid
	    Execution time
	    Hints for current plan
	    Rows hint (feedback info)
	    and so on

	Default setting is "OFF".

- ``pg_plan_advsr.widely``

	"ON": Enable creating hints even if EXPLAIN command without ANALYZE option.
	It allows creating various hints needed for reproducing a plan, but it doesn't create hints for fixing row estimation errors because there is no information of Actual rows.
	It also stores them in the plan_history table. If you want to get hints to reproduce a plan, this option helps you.
	Default setting is "OFF".

- ``pg_plan_advsr_enable_feedback()``

	This function allows you to use feedback loop for plan tuning.
	Actually, it is a wrapper for these commands:

	    set pg_plan_advsr.enabled to on;
	    set pg_hint_plan.enable_hint_table to on;
	    set pg_hint_plan.debug_print to on;
	
- ``pg_plan_advsr_disable_feedback()``

	This function disables using feedback loop for plan tuning. It is a wrapper for these commands:

	    set pg_plan_advsr.enabled to on;
	    set pg_hint_plan.enable_hint_table to off;
	    set pg_hint_plan.debug_print to off;

4 Usage
=======

TBA

There are four types of usage:

- Displaying Execution Plan Characteristics
- Automatic Execution Plan Tuning
- Automatic Hint Clause Generation
- Extended Statistics Suggestion

Details on how to use each are shown below.

- **Displaying Execution Plan Characteristics**

	First, Make sure ``pg_plan_advsr.enabled to on``.
	Then, Execute EXPLAIN ANALYZE command (which is your query).
	You can get the result with the ``DESCRIBE`` section appended, and you can see the number of joins, scans, errors in row count estimation, and so on.

	e.g.

		explain (analyze, verbose) select * from t where a = 1 and b = 1;
		
				                                               QUERY PLAN
		--------------------------------------------------------------------------------------------------------
		 Seq Scan on public.t  (cost=0.00..195.00 rows=100 width=8) (actual time=0.052..6.269 rows=100 loops=1)
		   Output: a, b
		   Filter: ((t.a = 1) AND (t.b = 1))
		   Rows Removed by Filter: 9900
		 Query Identifier: -3455024416178978571
		 Planning Time: 0.930 ms
		 Execution Time: 9.136 ms

		 DESCRIBE
		 ------------------------
		 application:    psql
		 pgsp_queryid:   -3455024416178978571
		 pgsp_planid:    3455841613
		 join_cnt:       0
		 join_rows_err:  0
		 join_err_ratio: 0.00
		 scan_cnt:       1
		 scan_rows_err:  0
		 scan_err_ratio: 0.00
		 lead hint:      LEADING( t  )
		 join hint:
		 scan hint:      SEQSCAN(t)
		 rows hint:
		(23 rows)

- **For auto plan tuning**

	First, Run ``select pg_plan_advsr_enable_feedback();``.
	Then, Execute EXPLAIN ANALYZE command (which is your query) repeatedly until row estimation errors had vanished.
	Finally, You can check a result of the tuning by using the below queries:

	  select pgsp_queryid, pgsp_planid, execution_time, scan_hint, join_hint, lead_hint from plan_repo.plan_history order by id;
	
	  select queryid, planid, plan from pg_store_plans where queryid='your pgsp_queryid in plan_history' order by first_call;
	
	See shell script file as an example: [JOB/auto_tune_31c.sh](https://github.com/ossc-db/pg_plan_advsr/blob/master/JOB/auto_tune_31c.sh)

	Demo of auto tuning (3x speed)
	![demo of auto tune](https://github.com/ossc-db/pg_plan_advsr/blob/master/JOB/img/auto_tune_31c_sql_demo.gif)

	If you'd like to reproduce the execution plans on other environments, you'd be better to read the other.


	Note:
	
	- A plan may temporarily worse than an initial plan during auto tuning phase.
	- Use stable data for auto plan tuning. This extension doesn't get converged plan (the ideal plan for the data) if it was updating concurrently.

- **For getting hints of current query**

	First, Run ``select pg_plan_advsr_disable_feedback();``.
	Then, Execute EXPLAIN ANALYZE command (which is your query).
	Finally, You can get hints by using the below queries:

	  select pgsp_queryid, pgsp_planid, execution_time, scan_hint, join_hint, lead_hint from plan_repo.plan_history order by id;

	e.g.
	
	   pgsp_queryid | pgsp_planid | execution_time |                scan_hint                |     join_hint      |        lead_hint
	  --------------+-------------+----------------+-----------------------------------------+--------------------+-------------------------
	     4173287301 |  3707748199 |        265.179 | SEQSCAN(t2) SEQSCAN(x) INDEXSCAN(t1)    | HASHJOIN(t2 t1 x) +| LEADING( (t2 (x t1 )) )
	                |             |                |                                         | NESTLOOP(t1 x)     |
	     4173287301 |  1101439786 |          2.149 | SEQSCAN(x) INDEXSCAN(t1) INDEXSCAN(t2)  | NESTLOOP(t2 t1 x) +| LEADING( ((x t1 )t2 ) )
	                |             |                |                                         | NESTLOOP(t1 x)     |

	  # \a
	  Output format is unaligned.
	  # \t
	  Tuples only is on.
		
	  select plan_repo.get_hint(1101439786);
		
	  /*+
	  LEADING( ((x t1 )t2 ) )
	  NESTLOOP(t2 t1 x)
	  NESTLOOP(t1 x)
	  SEQSCAN(x) INDEXSCAN(t1) INDEXSCAN(t2)
	  */
	  --1101439786
	
	You can use the hints to reproduce the execution plan anywhere. It also can be used to modify the execution plan by changing the hints manually.

- **For getting extended statistics suggestion**

	This feature is enabled when you use PG14 or above with pg_qualstats.
	First, Make sure ``pg_plan_advsr.enabled to on``.
	Then, Execute EXPLAIN ANALYZE command (which is your query).
	Finally, You can get extended statistics suggestion by using the below queries:

	  select * from plan_repo.get_extstat(queryid);

	e.g.

		# select * from plan_repo.get_extstat(-3455024416178978571);
		                 suggest
		------------------------------------------
		 CREATE STATISTICS ON a, b FROM public.t;
		(1 row)

	
5 Installation Requirements
===========================

pg_plan_advsr uses pg_hint_plan and pg_store_plans cooperatively.

- PostgreSQL 12 or above
- pg_hint_plan
- pg_store_plans
- pg_qualstats
	- if you'd like to use Extended statistic suggestion feature on PG14 or above
- RHEL/CentOS/Rocky = 7.x or above


6 Installation
==============

TBA

There are two methods to install the extension: Using building pg_plan_advsr manually.

- ``Build and install (make && make install)``

	- Prerequisite for installation
		- Install postgresql-devel package if you installed PostgreSQL by rpm files
		- Set the PATH environment variable to pg_config of your PostgreSQL
	
	Operations
	
	1. git clone extensions

		```
		-- Required
		git clone https://github.com/ossc-db/pg_hint_plan.git
		git clone https://github.com/ossc-db/pg_store_plans.git
		git clone https://github.com/ossc-db/pg_plan_advsr.git

		-- Optional: if you use Extended statistic suggestion feature on PG14 or above
		git clone https://github.com/powa-team/pg_qualstats.git
		```

	2. git checkout
		
		##### Set the appropriate version of PostgreSQL for the VERSION variable. For example, If you use PG12, see below:
		```
		export VERSION=12
		cd pg_hint_plan
		git checkout -b PG${VERSION} origin/PG${VERSION} && git checkout $(git describe --tag)
		cd ../pg_store_plans
		git checkout $(git describe --tag)
		```
		
	3. build and install

		```
		-- Required
		cd ../pg_hint_plan
		make -s && make -s install
		cp pg_stat_statements.c ../pg_plan_advsr/
		cp normalize_query.h ../pg_plan_advsr/
		
		cd ../pg_store_plans
		make -s USE_PGXS=1 all install
		cp pgsp_json*.[ch] ../pg_plan_advsr/
		
		cd ../pg_plan_advsr
		git describe --alway
		make
		make install

		-- Optional
		cd ../pg_qualstats
		make
		make install
		```

	4. edit PostgreSQL.conf

		```
		vi $PGDATA/postgresql.conf

		---- Add these lines -----------------------------------------------------
		-- Required
		shared_preload_libraries = 'pg_hint_plan, pg_plan_advsr, pg_store_plans'
		max_parallel_workers_per_gather = 0
		max_parallel_workers = 0
		compute_query_id = on

		or

		-- Optional
		shared_preload_libraries = 'pg_hint_plan, pg_plan_advsr, pg_store_plans, pg_qualstats'
		max_parallel_workers_per_gather = 0
		max_parallel_workers = 0
		compute_query_id = on
		pg_qualstats.resolve_oids = true
		pg_qualstats.sample_rate = 1
		-----------------------------------------------------------------------------------

		---- Consider tweak these numbers -------------------------------------------------
		-- Use a large value than join numbers of your query
		geqo_threshold = 12 -> 20
		from_collapse_limit = 8 -> 20
		join_collapse_limit = 8 -> 20

		-- Optional
		random_page_cost = 4 -> 1
		-----------------------------------------------------------------------------------

	5. run create extension commands on psql

		```
		pg_ctl start
		psql

		-- Required
		create extension pg_hint_plan;
		create extension pg_store_plans;
		create extension pg_plan_advsr;

		-- Optional
		create extension pg_qualstats;
		```
	* You can try this extension with Join Order Benchmark as a example.
	See: [how_to_setup.md in JOB directory](https://github.com/ossc-db/pg_plan_advsr/blob/master/JOB/how_to_setup.md)


- ``Dockerfile (experimental)``

	Operations

		\# cd pg_plan_advsr/docker
		\# ./build.sh

	See: build.sh and Dockerfile


7 Internals
===========

TBA

These presentation materials are useful to know concepts and its architecture, and these show
a benchmark result by using Join order benchmark:

* [AUTO PLAN TUNING USING FEEDBACK LOOP at PGConf.Eu 2018](https://www.postgresql.eu/events/pgconfeu2018/schedule/session/2132-auto-plan-tuning-using-feedback-loop/)

* [AUTO PLAN TUNING USING FEEDBACK LOOP at PGConf.Russia 2019](https://pgconf.ru/en/2019/242844)


8 Limitations
=============

Not supported
------------
 - Handle InitPlans and SubPlans
 - Handle Append and MergeAppend
 - Fix bese-relation's estimated row error (This is pg_hint_plan's limitation)
 - Concurrent execution
 - Extended Statistics Suggestion for Grouping columuns and Expressions
 - Extended Statistics Suggestion on PG13 or below

Not tested
----------
 - Parallel query
 - Partitioned Table
 - JIT

See: [TODO file](https://github.com/ossc-db/pg_plan_advsr/blob/master/TODO)

pg_plan_advsr uses pg_hint_plan and pg_store_plans, it would be better to check these document to know their limitations.

* [pg_hint_plan](https://github.com/ossc-db/pg_hint_plan/blob/master/doc/pg_hint_plan.html)
* [pg_store_plans](https://github.com/ossc-db/pg_store_plans/blob/master/doc/index.html)


9 Support
=========

If you want to report a problem with pg_plan_advsr, please include the following information because we will analyze it by reproducing your problem:

 - Versions
	- PostgreSQL
	- pg_hint_plan
	- pg_store_plans
 - Query
 - DDL
	- CREATE TABLE
	- CREATE INDEX
 - Data (If possible)

If you have a problem or question or any kind of feedback, the preferred option is to open an issue on GitHub:
https://github.com/ossc-db/pg_plan_advsr/issues
This requires a GitHub account.
Of course, any Pull request welcome!


10 Author
=========

Tatsuro Yamada (yamatattsu at gmail dot com)

Copyright (c) 2019-2024, NIPPON TELEGRAPH AND TELEPHONE CORPORATION


11 Acknowledgments
==================

The following individuals (in alphabetical order) have contributed to pg_plan_advsr as patch authors, reviewers, testers, advisers, or reporters of issues. Thanks a lot!

Amit Langote  
David Pitts  
Etsuro Fujita  
Hironobu Suzuki  
Julien Rouhaud  
Kaname Furutani  
Kyotaro Horiguchi  
Laurenz Albe  
Nuko Yokohama  
Sam Xu  
