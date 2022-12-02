## What is DBLINK().
DBLINK() is a Vertica User Defined Transform Function coded in C++ to run SQL against other databases.  

For example, the following will run a row count in PostgreSQL and bring the result (6,001,215) in Vertica:

```sql
SELECT DBLINK(USING PARAMETERS
    cid='pgdb', 
    query='SELECT COUNT(*) FROM tpch.lineitem'
) OVER();
count
--------
6,001,215
--- 1 row selected in 0.228s (prep 0.17s, exec 0.211s, fetch 0.000/0.000s)
```
## What DBLINK() can be used for.
DBLINK() is a Vertica function to **push SQL** to other databases and retrieve the result of the remote execution back in Vertica. DBLINK() can push **any type of SQL commands accepted by the remote database**:

* DDL statements (for example to CREATE a table in the remote database)
* DML statements to manipulate data in the remote database (INSERT, UPDATE, etc)
* DQL statements to SELECT data from the remote database using the SQL dialect and functions available on the remote database
* DCL statements like GRANT/REVOKE

Here you have a few examples...

The following statement will create in Vertica a table ```public.customer``` containing 10% of randomly selected data from the PostreSQL table ```tpch.customer```: 

```sql
CREATE TABLE public.customer AS 
    SELECT dblink(USING PARAMETERS 
        cid='pgdb', 
        query='SELECT * FROM tpch.customer WHERE RANDOM() < 0.1') 
OVER();
```

The following will create an empty table in Vertica corresponding to the table definition in the remote database:

```sql
CREATE TABLE public.customer AS 
    SELECT dblink(USING PARAMETERS 
        cid='pgdb', 
        query='SELECT * FROM tpch.customer LIMIT 0') 
OVER();
```

This other one will group-by the result of a join between the Vertica table ```tpch.nation``` and the MySQL table ```tpch.region```:
 
```sql
SELECT r.r_name, count(*)
FROM tpch.nation n
    LEFT OUTER JOIN
        ( SELECT dblink(USING PARAMETERS
            cid='mypg’, 
            query='SELECT r_name, r_regionkey FROM tpch.region'
            ) OVER()) r
    ON n.n_regionkey = r.r_regionkey
GROUP BY 1 ;
```

The following will just drop a PostgreSQL table if exists:

```sql
SELECT dblink(USING PARAMETERS 
	cid='pgdb', 
	query='DROP TABLE IF EXISTS public.t1') OVER();
```

Sometimes the SQL you want to *push* to the remote database can be quite complex. In these cases you might find useful to write the SQL in a file using your preferred editor and pass the file containing the SQL text to ```DBLINK()``` using the following syntax:

```sql
SELECT dblink(USING PARAMETERS 
	cid='mysql', 
	query='@/tmp/myscript.sql') OVER()";
```

## How to install/uninstall DBLINK()
### Prerequisites
DBLINK() uses ODBC to interact with the remote databases so you will have to install and configure on all nodes of your cluster:

- an ODBC Driver Manager (DBLINK has been tested with unixODBC)
- the specific ODBC Drivers for the remote databases you want to connect to
- configure the ODBC layer (see the examples here below)

