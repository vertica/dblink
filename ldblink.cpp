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
#include <strings.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>
  
#define DBLINK_CIDS			"/usr/local/etc/dblink.cids"	// Default Connection identifiers config file FIX: add a param
#define MAXCNAMELEN			128								// Max column name length
#define DEF_ROWSET 			100								// Default rowset
#define MAX_ROWSET 			1000							// Default rowset
#define DEF_THREADS			1								// Default number of fetch threads
#define MAX_THREADS			16								// Max number of fetch threads
#define DEF_BUFFER_MB		256								// Default per invocation fetch buffer budget
#define MIN_BUFFER_MB		16								// Min per invocation fetch buffer budget
#define MAX_BUFFER_MB		8192							// Max per invocation fetch buffer budget
#define DEF_TOTAL_BUFFER_MB	4096							// Default process wide fetch buffer ceiling
#define MIN_TOTAL_BUFFER_MB	64								// Min process wide fetch buffer ceiling
#define MAX_TOTAL_BUFFER_MB	65536							// Max process wide fetch buffer ceiling
#define DEF_QUERY_TIMEOUT	0								// Default statement timeout, 0 = unlimited
#define MAX_QUERY_TIMEOUT	86400							// Max statement timeout in seconds
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

// All the ODBC state belonging to ONE DBLINK() invocation. Never share it between
// instances: a single SQL statement can contain several DBLINK() calls running in
// the same UDx process.
struct OdbcState {
	SQLHENV Oenv ;				// ODBC Environment handle
	SQLHDBC Ocon ;				// ODBC Connection  handle
	SQLHSTMT Ost ;				// ODBC Statement  handle
	bool is_select ;			// Command is a SELECT
	std::string query ;			// Statement text
	SQLUSMALLINT Oncol ;		// Number of result set columns
	SQLSMALLINT *Odt ;			// Result set Data Type Array pointer
	SQLSMALLINT *Odd ;			// Result set Decimals Array pointer
	SQLULEN *Ors ;				// Result Set Column size pointer
	size_t *desz ;				// Data Element Size Array pointer
	SQLULEN nfr ;				// Number of fetched rows
	vint query_timeout ;		// Statement timeout in seconds, 0 = unlimited

	OdbcState() : Oenv(0), Ocon(0), Ost(0), is_select(false), query(""),
		Oncol(0), Odt(0), Odd(0), Ors(0), desz(0), nfr(0),
		query_timeout(DEF_QUERY_TIMEOUT) {}

	~OdbcState() { clean() ; }

	OdbcState(const OdbcState &) = delete ;
	OdbcState &operator=(const OdbcState &) = delete ;

	// Releases the remote connection only: the producers keep reading the metadata
	void closeConnection() {
		if ( Ost ) {
			(void)SQLCloseCursor(Ost);
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
	}

	// Idempotent
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
		closeConnection() ;
		Oncol = 0 ;
		nfr = 0 ;
	}
};

// One fetched rowset. Producer threads must not use the Vertica allocator, so the
// column buffers are plain malloc()ed memory owned by the Batch.
struct Batch {
	SQLULEN rows ;					// Number of rows fetched into this batch
	std::vector<void *> data ;		// One buffer per column, rowset * desz[j] bytes
	std::vector<SQLLEN *> ind ;		// One indicator array per column, rowset entries

	Batch() : rows(0) {}
	~Batch() { release() ; }

	Batch ( Batch &&o ) : rows(o.rows), data(std::move(o.data)), ind(std::move(o.ind)) {
		o.rows = 0 ;
	}
	Batch &operator= ( Batch &&o ) {
		if ( this != &o ) {
			release() ;
			rows = o.rows ;
			data = std::move(o.data) ;
			ind = std::move(o.ind) ;
			o.rows = 0 ;
		}
		return *this ;
	}
	Batch ( const Batch & ) = delete ;
	Batch &operator= ( const Batch & ) = delete ;

	bool alloc ( unsigned int ncol, const size_t *desz, size_t rowset ) {
		data.assign((size_t)ncol, (void *)0) ;
		ind.assign((size_t)ncol, (SQLLEN *)0) ;
		for ( unsigned int j = 0 ; j < ncol ; j++ ) {
			if ( ( data[j] = malloc(desz[j] * rowset) ) == NULL )
				return false ;
			if ( ( ind[j] = (SQLLEN *)malloc(sizeof(SQLLEN) * rowset) ) == NULL )
				return false ;
		}
		return true ;
	}

	void release() {
		for ( size_t j = 0 ; j < data.size() ; j++ )
			free(data[j]) ;
		for ( size_t j = 0 ; j < ind.size() ; j++ )
			free(ind[j]) ;
		data.clear() ;
		ind.clear() ;
		rows = 0 ;
	}
};

// Bounded hand off queue between the fetch threads and the Vertica writer thread
class BatchQueue {

	std::mutex qmutex ;
	std::condition_variable not_full ;
	std::condition_variable not_empty ;
	std::deque<Batch> queue ;
	std::deque<Batch> free_list ;	// Spent batches kept for reuse
	size_t cap ;
	size_t active ;			// Producers still running
	bool aborted ;

public:

	BatchQueue() : cap(0), active(0), aborted(true) {}

	~BatchQueue() {
		queue.clear() ;
		free_list.clear() ;
	}

	// Called by the main thread before any producer is started
	void start ( size_t capacity, size_t producers ) {
		std::lock_guard<std::mutex> guard(qmutex) ;
		queue.clear() ;
		free_list.clear() ;
		cap = capacity ;
		active = producers ;
		aborted = false ;
	}

	// Returns false when the consumer is gone: the producer must then stop
	bool push ( Batch &b ) {
		std::unique_lock<std::mutex> guard(qmutex) ;
		while ( !aborted && queue.size() >= cap )
			not_full.wait(guard) ;
		if ( aborted )
			return false ;
		queue.push_back(std::move(b)) ;
		guard.unlock() ;
		not_empty.notify_one() ;
		return true ;
	}

	// Returns false when every producer has finished and nothing is left
	bool pop ( Batch &b ) {
		std::unique_lock<std::mutex> guard(qmutex) ;
		while ( !aborted && queue.empty() && active > 0 )
			not_empty.wait(guard) ;
		if ( aborted || queue.empty() )
			return false ;
		b = std::move(queue.front()) ;
		queue.pop_front() ;
		guard.unlock() ;
		not_full.notify_one() ;
		return true ;
	}

	// Every batch of a query has the same layout, so a spent one can be refilled as is
	bool getFree ( Batch &b ) {
		std::lock_guard<std::mutex> guard(qmutex) ;
		if ( free_list.empty() )
			return false ;
		b = std::move(free_list.front()) ;
		free_list.pop_front() ;
		return true ;
	}

	void putFree ( Batch &b ) {
		{
			std::lock_guard<std::mutex> guard(qmutex) ;
			if ( free_list.size() < cap ) {
				b.rows = 0 ;
				free_list.push_back(std::move(b)) ;
				return ;
			}
		}
		b.release() ;
	}

	void producerDone() {
		{
			std::lock_guard<std::mutex> guard(qmutex) ;
			if ( active )
				active-- ;
		}
		not_empty.notify_all() ;
	}

	// Wakes every blocked thread: safe to call from cancel()
	void abort() {
		{
			std::lock_guard<std::mutex> guard(qmutex) ;
			aborted = true ;
		}
		not_full.notify_all() ;
		not_empty.notify_all() ;
	}

	void drain() {
		std::lock_guard<std::mutex> guard(qmutex) ;
		queue.clear() ;
		free_list.clear() ;
	}
};

