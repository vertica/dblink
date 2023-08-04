DBLINK Version 0.3.0 (10 May 2023)

* text and binary column length is now limited to the max Vertica supported length
* added support for connect_secret function parameter
* added support for dblink_secret session parameter
* added install_unfenced in the Makefile (getReturnType is called once if unfenced)
* it is now possible to set env variables through the UDx
* added non-Western language test

DBLINK Version 0.2.2 (25 Jan 2023)

* fixed wrong completion in DriverConnect
* replaced SQLFetch() with SQLFetchScroll()
* added ODBC64 in Makefile

DBLINK Version 0.2.1 (16 Dec 2022)

* New connection method via ``connect_secret`` SESSION PARAMETER

DBLINK Version 0.2.0 (01 Dec 2022)

* Fixed a bug with database connections being left open
* Improved error messages
* New section in the README on "How to report issues"
* Created a CHANGELOG


DBLINK Version 0.1.0 (13 Apr 2022)

* Initial Release