### Install DBLINK()
- First you need to [Setup a C++ Development Environment](https://www.vertica.com/docs/11.1.x/HTML/Content/Authoring/ExtendingVertica/C++/C++SDK.htm) 
- Then you will have to compile the DBLINK source code: ```make```
- Deploy the library in Vertica (as dbadmin): ```make deploy```
- Create a "Connection Identifier Database" (a simple text file) under ```/usr/local/etc/dblink.cids``` (see "How to configure DBLINK() here below"). You can use a different location by changing the ```DBLINK_CIDS``` define in the source code. 

Please have a look to the Makefile before running ```make``` and change it if needed. 

### Uninstall DBLINK()
You can uninstall the library with: ```make clean```

## How to configure DBLINK()
DBLINK() uses two mandatory parameters:

- ```cid``` (Connection Identifier) identify an entry in the connection identifier database
- ```query``` is the query being pushed on the remote database. If the first character of this parameter is ```@``` the rest is interpreted as the name of the file containing the query

and one optional parameter:
- ```rowset``` is the number of rows retrieved from the remote database during each SQLFetch() cycle. Default is 100.

So, for example, the following query will retrieve data from the remote database 500 rows at a time:

```sql
SELECT DBLINK(USING PARAMETERS 
    cid='pgdb', 
    query='SELECT c_custkey, c_nationkey, c_phone FROM tpch.customer ORDER BY 1', 
    rowset=500) OVER();
 c_custkey | c_nationkey |     c_phone     
-----------+-------------+-----------------
         1 |          15 | 25-989-741-2988
         2 |          13 | 23-768-687-3665
         3 |           1 | 11-719-748-3364
         4 |           4 | 14-128-190-5944
         5 |           3 | 13-750-942-6364
         6 |          20 | 30-114-968-4951
         7 |          18 | 28-190-982-9759
etc...
```

The "Connection Identifier Database" is a simple text file containing the codes used with ```cid```. This is an example:

```
$ cat dblink.cids 
# Vertica DBLINK Configuration File
#
# Connection IDs lines have the following format:
#    <mnemonic code>:<ODBC configuration>
# and are terminated by a SINGLE '\n' (ASCII dec 10, ASCII hex 0x0a) 
# Be aware of this! Windows editors might end lines with \r\n. In
# this case the Carriage Return is considered part of the ODBC config
# and can cause undefined ODBC Driver Behavior.
#
# Lines starting with '#' are considered comments
#
# Sample configuration:

pgdb:UID=mauro;PWD=xxx;DSN=pmf
myver:UID=mauro;PWD=xxx;DSN=vmf
mysql:USER=mauro;PASSWORD=xxx;DSN=mmf
```

## How to configure the ODBC Layer

As we said (see **Prerequisites** here above) you will have to install and configure the ODBC layer on all nodes for each database you want to connect to.

The ODBC configuration depends on the specific ODBC Driver Manager and Database ODBC Drivers. Here you have the configuration files to configure unixODBC the the PostgreSQL/MySQL drivers:

```
$ cat /etc/odbc.ini
[ODBC Data Sources]
PSQLODBC  = PostgreSQL ODBC
MYODBC  = MySQL ODBC

[pmf]
Description  = PostgreSQL mftest
Driver = PSQLODBC
Trace  = No
TraceFile  = sql.log
Database = pmf
Servername = mftest
UserName =
Password =
Port = 5432
SSLmode  = allow
ReadOnly = 0
Protocol = 7.4-1
FakeOidIndex = 0
ShowOidColumn  = 0
RowVersioning  = 0
ShowSystemTables = 0
ConnSettings =
Fetch  = 1000
Socket = 4096
UnknownSizes = 0
MaxVarcharSize = 1024
MaxLongVarcharSize = 8190
Debug  = 0
CommLog  = 0
Optimizer  = 0
Ksqo = 0
UseDeclareFetch  = 0
TextAsLongVarchar  = 1
UnknownsAsLongVarchar  = 0
BoolsAsChar  = 1
Parse  = 0
CancelAsFreeStmt = 0
ExtraSysTablePrefixes  = dd_
LFConversion = 0
UpdatableCursors = 0
DisallowPremature  = 0
TrueIsMinus1 = 0
BI = 0
ByteaAsLongVarBinary = 0
LowerCaseIdentifier  = 0
GssAuthUseGSS  = 0
XaOpt  = 1
UseServerSidePrepare = 0

[mmf]
Description  = MySQL mftest
Driver = MYODBC
SERVER = mftest
PORT = 3306
SQL-MODE = 'ANSI_QUOTES'

$ cat /etc/odbcinst.ini
[ODBC]
Trace=off
Tracefile=/tmp/uodbc.trc

[PSQLODBC]
Description=PostgreSQL ODBC Driver
Driver64=/usr/lib64/psqlodbcw.so
UsageCount=1

[MYODBC]
Driver=/usr/lib64/libmyodbc8w.so
UsageCount=1

[MySQL ODBC 8.0 ANSI Driver]
Driver=/usr/lib64/libmyodbc8a.so
UsageCount=1
```

## How to report an issue
Please report issues as follows:

1. Share the command you ran and the associated output as shown on your screen by using the standard Vertica SQL client ``vsql``
2. Share the Vertica version: ``SELECT VERSION();``
3. Share the DBLINK library metadata by running (as dbadmin) ``SELECT * FROM USER_LIBRARIES WHERE lib_name = 'ldblink';``
4. Attach the ODBC configuration files:
	- ``odbc.ini`` (please remove passwords or other confidential information)
	- ``odbcinst.ini``
5. Share the ODBC Driver Manager version and config. For example, with unixODBC, the output of the command ``odbcinst -j``
6. Share the ODBC traces obtained while running the command (see 1.). To enable the ODBC traces you have to set ``Trace = on`` in ``odbcinst.ini``. Do not forget to switch ODBC tracing off at the end...
