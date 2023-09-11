#
# dblink Makefile
# usage: make VERTICA_VERSION=<major.minor.patch> OSTAG=<ubuntu|centos>
#

SHELL=/bin/bash # because WSL defaults to /bin/sh

CURRENT_DIR = $(shell pwd)
REPO_DIR = $(shell cd ../ && pwd)

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
CXXFLAGS = -O3 -D HAVE_LONG_INT_64 -Wall -std=c++11 -shared -Wno-unused-value -DODBC64 -D_GLIBCXX_USE_CXX11_ABI=0 -fPIC
UDXLIBNAME = ldblink
UDXSRC = $(REPO_DIR)/$(UDXLIBNAME).cpp
UDXLIB = $(CURRENT_DIR)/$(UDXLIBNAME).so
INCPATH = -I/opt/vertica/sdk/include -I/opt/vertica/sdk/examples/HelperLibraries
VERPATH = /opt/vertica/sdk/include/Vertica.cpp

DOCKER = docker
CXXDOCKER = $(DOCKER) run --rm -u "$(shell id -u):$(shell id -g)" -w "$(REPO_DIR)" -v "$(REPO_DIR):$(REPO_DIR):rw" $(LOCAL_IMAGE) $(CXX)
COMPILE_VERSION_LE_12_0_4 = $(CXXDOCKER) $(CXXFLAGS) $(INCPATH) -o $@ $< $(VERPATH) -lodbc
CXXDOCKER_FOR_NEW_SDK = $(DOCKER) run --rm -u "$(shell id -u):$(shell id -g)" -w "$(REPO_DIR)" -v "$(REPO_DIR):$(REPO_DIR):rw" $(LOCAL_IMAGE) "$(REPO_DIR)"
CXXCMD = g++ $(CXXFLAGS)
COMPILE_VERSION_GREATER_THAN_12_0_4 = $(CXXDOCKER_FOR_NEW_SDK) "$(CXXCMD) $(INCPATH) -o $@ $< $(VERPATH) -lodbc"
OLD_SDK_MAX_VERSION = 12.0.4
GETMINVERSION = echo "$(VERTICA_VERSION) $(OLD_SDK_MAX_VERSION)" | tr " " "\n" | sort -V | head -n 1
MINVERSION=$(shell $(GETMINVERSION))

all: compile

debug: CXXFLAGS += -DDBLINK_DEBUG=1
debug: compile

compile: $(UDXLIB).$(VERSION_TAG) ## Compile dblink with verticasdk docker containers

$(UDXLIB).$(OSTAG)-v$(VERTICA_VERSION): $(UDXSRC) ## Creates binaries in the format ldblink.so.OS-vVERTICA_VERSION by using docker containers
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

clean-docker: check
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

