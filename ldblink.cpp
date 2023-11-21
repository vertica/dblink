// vim:ru:scs:si:sm:sw=4:sta:ts=4:tw=0

// (c) Copyright [2022-2023] Micro Focus or one of its affiliates.
// Licensed under the Apache License, Version 2.0 (the "License");
// You may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "Vertica.h"
#include "StringParsers.h"

using namespace Vertica;
using namespace std;

#include <sql.h>
#include <time.h>
#include <sqlext.h>
#include <iostream>
#include <fstream>
  
#define DBLINK_CIDS			"/usr/local/etc/dblink.cids"	// Default Connection identifiers config file FIX: add a param
#define MAXCNAMELEN			128								// Max column name length
#define DEF_ROWSET 			100								// Default rowset
#define MAX_ROWSET 			1000							// Default rowset
#define MAX_NUMERIC_CHARLEN 128								// Max NUMERIC size in characters
#define MAX_ODBC_ERROR_LEN  1024							// Max ODBC Error Length

enum DBs {
    GENERIC = 0,
    POSTGRES,
    VERTICA,
    SQLSERVER,
    TERADATA,
    ORACLE,
    MYSQL
};

class ODBCBase
{
public:
    SQLHENV Oenv = 0 ;				// ODBC Environment handle
    SQLHDBC Ocon = 0 ;				// ODBC Connection handle
    SQLHSTMT Ost = 0 ;				// ODBC Statement handle
    std::string cid_value = "" ;    // CID for ODBC connection
    bool is_select = false ;		// Command is a SELECT
    std::string query = "" ;		// set in Factory/getReturnType, used (non-DQL) in processPartition
    SQLUSMALLINT Oncol = 0 ;		// Number of result set columns
    SQLSMALLINT *Odt = 0 ;			// Result set Data Type Array pointer
    SQLSMALLINT *Odd = 0 ;			// Result set Decimals Array pointer
    SQLULEN *Ors = 0 ;				// Result Set Column size pointer
    size_t *desz = 0 ;				// Data Element Size Array pointer
    SQLULEN nfr ;					// Number of fetched rows
    SizedColumnTypes colInfo ;		// Set in getReturnType factory, used in processPartition

    // Preparation for database connection
    void setupConnection(ServerInterface &srvInterface) {
        std::string cid = "" ;
        std::string cid_env = "" ;
        std::string cid_file = DBLINK_CIDS ;
        std::string cid_name = "" ;
        bool connect = false ;

        if (query.length() == 0) {
            // Read Params:
            ParamReader params = srvInterface.getParamReader();
            if (params.containsParameter("cidfile")) {			// Start checking "cidfile" param
                cid_file = params.getStringRef("cidfile").str() ;
#ifdef DBLINK_DEBUG
    srvInterface.log("DEBUG ODBCBase read param cidfile=<%s>", cid_file.c_str() );
#endif
            }

            if (params.containsParameter("cid")) {				// Start checking "cid" param
                cid = params.getStringRef("cid").str() ;
#ifdef DBLINK_DEBUG
    srvInterface.log("DEBUG ODBCBase read param cid=<%s>", cid.c_str() );
#endif
            } else if (params.containsParameter("connect_secret")) {	// if "cid" is undef try with "connect_secret"
                connect = true ;
                cid = params.getStringRef("connect_secret").str() ;
            } else if (params.containsParameter("connect")) {	// if "cid" is undef try with "connect"
                connect = true ;
                cid = params.getStringRef("connect").str() ;
            } else if (srvInterface.getUDSessionParamReader("library").containsParameter("dblink_secret")) {
                // if "cid", "connect_secret" and "connect" are not defined try "dblink_secret" session param
                connect = true ;
                cid = srvInterface.getUDSessionParamReader("library").getStringRef("dblink_secret").str() ;
            } else {
                vt_report_error(101, "DBLINK. Missing connection parameters");
            }

            if (params.containsParameter("query")) {
                query = params.getStringRef("query").str() ;
#ifdef DBLINK_DEBUG
    srvInterface.log("DEBUG ODBCBase read param query=<%s>", query.c_str() );
#endif
            } else {
                vt_report_error(102, "DBLINK. Missing query parameter");
            }

            // Check connection parameters
            if (connect) {    // old VFQ connect style: connect='@/tmp/file.txt' will read CIDs from a different file
                if (cid[0] == '@') {
                    std::ifstream cids(cid.substr(1)) ;
                    if (cids.is_open()) {
                        std::stringstream ssFile;
                        ssFile << cids.rdbuf() ;
                        cid_value = ssFile.str() ;
                        cid_value.erase(std::remove(cid_value.begin(), cid_value.end(), '\n'), cid_value.end());
                    } else {
                        vt_report_error(103, "DBLINK. Error reading <%s>", cid.substr(1).c_str());
                    }
                } else {
                    cid_value = cid ;
                }
            } else {          // new CID connect style:
                std::ifstream cids(cid_file) ;
                if (cids.is_open()) {
                    std::string cline ;
                    size_t pos ;
                    while (getline(cids, cline)) {
                        if (cline[0] == '#' || cline.empty())
                            continue ;	// skip empty lines & comments
                        if ((pos = cline.find(":")) != std::string::npos) {
                            cid_name = cline.substr(0, pos) ;
                            if (cid_name == cid)
                                cid_value = cline.substr(pos + 1, std::string::npos) ;
                            else if (cid_name == cid + "$") {
                                cid_env = cline.substr(pos + 1, std::string::npos) ;
                                std::stringstream se_stream (cid_env) ; 
                                std::string token ;
                                while (std::getline(se_stream, token, ';')) { 
                                    size_t pos = 0 ; 
                                    if ( ( pos = token.find('=') ) && pos != std::string::npos ) { 
#ifdef DBLINK_DEBUG
    srvInterface.log("DEBUG ODBCBase setting <%s> to <%s>", token.substr(0, pos).c_str(), token.substr(pos+1).c_str() );
#endif
                                        setenv ( token.substr(0, pos).c_str(), token.substr(pos + 1).c_str(), 1); 
                                    }   
                                }   
                            }
                        } else {
                            continue ;	// skip malformed lines
                        }
                    }
                    cids.close() ;
                } else {
                    vt_report_error(104, "DBLINK. Error reading <%s>", cid_file);
                }
                if ( cid_value.empty() ) {
                    vt_report_error(105, "DBLINK. Error finding CID <%s> in <%s>", cid.c_str(), DBLINK_CIDS);
                }
            }

            // Check if "query" is a script file name:
            if ( query[0] == '@' ) {
                std::ifstream qscript(query.substr(1)) ;
                if ( qscript.is_open() ) {
                    std::stringstream ssFile;
                    ssFile << qscript.rdbuf() ;
                    query = ssFile.str() ;
                } else {
                    vt_report_error(106, "DBLINK. Error reading query from <%s>", query.substr(1).c_str());
                }
            }

            // Determine Statement type:
            query.erase(0, query.find_first_not_of(" \n\t\r")) ;
            if ( !strncasecmp(query.c_str(), "SELECT", 6) ) {
                is_select = true ;
            }
        }
    }

