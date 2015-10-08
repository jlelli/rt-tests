VERSION = 0.95
CC=$(CROSS_COMPILE)gcc
AR=$(CROSS_COMPILE)ar

OBJDIR = bld

sources = cyclictest.c \
	  hackbench.c \
	  pip_stress.c \
	  pi_stress.c \
	  pmqtest.c \
	  ptsematest.c \
	  rt-migrate-test.c \
	  sendme.c \
	  signaltest.c \
	  sigwaittest.c \
	  svsematest.c

LIBS	= -lrt -lpthread
RTTESTLIB = -lrttest -L$(OBJDIR)
EXTRA_LIBS ?= -ldl	# for get_cpu
DESTDIR	?=
prefix  ?= /usr/local
bindir  ?= $(prefix)/bin
mandir	?= $(prefix)/share/man
srcdir	?= $(prefix)/src

CFLAGS ?= -Wall -Wno-nonnull
CPPFLAGS += -D_GNU_SOURCE -Isrc/include
LDFLAGS ?=

PYLIB  ?= $(shell python -c 'import distutils.sysconfig;  print distutils.sysconfig.get_python_lib()')

ifndef DEBUG
	CFLAGS	+= -O2
else
	CFLAGS	+= -O0 -g
endif

# We make some gueses on how to compile rt-tests based on the machine type
# and the ostype. These can often be overridden.
dumpmachine := $(shell $(CC) -dumpmachine)

# The ostype is typically something like linux or android
ostype := $(lastword $(subst -, ,$(dumpmachine)))

machinetype := $(shell echo $(dumpmachine)| \
    sed -e 's/-.*//' -e 's/i.86/i386/' -e 's/mips.*/mips/' -e 's/ppc.*/powerpc/')

# The default is to assume you have libnuma installed, which is fine to do
# even on non-numa machines. If you don't want to install the numa libs, for
# example, they might not be available in an embedded environment, then
# compile with
# make NUMA=0
ifneq ($(filter x86_64 i386 ia64 mips powerpc,$(machinetype)),)
NUMA 	:= 1
endif

# The default is to assume that you only have numa_parse_cpustring
# If you are sure you have a version of libnuma with numa_parse_cpustring_all
# then compile with
# make HAVE_PARSE_CPUSTRING_ALL=1
ifeq ($(NUMA),1)
	CFLAGS += -DNUMA
	NUMA_LIBS = -lnuma
ifdef HAVE_PARSE_CPUSTRING_ALL
	CFLAGS += -DHAVE_PARSE_CPUSTRING_ALL
endif
endif

# Include any arch specific makefiles here. Make sure that TARGETS aren't
# evaluated until AFTER this include
include src/arch/bionic/Makefile
TARGETS = $(sources:.c=)

VPATH	= src/cyclictest:
VPATH	+= src/signaltest:
VPATH	+= src/pi_tests:
VPATH	+= src/rt-migrate-test:
VPATH	+= src/ptsematest:
VPATH	+= src/sigwaittest:
VPATH	+= src/svsematest:
VPATH	+= src/pmqtest:
VPATH	+= src/backfire:
VPATH	+= src/lib:
VPATH	+= src/hackbench:

$(OBJDIR)/%.o: %.c
	$(CC) -D VERSION=$(VERSION) -c $< $(CFLAGS) $(CPPFLAGS) -o $@

# Pattern rule to generate dependency files from .c files
$(OBJDIR)/%.d: %.c
	@$(CC) -MM $(CFLAGS) $(CPPFLAGS) $< | sed 's,\($*\)\.o[ :]*,\1.o $@ : ,g' > $@ || rm -f $@

.PHONY: all
all: $(OBJDIR) $(TARGETS) hwlatdetect

$(OBJDIR):
	mkdir $(OBJDIR)

# Include dependency files, automatically generate them if needed.
-include $(addprefix $(OBJDIR)/,$(sources:.c=.d))

cyclictest: $(OBJDIR)/cyclictest.o $(OBJDIR)/librttest.a
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBS) $(RTTESTLIB) $(NUMA_LIBS)

