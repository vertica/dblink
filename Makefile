CXX = g++
CXXFLAGS = -O3 -D HAVE_LONG_INT_64 -Wall -std=c++11 -shared -Wno-unused-value -DODBC64 -D_GLIBCXX_USE_CXX11_ABI=0 -fPIC 
INCPATH = -I/opt/vertica/sdk/include -I/opt/vertica/sdk/examples/HelperLibraries
VERPATH = /opt/vertica/sdk/include/Vertica.cpp
UDXLIBNAME = ldblink
UDXLIB = /tmp/$(UDXLIBNAME).so
UDXSRC = $(UDXLIBNAME).cpp

all: compile

debug: CXXFLAGS += -DDBLINK_DEBUG=1
debug: compile

compile: $(UDXSRC)
	$(CXX) $(CXXFLAGS) $(INCPATH) -o $(UDXLIB) $(UDXSRC) $(VERPATH) -lodbc

install: $(UDXLIB)
	@echo " \
	    CREATE OR REPLACE LIBRARY $(UDXLIBNAME) AS '$(UDXLIB)' LANGUAGE 'C++'; \
	    CREATE OR REPLACE TRANSFORM FUNCTION dblink AS LANGUAGE 'C++' NAME 'DBLinkFactory' LIBRARY $(UDXLIBNAME) ; \
		GRANT EXECUTE ON TRANSFORM FUNCTION dblink() TO PUBLIC ; \
		GRANT USAGE ON LIBRARY $(UDXLIBNAME) TO PUBLIC ; \
	" | vsql -U dbadmin  -X -f - -e

install_unfenced: $(UDXLIB)
	@echo " \
	    CREATE OR REPLACE LIBRARY $(UDXLIBNAME) AS '$(UDXLIB)' LANGUAGE 'C++'; \
	    CREATE OR REPLACE TRANSFORM FUNCTION dblink AS LANGUAGE 'C++' NAME 'DBLinkFactory' LIBRARY $(UDXLIBNAME) NOT FENCED ; \
		GRANT EXECUTE ON TRANSFORM FUNCTION dblink() TO PUBLIC ; \
		GRANT USAGE ON LIBRARY $(UDXLIBNAME) TO PUBLIC ; \
	" | vsql -U dbadmin  -X -f - -e
clean:
	@echo " \
	    DROP LIBRARY $(UDXLIBNAME) CASCADE ; \
	" | vsql -U dbadmin  -X -f - -e