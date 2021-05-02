pg_plan_advsr
=============

pg_plan_advsr is a PostgreSQL extension that provides Automated execution plan tuning using feedback loop.
This extension might help you if you have an analytic query which has many joins and aggregates and you'd like to get an efficient plan to reduce execution time. This extension is intended to use in a plan tuning phase at the end of system development.

* Note: For now, This extension is in POC phase. Not production ready. 
* master branch for PostgreSQL 10, 11, and 12 [![CI](https://github.com/ossc-db/pg_plan_advsr/workflows/CI/badge.svg)](https://github.com/ossc-db/pg_plan_advsr/actions?query=workflow%3ACI)

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

This is a simple example of how to use pg_plan_advsr.
More detailed information will be provided in the sections
[Options](#3-options) and [Usage](#4-usage).
There are Five steps in the example.

First, create two tables:

	create table t1 (a int, b int);
	insert into t1 (select a, random() * 1000 from generate_series(0, 999999) a);
	create index i_t1_a on t1 (a);
	analyze t1;

	create table t2 (a int, b int);
	insert into t2 (select a, random() * 1000 from generate_series(0, 999999) a);
	create index i_t2_a on t2 (a);
	analyze t2;

Second, execute the following queries, and you can get a plan and an execution time.

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

Third, enable feedback loop by the following command.

	select pg_plan_advsr_enable_feedback();

Fourth, execute the previous query (explain analyze ...) twice, and you can see the plan was changed and the execution time was shortened because pg_plan_advsr fixed an estimated row error therefore planner chose more efficient plan than before.

e.g.

#### Before tuning
	
	                                                          QUERY PLAN
	-------------------------------------------------------------------------------------------------------------------------------
	 Hash Join  (cost=9569.92..43000.92 rows=500000 width=16) (actual time=3.308..301.279 rows=201 loops=1)
	   Hash Cond: (t2.a = t1.a)
	   CTE x
	     ->  Limit  (cost=0.00..2.90 rows=201 width=8) (actual time=0.011..0.077 rows=201 loops=1)
	           ->  Seq Scan on t1 t1_1  (cost=0.00..14425.00 rows=1000000 width=8) (actual time=0.010..0.036 rows=201 loops=1)
	   ->  Seq Scan on t2  (cost=0.00..14425.00 rows=1000000 width=8) (actual time=0.024..98.820 rows=1000000 loops=1)
	   ->  Hash  (cost=875.02..875.02 rows=500000 width=12) (actual time=1.364..1.364 rows=201 loops=1)
	         Buckets: 524288  Batches: 2  Memory Usage: 4101kB
	         ->  Nested Loop  (cost=4.95..875.02 rows=500000 width=12) (actual time=0.245..1.186 rows=201 loops=1)
	               ->  HashAggregate  (cost=4.52..6.52 rows=200 width=4) (actual time=0.231..0.296 rows=201 loops=1)
	                     Group Key: x.a
	                     ->  CTE Scan on x  (cost=0.00..4.02 rows=201 width=4) (actual time=0.013..0.148 rows=201 loops=1)
	               ->  Index Scan using i_t1_a on t1  (cost=0.42..4.33 rows=1 width=8) (actual time=0.003..0.004 rows=1 loops=201)
	                     Index Cond: (a = x.a)
	 Planning time: 0.540 ms
	 Execution time: 329.571 ms
	(16 rows)
	

#### After tuning

The topmost join method is changed from Hash Join to Nested loop. The execution time changed 329 ms -> 34 ms.
	
	                                                        QUERY PLAN
	---------------------------------------------------------------------------------------------------------------------------
	 Nested Loop  (cost=8.27..971.76 rows=201 width=16) (actual time=0.265..2.148 rows=201 loops=1)
	   CTE x
	     ->  Limit  (cost=0.00..2.90 rows=201 width=8) (actual time=0.016..0.077 rows=201 loops=1)
	           ->  Seq Scan on t1 t1_1  (cost=0.00..14425.00 rows=1000000 width=8) (actual time=0.015..0.042 rows=201 loops=1)
	   ->  Nested Loop  (cost=4.95..875.02 rows=201 width=12) (actual time=0.256..1.251 rows=201 loops=1)
	         ->  HashAggregate  (cost=4.52..6.52 rows=200 width=4) (actual time=0.241..0.314 rows=201 loops=1)
	               Group Key: x.a
	               ->  CTE Scan on x  (cost=0.00..4.02 rows=201 width=4) (actual time=0.019..0.153 rows=201 loops=1)
	         ->  Index Scan using i_t1_a on t1  (cost=0.42..4.33 rows=1 width=8) (actual time=0.003..0.004 rows=1 loops=201)
	               Index Cond: (a = x.a)
	   ->  Index Scan using i_t2_a on t2  (cost=0.42..0.46 rows=1 width=8) (actual time=0.003..0.003 rows=1 loops=201)
	         Index Cond: (a = t1.a)
	 Planning time: 1.068 ms
	 Execution time: 34.459 ms
	(14 rows)
	

Finally, you can see plan changes and execution time changes to check plan_repo.plan_history table and pg_store_plans view, if you want. Of course, you can bring the execution plan from the verification environment to other environments because this extension use optimizer hint internally and it is just strings (text).

See: [Usage](#4-usage) for more details.



2 Objects created by the extension
==================================

Functions
---------
- FUNCTION ``pg_plan_advsr_enable_feedback()`` RETURNS void
- FUNCTION ``pg_plan_advsr_disable_feedback()`` RETURNS void
- FUNCTION ``plan_repo.get_hint(bigint)`` RETURNS text

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
	 rows_hint           | text                        | Rows_hint of this plan
	 scan_hint           | text                        | Scan_hint of this plan
	 join_hint           | text                        | Join_hint of this plan
	 lead_hint           | text                        | Leading_hint of this plan
	 diff_of_scans       | numeric                     | Sum of estimation row error of scans
	 max_diff_ratio_scan | numeric                     | Maximum estimation row error ratio of scans
	 diff_of_joins       | numeric                     | Sum of estimation row error of joins
	 max_diff_ratio_join | numeric                     | Maximum estimation row error ratio of joins
	 join_cnt            | integer                     | Join number of this plan
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

There are two types of usage.

- For auto plan tuning

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

- For only getting hints of current query to reproduce a plan on other databases

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

	
5 Installation Requirements
===========================

pg_plan_advsr uses pg_hint_plan and pg_store_plans cooperatively.

- PostgreSQL => 10.4, 11, and 12
- pg_hint_plan => 1.3.2
- pg_store_plans => 1.3
- RHEL/CentOS = 7.x (6.x is not tested but I suppose it works)


6 Installation
==============

TBA

There are two methods to install the extension: Using Dockerfile or building pg_plan_advsr manually.

- ``Dockerfile (experimental)``

	Operations

		\# cd pg_plan_advsr/docker
		\# ./build.sh
	
	See: build.sh and Dockerfile

- ``Build and install (make && make install)``

	- Prerequisite for installation
		- Install postgresql-devel package if you installed PostgreSQL by rpm files
		- Set the PATH environment variable to pg_config of your PostgreSQL
	
	Operations
	
	1. git clone extensions

		```
		$ git clone https://github.com/ossc-db/pg_hint_plan.git
		$ git clone https://github.com/ossc-db/pg_store_plans.git
		$ git clone https://github.com/ossc-db/pg_plan_advsr.git
		```

	2. git checkout
		
		##### for PG10
		```
			$ cd pg_hint_plan 
			$ git checkout -b PG10 origin/PG10 && git checkout $(git describe --tag)
			$ cd ../pg_store_plans 
			$ git checkout -b R1.3 origin/R1.3 && git checkout $(git describe --tag)
		```
	
		##### for PG11
		```
			$ cd pg_hint_plan 
			$ git checkout -b PG11 origin/PG11 && git checkout $(git describe --tag)
			$ cd ../pg_store_plans 
			$ git checkout -b R1.3 origin/R1.3 && git checkout $(git describe --tag)
		```
		
		##### for PG12
		```
			$ cd pg_hint_plan 
			$ git checkout -b PG12 origin/PG12 && git checkout $(git describe --tag)
			$ cd ../pg_store_plans 
			$ git checkout $(git describe --tag)
		```
		
	3. build and install 

		```
		$ cd ../pg_hint_plan 
		$ make -s && make -s install
		$ cp pg_stat_statements.c ../pg_plan_advsr/
		$ cp normalize_query.h ../pg_plan_advsr/
		
		$ cd ../pg_store_plans 
		$ make -s USE_PGXS=1 all install
		$ cp pgsp_json*.[ch] ../pg_plan_advsr/
		
		$ cd ../pg_plan_advsr/
		$ git describe --alway
		
		$ make
		$ make install
		```

	4. edit PostgreSQL.conf

		```
		$ vi $PGDATA/postgresql.conf

		---- Add this line ----------------------------------------------------------------
		shared_preload_libraries = 'pg_hint_plan, pg_plan_advsr, pg_store_plans'
		max_parallel_workers_per_gather = 0
		max_parallel_workers = 0
		-----------------------------------------------------------------------------------

		---- Consider increase these numbers (optional, these are based on your query) ----
		geqo_threshold = 12 -> XX
		from_collapse_limit = 8 -> YY
		join_collapse_limit = 8 -> ZZ
		-----------------------------------------------------------------------------------

		---- Consider decrease the number (optional, it is based on your storage) ---------
		random_page_cost = 4 -> 2 (example)
		-----------------------------------------------------------------------------------
		```

	5. run create extension commands on psql

		```
		$ pg_ctl start
		$ psql 

		create extension pg_hint_plan;
		create extension pg_store_plans;
		create extension pg_plan_advsr;
		```


	* You can try this extension with Join Order Benchmark as a example.
	See: [how_to_setup.md in JOB directory](https://github.com/ossc-db/pg_plan_advsr/blob/master/JOB/how_to_setup.md)


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

Copyright (c) 2019-2021, NIPPON TELEGRAPH AND TELEPHONE CORPORATION


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
