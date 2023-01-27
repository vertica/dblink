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


docker-compose exec vertica vsql -X -c \
  "CREATE OR REPLACE LIBRARY dblink AS '/tmp/ldblink.so' LANGUAGE 'C++';
   CREATE OR REPLACE TRANSFORM FUNCTION dblink AS LANGUAGE 'C++' NAME 'DBLinkFactory' LIBRARY dblink ;
  GRANT EXECUTE ON TRANSFORM FUNCTION dblink() TO PUBLIC ;
  GRANT USAGE ON LIBRARY dblink TO PUBLIC ; "

# TESTS

# create data in mysql
docker-compose exec mysql mysql --password=password -e "drop database tpch;" >/dev/null || true
docker-compose exec mysql mysql db --password=password -e 'create schema tpch;' >/dev/null
docker-compose exec mysql mysql db --password=password -e 'create table tpch.customer (id int, name varchar(100), birthday date);' >/dev/null
docker-compose exec mysql mysql db --password=password -e "insert into tpch.customer values (1, 'alice', '1970-01-01');" >/dev/null
docker-compose exec mysql mysql db --password=password -e "insert into tpch.customer values (2, 'bob', '2022-02-02');" >/dev/null
docker-compose exec mysql mysql db --password=password -e "GRANT ALL PRIVILEGES ON tpch.* TO 'mauro'@'%'" >/dev/null

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

# basic tests to read mysql data in vertica using three different ways of specifying the credentials
check_output "Connecting with cid" "$(docker-compose exec vertica vsql -X -c \
"SELECT DBLINK(USING PARAMETERS 
  cid='mysql', 
    query='select * from tpch.customer order by id') OVER();")"

check_output "Connecting with connect_secret" "$(docker-compose exec vertica vsql -X -c \
"SELECT DBLINK(USING PARAMETERS 
  connect_secret='USER=mauro;PASSWORD=xxx;DSN=mmf', 
    query='select * from tpch.customer order by id') OVER();")"

check_output "Connecting with dblink_secret" "$(docker-compose exec vertica vsql -X -c \
"ALTER SESSION SET UDPARAMETER FOR dblink dblink_secret = 'USER=mauro;PASSWORD=xxx;DSN=mmf' ;
SELECT DBLINK(USING PARAMETERS 
    query='select * from tpch.customer order by id') OVER();")"

# no errors?  Success!
