#
# dblink Makefile
# usage: make VERTICA_VERSION=<major.minor.patch> OSTAG=<ubuntu|centos>
#

SHELL=/bin/bash # because WSL defaults to /bin/sh
VERTICA_SDK_IMAGE=vertica/verticasdk:$(OSTAG)-v$(VERTICA_VERSION)
LOCAL_IMAGE=dblink_builder:$(OSTAG)-v$(VERTICA_VERSION)
LOCAL_CONTAINER=dblink_builder
CXX = docker run --rm -u "$(shell id -u):$(shell id -g)" -w "$(PWD)" -v "$(PWD):$(PWD):rw" $(LOCAL_IMAGE) g++
CXXFLAGS = -O3 -D HAVE_LONG_INT_64 -Wall -std=c++11 -shared -Wno-unused-value -D_GLIBCXX_USE_CXX11_ABI=0 -fPIC
INCPATH = -I/opt/vertica/sdk/include -I/opt/vertica/sdk/examples/HelperLibraries
VERPATH = /opt/vertica/sdk/include/Vertica.cpp
UDXLIBNAME = ldblink
UDXLIB = $(UDXLIBNAME).so
UDXSRC = $(UDXLIBNAME).cpp

$(UDXLIB): $(UDXLIB).$(OSTAG)-v$(VERTICA_VERSION)
	@ln -snf $< $@

$(UDXLIB).$(OSTAG)-v$(VERTICA_VERSION): $(UDXSRC) container
	$(CXX) $(CXXFLAGS) $(INCPATH) -o $@ $< $(VERPATH) -lodbc

.PHONY: check
check:
	@if ! [[ "$(VERTICA_VERSION)" =~ ^[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*$$ ]]; then \
	  echo "VERTICA_VERSION must be set to the version of Vertica, i.e.  12.0.2" >&2; \
	  exit 1; \
	fi
	@if ! [[ "$(OSTAG)" =~ ^(centos|ubuntu)$$ ]]; then \
	  echo "OSTAG must be set to either centos or ubuntu" >&2; \
	  exit 1; \
	fi
	@echo compiling for $(OSTAG) and Vertica version $(VERTICA_VERSION)

.PHONY: container
container: check
	if ! docker image inspect $(LOCAL_IMAGE) >/dev/null 2>&1; then \
	  docker container rm $(LOCAL_CONTAINER) 2>/dev/null ; \
	  if [[ $(OSTAG) == "centos" ]]; then \
	    docker run --name $(LOCAL_CONTAINER) -u 0 $(VERTICA_SDK_IMAGE) bash -c "yum install -y unixODBC-devel;" || exit 1; \
	  else \
	    docker run --name $(LOCAL_CONTAINER) -u 0 $(VERTICA_SDK_IMAGE) bash -c "apt-get update && apt-get install -y unixodbc-dev" || exit 1; \
	  fi ; \
	  docker container commit --change "USER dbadmin" $(LOCAL_CONTAINER) $(LOCAL_IMAGE); \
	fi
	@docker container rm $(LOCAL_CONTAINER) 2>/dev/null || true

install: $(UDXLIB)
	@echo " \
	    CREATE OR REPLACE LIBRARY $(UDXLIBNAME) AS '$(UDXLIB)' LANGUAGE 'C++'; \
	    CREATE OR REPLACE TRANSFORM FUNCTION dblink AS LANGUAGE 'C++' NAME 'DBLinkFactory' LIBRARY $(UDXLIBNAME) ; \
		GRANT EXECUTE ON TRANSFORM FUNCTION dblink() TO PUBLIC ; \
	" | vsql -U dbadmin  -X -f - -e

uninstall:
	@echo " \
	    DROP LIBRARY $(UDXLIBNAME) CASCADE ; \
	" | vsql -U dbadmin  -X -f - -e

clean: check
	docker container rm $(LOCAL_CONTAINER) || true
	docker image rm $(LOCAL_IMAGE) || true
	#docker image rm $(VERTICA_SDK_IMAGE) || true
