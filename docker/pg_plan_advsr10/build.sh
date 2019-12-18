#!/bin/sh

docker build -t test/pg_plan_advsr:10 . && \
docker run -d --name pg_plan_advsr10 test/pg_plan_advsr:10

