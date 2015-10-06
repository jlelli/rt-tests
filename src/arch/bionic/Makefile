
# Bionic (android) does not have:
# - pthread barriers
# - pthread_[gs]etaffinity
#
# Typically see something like "aarch64-linux-android"
ifeq (android,$(ostype))
	USE_BIONIC := 1
	CFLAGS += -DNO_PTHREAD_BARRIER
	CFLAGS += -DNO_PTHREAD_SETAFFINITY

	LDFLAGS += -pie
# -lrt and -lpthread is in standard bionic library, no standalone library
	LIBS := $(filter-out -lrt,$(LIBS))
	LIBS := $(filter-out -lpthread,$(LIBS))

# BIONIC does not support PI, barriers and has different files in
# include/. This means that currently, only these binaries will compile
# and link properly:
# - cyclictest
# - hackbench
# - hwlatdetect
	sources := cyclictest.c hackbench.c hwlatdetect.c
endif

