#!/bin/bash

psql -f all_reset.sql

for i in `seq 1 16`; do 
    echo "=== ${i} ===" && psql -f 31c_test.sql -P pager; 
done

psql -c "select id, pgsp_queryid, pgsp_planid, diff_of_joins, execution_time from plan_repo.plan_history order by id;"

