`DBLINK()` is a Vertica [User Defined Transform Function](https://www.vertica.com/docs/latest/HTML/Content/Authoring/ExtendingVertica/UDx/TransformFunctions/TransformFunctions.htm) coded in C++ to run SQL against other databases.  

For example, the following statement runs a row count in PostgreSQL and retrieves the result (6,001,215) in Vertica:

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
## Usage
`DBLINK()` is a Vertica function that pushes SQL to other databases and retrieves the result of the remote execution back in Vertica. `DBLINK()` can push any type of SQL commands that the remote database accepts:

* DDL statements. For example, `CREATE` a table in the remote database.
* DML statements to manipulate data in the remote database (`INSERT`, `UPDATE`, etc.).
* DQL statements to `SELECT` data from the remote database using the SQL dialect and functions available on the remote database.
* DCL statements like `GRANT` and `REVOKE`.

### Examples

The following statement creates a table in Vertica named `public.customer` that contains 10% of randomly selected data from the PostreSQL table `tpch.customer`: 

```sql
CREATE TABLE public.customer AS 
    SELECT DBLINK(USING PARAMETERS 
        cid='pgdb', 
        query='SELECT * FROM tpch.customer WHERE RANDOM() < 0.1') 
OVER();
```

The following statement creates an empty table in Vertica corresponding to the table definition in the remote database:

```sql
CREATE TABLE public.customer AS 
    SELECT DBLINK(USING PARAMETERS 
        cid='pgdb', 
        query='SELECT * FROM tpch.customer LIMIT 0') 
OVER();
```

This statement will group-by the result of a `JOIN` between the Vertica table `tpch.nation` and the MySQL table `tpch.region`:
 
```sql
SELECT r.r_name, count(*)
FROM tpch.nation n
    LEFT OUTER JOIN
        ( SELECT DBLINK(USING PARAMETERS
            cid='mypg’, 
            query='SELECT r_name, r_regionkey FROM tpch.region'
            ) OVER()) r
    ON n.n_regionkey = r.r_regionkey
GROUP BY 1 ;
```

The following statement drops a PostgreSQL table if exists:

```sql
SELECT DBLINK(USING PARAMETERS 
	cid='pgdb', 
	query='DROP TABLE IF EXISTS public.t1') OVER();
```

Sometimes the SQL that you want to push to the remote database is quite complex. In these cases, you might find useful to write the SQL in a file using your preferred editor, and then pass the file containing the SQL text to `DBLINK()` using the following syntax:

```sql
SELECT DBLINK(USING PARAMETERS 
	cid='mysql', 
	query='@/tmp/myscript.sql') OVER()";
```

## Installation

Install and uninstall `DBLINK()` with the repository [Makefile](Makefile).

### Prerequisites
`DBLINK()` uses ODBC to interact with the remote databases. You must install and configure the following on all nodes in your cluster:

- [ODBC Driver Manager](https://learn.microsoft.com/en-us/sql/connect/odbc/linux-mac/installing-the-driver-manager?view=sql-server-ver16) (DBLINK has been tested with unixODBC)
- Specific ODBC Drivers for the remote database that you want to connect to
- [Configure the ODBC layer](#configure-the-odbc-layer)

### Install DBLINK()

> Before you run `make` commands, review the Makefile and make changes as needed.

1. Compile the DBLINK source code for the appropriate Vertica version and Linux distribution. For example, `make VERTICA_VERSION=12.0.2 OSTAG=ubuntu`.
2. Deploy the library in Vertica as dbadmin with `make install`.
3. Create a [Connection Identifier Database](#connection-identifier-database) (a simple text file) under `/usr/local/etc/dblink.cids`. You can use a different location by changing the `DBLINK_CIDS` define in the source code. 


### Uninstall DBLINK()
You can uninstall the library with `make clean`.

## Using DBLINK()

`DBLINK()` requires two parameters and accepts one optional parameter with the following syntax:

```sql
DBLINK(USING PARAMETERS cid=value, query=value[, rowset=value]);
```
#### Parameters


| Name     | Required | Description  |
|----------------|----------|--------------|
| `cid`    | Yes      | [Connection Identifier Database](#connection-identifier-database). Identifies an entry in the connection identifier database.  |
| `query`  | Yes      | The query being pushed on the remote database. If the first character of this parameter is `@`, the rest is interpreted as the name of the file containing the query. |
| `rowset` | No      | Number of rows retrieved from the remote database during each SQLFetch() cycle. Default is 100. |

For example, the following query retrieves data from the remote database 500 rows at a time:

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
...
```
#### Connection Identifier Database

The Connection Identifier Database is a simple text file containing the codes used with ```cid```. This is an example:

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

## Configure the ODBC Layer

You must install and configure the ODBC layer on all nodes for each database that you want to connect to.

The ODBC configuration depends on the specific ODBC Driver Manager and Database ODBC Drivers. The following is an example configuration file that configures unixODBC and the the PostgreSQL/MySQL drivers:

```shell
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

## Report an issue

To report an issue, provide following information:

- Command that you ran and the associated output as shown on your screen by using the standard Vertica SQL client `vsql`.
- Vertica version: `SELECT VERSION();`.
- `DBLINK` library metadata by running statement the following as dbadmin:
   ```
   => SELECT * FROM USER_LIBRARIES WHERE lib_name = 'ldblink';
   ```
- Attach the ODBC the following configuration files:
	- `odbc.ini` (please remove passwords or other confidential information)
	- `odbcinst.ini`
- ODBC Driver Manager version and config. For example, with unixODBC, the output of the command `odbcinst -j`.
- ODBC traces obtained while running the command (see 1.). To enable the ODBC traces you have to set `Trace = on` in `odbcinst.ini`. Do not forget to switch ODBC tracing off at the end.
