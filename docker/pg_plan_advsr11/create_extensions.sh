#!/bin/sh

# Edit postgresql.conf
echo "shared_preload_libraries = 'pg_hint_plan, pg_plan_advsr, pg_store_plans'" >> /var/lib/postgresql/data/postgresql.conf
echo "max_parallel_workers_per_gather = 0" >> /var/lib/postgresql/data/postgresql.conf
echo "max_parallel_workers = 0" >> /var/lib/postgresql/data/postgresql.conf
echo "geqo_threshold = 20" >> /var/lib/postgresql/data/postgresql.conf
echo "from_collapse_limit = 16" >> /var/lib/postgresql/data/postgresql.conf
echo "join_collapse_limit = 16" >> /var/lib/postgresql/data/postgresql.conf
echo "random_page_cost = 2" >> /var/lib/postgresql/data/postgresql.conf

# Create extensons
psql -c "create extension pg_hint_plan;"
psql -c "create extension pg_store_plans;"
psql -c "create extension pg_plan_advsr;"

