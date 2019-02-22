pg_plan_advsr/JOB
=================

Get schema and queries of JOB
-------
	git clone https://github.com/gregrahn/join-order-benchmark 


Get data (1.2GB)
----------------
	wget https://homepages.cwi.nl/~boncz/job/imdb.tgz

	mkdir imdb
	tar xvzf imdb.tgz -C ./imdb


Create schema
-------------
	psql -f ./join-order-benchmark/schema.sql


Load data and Create index
--------------------------
* Modify directrories in the sql file

	vi ./load_csv.sql
            or
    sed -i -e "s/path_to/\/ocz\/pg104/g" load_csv.sql

* Load csv files and create indexes. You can have a tea of coffee during the following commands.

	psql -f ./load_csv.sql
	psql -f ./join-order-benchmark/fkindexes.sql


Analyze tables
--------------
	psql -f ./analyze_table.sql


Auto tune
-------------------
* This script execute 31c_test.sql 16 times using psql to get an efficient plan.

	./auto_tune_31c.sh


e.g. 

* I created two files such as hinted and not hinted. The hints were a result of auto tuning on my environment. 
* Which plan is fast on your environment?

	psql -f 31c_org.sql

	psql -f 31c_hinted.sql

