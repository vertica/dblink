#
# dblink Makefile
# usage: make VERTICA_VERSION=<major.minor.patch> OSTAG=<ubuntu|centos>
#

SHELL=/bin/bash # because WSL defaults to /bin/sh

# build local unless VERSION_TAG and OSTAG are speified
VERSION_TAG=local
ifdef OSTAG
ifdef VERTICA_VERSION
export OSTAG
export VERTICA_VERSION
VERSION_TAG=$(OSTAG)-v$(VERTICA_VERSION)
VERTICA_SDK_IMAGE=vertica/verticasdk:$(VERSION_TAG)
LOCAL_IMAGE=dblink_builder:$(VERSION_TAG)
endif
endif

CXX = g++
DOCKER = docker
CXXDOCKER = $(DOCKER) run --rm -u "$(shell id -u):$(shell id -g)" -w "$(PWD)" -v "$(PWD):$(PWD):rw" $(LOCAL_IMAGE) g++
CXXFLAGS = -O3 -D HAVE_LONG_INT_64 -Wall -std=c++11 -shared -Wno-unused-value -D_GLIBCXX_USE_CXX11_ABI=0 -fPIC
INCPATH = -I/opt/vertica/sdk/include -I/opt/vertica/sdk/examples/HelperLibraries
VERPATH = /opt/vertica/sdk/include/Vertica.cpp
UDXLIBNAME = ldblink
UDXLIB = $(shell pwd)/$(UDXLIBNAME).so
UDXSRC = $(UDXLIBNAME).cpp
COMPILE_VERSION_LE_12_0_4 = $(CXXDOCKER) $(CXXFLAGS) $(INCPATH) -o $@ $< $(VERPATH) -lodbc

CXXDOCKER_FOR_NEW_SDK = $(DOCKER) run --rm -u "$(shell id -u):$(shell id -g)" -w "$(PWD)" -v "$(PWD):$(PWD):rw" $(LOCAL_IMAGE) "$(PWD)"
CXXCMD = g++ $(CXXFLAGS)
COMPILE_VERSION_GREATER_THAN_12_0_4 = $(CXXDOCKER_FOR_NEW_SDK) "$(CXXCMD) $(INCPATH) -o $@ $< $(VERPATH) -lodbc"
OLD_SDK_MAX_VERSION = 12.0.4
GETMINVERSION = echo "$(VERTICA_VERSION) $(OLD_SDK_MAX_VERSION)" | tr " " "\n" | sort -V | head -n 1
MINVERSION=$(shell $(GETMINVERSION))

$(UDXLIB): $(UDXLIB).$(VERSION_TAG)
	@ln -snf $< $@

$(UDXLIB).local: $(UDXSRC) ## Creates ldblink.so.local which is the binary from your local build environment
	@echo $(CXX) $(CXXFLAGS) $(INCPATH) -o $@ $< $(VERPATH) -lodbc
	@$(CXX) $(CXXFLAGS) $(INCPATH) -o $@ $< $(VERPATH) -lodbc || \
	  if [[ ! -r /opt/vertica/sdk/include/Vertica.cpp ]] || ! type -p $(CXX) >/dev/null 2>&1 ; then \
	    echo "usage: make VERTICA_VERSION=<major.minor.patch> OSTAG=<ubuntu|centos>" >&2; \
	    exit 1; \
	  else \
	    echo "to build with docker: make VERTICA_VERSION=<major.minor.patch> OSTAG=<ubuntu|centos>" >&2; \
	    exit 1; \
	  fi

$(UDXLIB).$(OSTAG)-v$(VERTICA_VERSION): $(UDXSRC) ## Creates binaries in the format ldblink.so.OS-vVERTICA_VERSION by using docker conatainers
	@$(MAKE) .container.$(OSTAG)-v$(VERTICA_VERSION)
ifeq ($(MINVERSION), $(VERTICA_VERSION))
	$(COMPILE_VERSION_LE_12_0_4)
else
	$(COMPILE_VERSION_GREATER_THAN_12_0_4)
endif

.PHONY: check
check:
	@if ! type -p $(DOCKER) >/dev/null 2>&1 ; then \
	  echo "Cannot find docker.  To build with the local sdk in" >&2; \
	  echo "/opt/vertica, unset VERSION_VERTICA and OSTAG" >&2; \
	fi
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
container: check .container.$(OSTAG)-v$(VERTICA_VERSION)
	@if ! $(DOCKER) image inspect $(LOCAL_IMAGE) >/dev/null 2>&1; then \
	  rm .container.$(OSTAG)-v$(VERTICA_VERSION) ; \
	  make .container.$(OSTAG)-v$(VERTICA_VERSION) ; \
	fi

.container.$(OSTAG)-v$(VERTICA_VERSION): tests/odbc.ini tests/odbcinst.ini tests/dblink.cids
	@$(DOCKER) build -f Dockerfile_$(OSTAG) -t $(LOCAL_IMAGE) --build-arg=IMAGE=$(VERTICA_SDK_IMAGE) .
	@touch .container.$(OSTAG)-v$(VERTICA_VERSION)

install: $(UDXLIB)
	@echo " \
	    CREATE OR REPLACE LIBRARY $(UDXLIBNAME) AS '$(UDXLIB)' LANGUAGE 'C++'; \
	    CREATE OR REPLACE TRANSFORM FUNCTION dblink AS LANGUAGE 'C++' NAME 'DBLinkFactory' LIBRARY $(UDXLIBNAME) ; \
		GRANT EXECUTE ON TRANSFORM FUNCTION dblink() TO PUBLIC ; \
		GRANT USAGE ON LIBRARY $(UDXLIBNAME) TO PUBLIC ; \
	" | vsql -U dbadmin  -X -f - -e

uninstall:
	@echo " \
	    DROP LIBRARY $(UDXLIBNAME) CASCADE ; \
	" | vsql -U dbadmin  -X -f - -e

clean: check
	@$(DOCKER) image rm $(LOCAL_IMAGE) || true
	@ rm -f .container.$(OSTAG)-v$(VERTICA_VERSION)
	#$(DOCKER) image rm $(VERTICA_SDK_IMAGE) || true

# build all versions for release purposes
release:
	@for i in $$(curl https://hub.docker.com/v2/namespaces/vertica/repositories/verticasdk/tags | perl -nE 'print join "\n",m/(?:ubuntu|centos)-v\d+\.\d+\.\d+/g') ; do \
	  $(MAKE) VERTICA_VERSION="$${i##*-v}" OSTAG="$${i%%-v*}"  || ((errors++));\
	done; \
	((errors==0)) # return an error if there are errors

.PHONY: test
test: $(UDXLIB).$(VERSION_TAG) .container.$(OSTAG)-v$(VERTICA_VERSION)
	@cd tests; OSTAG=$(OSTAG) VERTICA_VERSION=$(VERTICA_VERSION) ./test_script.sh

