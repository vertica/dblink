#!/bin/bash -e
# NOTE: the -e above means any failure will cause the whole script to fail

# first chose a unique project name for docker-compose
cd "$(dirname ${BASH_SOURCE[0]})" || exit $?
source .env || exit $?

if [[ -z $VERTICA_VERSION || -z $OSTAG ]] ; then
  echo "usage: make test VERTICA_VERSION=11.1.1 OSTAG=centos" >&2
  exit 1
fi

docker-compose up -d --force-recreate
docker exec ${COMPOSE_PROJECT_NAME}_vertica_1 true || (echo docker-compose could not start ${COMPOSE_PROJECT_NAME}_vertica_1 >&2; false)

# clean on exit
trap "docker-compose down" EXIT

# install dblink
docker cp ../ldblink.so.${OSTAG}-v${VERTICA_VERSION} ${COMPOSE_PROJECT_NAME}_vertica_1:/tmp/ldblink.so

echo waiting for vertica to start
timeout=30
while ((timeout--)) && ! docker logs ${COMPOSE_PROJECT_NAME}_vertica_1 | grep -i 'Database vsdk.*succe'; do
  sleep 10;
done
if ((timeout<0)); then
  echo Timeout waiting for vertica to start
  docker logs ${COMPOSE_PROJECT_NAME}_vertica_1 
fi


docker-compose exec -T vertica vsql -X -c \
  "CREATE OR REPLACE LIBRARY dblink AS '/tmp/ldblink.so' LANGUAGE 'C++';
   CREATE OR REPLACE TRANSFORM FUNCTION dblink AS LANGUAGE 'C++' NAME 'DBLinkFactory' LIBRARY dblink ;
  GRANT EXECUTE ON TRANSFORM FUNCTION dblink() TO PUBLIC ;
  GRANT USAGE ON LIBRARY dblink TO PUBLIC ; "

# TESTS

# create data in mysql
docker-compose exec -T mysql mysql --password=password -e "drop database tpch;" >/dev/null || true
docker-compose exec -T mysql mysql db --password=password -e 'create schema tpch;' >/dev/null
docker-compose exec -T mysql mysql db --password=password -e 'create table tpch.customer (id int, name varchar(100), birthday date);' >/dev/null
docker-compose exec -T mysql mysql db --password=password -e "insert into tpch.customer values (1, 'alice', '1970-01-01');" >/dev/null
docker-compose exec -T mysql mysql db --password=password -e "insert into tpch.customer values (2, 'bob', '2022-02-02');" >/dev/null
# enough rows to split across several fetch threads, plus a few NULL split keys
rows=""
for i in $(seq 3 50); do
  rows="${rows}${rows:+,}($i, 'user$i', '2020-01-01')"
done
docker-compose exec -T mysql mysql db --password=password -e "insert into tpch.customer values $rows;" >/dev/null
docker-compose exec -T mysql mysql db --password=password -e \
  "insert into tpch.customer values (NULL, 'nullkey1', '2021-01-01'), (NULL, 'nullkey2', '2021-01-02'), (NULL, 'nullkey3', '2021-01-03');" >/dev/null
docker-compose exec -T mysql mysql db --password=password -e "GRANT ALL PRIVILEGES ON tpch.* TO 'mauro'@'%'" >/dev/null

TOTAL_ROWS=53
NULL_ROWS=3
SPLIT_QUERY="select id, name from tpch.customer"

function check_output {
  msg=$1
  printf "%-50s" "$msg..."
  output=$2
  if ! [[ $output =~ id.*alice.*bob ]]; then
    echo "FAILED"
    echo "$output"
    return 1
  fi
  echo "ok"
}

# rows as a sorted, unaligned, newline separated list so that row order does not matter
function vsql_rows {
  docker-compose exec -T vertica vsql -X -t -A -F '|' -c "$1" | tr -d '\r' | sed '/^$/d' | sort
}

function check_equal {
  msg=$1
  printf "%-50s" "$msg..."
  if [[ "$2" != "$3" ]]; then
    echo "FAILED"
    echo "expected: <$2>"
    echo "got:      <$3>"
    return 1
  fi
  echo "ok"
}

# basic tests to read mysql data in vertica using three different ways of specifying the credentials
check_output "Connecting with cid" "$(docker-compose exec -T vertica vsql -X -c \
"SELECT DBLINK(USING PARAMETERS 
  cid='mysql', 
    query='select * from tpch.customer order by id') OVER();")"

check_output "Connecting with connect_secret" "$(docker-compose exec -T vertica vsql -X -c \
"SELECT DBLINK(USING PARAMETERS 
  connect_secret='USER=mauro;PASSWORD=xxx;DSN=mmf', 
    query='select * from tpch.customer order by id') OVER();")"

check_output "Connecting with dblink_secret" "$(docker-compose exec -T vertica vsql -X -c \
"ALTER SESSION SET UDPARAMETER FOR dblink dblink_secret = 'USER=mauro;PASSWORD=xxx;DSN=mmf' ;
SELECT DBLINK(USING PARAMETERS 
    query='select * from tpch.customer order by id') OVER();")"

# parallel fetch tests
serial_rows="$(vsql_rows "SELECT DBLINK(USING PARAMETERS
  cid='mysql',
    query='$SPLIT_QUERY') OVER();")"
check_equal "Single threaded row count" "$TOTAL_ROWS" "$(printf '%s\n' "$serial_rows" | wc -l | tr -d ' ')"

parallel_rows="$(vsql_rows "SELECT DBLINK(USING PARAMETERS
  cid='mysql',
    query='$SPLIT_QUERY', num_threads=4, split_column='id') OVER();")"
check_equal "Parallel fetch matches single threaded" "$serial_rows" "$parallel_rows"

# the outer ranges are open ended, so bounds narrower than the data lose nothing
narrow_rows="$(vsql_rows "SELECT DBLINK(USING PARAMETERS
  cid='mysql',
    query='$SPLIT_QUERY', num_threads=4, split_column='id',
    split_min=10, split_max=20) OVER();")"
check_equal "Narrow bounds still return every row" "$serial_rows" "$narrow_rows"

check_equal "NULL split keys returned exactly once" "$NULL_ROWS" \
  "$(printf '%s\n' "$parallel_rows" | grep -c '^|nullkey' || true)"

degenerate_rows="$(vsql_rows "SELECT DBLINK(USING PARAMETERS
  cid='mysql',
    query='$SPLIT_QUERY', num_threads=4, split_column='id',
    split_min=25, split_max=25) OVER();")"
check_equal "Equal split bounds return every row" "$serial_rows" "$degenerate_rows"

# query_timeout is enforced with SQLCancel, so a slow statement must stop early
printf "%-50s" "query_timeout cancels a slow statement..."
start=$SECONDS
if docker-compose exec -T vertica vsql -X -c \
  "SELECT DBLINK(USING PARAMETERS
     cid='mysql',
     query='select sleep(60)', query_timeout=5) OVER();" >/dev/null 2>&1 ; then
  echo "FAILED"
  echo "the statement was expected to be cancelled by query_timeout"
  exit 1
fi
elapsed=$(( SECONDS - start ))
if (( elapsed < 3 || elapsed > 30 )); then
  echo "FAILED"
  echo "query_timeout=5 ended the statement after ${elapsed}s"
  exit 1
fi
echo "ok (${elapsed}s)"

# no errors?  Success!