    // Connect to database
    void connect() {
        SQLRETURN Oret = 0 ;

        if (!Ocon) {
            if (!SQL_SUCCEEDED(Oret=SQLAllocHandle(SQL_HANDLE_ENV, (SQLHANDLE)SQL_NULL_HANDLE, &Oenv))){
                ex_err(0, 0, 107, "Error allocating Environment Handle");
            }
            if (!SQL_SUCCEEDED(Oret=SQLSetEnvAttr(Oenv, SQL_ATTR_ODBC_VERSION, (void *) SQL_OV_ODBC3, SQL_IS_UINTEGER))){
                ex_err(0, 0, 108, "Error setting SQL_OV_ODBC3");
            }
            if (!SQL_SUCCEEDED(Oret=SQLAllocHandle(SQL_HANDLE_DBC, Oenv, &Ocon))){
                ex_err(0, 0, 109, "Error allocating Connection Handle");
            }
            if (!SQL_SUCCEEDED(Oret=SQLDriverConnect(Ocon, (SQLHWND)NULL, (SQLCHAR *)cid_value.c_str(), SQL_NTS, NULL, 0, NULL, SQL_DRIVER_NOPROMPT))){
                ex_err(SQL_HANDLE_DBC, Ocon, 110, "Error connecting to target database");
            }
        }
    }

    // Free allocated resources and disconnect from database
    void clean() {
        if ( Odt ) {
            free(Odt) ;
            Odt = 0 ;
        }
        if ( Odd ) {
            free(Odd) ;
            Odd = 0 ;
        }
        if ( Ors ) {
            free(Ors);
            Ors = 0 ;
        }
        if ( desz ) {
            free(desz);
            desz = 0 ;
        }
        if ( Ost ) {
            (void)SQLFreeHandle(SQL_HANDLE_STMT, Ost);
            Ost = 0 ;
        }
        if ( Ocon ) {
            (void)SQLDisconnect(Ocon);
            (void)SQLFreeHandle(SQL_HANDLE_DBC, Ocon);
            Ocon = 0 ;
        }
        if ( Oenv ) {
            (void)SQLFreeHandle(SQL_HANDLE_ENV, Oenv);
            Oenv = 0 ;
        }

        query = "";
    }

    // Show database error messages and disconnect from database
    void ex_err ( SQLSMALLINT htype, SQLHANDLE Oh, int loc , const char *vtext ) {
        SQLCHAR Oerr_state[6] ;					// ODBC Error State
        SQLINTEGER Oerr_native = 0 ;			// ODBC Error Native Code
        SQLCHAR Oerr_text[MAX_ODBC_ERROR_LEN] ;	// ODBC Error Text
        SQLRETURN Oret = 0 ;
        SQLSMALLINT Oln = 0 ;

        Oerr_state[0] = Oerr_text[0] = '\0' ;

        if ( htype == 0 ) {
            vt_report_error(loc, "DBLINK. %s", vtext);
        } else if ( (Oret = SQLGetDiagRec(htype, Oh, 1, Oerr_state, &Oerr_native, Oerr_text, (SQLSMALLINT)MAX_ODBC_ERROR_LEN, &Oln)) != SQL_SUCCESS ) {
            clean();
            vt_report_error(loc, "DBLINK. %s. Unable to display ODBC error message", vtext);
        } else {
            clean();
            vt_report_error(loc, "DBLINK. %s. State %s. Native Code %d. Error text: %s%c",
                vtext, (char *)Oerr_state, (int)Oerr_native, (char *) Oerr_text, ( Oln > MAX_ODBC_ERROR_LEN ) ? '>' : '.' ) ;
        }
    }
};

class DBLink : public TransformFunction, ODBCBase
{
    DBs dbt ;
    size_t rowset ;  // Fetch rowset

    virtual void setup(ServerInterface &srvInterface, const SizedColumnTypes &argTypes) {
        SQLRETURN Oret = 0 ;
        SQLHSTMT Ostmt = 0 ;
        SQLCHAR Obuff[64];

        setupConnection(srvInterface);  // Prepare to connect
        connect();                      // Connect to database

        memset(&Obuff[0], 0, sizeof(Obuff));

        // Check the DBMS we are connecting to:
        if (!SQL_SUCCEEDED(Oret = SQLAllocHandle(SQL_HANDLE_STMT, Ocon, &Ostmt))) {
            ex_err(SQL_HANDLE_DBC, Ocon, 201, "Error allocating Statement Handle");
        }
        if (!SQL_SUCCEEDED(Oret = SQLGetInfo(Ocon, SQL_DBMS_NAME, (SQLPOINTER)Obuff, (SQLSMALLINT)sizeof(Obuff), NULL))) {
            ex_err(SQL_HANDLE_DBC, Ocon, 202, "Error getting remote DBMS Name");
        }
        if ( !strcmp((char *)Obuff, "Oracle") ) {
            dbt = ORACLE ;
        } else {
            dbt = GENERIC ;
        }
        (void)SQLFreeHandle(SQL_HANDLE_STMT, Ostmt);

        // Read/Set rowset Param:
        ParamReader params = srvInterface.getParamReader();
        if( params.containsParameter("rowset") ) {
            vint rowset_param = params.getIntRef("rowset") ;
            if ( rowset_param < 1 || rowset_param > MAX_ROWSET ) {
                ex_err(0, 0, 203, "DBLINK. Error rowset out of range");
            } else {
                rowset = (size_t) rowset_param ;
            }
        } else {
            rowset = DEF_ROWSET ;
        }
    }

