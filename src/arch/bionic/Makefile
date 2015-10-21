
# Bionic (android) does not have:
# - pthread barriers
# - pthread_[gs]etaffinity
#
# Typically see something like "aarch64-linux-android"
ifeq (android,$(ostype))
	USE_BIONIC := 1
	CFLAGS += -DPTHREAD_BIONIC

	LDFLAGS += -pie
# -lrt and -lpthread is in standard bionic library, no standalone library
	LIBS := $(filter-out -lrt,$(LIBS))
	LIBS := $(filter-out -lpthread,$(LIBS))

# BIONIC does not support PI, barriers and has different files in
# include/. This means that currently, only these binaries will compile
# and link properly:
# - cyclictest
# - hackbench
	sources := cyclictest.c hackbench.c
endif
