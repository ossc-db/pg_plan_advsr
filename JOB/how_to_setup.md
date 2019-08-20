pg_plan_advsr/JOB
=================

You can create your verification environment for pg_plan_advsr by using the following seven procedures.

1 Get schema and queries of JOB
-------------------------------
	git clone https://github.com/gregrahn/join-order-benchmark 


2 Get dataset (1.2GB)
---------------------
	wget https://homepages.cwi.nl/~boncz/job/imdb.tgz

	mkdir imdb
	tar xvzf imdb.tgz -C ./imdb


3 Create schema
---------------
	psql -f ./join-order-benchmark/schema.sql


4 Load dataset and Create indexes
---------------------------------
* Modify directrories in the following sql file to fit your environment
	
		vi ./load_csv.sql
	            or
		sed -i -e "s/path_to\/imdb/ocz\/pg104\/imdb/g" ./load_csv.sql

* Load csv files and create indexes. You can have a tea or coffee during the following commands because it takes a time.

		psql -f ./load_csv.sql
		psql -f ./join-order-benchmark/fkindexes.sql


5 Analyze tables
----------------
	psql -f ./analyze_table.sql


6 Modify parameters according to planner
----------------------------------------
* See parameters: [6 Install in README.md](https://github.com/ossc-db/pg_plan_advsr/#6-installation)


7 Try Auto tune (example)
-------------------------
* This script execute 31c_test.sql 16 times using psql to get an efficient plan as a example. You can use other queries from the directory "./join-order-benchmark/" and the modfy the shell script.

		./auto_tune_31c.sh

* You can check "plan and execution time changes" of the query by using following queries:

		select pgsp_queryid, pgsp_planid, execution_time, scan_hint, join_hint, lead_hint from plan_repo.plan_history order by id;
		select queryid, planid, plan from pg_store_plans where queryid='your pgsp_queryid in plan_history' order by first_call;

See: [4 Usage in README.md](https://github.com/ossc-db/pg_plan_advsr/#4-usage)


Result of Auto tune on my environment
-------------------------------------
* I share two sql files such as hinted and not hinted. The hint (optimizer hint) were a result of auto tuning, and it allows to ideal plan on my environment. 
* Note: You have to disable feedback before running above sql files like this:

		select pg_plan_advsr_disable_feedback();

* Which plan is fast on your environment?

		psql -f 31c_org.sql

		psql -f 31c_hinted.sql

* My result is below, the hinted sql file is over 30 times faster than not hinted sql file. :)

 31c_org.sql

 Iterations         |       1st |       2nd |       3rd|
 -------------------|-----------|-----------|----------|
 Planning time (ms) |   269.044 |   238.506 |   263.925|
 Execution time (ms)| 76372.644 | 68143.267 | 68784.149|

 31c_hinted.sql

 Iterations         |       1st |       2nd |       3rd|
 -------------------|-----------|-----------|----------|
 Planning time (ms) |   142.690 |   143.255 |   141.759|
 Execution time (ms)|  2171.253 |  2178.805 |  2132.115|


Enjoy! :)