    virtual void cancel(ServerInterface &srvInterface)
    {
        SQLRETURN Oret = 0 ;
        if ( Ost ) {
            if (!SQL_SUCCEEDED(Oret = SQLCancel(Ost)))
                ex_err(SQL_HANDLE_STMT, Ost, 301, "Error canceling SQL statement");
        }
        clean() ;
    }

    virtual void destroy(ServerInterface &srvInterface, const SizedColumnTypes &argTypes)
    {
        clean() ;
    }

    virtual void processPartition(ServerInterface &srvInterface,
                                  PartitionReader & inputReader,
                                  PartitionWriter & outputWriter)
    {
        SQLRETURN Oret = 0 ;
        SQLPOINTER Odp = 0 ; // Data Element Pointer
        SQLULEN    Odl = 0 ; // Data Element Length
        SQLPOINTER *Ores ;   // result array pointers pointer
        SQLLEN **Olen ;      // length array pointers pointer
        SQLSMALLINT Onamel = 0 ;
        SQLSMALLINT Onull  = 0 ;
        SQLCHAR Ocname[MAXCNAMELEN] ;
        StringParsers parser ;

        // Get columns list set in Factory
        colInfo = outputWriter.getTypeMetaData();

        try
        {
            // ODBC Statement preparation:
            if (!SQL_SUCCEEDED(Oret = SQLAllocHandle(SQL_HANDLE_STMT, Ocon, &Ost))) {
                ex_err(SQL_HANDLE_DBC, Ocon, 401, "Error allocating Statement Handle");
            }

            if ( is_select ) {
                if (!SQL_SUCCEEDED(Oret = SQLPrepare(Ost, (SQLCHAR *)query.c_str(), SQL_NTS))) {
                    ex_err(SQL_HANDLE_STMT, Ost, 402, "Error preparing the statement");
                }
                if (!SQL_SUCCEEDED(Oret = SQLNumResultCols(Ost, (SQLSMALLINT *)&Oncol))) {
                    ex_err(SQL_HANDLE_STMT, Ost, 403, "Error finding the number of resulting columns");
                }
                if ( (Odt = (SQLSMALLINT *)calloc ((size_t)Oncol, sizeof(SQLSMALLINT))) == (void *)NULL ) {
                    ex_err(0, 0, 404, "Error allocating data types array");
                }
                if ( (Ors = (SQLULEN *)calloc ((size_t)Oncol, sizeof(SQLULEN))) == (void *)NULL ) {
                    ex_err(0, 0, 405, "Error allocating result set columns size array");
                }
                if ( (desz = (size_t *)calloc ((size_t)Oncol, sizeof(size_t))) == (void *)NULL ) {
                    ex_err(0, 0, 406, "Error allocating data element size array");
                }
                if ( (Odd = (SQLSMALLINT *)calloc ((size_t)Oncol, sizeof(SQLSMALLINT))) == (void *)NULL ) {
                    ex_err(0, 0, 407, "Error allocating result set decimal size array");
                }

                // Allocate memory for Result Set and length array pointers:
                Ores = (SQLPOINTER *)srvInterface.allocator->alloc(Oncol * sizeof(SQLPOINTER)) ;
                Olen = (SQLLEN **)srvInterface.allocator->alloc(Oncol * sizeof(SQLLEN *)) ;

                // Allocate space for each column, check data type, and bind it:
                for ( unsigned int j = 0 ; j < Oncol ; j++ ) {
                    SQLLEN Ool = 0 ;
                    if ( !SQL_SUCCEEDED(Oret = SQLDescribeCol(Ost, (SQLUSMALLINT)(j + 1), Ocname, (SQLSMALLINT)MAXCNAMELEN, &Onamel, &Odt[j], &Ors[j], &Odd[j], &Onull))) {
                        ex_err(SQL_HANDLE_STMT, Ost, 120, "Error getting column description");
                    }
                    Olen[j] = (SQLLEN *)srvInterface.allocator->alloc(sizeof(SQLLEN) * rowset);
#ifdef DBLINK_DEBUG
  srvInterface.log("DEBUG DBLink SQLDescribeCol src column=%u name=%s data_type=%d length=%zu", j, (char *)Ocname, Odt[j], Ors[j]);
#endif

                    switch(Odt[j]) {
                        case SQL_SMALLINT:
                        case SQL_INTEGER:
                        case SQL_TINYINT:
                        case SQL_BIGINT:
                            if (colInfo.getColumnType(j).getTypeOid() != Int8OID)
                                ex_err(0, 0, 410, "Mismatch data type");
                            if ( dbt == ORACLE )
                                desz[j] = (size_t)(Ors[j] + 1) ;
                            Ores[j] = (SQLPOINTER)srvInterface.allocator->alloc(desz[j] * rowset);
                            if (!SQL_SUCCEEDED(Oret = SQLBindCol(Ost, j + 1, (dbt == ORACLE) ? SQL_C_CHAR : SQL_C_SBIGINT, Ores[j], desz[j], Olen[j])))
                                ex_err(SQL_HANDLE_STMT, Ost, 411, "Error binding column");
                            break ;
                        case SQL_REAL:
                        case SQL_DOUBLE:
                        case SQL_FLOAT:
                            if (colInfo.getColumnType(j).getTypeOid() != Float8OID)
                                ex_err(0, 0, 410, "Mismatch data type");
                            Ores[j] = (SQLPOINTER)srvInterface.allocator->alloc(desz[j] * rowset);
                            if (!SQL_SUCCEEDED(Oret = SQLBindCol(Ost, j + 1, SQL_C_DOUBLE, Ores[j], desz[j], Olen[j])))
                                ex_err(SQL_HANDLE_STMT, Ost, 411, "Error binding column");
                            break ;
                        case SQL_NUMERIC:
                        case SQL_DECIMAL:
                            if (colInfo.getColumnType(j).getTypeOid() != NumericOID || colInfo.getColumnType(j).getNumericPrecision() != (int32)Ors[j] || colInfo.getColumnType(j).getNumericScale() != (int32)Odd[j])
                                ex_err(0, 0, 410, "Mismatch data type");
                            desz[j] = (size_t)(Ors[j] + 1);
                            Ores[j] = (SQLPOINTER)srvInterface.allocator->alloc(desz[j] * rowset);
                            if (!SQL_SUCCEEDED(Oret = SQLBindCol(Ost, j + 1, SQL_C_CHAR, Ores[j], desz[j], Olen[j])))
                                ex_err(SQL_HANDLE_STMT, Ost, 411, "Error binding column");
                            break ;
                        case SQL_CHAR:
                        case SQL_WCHAR:
                            if (!SQL_SUCCEEDED(Oret = SQLColAttribute(Ost, (SQLUSMALLINT)(j + 1), SQL_DESC_OCTET_LENGTH, (SQLPOINTER)NULL, (SQLSMALLINT)0, (SQLSMALLINT *)NULL, &Ool)))
                                ex_err(SQL_HANDLE_STMT, Ost, 120, "Error getting column description");
                            if (Ool > 0 && (SQLULEN)Ool > Ors[j])
                                Ors[j] = Ool;
                            if (Ors[j] > 65000)
                                Ors[j] = 65000;
                            desz[j] = (size_t)(Ors[j] + 1);
                            if (!Ors[j])
                                Ors[j] = 1;
                            if (colInfo.getColumnType(j).getTypeOid() != CharOID || colInfo.getColumnType(j).getStringLength() != (int32)Ors[j])
                                ex_err(0, 0, 410, "Mismatch data type");
                            Ores[j] = (SQLPOINTER)srvInterface.allocator->alloc(desz[j] * rowset);
                            if (!SQL_SUCCEEDED(Oret = SQLBindCol(Ost, j + 1, SQL_C_CHAR, Ores[j], desz[j], Olen[j])))
                                ex_err(SQL_HANDLE_STMT, Ost, 411, "Error binding column");
                            break ;
                        case SQL_VARCHAR:
                        case SQL_WVARCHAR:
                            if (!SQL_SUCCEEDED(Oret = SQLColAttribute(Ost, (SQLUSMALLINT)(j + 1), SQL_DESC_OCTET_LENGTH, (SQLPOINTER)NULL, (SQLSMALLINT)0, (SQLSMALLINT *)NULL, &Ool)))
                                ex_err(SQL_HANDLE_STMT, Ost, 120, "Error getting column description");
                            if (Ool > 0 && (SQLULEN)Ool > Ors[j])
                                Ors[j] = Ool;
                            if (Ors[j] > 65000)
                                Ors[j] = 65000;
                            desz[j] = (size_t)(Ors[j] + 1);
                            if (!Ors[j])
                                Ors[j] = 1;
                            if (colInfo.getColumnType(j).getTypeOid() != VarcharOID || colInfo.getColumnType(j).getStringLength() != (int32)Ors[j])
                                ex_err(0, 0, 410, "Mismatch data type");
                            Ores[j] = (SQLPOINTER)srvInterface.allocator->alloc(desz[j] * rowset);
                            if (!SQL_SUCCEEDED(Oret = SQLBindCol(Ost, j + 1, SQL_C_CHAR, Ores[j], desz[j], Olen[j])))
                                ex_err(SQL_HANDLE_STMT, Ost, 411, "Error binding column");
                            break ;
                        case SQL_LONGVARCHAR:
                        case SQL_WLONGVARCHAR:
                            if (!SQL_SUCCEEDED(Oret = SQLColAttribute(Ost, (SQLUSMALLINT)(j + 1), SQL_DESC_OCTET_LENGTH, (SQLPOINTER)NULL, (SQLSMALLINT)0, (SQLSMALLINT *)NULL, &Ool)))
                                ex_err(SQL_HANDLE_STMT, Ost, 120, "Error getting column description");
                            if (Ool > 0 && (SQLULEN)Ool > Ors[j])
                                Ors[j] = Ool;
                            if (Ors[j] > 32000000)
                                Ors[j] = 32000000;
                            desz[j] = (size_t)(Ors[j] + 1);
                            if (!Ors[j])
                                Ors[j] = 1;
                            if (colInfo.getColumnType(j).getTypeOid() != LongVarcharOID || colInfo.getColumnType(j).getStringLength() != (int32)Ors[j])
                                ex_err(0, 0, 410, "Mismatch data type");
                            Ores[j] = (SQLPOINTER)srvInterface.allocator->alloc(desz[j] * rowset);
                            if (!SQL_SUCCEEDED(Oret = SQLBindCol(Ost, j + 1, SQL_C_CHAR, Ores[j], desz[j], Olen[j])))
                                ex_err(SQL_HANDLE_STMT, Ost, 411, "Error binding column");
                            break ;
                        case SQL_TYPE_TIME:
                            if (colInfo.getColumnType(j).getTypeOid() != TimeOID || colInfo.getColumnType(j).getTimePrecision() != (int32)Odd[j])
                                ex_err(0, 0, 410, "Mismatch data type");
                            Ores[j] = (SQLPOINTER)srvInterface.allocator->alloc(desz[j] * rowset);
                            if (!SQL_SUCCEEDED(Oret = SQLBindCol(Ost, j + 1, SQL_C_TIME, Ores[j], desz[j], Olen[j])))
                                ex_err(SQL_HANDLE_STMT, Ost, 411, "Error binding column");
                            break ;
                        case SQL_TYPE_DATE:
                            if (colInfo.getColumnType(j).getTypeOid() != DateOID)
                                ex_err(0, 0, 410, "Mismatch data type");
                            Ores[j] = (SQLPOINTER)srvInterface.allocator->alloc(desz[j] * rowset);
                            if (!SQL_SUCCEEDED(Oret = SQLBindCol(Ost, j + 1, SQL_C_DATE, Ores[j], desz[j], Olen[j])))
                                ex_err(SQL_HANDLE_STMT, Ost, 411, "Error binding column");
                            break ;
                        case SQL_TYPE_TIMESTAMP:
                            if (colInfo.getColumnType(j).getTypeOid() != TimestampOID || colInfo.getColumnType(j).getTimestampPrecision() != (int32)Odd[j])
                                ex_err(0, 0, 410, "Mismatch data type");
                            Ores[j] = (SQLPOINTER)srvInterface.allocator->alloc(desz[j] * rowset);
                            if (!SQL_SUCCEEDED(Oret = SQLBindCol(Ost, j + 1, SQL_C_TIMESTAMP, Ores[j], desz[j], Olen[j])))
                                ex_err(SQL_HANDLE_STMT, Ost, 411, "Error binding column");
                            break ;
                        case SQL_BIT:
                            if (colInfo.getColumnType(j).getTypeOid() != BoolOID)
                                ex_err(0, 0, 410, "Mismatch data type");
                            Ores[j] = (SQLPOINTER)srvInterface.allocator->alloc(desz[j] * rowset);
                            if (!SQL_SUCCEEDED(Oret = SQLBindCol(Ost, j + 1, SQL_C_BIT, Ores[j], desz[j], Olen[j])))
                                ex_err(SQL_HANDLE_STMT, Ost, 411, "Error binding column");
                            break ;
                        case SQL_BINARY:
                            if (Ors[j] > 65000)
                                Ors[j] = 65000;
                            if (colInfo.getColumnType(j).getTypeOid() != BinaryOID || colInfo.getColumnType(j).getStringLength() != (int32)Ors[j])
                                ex_err(0, 0, 410, "Mismatch data type");
                            desz[j] = (size_t)(Ors[j] + 1) ;
                            Ores[j] = (SQLPOINTER)srvInterface.allocator->alloc(desz[j] * rowset);
                            if (!SQL_SUCCEEDED(Oret = SQLBindCol(Ost, j + 1, SQL_C_BINARY, Ores[j], desz[j], Olen[j])))
                                ex_err(SQL_HANDLE_STMT, Ost, 411, "Error binding column");
                            break ;
                        case SQL_VARBINARY:
                            if (Ors[j] > 65000)
                                Ors[j] = 65000;
                            if (colInfo.getColumnType(j).getTypeOid() != VarbinaryOID || colInfo.getColumnType(j).getStringLength() != (int32)Ors[j])
                                ex_err(0, 0, 410, "Mismatch data type");
                            desz[j] = (size_t)(Ors[j] + 1) ;
                            Ores[j] = (SQLPOINTER)srvInterface.allocator->alloc(desz[j] * rowset);
                            if (!SQL_SUCCEEDED(Oret = SQLBindCol(Ost, j + 1, SQL_C_BINARY, Ores[j], desz[j], Olen[j])))
                                ex_err(SQL_HANDLE_STMT, Ost, 411, "Error binding column");
                            break ;
                        case SQL_LONGVARBINARY:
                            if (Ors[j] > 32000000)
                                Ors[j] = 32000000;
                            if (colInfo.getColumnType(j).getTypeOid() != LongVarbinaryOID || colInfo.getColumnType(j).getStringLength() != (int32)Ors[j])
                                ex_err(0, 0, 410, "Mismatch data type");
                            desz[j] = (size_t)(Ors[j] + 1) ;
                            Ores[j] = (SQLPOINTER)srvInterface.allocator->alloc(desz[j] * rowset);
                            if (!SQL_SUCCEEDED(Oret = SQLBindCol(Ost, j + 1, SQL_C_BINARY, Ores[j], desz[j], Olen[j])))
                                ex_err(SQL_HANDLE_STMT, Ost, 411, "Error binding column");
                            break ;
                        case SQL_INTERVAL_YEAR_TO_MONTH:
                            if (colInfo.getColumnType(j).getTypeOid() != IntervalYMOID || colInfo.getColumnType(j).getIntervalRange() != INTERVAL_YEAR2MONTH)
                                ex_err(0, 0, 410, "Mismatch data type");
                            Ores[j] = (SQLPOINTER)srvInterface.allocator->alloc(desz[j] * rowset);
                            if (!SQL_SUCCEEDED(Oret = SQLBindCol(Ost, j + 1, SQL_C_INTERVAL_YEAR_TO_MONTH, Ores[j], desz[j], Olen[j])))
                                ex_err(SQL_HANDLE_STMT, Ost, 411, "Error binding column");
                            break ;
                        case SQL_INTERVAL_DAY_TO_SECOND:
                            if (colInfo.getColumnType(j).getTypeOid() != IntervalOID || colInfo.getColumnType(j).getIntervalRange() != INTERVAL_DAY2SECOND || colInfo.getColumnType(j).getIntervalPrecision() != (int32)Odd[j])
                                ex_err(0, 0, 410, "Mismatch data type");
                            Ores[j] = (SQLPOINTER)srvInterface.allocator->alloc(desz[j] * rowset);
                            if (!SQL_SUCCEEDED(Oret = SQLBindCol(Ost, j + 1, SQL_C_INTERVAL_DAY_TO_SECOND, Ores[j], desz[j], Olen[j])))
                                ex_err(SQL_HANDLE_STMT, Ost, 411, "Error binding column");
                            break;
                    }
                }

                // Set Statement attributes:
                if (!SQL_SUCCEEDED(Oret=SQLSetStmtAttr(Ost, SQL_ATTR_ROW_BIND_TYPE, (SQLPOINTER)SQL_BIND_BY_COLUMN, 0))) {
                    ex_err(SQL_HANDLE_STMT, Ost, 412, "Error setting statement attribute SQL_ATTR_ROW_BIND_TYPE");
                }
                if (!SQL_SUCCEEDED(Oret=SQLSetStmtAttr(Ost, SQL_ATTR_ROW_ARRAY_SIZE, (SQLPOINTER)rowset, 0))) {
                    ex_err(SQL_HANDLE_STMT, Ost, 412, "Error setting statement attribute SQL_ATTR_ROW_ARRAY_SIZE");
                }
                if (!SQL_SUCCEEDED(Oret=SQLSetStmtAttr(Ost, SQL_ATTR_ROWS_FETCHED_PTR, &nfr, 0))) {
                    ex_err(SQL_HANDLE_STMT, Ost, 412, "Error setting statement attribute SQL_ATTR_ROWS_FETCHED_PTR");
                }

                // Execute Statement:
                if (!SQL_SUCCEEDED(Oret=SQLExecute(Ost)) && Oret != SQL_NO_DATA ) {
                    ex_err(SQL_HANDLE_STMT, Ost, 413, "Error executing the statement");
                }

                // Fetch loop:
                while ( SQL_SUCCEEDED(Oret=SQLFetchScroll(Ost, SQL_FETCH_NEXT, 0)) && !isCanceled() ) {
                    for ( unsigned int i = 0 ; i < nfr ; i++, outputWriter.next() ) {
                        for ( unsigned int j = 0 ; j < Oncol ; j++ ) {
                            Odp = (SQLPOINTER)((uint8_t *)Ores[j] + desz[j] * i) ;
                            Odl = Olen[j][i] ;
                            
                            if ( (int)Odl == (int)SQL_NULL_DATA ) {
                                outputWriter.setNull(j) ;
                                continue ;
                            }

                            switch(Odt[j]) {
                                case SQL_SMALLINT:
                                case SQL_INTEGER:
                                case SQL_TINYINT:
                                case SQL_BIGINT:
                                    if ( dbt == ORACLE ) {
                                        outputWriter.setInt(j, ((int)Odl == SQL_NTS) ? vint_null : (vint)atoll((char *)Odp) ) ;
                                    } else {
                                        outputWriter.setInt(j, *(SQLBIGINT *)Odp) ;
                                    }
                                    break ;
                                case SQL_REAL:
                                case SQL_DOUBLE:
                                case SQL_FLOAT:
                                    outputWriter.setFloat(j, *(SQLDOUBLE *)Odp) ;
                                    break ;
                                case SQL_NUMERIC:
                                case SQL_DECIMAL:
                                    {
                                        std::string rejectReason = "Unrecognized remote database format" ;
                                        if ( *(char *)Odp == '\0' ) { // some DBs might use empty strings for NUMERIC nulls
                                            outputWriter.setNull(j) ;
                                        } else {
                                            if (!parser.parseNumeric((char*)Odp, (size_t)Odl, j,
                                                    outputWriter.getNumericRef(j), colInfo.getColumnType(j), rejectReason)) {
                                                ex_err(0, 0, 414, "Error parsing Numeric");
                                            }
                                        }
                                        break ;
                                    }
                                case SQL_CHAR:
                                case SQL_WCHAR:
                                case SQL_VARCHAR:
                                case SQL_WVARCHAR:
                                case SQL_LONGVARCHAR:
                                case SQL_WLONGVARCHAR:
                                case SQL_BINARY:
                                case SQL_VARBINARY:
                                case SQL_LONGVARBINARY:
                                    if ( (int)Odl == SQL_NTS )
                                        Odl = (SQLULEN)strnlen((char *)Odp , desz[j]);
                                    outputWriter.getStringRef(j).copy((char *)Odp, Odl ) ;
                                    break ;
                                case SQL_TYPE_TIME:
                                    {
                                        SQL_TIME_STRUCT &st = *(SQL_TIME_STRUCT *)Odp ;
                                        outputWriter.setTime(j, getTimeFromUnixTime(st.second + st.minute * 60 + st.hour * 3600 ) );
                                        break ;
                                    }
                                case SQL_TYPE_DATE:
                                    {
                                        SQL_DATE_STRUCT &sd = *(SQL_DATE_STRUCT *)Odp ;
                                        struct tm d = { 0, 0, 0, sd.day, sd.month - 1, sd.year - 1900, 0, 0, -1 } ;
                                        time_t utime = mktime ( &d ) ;
                                        outputWriter.setDate(j, getDateFromUnixTime(utime + d.tm_gmtoff) ) ;
                                        break ;
                                    }
                                case SQL_TYPE_TIMESTAMP:
                                    {
                                        SQL_TIMESTAMP_STRUCT &ss = *(SQL_TIMESTAMP_STRUCT *)Odp ;
                                        struct tm ts = { ss.second, ss.minute, ss.hour, ss.day, ss.month - 1, ss.year - 1900, 0, 0, -1 } ;
                                        time_t utime = mktime ( &ts ) ;
                                        outputWriter.setTimestamp(j, getTimestampFromUnixTime(utime + ts.tm_gmtoff) + ss.fraction / 1000 ) ;
                                        break ;
                                    }
                                case SQL_BIT:
                                    outputWriter.setBool(j, *(SQLCHAR *)Odp == SQL_TRUE ? VTrue : VFalse);
                                    break ;
                                case SQL_INTERVAL_YEAR_TO_MONTH: // Vertica stores these Intervals as durations in months
                                    {
                                        SQL_INTERVAL_STRUCT &intv = *(SQL_INTERVAL_STRUCT*)Odp;
                                        if (intv.interval_type != SQL_IS_YEAR_TO_MONTH) {
                                            ex_err(0, 0, 415, "Unsupported INTERVAL data type. Expecting SQL_IS_YEAR_TO_MONTH");
                                        }
                                        Interval ret = (  (intv.intval.year_month.year*MONTHS_PER_YEAR)
                                                        + (intv.intval.year_month.month))
                                                        * (intv.interval_sign == SQL_TRUE ? -1 : 1);
                                        outputWriter.setInterval(j, ret);
                                        break ;
                                    }
                                case SQL_INTERVAL_DAY_TO_SECOND: // Vertica stores these Intervals as durations in microseconds
                                    {
                                        SQL_INTERVAL_STRUCT &intv = *(SQL_INTERVAL_STRUCT*)Odp;
                                        if (intv.interval_type != SQL_IS_DAY_TO_SECOND) {
                                            ex_err(0, 0, 416, "Unsupported INTERVAL data type. Expecting SQL_IS_DAY_TO_SECOND");
                                        }
                                        
                                        Interval ret = (  (intv.intval.day_second.day*usPerDay)
                                                        + (intv.intval.day_second.hour*usPerHour)
                                                        + (intv.intval.day_second.minute*usPerMinute)
                                                        + (intv.intval.day_second.second*usPerSecond)
                                                        + (intv.intval.day_second.fraction/1000))
                                                        * (intv.interval_sign == SQL_TRUE ? -1 : 1);
                                        outputWriter.setInterval(j, ret);
                                        break ;
                                    }
                                default:
                                    vt_report_error(417, "DBLINK. Unsupported data type for column %u", j);
                                    break ;
                            }
                        }
                    }
                }
            } else {
                if (!SQL_SUCCEEDED(Oret=SQLExecDirect (Ost, (SQLCHAR *)query.c_str(), SQL_NTS))) {
                    ex_err(SQL_HANDLE_STMT, Ost, 418, "Error executing statement");
                }
                outputWriter.setInt(0, (vint)Oret) ;
                outputWriter.next() ;
            }
            clean() ;
        }
        catch (exception& e)
        {
            clean();
            vt_report_error(400, "Exception while processing partition: [%s]", e.what());
        }
    }
};

