#!/bin/bash

psql -f all_reset.sql

for i in `seq 1 17`; do 
    echo "=== ${i} ===" && psql -f 31c_test.sql -P pager; 
done

psql -c "select id, pgsp_queryid, pgsp_planid, join_rows_err, execution_time::numeric(8, 3) from plan_repo.plan_history order by id;"

