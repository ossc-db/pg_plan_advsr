#! /bin/sh
cat $1 | \
sed 's/cost=[\.0-9]*/cost=xxx/;s/width=[0-9]*/width=xxx/;s/time=[\.0-9]*/time=xxx/' |\
grep -v "Buckets:" | grep -v "Planning [tT]ime:" | grep -v "Execution [tT]ime:"
