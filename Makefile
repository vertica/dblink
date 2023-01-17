#
# dblink Makefile
# usage: make VERTICA_VERSION=<major.minor.patch> OSTAG=<ubuntu|centos>
#

SHELL=/bin/bash # because WSL defaults to /bin/sh

# build local unless VERSION_TAG and OSTAG are speified
VERSION_TAG=local
ifdef OSTAG
ifdef VERTICA_VERSION
VERSION_TAG=$(OSTAG)-v$(VERTICA_VERSION)
VERTICA_SDK_IMAGE=vertica/verticasdk:$(VERSION_TAG)
LOCAL_IMAGE=dblink_builder:$(VERSION_TAG)
endif
endif

LOCAL_CONTAINER=dblink_builder
CXX = g++
DOCKER = docker
CXXDOCKER = $(DOCKER) run --rm -u "$(shell id -u):$(shell id -g)" -w "$(PWD)" -v "$(PWD):$(PWD):rw" $(LOCAL_IMAGE) g++
CXXFLAGS = -O3 -D HAVE_LONG_INT_64 -Wall -std=c++11 -shared -Wno-unused-value -D_GLIBCXX_USE_CXX11_ABI=0 -fPIC
INCPATH = -I/opt/vertica/sdk/include -I/opt/vertica/sdk/examples/HelperLibraries
VERPATH = /opt/vertica/sdk/include/Vertica.cpp
UDXLIBNAME = ldblink
UDXLIB = $(UDXLIBNAME).so
UDXSRC = $(UDXLIBNAME).cpp

$(UDXLIB): $(UDXLIB).$(VERSION_TAG)
	@ln -snf $< $@

$(UDXLIB).local: $(UDXSRC)
	@echo $(CXX) $(CXXFLAGS) $(INCPATH) -o $@ $< $(VERPATH) -lodbc
	@$(CXX) $(CXXFLAGS) $(INCPATH) -o $@ $< $(VERPATH) -lodbc || \
	  if [[ ! -r /opt/vertica/sdk/include/Vertica.cpp ]] || ! type -p $(CXX) >/dev/null 2>&1 ; then \
	    echo "usage: make VERTICA_VERSION=<major.minor.patch> OSTAG=<ubuntu|centos>" >&2; \
	    exit 1; \
	  else \
	    echo "to build with docker: make VERTICA_VERSION=<major.minor.patch> OSTAG=<ubuntu|centos>" >&2; \
	    exit 1; \
	  fi

$(UDXLIB).$(OSTAG)-v$(VERTICA_VERSION): check container

$(UDXLIB).$(OSTAG)-v$(VERTICA_VERSION): $(UDXSRC)
	$(CXXDOCKER) $(CXXFLAGS) $(INCPATH) -o $@ $< $(VERPATH) -lodbc

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
container: check
	if ! $(DOCKER) image inspect $(LOCAL_IMAGE) >/dev/null 2>&1; then \
	  $(DOCKER) container rm $(LOCAL_CONTAINER) 2>/dev/null ; \
	  if [[ $(OSTAG) == "centos" ]]; then \
	    $(DOCKER) run --name $(LOCAL_CONTAINER) -u 0 $(VERTICA_SDK_IMAGE) bash -c "yum install -y unixODBC-devel;" || exit 1; \
	  else \
	    $(DOCKER) run --name $(LOCAL_CONTAINER) -u 0 $(VERTICA_SDK_IMAGE) bash -c "apt-get update && apt-get install -y unixodbc-dev" || exit 1; \
	  fi ; \
	  $(DOCKER) container commit --change "USER dbadmin" $(LOCAL_CONTAINER) $(LOCAL_IMAGE); \
	fi
	@$(DOCKER) container rm $(LOCAL_CONTAINER) 2>/dev/null || true

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
	$(DOCKER) container rm $(LOCAL_CONTAINER) || true
	$(DOCKER) image rm $(LOCAL_IMAGE) || true
	#$(DOCKER) image rm $(VERTICA_SDK_IMAGE) || true

# build all versions for release purposes
release:
	@for i in $$(curl https://hub.docker.com/v2/namespaces/vertica/repositories/verticasdk/tags | perl -nE 'print join "\n",m/(?:ubuntu|centos)-v\d+\.\d+\.\d+/g') ; do \
	  $(MAKE) VERTICA_VERSION="$${i##*-v}" OSTAG="$${i%%-v*}" ;\
	done

test:
	@echo "[[ tests go here ]]"