class DBLinkFactory : public TransformFunctionFactory, ODBCBase
{
    virtual void getPrototype(ServerInterface &srvInterface,
                              ColumnTypes &argTypes,
                              ColumnTypes &returnType)
    {
        returnType.addAny();
    }

    virtual void getReturnType(ServerInterface &srvInterface,
                               const SizedColumnTypes &inputTypes,
                               SizedColumnTypes &outputTypes)
    {
        SQLRETURN Oret = 0 ;
        SQLSMALLINT Onamel = 0 ;
        SQLSMALLINT Onull = 0 ;
        SQLCHAR Ocname[MAXCNAMELEN] ;

        setupConnection(srvInterface);
        connect();

        // ODBC Statement preparation:
        if (!SQL_SUCCEEDED(Oret=SQLAllocHandle(SQL_HANDLE_STMT, Ocon, &Ost))){
            ex_err(SQL_HANDLE_DBC, Ocon, 111, "Error allocating Statement Handle");
        }
        if ( is_select ) {
            if (!SQL_SUCCEEDED(Oret=SQLPrepare(Ost, (SQLCHAR *)query.c_str(), SQL_NTS))) {
                ex_err(SQL_HANDLE_STMT, Ost, 112, "Error preparing the statement");
            }
            if (!SQL_SUCCEEDED(Oret=SQLNumResultCols(Ost, (SQLSMALLINT *)&Oncol))) {
                ex_err(SQL_HANDLE_STMT, Ost, 115, "Error finding the number of resulting columns");
            }
            if ( (Odt = (SQLSMALLINT *)calloc ((size_t)Oncol, sizeof(SQLSMALLINT))) == (void *)NULL ) {
                ex_err(0, 0, 116, "Error allocating data types array");
            }
            if ( (Ors = (SQLULEN *)calloc ((size_t)Oncol, sizeof(SQLULEN))) == (void *)NULL ) {
                ex_err(0, 0, 117, "Error allocating result set columns size array");
            }
            if ( (desz = (size_t *)calloc ((size_t)Oncol, sizeof(size_t))) == (void *)NULL ) {
                ex_err(0, 0, 118, "Error allocating data element size array");
            }
            if ( (Odd = (SQLSMALLINT *)calloc ((size_t)Oncol, sizeof(SQLSMALLINT))) == (void *)NULL ) {
                ex_err(0, 0, 119, "Error allocating result set decimal size array");
            }
            for ( unsigned int j = 0 ; j < Oncol ; j++ ) {
                SQLLEN Ool = 0 ;
                if ( !SQL_SUCCEEDED(Oret=SQLDescribeCol(Ost, (SQLUSMALLINT)(j+1),
                        Ocname, (SQLSMALLINT) MAXCNAMELEN, &Onamel,
                        &Odt[j], &Ors[j], &Odd[j], &Onull))) {
                    ex_err(SQL_HANDLE_STMT, Ost, 120, "Error getting column description");
                }
#ifdef DBLINK_DEBUG
  srvInterface.log("DEBUG DBLinkFactory SQLDescribeCol src column=%u name=%s data_type=%d length=%zu", j, (char *)Ocname, Odt[j], Ors[j]);
#endif
                std::string cname((char *)Ocname);
                switch(Odt[j]) {
                    case SQL_SMALLINT:
                    case SQL_INTEGER:
                    case SQL_TINYINT:
                    case SQL_BIGINT:
                        // we change this later on if the remote db is Oracle
                        desz[j] = sizeof(vint) ;
                        outputTypes.addInt(cname) ;
                        break ;
                    case SQL_REAL:
                    case SQL_DOUBLE:
                    case SQL_FLOAT:
                        desz[j] = sizeof(vfloat) ;
                        outputTypes.addFloat(cname) ;
                        break ;
                    case SQL_NUMERIC:
                    case SQL_DECIMAL:
                        desz[j] = MAX_NUMERIC_CHARLEN ;
                        outputTypes.addNumeric((int32)Ors[j], (int32)Odd[j], cname) ;
                        break ;
                    case SQL_CHAR:
                    case SQL_WCHAR:
                        if( !SQL_SUCCEEDED(Oret=SQLColAttribute(Ost, (SQLUSMALLINT)(j+1), SQL_DESC_OCTET_LENGTH,
                            (SQLPOINTER) NULL, (SQLSMALLINT) 0, (SQLSMALLINT *) NULL, &Ool))) {
                                ex_err(SQL_HANDLE_STMT, Ost, 120, "Error getting column description");
                        }
#ifdef DBLINK_DEBUG
  srvInterface.log("DEBUG DBLinkFactory SQLColAttribute SQL_DESC_OCTET_LENGTH src column=%u name=%s data_type=%d length=%ld", j, (char *)Ocname, Odt[j], Ool);
#endif
                        if ( Ool > 0 && (SQLULEN)Ool > Ors[j] ) 
                            Ors[j] = Ool ;
                        if ( Ors[j] > 65000 ) {
                            srvInterface.log("DBLINK SQL_[W]CHAR column %s of length %zu limited to 65000 bytes", (char *)Ocname, Ors[j]);
                            Ors[j] = 65000;
                        }
                        desz[j] = (size_t)(Ors[j] + 1) ;
                        if ( !Ors[j] )
                            Ors[j] = 1 ;
                        outputTypes.addChar((int32)Ors[j], cname) ;
                        break ;
                    case SQL_VARCHAR:
                    case SQL_WVARCHAR:
                        if( !SQL_SUCCEEDED(Oret=SQLColAttribute(Ost, (SQLUSMALLINT)(j+1), SQL_DESC_OCTET_LENGTH,
                            (SQLPOINTER) NULL, (SQLSMALLINT) 0, (SQLSMALLINT *) NULL, &Ool))) {
                                ex_err(SQL_HANDLE_STMT, Ost, 120, "Error getting column description");
                        }
#ifdef DBLINK_DEBUG
  srvInterface.log("DEBUG DBLinkFactory SQLColAttribute SQL_DESC_OCTET_LENGTH src column=%u name=%s data_type=%d length=%ld", j, (char *)Ocname, Odt[j], Ool);
#endif
                        if ( Ool > 0 && (SQLULEN)Ool > Ors[j] ) 
                            Ors[j] = Ool ;
                        if ( Ors[j] > 65000 ) {
                            srvInterface.log("DBLINK SQL_[W]VARCHAR column %s of length %zu limited to 65000 bytes", (char *)Ocname, Ors[j]);
                            Ors[j] = 65000;
                        }
                        desz[j] = (size_t)(Ors[j] + 1) ;
                        if ( !Ors[j] )
                            Ors[j] = 1 ;
                        outputTypes.addVarchar((int32)Ors[j], cname) ;
                        break ;
                    case SQL_LONGVARCHAR:
                    case SQL_WLONGVARCHAR:
                        if( !SQL_SUCCEEDED(Oret=SQLColAttribute(Ost, (SQLUSMALLINT)(j+1), SQL_DESC_OCTET_LENGTH,
                            (SQLPOINTER) NULL, (SQLSMALLINT) 0, (SQLSMALLINT *) NULL, &Ool))) {
                                ex_err(SQL_HANDLE_STMT, Ost, 120, "Error getting column description");
                        }
#ifdef DBLINK_DEBUG
  srvInterface.log("DEBUG DBLinkFactory SQLColAttribute SQL_DESC_OCTET_LENGTH src column=%u name=%s data_type=%d length=%ld", j, (char *)Ocname, Odt[j], Ool);
#endif
                        if ( Ool > 0 && (SQLULEN)Ool > Ors[j] ) 
                            Ors[j] = Ool ;
                        if ( Ors[j] > 32000000 ) {
                            srvInterface.log("DBLINK SQL_LONG[W]VARCHAR column %s of length %zu limited to 32000000 bytes", (char *)Ocname, Ors[j]);
                            Ors[j] = 32000000;
                        }
                        desz[j] = (size_t)(Ors[j] + 1) ;
                        if ( !Ors[j] )
                            Ors[j] = 1 ;
                        outputTypes.addLongVarchar((int32)Ors[j], cname) ;
                        break ;
                    case SQL_TYPE_TIME:
                        desz[j] = sizeof(SQL_TIME_STRUCT) ;
                        outputTypes.addTime((int32)Odd[j], cname) ;
                        break ;
                    case SQL_TYPE_DATE:
                        desz[j] = sizeof(SQL_DATE_STRUCT) ;
                        outputTypes.addDate(cname) ;
                        break ;
                    case SQL_TYPE_TIMESTAMP:
                        desz[j] = sizeof(SQL_TIMESTAMP_STRUCT) ;
                        outputTypes.addTimestamp((int32)Odd[j], cname) ;
                        break ;
                    case SQL_BIT:
                        desz[j] = 1 ;
                        outputTypes.addBool(cname) ;
                        break ;
                    case SQL_BINARY:
                        if ( Ors[j] > 65000 ) {
                            srvInterface.log("DBLINK SQL_BINARY column %s of length %zu limited to 65000 bytes", (char *)Ocname, Ors[j]);
                            Ors[j] = 65000;
                        }
                        desz[j] = (size_t)(Ors[j] + 1) ;
                        outputTypes.addBinary((int32)Ors[j], cname) ;
                        break ;
                    case SQL_VARBINARY:
                        if ( Ors[j] > 65000 ) {
                            srvInterface.log("DBLINK SQL_VARBINARY column %s of length %zu limited to 65000 bytes", (char *)Ocname, Ors[j]);
                            Ors[j] = 65000;
                        }
                        desz[j] = (size_t)(Ors[j] + 1) ;
                        outputTypes.addVarbinary((int32)Ors[j], cname) ;
                        break ;
                    case SQL_LONGVARBINARY:
                        if ( Ors[j] > 32000000 ) {
                            srvInterface.log("DBLINK SQL_LONGVARBINARY column %s of length %zu limited to 32000000 bytes", (char *)Ocname, Ors[j]);
                            Ors[j] = 32000000;
                        }
                        desz[j] = (size_t)(Ors[j] + 1) ;
                        outputTypes.addLongVarbinary((int32)Ors[j], cname) ;
                        break ;
                    case SQL_INTERVAL_YEAR_TO_MONTH:
                        desz[j] = sizeof(SQL_INTERVAL_STRUCT) ;
                        outputTypes.addIntervalYM(INTERVAL_YEAR2MONTH, cname) ;
                        break ;
                    case SQL_INTERVAL_DAY_TO_SECOND:
                        desz[j] = sizeof(SQL_INTERVAL_STRUCT) ;
                        outputTypes.addInterval((int32)Odd[j], INTERVAL_DAY2SECOND, cname) ;
                        break ;
                    default:
                        vt_report_error(121, "DBLINK. Unsupported data type for column %u", j);
                        clean();
                }
            }
            colInfo = outputTypes ;
        } else {
            outputTypes.addInt("dblink") ;
        }
        clean();
    }