signaltest: $(OBJDIR)/signaltest.o $(OBJDIR)/librttest.a
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBS) $(RTTESTLIB)

pi_stress: $(OBJDIR)/pi_stress.o $(OBJDIR)/librttest.a
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBS) $(RTTESTLIB)

hwlatdetect:  src/hwlatdetect/hwlatdetect.py
	chmod +x src/hwlatdetect/hwlatdetect.py
	ln -s src/hwlatdetect/hwlatdetect.py hwlatdetect

rt-migrate-test: $(OBJDIR)/rt-migrate-test.o $(OBJDIR)/librttest.a
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBS) $(RTTESTLIB)

ptsematest: $(OBJDIR)/ptsematest.o $(OBJDIR)/librttest.a
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBS) $(RTTESTLIB) $(EXTRA_LIBS)

sigwaittest: $(OBJDIR)/sigwaittest.o $(OBJDIR)/librttest.a
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBS) $(RTTESTLIB) $(EXTRA_LIBS)

svsematest: $(OBJDIR)/svsematest.o $(OBJDIR)/librttest.a
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBS) $(RTTESTLIB) $(EXTRA_LIBS)

pmqtest: $(OBJDIR)/pmqtest.o $(OBJDIR)/librttest.a
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBS) $(RTTESTLIB) $(EXTRA_LIBS)

sendme: $(OBJDIR)/sendme.o $(OBJDIR)/librttest.a
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBS) $(RTTESTLIB) $(EXTRA_LIBS)

pip_stress: $(OBJDIR)/pip_stress.o $(OBJDIR)/librttest.a
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBS) $(RTTESTLIB)

hackbench: $(OBJDIR)/hackbench.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBS)

LIBOBJS =$(addprefix $(OBJDIR)/,error.o rt-get_cpu.o rt-sched.o rt-utils.o)
$(OBJDIR)/librttest.a: $(LIBOBJS)
	$(AR) rcs $@ $^

CLEANUP  = $(TARGETS) *.o .depend *.*~ *.orig *.rej rt-tests.spec *.d *.a
CLEANUP += $(if $(wildcard .git), ChangeLog)

.PHONY: clean
clean:
	for F in $(CLEANUP); do find -type f -name $$F | xargs rm -f; done
	rm -f rt-tests-*.tar
	rm -f hwlatdetect
	rm -f tags

RPMDIRS = BUILD BUILDROOT RPMS SRPMS SPECS
.PHONY: distclean
distclean: clean
	rm -rf $(RPMDIRS) releases *.tar.gz rt-tests.spec tmp

.PHONY: rebuild
rebuild:
	$(MAKE) clean
	$(MAKE) all

.PHONY: changelog
changelog:
	git log >ChangeLog

.PHONY: install
install: all install_hwlatdetect
	mkdir -p "$(DESTDIR)$(bindir)" "$(DESTDIR)$(mandir)/man4"
	mkdir -p "$(DESTDIR)$(srcdir)" "$(DESTDIR)$(mandir)/man8"
	cp $(TARGETS) "$(DESTDIR)$(bindir)"
	install -D -m 644 src/backfire/backfire.c "$(DESTDIR)$(srcdir)/backfire/backfire.c"
	install -m 644 src/backfire/Makefile "$(DESTDIR)$(srcdir)/backfire/Makefile"
	gzip -c src/backfire/backfire.4 >"$(DESTDIR)$(mandir)/man4/backfire.4.gz"
	gzip -c src/cyclictest/cyclictest.8 >"$(DESTDIR)$(mandir)/man8/cyclictest.8.gz"
	gzip -c src/pi_tests/pi_stress.8 >"$(DESTDIR)$(mandir)/man8/pi_stress.8.gz"
	gzip -c src/ptsematest/ptsematest.8 >"$(DESTDIR)$(mandir)/man8/ptsematest.8.gz"
	gzip -c src/sigwaittest/sigwaittest.8 >"$(DESTDIR)$(mandir)/man8/sigwaittest.8.gz"
	gzip -c src/svsematest/svsematest.8 >"$(DESTDIR)$(mandir)/man8/svsematest.8.gz"
	gzip -c src/pmqtest/pmqtest.8 >"$(DESTDIR)$(mandir)/man8/pmqtest.8.gz"
	gzip -c src/backfire/sendme.8 >"$(DESTDIR)$(mandir)/man8/sendme.8.gz"
	gzip -c src/hackbench/hackbench.8 >"$(DESTDIR)$(mandir)/man8/hackbench.8.gz"
	gzip -c src/signaltest/signaltest.8 >"$(DESTDIR)$(mandir)/man8/signaltest.8.gz"

