#
# SPIMAP (SPecies Informed Max A Posteriori gene tree reconstruction)
# Matt Rasmussen
# Copyright 2007-2011
# modified for WGD by Charles Elie Rabier
# Makefile
#

# install prefix paths
prefix = /usr


# C++ compiler options
CXX = g++
#CXX = /s/gcc-4.6.1/bin/g++
# compiler above: problem later:
# "/usr/lib64/libstdc++.so.6: version `GLIBCXX_3.4.15' not found"
# problem with various gcc compilers: long list of errors at the end of compiling.

CFLAGS := $(CFLAGS) \
    -Wall -fPIC \
    -Isrc

# GSL is the only third party dependency of the SPIMAP C++
# If GSL is not automatically detected you can manually specify its location
# using this varibale
GSL_LIBS=`gsl-config --libs`

# example of hard coding GSL location
#GSL_LIBS=-L/usr/lib -lgsl -lgslcblas -lm


#=============================================================================
# optional CFLAGS

# profiling
ifdef PROFILE
	CFLAGS := $(CFLAGS) -pg
endif

# debugging
ifdef DEBUG	
	CFLAGS := $(CFLAGS) -g
else
	CFLAGS := $(CFLAGS) -O3
endif


#=============================================================================
# SPIMAP program files

# package
PKG_VERSION=1.2
PKG_NAME=spimap
SPIMAP_PKG=dist/$(PKG_NAME)-$(PKG_VERSION).tar.gz
PKG_DIR=dist/$(PKG_NAME)-$(PKG_VERSION)

# program files
SPIMAP_PROG = bin/spimap
SPIMAP_DEBUG = bin/spimap-debug
SCRIPTS =  bin/spimap-prep-rates \
           bin/spimap-train-rates \
           bin/spimap-prep-duploss \
           bin/spimap-train-duploss \
           bin/spimap-sim \
           bin/viewtree
BINARIES = $(SPIMAP_PROG) $(SCRIPTS)

SPIDIR_SRC = \
    src/birthdeath.cpp \
    src/birthdeath_ml.cpp \
    src/branch_prior.cpp \
    src/branch_prior_train.cpp \
    src/common.cpp \
    src/distmatrix.cpp \
    src/gamma.cpp \
    src/hky.cpp \
    src/logging.cpp \
    src/model.cpp \
    src/model_params.cpp \
    src/newick.cpp \
    src/nj.cpp \
    src/parsimony.cpp \
    src/parsing.cpp \
    src/phylogeny.cpp \
    src/search.cpp \
    src/seq.cpp \
    src/seq_likelihood.cpp \
    src/Sequences.cpp \
    src/top_change.cpp \
    src/top_prior.cpp \
    src/top_prior_extra.cpp \
    src/Tree.cpp \
    src/treevis.cpp \
    src/WGD.cpp

SPIDIR_OBJS = $(SPIDIR_SRC:.cpp=.o)

PROG_SRC = src/spimap.cpp 
PROG_OBJS = src/spimap.o $(SPIDIR_OBJS)
PROG_LIBS = $(GSL_LIBS)


#=======================
# SPIDIR C-library files
LIBSPIDIR = lib/libspidir.a
LIBSPIDIR_SHARED_NAME = libspidir.so
LIBSPIDIR_SHARED = lib/$(LIBSPIDIR_SHARED_NAME)
LIBSPIDIR_SHARED_INSTALL = $(prefix)/lib/$(LIBSPIDIR_SHARED_NAME)
LIBSPIDIR_OBJS = $(SPIDIR_OBJS)


#=============================================================================
# targets

# default targets
all: $(SPIMAP_PROG) $(LIBSPIDIR) $(LIBSPIDIR_SHARED)

debug: $(SPIMAP_DEBUG)

# SPIDIR stand-alone program
$(SPIMAP_PROG): $(PROG_OBJS) 
	$(CXX) $(CFLAGS) $(PROG_OBJS) $(PROG_LIBS) -o $(SPIMAP_PROG)

$(SPIMAP_DEBUG): $(PROG_OBJS) 
	$(CXX) $(CFLAGS) $(PROG_OBJS) $(PROG_LIBS) -o $(SPIMAP_DEBUG)


#-----------------------------
# maximum likelihood program
maxml: maxml.o $(SPIDIR_OBJS)
	$(CXX) $(CFLAGS) maxml.o $(SPIDIR_OBJS) $(PROG_LIBS) -o maxml

#-----------------------------
# SPIDIR C-library
lib: $(LIBSPIDIR) $(LIBSPIDIR_SHARED)

$(LIBSPIDIR): $(LIBSPIDIR_OBJS)
	mkdir -p lib
	$(AR) -r $(LIBSPIDIR) $(LIBSPIDIR_OBJS)

$(LIBSPIDIR_SHARED): $(LIBSPIDIR_OBJS) 
	mkdir -p lib
	$(CXX) -o $(LIBSPIDIR_SHARED) -shared $(LIBSPIDIR_OBJS) $(PROG_LIBS)


#-----------------------------
# packaging

pkg:
	python make-pkg.py $(PKG_DIR)

$(SPIMAP_PKG):
	python make-pkg.py $(PKG_DIR)

#-----------------------------
# install

install: $(BINARIES) $(LIBSPIDIR_SHARED_INSTALL)
	mkdir -p $(prefix)/bin
	cp $(BINARIES) $(prefix)/bin
	python setup.py install --prefix=$(prefix)

pylib: $(LIBSPIDIR_SHARED_INSTALL)
	python setup.py install --prefix=$(prefix)


$(LIBSPIDIR_SHARED_INSTALL): $(LIBSPIDIR_SHARED)
	mkdir -p $(prefix)/lib
	cp $(LIBSPIDIR_SHARED) $(LIBSPIDIR_SHARED_INSTALL)

#=============================================================================
# basic rules

$(SPIDIR_OBJS): %.o: %.cpp
	$(CXX) -c $(CFLAGS) -o $@ $<

src/spimap.o: src/spimap.cpp
	$(CXX) -c $(CFLAGS) -o $@ $<

clean:
	rm -f $(PROG_OBJS) $(SPIMAP_PROG) $(LIBSPIDIR) $(LIBSPIDIR_SHARED)

clean-obj:
	rm -f $(PROG_OBJS)


#=============================================================================
# dependencies

dep:
	touch Makefile.dep
	makedepend -f Makefile.dep -Y src/*.cpp src/*.h

Makefile.dep:
	touch Makefile.dep
	makedepend -f Makefile.dep -Y src/*.cpp src/*.h

include Makefile.dep
# DO NOT DELETE