// Process wide fetch buffer accounting. Several DBLINK() calls share one UDx process and
// the Vertica resource manager cannot see these buffers, so they are tracked cooperatively.
static std::atomic<uint64_t> g_dblink_bytes_inflight(0) ;

// Reserves as much of "want" as the ceiling allows but never less than "floor_bytes".
// Returns the granted amount, or 0 when not even "floor_bytes" fits.
static uint64_t reserve_buffers ( uint64_t want, uint64_t floor_bytes, uint64_t ceiling )
{
	uint64_t inflight = g_dblink_bytes_inflight.load() ;

	for ( ; ; ) {
		uint64_t room = ( ceiling > inflight ) ? ( ceiling - inflight ) : 0 ;
		uint64_t grant = ( want < room ) ? want : room ;
		if ( grant < floor_bytes )
			return 0 ;
		if ( g_dblink_bytes_inflight.compare_exchange_weak(inflight, inflight + grant) )
			return grant ;
	}
}

struct BufferReservation {
	uint64_t bytes ;

	BufferReservation() : bytes(0) {}
	~BufferReservation() { release() ; }

	BufferReservation ( const BufferReservation & ) = delete ;
	BufferReservation &operator= ( const BufferReservation & ) = delete ;

	void hold ( uint64_t n ) {
		release() ;
		bytes = n ;
	}

	// Idempotent
	void release() {
		if ( bytes ) {
			g_dblink_bytes_inflight.fetch_sub(bytes) ;
			bytes = 0 ;
		}
	}
};

// Interrupts a remote statement that runs for longer than query_timeout. The Vertica ODBC
// driver ignores SQL_ATTR_QUERY_TIMEOUT, so the timeout is enforced with SQLCancel(), the
// only interrupt the driver honours on a statement in progress.
class StmtCancelTimer {

	SQLHSTMT hstmt ;
	std::thread timer ;
	std::mutex tmutex ;
	std::condition_variable tcv ;
	std::atomic<bool> fired_flag ;
	bool done ;

public:

	StmtCancelTimer ( SQLHSTMT h, int secs ) : hstmt(h), fired_flag(false), done(false) {
		if ( secs <= 0 )
			return ;
		timer = std::thread([this, secs] {
			std::unique_lock<std::mutex> guard(tmutex) ;
			if ( !tcv.wait_for(guard, std::chrono::seconds(secs), [this]{ return done ; }) ) {
				fired_flag.store(true) ;
				(void)SQLCancel(hstmt) ;
			}
		}) ;
	}

	~StmtCancelTimer() { stop() ; }

	StmtCancelTimer ( const StmtCancelTimer & ) = delete ;
	StmtCancelTimer &operator= ( const StmtCancelTimer & ) = delete ;

	bool fired() const { return fired_flag.load() ; }

	// Idempotent, and always joins before the caller can free the statement handle
	void stop() {
		{
			std::lock_guard<std::mutex> guard(tmutex) ;
			done = true ;
		}
		tcv.notify_all() ;
		if ( timer.joinable() )
			timer.join() ;
	}
};

// Strict integer conversion: rejects empty, partial, out of range and trailing garbage
static bool parse_bound ( const char *s, vint &out )
{
	char *endptr = 0 ;
	long long value = 0 ;

	if ( s == NULL || *s == '\0' )
		return false ;
	while ( isspace((unsigned char)*s) )
		s++ ;
	errno = 0 ;
	value = strtoll(s, &endptr, 10) ;
	if ( endptr == s || errno == ERANGE )
		return false ;
	while ( isspace((unsigned char)*endptr) )
		endptr++ ;
	if ( *endptr != '\0' )
		return false ;
	out = (vint)value ;
	return true ;
}

// Only a plain unquoted identifier may be interpolated into the generated SQL
static bool valid_identifier ( const std::string &s )
{
	if ( s.empty() || s.size() > MAXCNAMELEN )
		return false ;
	for ( size_t i = 0 ; i < s.size() ; i++ ) {
		char c = s[i] ;
		bool alpha = ( c >= 'A' && c <= 'Z' ) || ( c >= 'a' && c <= 'z' ) || c == '_' ;
		if ( i == 0 ) {
			if ( !alpha )
				return false ;
		} else if ( !alpha && !( c >= '0' && c <= '9' ) && c != '$' ) {
			return false ;
		}
	}
	return true ;
}

void ex_err ( SQLSMALLINT htype, SQLHANDLE Oh, int loc , const char *vtext ) {
	SQLCHAR Oerr_state[6] ;					// ODBC Error State
	SQLINTEGER Oerr_native = 0 ;			// ODBC Error Native Code
	SQLCHAR Oerr_text[MAX_ODBC_ERROR_LEN] ;	// ODBC Error Text
	SQLRETURN Oret = 0 ;
	SQLSMALLINT Oln = 0 ;

	Oerr_state[0] = Oerr_text[0] = '\0' ;

	if ( htype == 0 ) {
		vt_report_error(loc, "DBLINK. %s", vtext);
	} else if ( ( Oret = SQLGetDiagRec ( htype, Oh, 1, Oerr_state, &Oerr_native, Oerr_text,
			(SQLSMALLINT)MAX_ODBC_ERROR_LEN, &Oln) ) != SQL_SUCCESS ) {
		vt_report_error(loc, "DBLINK. %s. Unable to display ODBC error message", vtext);
	} else {
		vt_report_error(loc, "DBLINK. %s. State %s. Native Code %d. Error text: %s%c",
			vtext, (char *)Oerr_state, (int)Oerr_native, (char *) Oerr_text,
			( Oln > MAX_ODBC_ERROR_LEN ) ? '>' : '.' ) ;
	}
}

