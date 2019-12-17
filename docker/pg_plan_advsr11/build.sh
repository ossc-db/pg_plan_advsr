#!/bin/sh

docker build -t test/pg_plan_advsr:11 . && \
docker run -d --name pg_plan_advsr11 test/pg_plan_advsr:11