    virtual void getParameterType(ServerInterface &srvInterface, SizedColumnTypes &parameterTypes)
    {
        parameterTypes.addVarchar(1024, "cid",  { true, false, false, "Connection Identifier Database. Identifies an entry in the connection identifier database." });
        parameterTypes.addVarchar(1024, "connect",  { true, false, false, "The ODBC connection string containing the DSN and credentials." });
        parameterTypes.addVarchar(1024, "connect_secret",  { true, false, false, "The ODBC connection string containing the DSN and credentials." });
        parameterTypes.addVarchar(1024, "cidfile",  { true, false, false, "Connection Identifier File Path." });
        parameterTypes.addVarchar(65000, "query",  { true, false, false, "The query being pushed on the remote database. Or, '@' followed by the name of the file containing the query." });
        parameterTypes.addInt("rowset",  { true, false, false, "Number of rows retrieved from the remote database during each SQLFetch() cycle. Default is 100." });
    }

    virtual TransformFunction *createTransformFunction( ServerInterface &srvInterface )
    {
        return vt_createFuncObject<DBLink>(srvInterface.allocator);
    }
};
RegisterFactory(DBLinkFactory);

RegisterLibrary (
    "Maurizio Felici",
    __DATE__,
    "0.3.0",
    "12.0.4",
    "maurizio.felici@vertica.com",
    "DBLINK: run SQL on other databases",
    "",
    ""
);
