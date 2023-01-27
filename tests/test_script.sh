#!/bin/bash -e

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

# set up ODBC first
# install dblink.cids
docker cp dblink.cids ${COMPOSE_PROJECT_NAME}_vertica_1:/usr/local/etc/dblink.cids
#docker-compose exec -u 0 -T vertica tee /usr/local/etc/dblink.cids < dblink.cids >/dev/null
# install odbc.ini
docker cp odbc.ini ${COMPOSE_PROJECT_NAME}_vertica_1:/etc/odbc.ini
docker cp odbcinst.ini ${COMPOSE_PROJECT_NAME}_vertica_1:/etc/odbcinst.ini
#docker-compose exec -u 0 -T vertica tee /etc/odbc.ini < odbc.ini >/dev/null

# install dblink
docker cp ../ldblink.so.${OSTAG}-v${VERTICA_VERSION} ${COMPOSE_PROJECT_NAME}_vertica_1:/tmp/ldblink.so

# install mysql drivers
docker-compose exec -u 0 vertica wget -q https://dev.mysql.com/get/Downloads/MySQL-8.0/mysql-community-client-plugins_8.0.32-1ubuntu18.04_amd64.deb
docker-compose exec -u 0 vertica wget -q https://dev.mysql.com/get/Downloads/Connector-ODBC/8.0/mysql-connector-odbc_8.0.32-1ubuntu18.04_amd64.deb
docker-compose exec -u 0 vertica dpkg -i mysql-community-client-plugins_8.0.32-1ubuntu18.04_amd64.deb mysql-connector-odbc_8.0.32-1ubuntu18.04_amd64.deb
docker-compose exec -u 0 vertica mkdir -p /usr/lib64
docker-compose exec -u 0 vertica ln -snf /usr/lib/x86_64-linux-gnu/odbc/libmyodbc8w.so /usr/lib64/libmyodbc8w.so

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
docker-compose exec mysql mysql db --password=password -e "insert into tpch.customer values (1, 'bob', '2022-02-02');" >/dev/null
docker-compose exec mysql mysql db --password=password -e "GRANT ALL PRIVILEGES ON tpch.* TO 'mauro'@'%'" >/dev/null

# read the data in vertica
output=$(docker-compose exec vertica vsql -X -c \
"SELECT DBLINK(USING PARAMETERS 
  cid='mysql', 
    query='select * from tpch.customer') OVER();")
echo "$output" | grep -- id
echo "$output" | grep -- -----
echo "$output" | grep -- alice
echo "$output" | grep -- bob

# no errors?  Success!