.PHONY: install_hwlatdetect
install_hwlatdetect: hwlatdetect
	if test -n "$(PYLIB)" ; then \
		mkdir -p "$(DESTDIR)$(bindir)" "$(DESTDIR)$(mandir)/man8" ; \
		install -D -m 755 src/hwlatdetect/hwlatdetect.py $(DESTDIR)$(PYLIB)/hwlatdetect.py ; \
		rm -f "$(DESTDIR)$(bindir)/hwlatdetect" ; \
		ln -s $(PYLIB)/hwlatdetect.py "$(DESTDIR)$(bindir)/hwlatdetect" ; \
		gzip -c src/hwlatdetect/hwlatdetect.8 >"$(DESTDIR)$(mandir)/man8/hwlatdetect.8.gz" ; \
	fi
.PHONY: release
release: distclean changelog
	mkdir -p releases
	mkdir -p tmp/rt-tests
	cp -r Makefile COPYING ChangeLog MAINTAINERS doc README.markdown src tmp/rt-tests
	rm -f rt-tests-$(VERSION).tar rt-tests-$(VERSION).tar.asc
	tar -C tmp -cf rt-tests-$(VERSION).tar rt-tests
	gpg2 --default-key clrkwllms@kernel.org --detach-sign --armor rt-tests-$(VERSION).tar
	gzip rt-tests-$(VERSION).tar
	rm -f ChangeLog
	cp rt-tests-$(VERSION).tar.gz rt-tests-$(VERSION).tar.asc releases

.PHONY: tarball
tarball:
	git archive --worktree-attributes --prefix=rt-tests-${VERSION}/ -o rt-tests-${VERSION}.tar v${VERSION}

.PHONY: push
push:	release
	scripts/do-git-push $(VERSION)

.PHONY: pushtest
pushtest: release
	scripts/do-git-push --test $(VERSION)

rt-tests.spec: Makefile rt-tests.spec-in
	sed s/__VERSION__/$(VERSION)/ <$@-in >$@
ifeq ($(NUMA),1)
	sed -i -e 's/__MAKE_NUMA__/NUMA=1/' $@
	sed -i -e 's/__BUILDREQUIRES_NUMA__/numactl-devel/' $@
else
	sed -i -e 's/__MAKE_NUMA__//' $@
	sed -i -e 's/__BUILDREQUIRES_NUMA__//' $@
endif


HERE	:=	$(shell pwd)
RPMARGS	:=	--define "_topdir $(HERE)" 	\
		--define "_sourcedir $(HERE)/releases" 	\
		--define "_builddir $(HERE)/BUILD" 	\

.PHONY: rpm
rpm:	rpmdirs release rt-tests.spec
	rpmbuild -ba $(RPMARGS) rt-tests.spec

.PHONY: rpmdirs
rpmdirs:
	@[ -d BUILD ]  || mkdir BUILD
	@[ -d RPMS ]   || mkdir RPMS
	@[ -d SRPMS ]  || mkdir SRPMS

.PHONY: help
help:
	@echo ""
	@echo " rt-tests useful Makefile targets:"
	@echo ""
	@echo "    all       :  build all tests (default"
	@echo "    install   :  install tests to local filesystem"
	@echo "    release   :  build source tarfile"
	@echo "    rpm       :  build RPM package"
	@echo "    clean     :  remove object files"
	@echo "    distclean :  remove all generated files"
	@echo "    help      :  print this message"

.PHONY: tags
tags:
	ctags -R --extra=+f --c-kinds=+p --exclude=tmp --exclude=BUILD *