// Resolve the connection parameters into an ODBC connection string, read the query
// and work out whether it is a DQL statement. Shared by getReturnType() and setup().
void get_connection_info ( ServerInterface &srvInterface, OdbcState &st, std::string &cid_value )
{
	std::string cid = "" ;
	std::string cid_env = "" ;
	std::string cid_file = DBLINK_CIDS ;
	std::string cid_name = "" ;
	std::string &query = st.query ;
	bool connect = false ;

	// Read Params:
	ParamReader params = srvInterface.getParamReader();
	if( params.containsParameter("cidfile") ) {			// Start checking "cidfile" param
		cid_file = params.getStringRef("cidfile").str() ;
#ifdef DBLINK_DEBUG
  srvInterface.log("DEBUG DBLINK read param cidfile=<%s>", cid_file.c_str() );
#endif
	}
	if( params.containsParameter("cid") ) {				// Start checking "cid" param
		cid = params.getStringRef("cid").str() ;
#ifdef DBLINK_DEBUG
  srvInterface.log("DEBUG DBLINK read param cid=<%s>", cid.c_str() );
#endif
	} else if( params.containsParameter("connect_secret") ) {	// if "cid" is undef try with "connect_secret"
		connect = true ;
		cid = params.getStringRef("connect_secret").str() ;
	} else if( params.containsParameter("connect") ) {	// if "cid" is undef try with "connect"
		connect = true ;
		cid = params.getStringRef("connect").str() ;
	} else if (srvInterface.getUDSessionParamReader("library").containsParameter("dblink_secret")) {
		// if "cid", "connect_secret" and "connect" are not defined try "dblink_secret" session param
		connect = true ;
		cid = srvInterface.getUDSessionParamReader("library").getStringRef("dblink_secret").str() ;
	} else {
		vt_report_error(101, "DBLINK. Missing connection parameters");
	}
	if( params.containsParameter("query") ) {
		query = params.getStringRef("query").str() ;
#ifdef DBLINK_DEBUG
  srvInterface.log("DEBUG DBLINK read param query=<%s>", query.c_str() );
#endif
	} else {
		vt_report_error(102, "DBLINK. Missing query parameter");
	}

	// Both the planning connection and the instance connection need this
	if( params.containsParameter("query_timeout") ) {
		st.query_timeout = params.getIntRef("query_timeout") ;
		if ( st.query_timeout < 0 || st.query_timeout > MAX_QUERY_TIMEOUT )
			vt_report_error(213, "DBLINK. Error query_timeout out of range");
	}

	// Check connection parameters
	if ( connect ) { 	// old VFQ connect style: connect='@/tmp/file.txt' will read CIDs from a different file
		if ( cid[0] == '@' ) {
			std::ifstream cids(cid.substr(1)) ;
			if ( cids.is_open() ) {
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
	} else {			// new CID connect style:
		std::ifstream cids(cid_file) ;
		if ( cids.is_open() ) {
			std::string cline ;
			size_t pos ;
			while ( getline(cids, cline) ) {
				if ( cline[0] == '#' || cline.empty() )
					continue ;	// skip empty lines & comments
				if ( ( pos = cline.find(":") ) != std::string::npos ) {
					cid_name = cline.substr(0, pos) ;
					if ( cid_name == cid )
						cid_value = cline.substr(pos + 1, std::string::npos ) ;
					else if ( cid_name == cid + "$" ) {
						cid_env = cline.substr(pos + 1, std::string::npos ) ;
						std::stringstream se_stream ( cid_env ) ; 
						std::string token ;
						while ( std::getline ( se_stream, token, ';' ) ) { 
							size_t pos = 0 ; 
							if ( ( pos = token.find('=') ) && pos != std::string::npos ) { 
#ifdef DBLINK_DEBUG
  srvInterface.log("DEBUG DBLINK setting <%s> to <%s>", token.substr(0, pos).c_str(), token.substr(pos+1).c_str() );
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
			vt_report_error(104, "DBLINK. Error reading <%s>", cid_file.c_str());
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
	if ( !strncasecmp(query.c_str(), "SELECT", 6) )
		st.is_select = true ;
}

// Describe the prepared result set: fills the OdbcState arrays and the column types.
// Shared by getReturnType() and setup().
void describe_result_set ( ServerInterface &srvInterface, OdbcState &st, SizedColumnTypes &outputTypes )
{
	SQLRETURN Oret = 0 ;
	SQLSMALLINT Onamel = 0 ;
	SQLSMALLINT Onull = 0 ;
	SQLCHAR Ocname[MAXCNAMELEN] ;
	SQLHSTMT Ost = st.Ost ;
	SQLUSMALLINT &Oncol = st.Oncol ;
	SQLSMALLINT *&Odt = st.Odt ;
	SQLSMALLINT *&Odd = st.Odd ;
	SQLULEN *&Ors = st.Ors ;
	size_t *&desz = st.desz ;

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
  srvInterface.log("DEBUG DBLINK SQLDescribeCol src column=%u name=%s data_type=%d length=%zu", j, (char *)Ocname, Odt[j], Ors[j]);
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
  srvInterface.log("DEBUG DBLINK SQLColAttribute SQL_DESC_OCTET_LENGTH src column=%u name=%s data_type=%d length=%ld", j, (char *)Ocname, Odt[j], Ool);
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
  srvInterface.log("DEBUG DBLINK SQLColAttribute SQL_DESC_OCTET_LENGTH src column=%u name=%s data_type=%d length=%ld", j, (char *)Ocname, Odt[j], Ool);
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
  srvInterface.log("DEBUG DBLINK SQLColAttribute SQL_DESC_OCTET_LENGTH src column=%u name=%s data_type=%d length=%ld", j, (char *)Ocname, Odt[j], Ool);
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
		}
	}
}

// Open the environment, connection and statement handles held by "st". The Vertica ODBC
// driver ignores the login and connection timeout attributes, so reaching a dead host is
// bounded by the OS TCP timeout only: tune the socket timeouts or use BackupServerNode.
void odbc_connect ( OdbcState &st, const std::string &cid_value )
{
	SQLRETURN Oret = 0 ;

	if (!SQL_SUCCEEDED(Oret=SQLAllocHandle(SQL_HANDLE_ENV, (SQLHANDLE)SQL_NULL_HANDLE, &st.Oenv))){
		ex_err(0, 0, 107, "Error allocating Environment Handle");
	}
	if (!SQL_SUCCEEDED(Oret=SQLSetEnvAttr(st.Oenv, SQL_ATTR_ODBC_VERSION, (void *) SQL_OV_ODBC3, 0))){
		ex_err(0, 0, 108, "Error setting SQL_OV_ODBC3");
	}
	if (!SQL_SUCCEEDED(Oret=SQLAllocHandle(SQL_HANDLE_DBC, st.Oenv, &st.Ocon))){
		ex_err(0, 0, 109, "Error allocating Connection Handle");
	}
	if (!SQL_SUCCEEDED(Oret=SQLDriverConnect(st.Ocon, (SQLHWND)NULL, (SQLCHAR *)cid_value.c_str(), SQL_NTS, NULL, 0, NULL, SQL_DRIVER_NOPROMPT))){
		ex_err(SQL_HANDLE_DBC, st.Ocon, 110, "Error connecting to target database");
	}
	if (!SQL_SUCCEEDED(Oret=SQLAllocHandle(SQL_HANDLE_STMT, st.Ocon, &st.Ost))){
		ex_err(SQL_HANDLE_DBC, st.Ocon, 111, "Error allocating Statement Handle");
	}
}

// Thread safe variants of the above: fetch threads must never call vt_report_error()
std::string odbc_diag ( SQLSMALLINT htype, SQLHANDLE Oh, const char *vtext )
{
	SQLCHAR Oerr_state[6] ;
	SQLINTEGER Oerr_native = 0 ;
	SQLCHAR Oerr_text[MAX_ODBC_ERROR_LEN] ;
	SQLSMALLINT Oln = 0 ;

	Oerr_state[0] = Oerr_text[0] = '\0' ;

	if ( SQLGetDiagRec ( htype, Oh, 1, Oerr_state, &Oerr_native, Oerr_text,
			(SQLSMALLINT)MAX_ODBC_ERROR_LEN, &Oln) != SQL_SUCCESS ) {
		return std::string(vtext) + ". Unable to display ODBC error message" ;
	}
	return std::string(vtext) + ". State " + (char *)Oerr_state + ". Native Code "
		+ std::to_string((long long)Oerr_native) + ". Error text: " + (char *)Oerr_text ;
}

bool odbc_connect_nothrow ( OdbcState &st, const std::string &cid_value, std::string &err )
{
	SQLRETURN Oret = 0 ;

	if (!SQL_SUCCEEDED(Oret=SQLAllocHandle(SQL_HANDLE_ENV, (SQLHANDLE)SQL_NULL_HANDLE, &st.Oenv))){
		err = "Error allocating Environment Handle" ;
		return false ;
	}
	if (!SQL_SUCCEEDED(Oret=SQLSetEnvAttr(st.Oenv, SQL_ATTR_ODBC_VERSION, (void *) SQL_OV_ODBC3, 0))){
		err = "Error setting SQL_OV_ODBC3" ;
		return false ;
	}
	if (!SQL_SUCCEEDED(Oret=SQLAllocHandle(SQL_HANDLE_DBC, st.Oenv, &st.Ocon))){
		err = "Error allocating Connection Handle" ;
		return false ;
	}
	if (!SQL_SUCCEEDED(Oret=SQLDriverConnect(st.Ocon, (SQLHWND)NULL, (SQLCHAR *)cid_value.c_str(), SQL_NTS, NULL, 0, NULL, SQL_DRIVER_NOPROMPT))){
		err = odbc_diag(SQL_HANDLE_DBC, st.Ocon, "Error connecting to target database") ;
		return false ;
	}
	if (!SQL_SUCCEEDED(Oret=SQLAllocHandle(SQL_HANDLE_STMT, st.Ocon, &st.Ost))){
		err = odbc_diag(SQL_HANDLE_DBC, st.Ocon, "Error allocating Statement Handle") ;
		return false ;
	}
	return true ;
}

// C type used to bind a column: must match the SQLBindCol calls in processPartition()
SQLSMALLINT batch_c_type ( SQLSMALLINT Odt, DBs dbt )
{
	switch(Odt) {
		case SQL_SMALLINT:
		case SQL_INTEGER:
		case SQL_TINYINT:
		case SQL_BIGINT:
			return ( dbt == ORACLE ) ? SQL_C_CHAR : SQL_C_SBIGINT ;
		case SQL_REAL:
		case SQL_DOUBLE:
		case SQL_FLOAT:
			return SQL_C_DOUBLE ;
		case SQL_NUMERIC:
		case SQL_DECIMAL:
		case SQL_WCHAR:
		case SQL_WVARCHAR:
		case SQL_WLONGVARCHAR:
		case SQL_CHAR:
		case SQL_VARCHAR:
		case SQL_LONGVARCHAR:
			return SQL_C_CHAR ;
		case SQL_TYPE_TIME:
			return SQL_C_TIME ;
		case SQL_TYPE_DATE:
			return SQL_C_DATE ;
		case SQL_TYPE_TIMESTAMP:
			return SQL_C_TIMESTAMP ;
		case SQL_BIT:
			return SQL_C_BIT ;
		case SQL_BINARY:
		case SQL_VARBINARY:
		case SQL_LONGVARBINARY:
			return SQL_C_BINARY ;
		case SQL_INTERVAL_YEAR_TO_MONTH:
			return SQL_C_INTERVAL_YEAR_TO_MONTH ;
		case SQL_INTERVAL_DAY_TO_SECOND:
			return SQL_C_INTERVAL_DAY_TO_SECOND ;
	}
	return SQL_C_DEFAULT ;
}

class DBLink : public TransformFunction
{

	OdbcState odbc ;			// Per instance ODBC state
	SizedColumnTypes colInfo ;	// Set in setup(), used in processPartition
	DBs dbt ;
	SQLPOINTER *Ores ;     // result array pointers pointer
	SQLLEN **Olen ;        // length array pointers pointer
	StringParsers parser ;
	size_t rowset ;		// Fetch rowset
	std::atomic<bool> cancelled ;
	std::mutex handleMutex ;	// Serializes cancel() with the handle cleanup
	size_t num_threads ;		// Parallel fetch threads, 1 = single threaded
	std::string split_column ;	// Integer column the query is split on
	vint split_min ;
	vint split_max ;
	size_t max_buffer_mb ;		// Budget for the parallel fetch buffers
	size_t max_total_buffer_mb ;// Process wide ceiling shared by every DBLINK() call
	bool parallel ;				// Parallel fetch possible and requested
	std::string conn_str ;		// Connection string reused by the fetch threads
	BatchQueue queue ;
	std::vector<std::thread> producers ;
	std::vector<std::string> producerErr ;	// One slot per producer, read after joining
	std::vector<SQLHSTMT> producerStmts ;	// Live producer statements, guarded by handleMutex
	BufferReservation reservation ;			// Released once the producers are joined

	// Keeps a producer statement reachable from cancel() for as long as it is fetching
	class StmtGuard {
		DBLink &owner ;
		SQLHSTMT stmt ;
	public:
		StmtGuard ( DBLink &o, SQLHSTMT h ) : owner(o), stmt(h) {
			std::lock_guard<std::mutex> guard(owner.handleMutex) ;
			owner.producerStmts.push_back(stmt) ;
		}
		~StmtGuard() {
			std::lock_guard<std::mutex> guard(owner.handleMutex) ;
			for ( size_t i = 0 ; i < owner.producerStmts.size() ; i++ ) {
				if ( owner.producerStmts[i] == stmt ) {
					owner.producerStmts.erase(owner.producerStmts.begin() + i) ;
					break ;
				}
			}
		}
		StmtGuard ( const StmtGuard & ) = delete ;
		StmtGuard &operator= ( const StmtGuard & ) = delete ;
	};

	// Idempotent: safe to call from destroy(), from the destructor and after an error
	void closeHandles()
	{
		std::lock_guard<std::mutex> guard(handleMutex) ;
		odbc.clean() ;
	}

	// Idempotent: no fetch thread may outlive processPartition()
	void joinProducers()
	{
		queue.abort() ;
		for ( size_t i = 0 ; i < producers.size() ; i++ ) {
			if ( producers[i].joinable() )
				producers[i].join() ;
		}
		producers.clear() ;
		queue.drain() ;
		reservation.release() ;
	}

	virtual void setup(ServerInterface &srvInterface, const SizedColumnTypes &argTypes)
	{
		SQLRETURN Oret = 0 ;
		SQLCHAR Obuff[64];
		std::string cid_value = "" ;

		memset(&Obuff[0], 0, sizeof(Obuff));

		// This instance owns its own connection: it must never touch another instance's
		get_connection_info(srvInterface, odbc, cid_value) ;
		odbc_connect(odbc, cid_value) ;

		// Check the DBMS we are connecting to:
		if (!SQL_SUCCEEDED(Oret=SQLGetInfo(odbc.Ocon, SQL_DBMS_NAME,
        	(SQLPOINTER)Obuff, (SQLSMALLINT)sizeof(Obuff), NULL))) {
			ex_err(SQL_HANDLE_DBC, odbc.Ocon, 202, "Error getting remote DBMS Name");
    	}
		if ( !strcmp((char *)Obuff, "Oracle") ) {
			dbt = ORACLE ;
		} else {
			dbt = GENERIC ;
		}

		// ODBC Statement preparation:
		if ( odbc.is_select ) {
			if (!SQL_SUCCEEDED(Oret=SQLPrepare(odbc.Ost, (SQLCHAR *)odbc.query.c_str(), SQL_NTS))) {
				ex_err(SQL_HANDLE_STMT, odbc.Ost, 112, "Error preparing the statement");
    		}
			describe_result_set(srvInterface, odbc, colInfo) ;
		}

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

		// Read/Set parallel fetch Params:
		if( params.containsParameter("num_threads") ) {
			vint threads_param = params.getIntRef("num_threads") ;
			if ( threads_param < 1 || threads_param > MAX_THREADS ) {
				ex_err(0, 0, 204, "DBLINK. Error num_threads out of range");
			} else {
				num_threads = (size_t) threads_param ;
			}
		}
		if( params.containsParameter("max_buffer_mb") ) {
			vint buffer_param = params.getIntRef("max_buffer_mb") ;
			if ( buffer_param < MIN_BUFFER_MB || buffer_param > MAX_BUFFER_MB ) {
				ex_err(0, 0, 205, "DBLINK. Error max_buffer_mb out of range");
			} else {
				max_buffer_mb = (size_t) buffer_param ;
			}
		}
		if( params.containsParameter("max_total_buffer_mb") ) {
			vint total_param = params.getIntRef("max_total_buffer_mb") ;
			if ( total_param < MIN_TOTAL_BUFFER_MB || total_param > MAX_TOTAL_BUFFER_MB ) {
				ex_err(0, 0, 206, "DBLINK. Error max_total_buffer_mb out of range");
			} else {
				max_total_buffer_mb = (size_t) total_param ;
			}
		}
		if( params.containsParameter("split_column") ) {
			split_column = params.getStringRef("split_column").str() ;
			if ( !split_column.empty() && !valid_identifier(split_column) ) {
				vt_report_error(211, "DBLINK. Invalid split_column <%s>. Only a plain unquoted identifier is accepted",
					split_column.c_str());
			}
		}
		if ( num_threads > 1 && !split_column.empty() && odbc.is_select ) {
			if ( params.containsParameter("split_min") && params.containsParameter("split_max") ) {
				split_min = params.getIntRef("split_min") ;
				split_max = params.getIntRef("split_max") ;
				parallel = ( split_min <= split_max ) ;
				if ( !parallel )
					srvInterface.log("DBLINK. split_min is greater than split_max: falling back to a single fetch thread");
			} else if ( !remoteBounds(split_min, split_max) ) {
				srvInterface.log("DBLINK. <%s> did not yield integer MIN/MAX bounds: falling back to a single fetch thread",
					split_column.c_str());
			} else if ( split_min > split_max ) {
				srvInterface.log("DBLINK. <%s> MIN is greater than MAX: falling back to a single fetch thread",
					split_column.c_str());
			} else {
				parallel = true ;
			}
			if ( parallel )
				conn_str = cid_value ;
		}
	}

	// Remote MIN/MAX of the split column, run on the instance connection in setup()
	bool remoteBounds ( vint &bmin, vint &bmax )
	{
		SQLRETURN Oret = 0 ;
		SQLHSTMT Obst = 0 ;
		SQLCHAR Ocmin[MAX_NUMERIC_CHARLEN] ;
		SQLCHAR Ocmax[MAX_NUMERIC_CHARLEN] ;
		SQLLEN Olmin = 0 ;
		SQLLEN Olmax = 0 ;
		bool found = false ;
		std::string bquery = "SELECT MIN(" + split_column + "), MAX(" + split_column +
			") FROM ( " + odbc.query + " ) dblink_split_src" ;

		Ocmin[0] = Ocmax[0] = '\0' ;

		if (!SQL_SUCCEEDED(Oret=SQLAllocHandle(SQL_HANDLE_STMT, odbc.Ocon, &Obst)))
			return false ;
		Oret = SQLExecDirect(Obst, (SQLCHAR *)bquery.c_str(), SQL_NTS) ;
		if ( SQL_SUCCEEDED(Oret) )
			Oret = SQLBindCol(Obst, 1, SQL_C_CHAR, Ocmin, (SQLLEN)sizeof(Ocmin), &Olmin) ;
		if ( SQL_SUCCEEDED(Oret) )
			Oret = SQLBindCol(Obst, 2, SQL_C_CHAR, Ocmax, (SQLLEN)sizeof(Ocmax), &Olmax) ;
		if ( SQL_SUCCEEDED(Oret) )
			Oret = SQLFetch(Obst) ;
		if ( SQL_SUCCEEDED(Oret) && Olmin != SQL_NULL_DATA && Olmax != SQL_NULL_DATA ) {
			found = parse_bound((char *)Ocmin, bmin) && parse_bound((char *)Ocmax, bmax) ;
		}
		(void)SQLCloseCursor(Obst) ;
		(void)SQLFreeHandle(SQL_HANDLE_STMT, Obst) ;
		return found ;
	}

    virtual void cancel(ServerInterface &srvInterface)
    {
		// Called from another thread while processPartition() runs: never free handles here
		cancelled.store(true) ;
		queue.abort() ;
		std::lock_guard<std::mutex> guard(handleMutex) ;
		if ( odbc.Ost ) {
			(void)SQLCancel(odbc.Ost) ;
        }
		for ( size_t i = 0 ; i < producerStmts.size() ; i++ )
			(void)SQLCancel(producerStmts[i]) ;
    }

	// Fetch thread body. It must never touch outputWriter, srvInterface, the Vertica
	// allocator, isCanceled(), vt_report_error() or the parser: those are main thread only
	void producerFetch ( size_t idx, vint lo, vint hi, bool last )
	{
		OdbcState pst ;
		SQLRETURN Oret = 0 ;
		SQLULEN pnfr = 0 ;
		SQLSMALLINT pncol = 0 ;
		const unsigned int ncol = (unsigned int)odbc.Oncol ;
		std::string pquery = "SELECT * FROM ( " + odbc.query + " ) dblink_split_src" ;
		std::string pred = "" ;

		// The outer partitions are deliberately unbounded so that the ranges together cover
		// every value: stale MIN/MAX bounds or rows inserted while the fetch runs would
		// otherwise be returned by no thread at all. Producer 0 also owns the NULL
		// partition, since the range predicate is UNKNOWN for a NULL split value.
		if ( idx == 0 && last ) {
			pred = "" ;
		} else if ( idx == 0 ) {
			pred = "( " + split_column + " < " + std::to_string((long long)hi) + " ) OR " +
				split_column + " IS NULL" ;
		} else if ( last ) {
			pred = split_column + " >= " + std::to_string((long long)lo) ;
		} else {
			pred = split_column + " >= " + std::to_string((long long)lo) + " AND " +
				split_column + " < " + std::to_string((long long)hi) ;
		}
		if ( !pred.empty() )
			pquery += " WHERE " + pred ;

		pst.query_timeout = odbc.query_timeout ;
		if ( !odbc_connect_nothrow(pst, conn_str, producerErr[idx]) )
			return ;

		// Declared after pst so it is destroyed first: the handle must never be freed
		// while cancel() can still reach it
		StmtGuard sguard(*this, pst.Ost) ;

		// Set Statement attributes:
		if (!SQL_SUCCEEDED(Oret=SQLSetStmtAttr(pst.Ost, SQL_ATTR_ROW_BIND_TYPE, (SQLPOINTER)SQL_BIND_BY_COLUMN, 0))) {
			producerErr[idx] = odbc_diag(SQL_HANDLE_STMT, pst.Ost, "Error setting statement attribute SQL_ATTR_ROW_BIND_TYPE") ;
			return ;
		}
		if (!SQL_SUCCEEDED(Oret=SQLSetStmtAttr(pst.Ost, SQL_ATTR_ROW_ARRAY_SIZE, (SQLPOINTER)rowset, 0))) {
			producerErr[idx] = odbc_diag(SQL_HANDLE_STMT, pst.Ost, "Error setting statement attribute SQL_ATTR_ROW_ARRAY_SIZE") ;
			return ;
		}
		if (!SQL_SUCCEEDED(Oret=SQLSetStmtAttr(pst.Ost, SQL_ATTR_ROWS_FETCHED_PTR, &pnfr, 0))) {
			producerErr[idx] = odbc_diag(SQL_HANDLE_STMT, pst.Ost, "Error setting statement attribute SQL_ATTR_ROWS_FETCHED_PTR") ;
			return ;
		}
		// Each producer watches its own statement, and the timer is always joined before
		// pst frees the handle it would cancel
		StmtCancelTimer wdog(pst.Ost, (int)odbc.query_timeout) ;

		if (!SQL_SUCCEEDED(Oret=SQLExecDirect(pst.Ost, (SQLCHAR *)pquery.c_str(), SQL_NTS)) && Oret != SQL_NO_DATA ) {
			producerErr[idx] = wdog.fired() ? "Remote statement canceled: query_timeout exceeded"
				: odbc_diag(SQL_HANDLE_STMT, pst.Ost, "Error executing the statement") ;
			return ;
		}

		// The column metadata comes from the user query, the producer runs a wrapped one
		if (!SQL_SUCCEEDED(Oret=SQLNumResultCols(pst.Ost, &pncol))) {
			producerErr[idx] = odbc_diag(SQL_HANDLE_STMT, pst.Ost, "Error finding the number of resulting columns") ;
			return ;
		}
		if ( (unsigned int)pncol != ncol ) {
			producerErr[idx] = "The split query returns " + std::to_string((long long)pncol) +
				" columns, the described query returns " + std::to_string((long long)ncol) ;
			return ;
		}

		// Fetch loop:
		while ( !cancelled.load() ) {
			Batch b ;
			if ( !queue.getFree(b) && !b.alloc(ncol, odbc.desz, rowset) ) {
				producerErr[idx] = "Error allocating fetch buffers" ;
				return ;
			}
			// A recycled batch has different buffer addresses, so rebind every column
			for ( unsigned int j = 0 ; j < ncol ; j++ ) {
				if (!SQL_SUCCEEDED(Oret=SQLBindCol(pst.Ost, j+1, batch_c_type(odbc.Odt[j], dbt), b.data[j], odbc.desz[j], b.ind[j]))) {
					producerErr[idx] = odbc_diag(SQL_HANDLE_STMT, pst.Ost, "Error binding column") ;
					return ;
				}
			}
			if (!SQL_SUCCEEDED(Oret=SQLFetchScroll(pst.Ost, SQL_FETCH_NEXT, 0))) {
				// A cancelled statement surfaces as HY008 or as an empty fetch: either way the
				// result set is incomplete and must be reported instead of silently truncated
				if ( wdog.fired() )
					producerErr[idx] = "Remote fetch canceled: query_timeout exceeded" ;
				else if ( Oret != SQL_NO_DATA )
					producerErr[idx] = odbc_diag(SQL_HANDLE_STMT, pst.Ost, "Error fetching from the remote database") ;
				return ;
			}
			b.rows = pnfr ;
			if ( !queue.push(b) )
				return ;
		}
	}

	// An exception escaping a thread would terminate the process
	void producerMain ( size_t idx, vint lo, vint hi, bool last )
	{
		try {
			producerFetch(idx, lo, hi, last) ;
		} catch ( std::exception &e ) {
			producerErr[idx] = std::string("Exception in fetch thread: ") + e.what() ;
		} catch ( ... ) {
			producerErr[idx] = "Unknown exception in fetch thread" ;
		}
		queue.producerDone() ;
	}

	// Parallel fetch: num_threads producers, each on its own connection, feeding this
	// thread. Row order is NOT preserved, so num_threads > 1 must not be used when the
	// remote query relies on ORDER BY. Returns false when the process wide buffer budget
	// cannot cover even a minimal parallel fetch, in which case nothing has been started
	// and the caller must use the single threaded path.
	bool parallelFetch ( ServerInterface &srvInterface, PartitionWriter &outputWriter )
	{
		const unsigned int ncol = (unsigned int)odbc.Oncol ;
		size_t nthr = num_threads ;
		uint64_t bytes_per_row = 0 ;
		uint64_t bytes_per_batch = 0 ;
		uint64_t max_bytes = (uint64_t)max_buffer_mb * 1024 * 1024 ;
		uint64_t allowed_threads = 0 ;
		uint64_t inflight = 0 ;
		uint64_t remaining = 0 ;
		uint64_t cap = 0 ;
		uint64_t want_bytes = 0 ;
		uint64_t floor_bytes = 0 ;
		uint64_t granted = 0 ;
		uint64_t span = (uint64_t)split_max - (uint64_t)split_min ;
		uint64_t step = 0 ;
		uint64_t rem = 0 ;
		vint lo = split_min ;
		Batch b ;

		// Oracle returns integers as characters: same sizing the bind loop below does
		if ( dbt == ORACLE ) {
			for ( unsigned int j = 0 ; j < ncol ; j++ ) {
				switch(odbc.Odt[j]) {
					case SQL_SMALLINT:
					case SQL_INTEGER:
					case SQL_TINYINT:
					case SQL_BIGINT:
						odbc.desz[j] = (size_t)(odbc.Ors[j] + 1) ;
						break ;
				}
			}
		}

		// Memory budget. Up to 5 * nthr + 1 batches can be alive at once: 2 * nthr queued,
		// 2 * nthr recycled, one per producer being filled and one held by this thread.
		for ( unsigned int j = 0 ; j < ncol ; j++ )
			bytes_per_row += (uint64_t)odbc.desz[j] + (uint64_t)sizeof(SQLLEN) ;
		bytes_per_batch = bytes_per_row * (uint64_t)rowset ;
		if ( bytes_per_batch == 0 )
			bytes_per_batch = 1 ;
		allowed_threads = max_bytes / ( 3 * bytes_per_batch ) ;
		if ( allowed_threads < 1 )
			allowed_threads = 1 ;
		if ( (uint64_t)nthr > allowed_threads )
			nthr = (size_t)allowed_threads ;
		inflight = ( (uint64_t)nthr + 1 ) * bytes_per_batch ;
		remaining = ( max_bytes > inflight ) ? ( max_bytes - inflight ) : 0 ;
		cap = remaining / ( 2 * bytes_per_batch ) ;
		if ( cap > 2 * (uint64_t)nthr )
			cap = 2 * (uint64_t)nthr ;
		if ( cap < 1 )
			cap = 1 ;

		// max_buffer_mb is per invocation: charge the footprint against the process wide
		// ceiling too, otherwise N concurrent DBLINK() calls would multiply it
		want_bytes = ( (uint64_t)nthr + 1 + 2 * cap ) * bytes_per_batch ;
		floor_bytes = 4 * bytes_per_batch ;	// one thread, queue depth one
		granted = reserve_buffers(want_bytes, floor_bytes,
			(uint64_t)max_total_buffer_mb * 1024 * 1024) ;
		if ( granted == 0 ) {
			srvInterface.log("DBLINK. Parallel fetch needs %lld bytes, the process wide budget of %d MB is exhausted: fetching with a single thread",
				(long long)floor_bytes, (int)max_total_buffer_mb);
			return false ;
		}
		reservation.hold(granted) ;
		if ( granted < want_bytes ) {
			uint64_t batches = granted / bytes_per_batch ;
			uint64_t max_thr = batches - 3 ;	// leaves room for a queue depth of one
			if ( (uint64_t)nthr > max_thr )
				nthr = (size_t)max_thr ;
			cap = ( batches - (uint64_t)nthr - 1 ) / 2 ;
			if ( cap > 2 * (uint64_t)nthr )
				cap = 2 * (uint64_t)nthr ;
			if ( cap < 1 )
				cap = 1 ;
		}
		srvInterface.log("DBLINK. Parallel fetch: %lld bytes per batch, %d threads requested, %d used, queue depth %d, budget %d MB, reserved %lld of %lld bytes against a %d MB process wide ceiling",
			(long long)bytes_per_batch, (int)num_threads, (int)nthr, (int)cap, (int)max_buffer_mb,
			(long long)granted, (long long)want_bytes, (int)max_total_buffer_mb);

		step = span / (uint64_t)nthr ;
		rem = span % (uint64_t)nthr ;

		queue.start((size_t)cap, nthr) ;
		producerErr.assign(nthr, "") ;
		producers.reserve(nthr) ;

		// The producers open their own connections: this one would only sit idle
		{
			std::lock_guard<std::mutex> guard(handleMutex) ;
			odbc.closeConnection() ;
		}

		for ( size_t i = 0 ; i < nthr ; i++ ) {
			vint hi = (vint)((uint64_t)lo + step + ( (uint64_t)i < rem ? 1 : 0 )) ;
			producers.push_back(std::thread(&DBLink::producerMain, this, i, lo, hi, i == nthr - 1)) ;
			lo = hi ;
		}

		while ( queue.pop(b) ) {
			for ( unsigned int i = 0 ; i < b.rows && !isCanceled() && !cancelled.load() ; i++, outputWriter.next() ) {
				for ( unsigned int j = 0 ; j < ncol ; j++ ) {
					SQLPOINTER Odp = (SQLPOINTER)((uint8_t *)b.data[j] + odbc.desz[j] * i) ;
					SQLULEN Odl = (SQLULEN)b.ind[j][i] ;

					if ( (int)Odl == (int)SQL_NULL_DATA ) {
						outputWriter.setNull(j) ;
						continue ;
					}
					writeValue(outputWriter, j, Odp, Odl) ;
				}
			}
			queue.putFree(b) ;
			if ( isCanceled() || cancelled.load() )
				break ;
		}

		joinProducers() ;

		// A cancelled fetch reports HY008: Vertica is already tearing the query down
		if ( cancelled.load() )
			return true ;

		for ( size_t i = 0 ; i < producerErr.size() ; i++ ) {
			if ( !producerErr[i].empty() )
				vt_report_error(409, "DBLINK. Fetch thread %d. %s", (int)i, producerErr[i].c_str());
		}
		return true ;
	}

    virtual void destroy(ServerInterface &srvInterface, const SizedColumnTypes &argTypes)
    {
		closeHandles() ;
    }

	// Single value conversion, shared by the single threaded and the parallel fetch loops
	void writeValue ( PartitionWriter &outputWriter, unsigned int j, SQLPOINTER Odp, SQLULEN Odl )
	{
		switch(odbc.Odt[j]) {
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
							ex_err(0, 0, 404, "Error parsing Numeric");
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
					Odl = (SQLULEN)strnlen((char *)Odp , odbc.desz[j]);
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
						ex_err(0, 0, 405, "Unsupported INTERVAL data type. Expecting SQL_IS_YEAR_TO_MONTH");
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
						ex_err(0, 0, 406, "Unsupported INTERVAL data type. Expecting SQL_IS_DAY_TO_SECOND");
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
				vt_report_error(407, "DBLINK. Unsupported data type for column %u", j);
				break ;
		}
	}

    virtual void processPartition(ServerInterface &srvInterface,
                                  PartitionReader & inputReader,
                                  PartitionWriter & outputWriter)
	{
		SQLRETURN Oret = 0 ;
		SQLPOINTER Odp = 0 ; // Data Element Pointer
		SQLULEN    Odl = 0 ; // Data Element Length
		SQLHSTMT Ost = odbc.Ost ;
		SQLUSMALLINT Oncol = odbc.Oncol ;
		SQLSMALLINT *Odt = odbc.Odt ;
		SQLULEN *Ors = odbc.Ors ;
		size_t *desz = odbc.desz ;
		SQLULEN &nfr = odbc.nfr ;

		try
		{
			// parallelFetch() declines before starting anything when the process wide
			// buffer budget is exhausted: the single threaded path below then runs
			bool fetched = ( odbc.is_select && parallel && parallelFetch(srvInterface, outputWriter) ) ;

			if ( odbc.is_select && !fetched ) {

				// Allocate memory for Result Set and length array pointers:
				Ores = (SQLPOINTER *)srvInterface.allocator->alloc(Oncol * sizeof(SQLPOINTER)) ;
				Olen = (SQLLEN **)srvInterface.allocator->alloc(Oncol * sizeof(SQLLEN *)) ;

				// Allocate space for each column and bind it:
				for ( unsigned int j = 0 ; j < Oncol ; j++ ) {
					Olen[j] = (SQLLEN *)srvInterface.allocator->alloc(sizeof(SQLLEN) * rowset);
					switch(Odt[j]) {
						case SQL_SMALLINT:
						case SQL_INTEGER:
						case SQL_TINYINT:
						case SQL_BIGINT:
							if ( dbt == ORACLE )
								desz[j] = (size_t)(Ors[j] + 1) ;
							Ores[j] = (SQLPOINTER)srvInterface.allocator->alloc(desz[j] * rowset);
							if (!SQL_SUCCEEDED(Oret=SQLBindCol(Ost, j+1, (dbt==ORACLE) ? SQL_C_CHAR : SQL_C_SBIGINT, Ores[j], desz[j], Olen[j]))){
								ex_err(SQL_HANDLE_STMT, Ost, 401, "Error binding column");
							}
							break ;
						case SQL_REAL:
						case SQL_DOUBLE:
						case SQL_FLOAT:
							Ores[j] = (SQLPOINTER)srvInterface.allocator->alloc(desz[j] * rowset);
							if (!SQL_SUCCEEDED(Oret=SQLBindCol(Ost, j+1, SQL_C_DOUBLE, Ores[j], desz[j], Olen[j])))
								ex_err(SQL_HANDLE_STMT, Ost, 401, "Error binding column");
							break ;
						case SQL_NUMERIC:
						case SQL_DECIMAL:
							Ores[j] = (SQLPOINTER)srvInterface.allocator->alloc(desz[j] * rowset);
							if (!SQL_SUCCEEDED(Oret=SQLBindCol(Ost, j+1, SQL_C_CHAR, Ores[j], desz[j], Olen[j])))
								ex_err(SQL_HANDLE_STMT, Ost, 401, "Error binding column");
							break ;
						case SQL_WCHAR:
						case SQL_WVARCHAR:
						case SQL_WLONGVARCHAR:
						case SQL_CHAR:
						case SQL_VARCHAR:
						case SQL_LONGVARCHAR:
							Ores[j] = (SQLPOINTER)srvInterface.allocator->alloc(desz[j] * rowset);
							if (!SQL_SUCCEEDED(Oret=SQLBindCol(Ost, j+1, SQL_C_CHAR, Ores[j], desz[j], Olen[j])))
								ex_err(SQL_HANDLE_STMT, Ost, 401, "Error binding column");
							break ;
						case SQL_TYPE_TIME:
							Ores[j] = (SQLPOINTER)srvInterface.allocator->alloc(desz[j] * rowset);
							if (!SQL_SUCCEEDED(Oret=SQLBindCol(Ost, j+1, SQL_C_TIME, Ores[j], desz[j], Olen[j])))
								ex_err(SQL_HANDLE_STMT, Ost, 401, "Error binding column");
							break ;
						case SQL_TYPE_DATE:
							Ores[j] = (SQLPOINTER)srvInterface.allocator->alloc(desz[j] * rowset);
							if (!SQL_SUCCEEDED(Oret=SQLBindCol(Ost, j+1, SQL_C_DATE, Ores[j], desz[j], Olen[j])))
								ex_err(SQL_HANDLE_STMT, Ost, 401, "Error binding column");
							break ;
						case SQL_TYPE_TIMESTAMP:
							Ores[j] = (SQLPOINTER)srvInterface.allocator->alloc(desz[j] * rowset);
							if (!SQL_SUCCEEDED(Oret=SQLBindCol(Ost, j+1, SQL_C_TIMESTAMP, Ores[j], desz[j], Olen[j])))
								ex_err(SQL_HANDLE_STMT, Ost, 401, "Error binding column");
							break ;
						case SQL_BIT:
							Ores[j] = (SQLPOINTER)srvInterface.allocator->alloc(desz[j] * rowset);
							if (!SQL_SUCCEEDED(Oret=SQLBindCol(Ost, j+1, SQL_C_BIT, Ores[j], desz[j], Olen[j])))
								ex_err(SQL_HANDLE_STMT, Ost, 401, "Error binding column");
							break ;
						case SQL_BINARY:
						case SQL_VARBINARY:
						case SQL_LONGVARBINARY:
							Ores[j] = (SQLPOINTER)srvInterface.allocator->alloc(desz[j] * rowset);
							if (!SQL_SUCCEEDED(Oret=SQLBindCol(Ost, j+1, SQL_C_BINARY, Ores[j], desz[j], Olen[j])))
								ex_err(SQL_HANDLE_STMT, Ost, 401, "Error binding column");
							break ;
						case SQL_INTERVAL_YEAR_TO_MONTH:
							// FIX: support this data type
							Ores[j] = (SQLPOINTER)srvInterface.allocator->alloc(desz[j] * rowset);
							if (!SQL_SUCCEEDED(Oret=SQLBindCol(Ost, j+1, SQL_C_INTERVAL_YEAR_TO_MONTH, Ores[j], desz[j], Olen[j])))
								ex_err(SQL_HANDLE_STMT, Ost, 401, "Error binding column");
							break ;
						case SQL_INTERVAL_DAY_TO_SECOND:
							// FIX: support this data type
							Ores[j] = (SQLPOINTER)srvInterface.allocator->alloc(desz[j] * rowset);
							if (!SQL_SUCCEEDED(Oret=SQLBindCol(Ost, j+1, SQL_C_INTERVAL_DAY_TO_SECOND, Ores[j], desz[j], Olen[j])))
								ex_err(SQL_HANDLE_STMT, Ost, 401, "Error binding column");
							break;
					}
				}
				// Set Statement attributes:
				if (!SQL_SUCCEEDED(Oret=SQLSetStmtAttr(Ost, SQL_ATTR_ROW_BIND_TYPE, (SQLPOINTER)SQL_BIND_BY_COLUMN, 0))) {
					ex_err(SQL_HANDLE_STMT, Ost, 402, "Error setting statement attribute SQL_ATTR_ROW_BIND_TYPE");
				}
				if (!SQL_SUCCEEDED(Oret=SQLSetStmtAttr(Ost, SQL_ATTR_ROW_ARRAY_SIZE, (SQLPOINTER)rowset, 0))) {
					ex_err(SQL_HANDLE_STMT, Ost, 402, "Error setting statement attribute SQL_ATTR_ROW_ARRAY_SIZE");
				}
				if (!SQL_SUCCEEDED(Oret=SQLSetStmtAttr(Ost, SQL_ATTR_ROWS_FETCHED_PTR, &nfr, 0))) {
					ex_err(SQL_HANDLE_STMT, Ost, 402, "Error setting statement attribute SQL_ATTR_ROWS_FETCHED_PTR");
				}

				// The watchdog scope ends with the fetch loop, so the timer is joined well
				// before closeHandles() can free the statement it would cancel
				StmtCancelTimer wdog(Ost, (int)odbc.query_timeout) ;

				// Execute Stateent:
				if (!SQL_SUCCEEDED(Oret=SQLExecute(Ost)) && Oret != SQL_NO_DATA ) {
					ex_err(SQL_HANDLE_STMT, Ost, 403, "Error executing the statement");
				}

				// Fetch loop:
				while ( SQL_SUCCEEDED(Oret=SQLFetchScroll(Ost, SQL_FETCH_NEXT, 0)) && !isCanceled() && !cancelled.load() ) {
					for ( unsigned int i = 0 ; i < nfr && !isCanceled() && !cancelled.load() ; i++, outputWriter.next() ) {
						for ( unsigned int j = 0 ; j < Oncol ; j++ ) {
							Odp = (SQLPOINTER)((uint8_t *)Ores[j] + desz[j] * i) ;
							Odl = Olen[j][i] ;
							
							if ( (int)Odl == (int)SQL_NULL_DATA ) {
								outputWriter.setNull(j) ;
								continue ;
							}
							writeValue(outputWriter, j, Odp, Odl) ;
						}
					}
				}
				if ( wdog.fired() ) {
					ex_err(0, 0, 410, "Remote statement canceled: query_timeout exceeded");
				}
			} else if ( !odbc.is_select ) {
				StmtCancelTimer wdog(Ost, (int)odbc.query_timeout) ;

				if (!SQL_SUCCEEDED(Oret=SQLExecDirect (Ost, (SQLCHAR *)odbc.query.c_str(), SQL_NTS))) {
					ex_err(SQL_HANDLE_STMT, Ost, 408, "Error executing statement");
				}
				outputWriter.setInt(0, (vint)Oret) ;
				outputWriter.next() ;
			}
			closeHandles() ;
		}
		catch (exception& e)
		{
			joinProducers();
			closeHandles();
			vt_report_error(400, "Exception while processing partition: [%s]", e.what());
		}
	}

public:

	DBLink() : dbt(GENERIC), Ores(0), Olen(0), rowset(DEF_ROWSET), cancelled(false),
		num_threads(DEF_THREADS), split_min(0), split_max(0), max_buffer_mb(DEF_BUFFER_MB),
		max_total_buffer_mb(DEF_TOTAL_BUFFER_MB), parallel(false) {}

	virtual ~DBLink() { joinProducers() ; closeHandles() ; }
};

class DBLinkFactory : public TransformFunctionFactory
{
	virtual void getPrototype(ServerInterface &srvInterface,
                              ColumnTypes &argTypes,
                              ColumnTypes &returnType )
	{
		returnType.addAny();
	}
	virtual void getReturnType(ServerInterface &srvInterface,
                               const SizedColumnTypes &inputTypes,
                               SizedColumnTypes &outputTypes )
	{
		SQLRETURN Oret = 0 ;
		std::string cid_value = "" ;
		OdbcState st ;	// Short lived planning connection, closed by its destructor

		get_connection_info(srvInterface, st, cid_value) ;
		odbc_connect(st, cid_value) ;

		if ( st.is_select ) {
			if (!SQL_SUCCEEDED(Oret=SQLPrepare(st.Ost, (SQLCHAR *)st.query.c_str(), SQL_NTS))) {
				ex_err(SQL_HANDLE_STMT, st.Ost, 112, "Error preparing the statement");
    		}
			describe_result_set(srvInterface, st, outputTypes) ;
		} else {
			outputTypes.addInt("dblink") ;
		}
	}

    virtual void getParameterType(ServerInterface &srvInterface, SizedColumnTypes &parameterTypes)
	{
		parameterTypes.addVarchar(1024, "cid",  { true, false, false, "Connection Identifier Database. Identifies an entry in the connection identifier database." });
		parameterTypes.addVarchar(1024, "connect",  { true, false, false, "The ODBC connection string containing the DSN and credentials." });
		parameterTypes.addVarchar(1024, "connect_secret",  { true, false, false, "The ODBC connection string containing the DSN and credentials." });
		parameterTypes.addVarchar(1024, "cidfile",  { true, false, false, "Connection Identifier File Path." });
		parameterTypes.addVarchar(65000, "query",  { true, false, false, "The query being pushed on the remote database. Or, '@' followed by the name of the file containing the query." });
		parameterTypes.addInt("rowset",  { true, false, false, "Number of rows retrieved from the remote database during each SQLFetch() cycle. Default is 100." });
		parameterTypes.addInt("num_threads",  { true, false, false, "Number of parallel fetch threads, from 1 to 16. Default is 1. Values above 1 require split_column and do not preserve row order." });
		parameterTypes.addVarchar(1024, "split_column",  { true, false, false, "Integer column of the query the parallel fetch threads split their range on." });
		parameterTypes.addInt("split_min",  { true, false, false, "Lower bound of split_column. Default is MIN(split_column) read from the remote database." });
		parameterTypes.addInt("split_max",  { true, false, false, "Upper bound of split_column. Default is MAX(split_column) read from the remote database." });
		parameterTypes.addInt("max_buffer_mb",  { true, false, false, "Fetch buffer budget in MB for this call, 16 to 8192. Default 256. Threads or queue depth are reduced to fit." });
		parameterTypes.addInt("max_total_buffer_mb",  { true, false, false, "Fetch buffer ceiling in MB shared by all DBLINK calls in the process, 64 to 65536. Default 4096." });
		parameterTypes.addInt("query_timeout",  { true, false, false, "Seconds a remote statement may run before it is cancelled, 0 to 86400. Default 0, meaning unlimited." });
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
