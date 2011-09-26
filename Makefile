VERSION_STRING = 0.83

sources = cyclictest.c signaltest.c pi_stress.c rt-migrate-test.c	\
	  ptsematest.c sigwaittest.c svsematest.c pmqtest.c sendme.c 	\
	  pip_stress.c hackbench.c

TARGETS = $(sources:.c=)

LIBS 	= -lrt -lpthread
EXTRA_LIBS ?= -ldl	# for get_cpu
DESTDIR	?=
prefix  ?= /usr/local
bindir  ?= $(prefix)/bin
mandir	?= $(prefix)/share/man
srcdir	?= $(prefix)/src

machinetype = $(shell uname -m | \
    sed -e 's/i.86/i386/' -e 's/mips.*/mips/' -e 's/ppc.*/powerpc/')
ifneq ($(filter x86_64 i386 ia64 mips powerpc,$(machinetype)),)
NUMA 	:= 1
endif

CFLAGS = -D_GNU_SOURCE -Wall -Wno-nonnull -Isrc/include

PYLIB  := $(shell python -c 'import distutils.sysconfig;  print distutils.sysconfig.get_python_lib()')

ifndef DEBUG
	CFLAGS	+= -O2
else
	CFLAGS	+= -O0 -g
endif

ifeq ($(NUMA),1)
	CFLAGS += -DNUMA
	NUMA_LIBS = -lnuma
endif

VPATH	= src/cyclictest:
VPATH	+= src/signaltest:
VPATH	+= src/pi_tests:
VPATH	+= src/rt-migrate-test:
VPATH	+= src/ptsematest:
VPATH	+= src/sigwaittest:
VPATH	+= src/svsematest:
VPATH	+= src/pmqtest:
VPATH	+= src/backfire:
VPATH	+= src/lib
VPATH	+= src/hackbench

%.o: %.c
	$(CC) -D VERSION_STRING=$(VERSION_STRING) -c $< $(CFLAGS)

# Pattern rule to generate dependency files from .c files
%.d: %.c
	@$(CC) -MM $(CFLAGS) $< | sed 's,\($*\)\.o[ :]*,\1.o $@ : ,g' > $@ || rm -f $@

.PHONY: all
all: $(TARGETS) hwlatdetect

# Include dependency files, automatically generate them if needed.
-include $(sources:.c=.d)

cyclictest: cyclictest.o rt-utils.o
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS) $(NUMA_LIBS)

signaltest: signaltest.o rt-utils.o
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

pi_stress: pi_stress.o
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

hwlatdetect:  src/hwlatdetect/hwlatdetect.py
	chmod +x src/hwlatdetect/hwlatdetect.py
	ln -s src/hwlatdetect/hwlatdetect.py hwlatdetect

rt-migrate-test: rt-migrate-test.o
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

ptsematest: ptsematest.o rt-utils.o rt-get_cpu.o
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS) $(EXTRA_LIBS)

sigwaittest: sigwaittest.o rt-utils.o rt-get_cpu.o
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS) $(EXTRA_LIBS)

svsematest: svsematest.o rt-utils.o rt-get_cpu.o
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS) $(EXTRA_LIBS)

pmqtest: pmqtest.o rt-utils.o rt-get_cpu.o
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS) $(EXTRA_LIBS)

sendme: sendme.o rt-utils.o rt-get_cpu.o
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS) $(EXTRA_LIBS)

pip_stress: pip_stress.o error.o rt-utils.o
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

hackbench: hackbench.o
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

CLEANUP  = $(TARGETS) *.o .depend *.*~ *.orig *.rej rt-tests.spec *.d
CLEANUP += $(if $(wildcard .git), ChangeLog)

.PHONY: clean
clean:
	for F in $(CLEANUP); do find -type f -name $$F | xargs rm -f; done
	rm -f hwlatdetect
	rm -f tags

.PHONY: distclean
distclean: clean
	rm -rf BUILD RPMS SRPMS releases *.tar.gz rt-tests.spec

.PHONY: changelog
changelog:
	git log >ChangeLog

.PHONY: install
install: all
	mkdir -p "$(DESTDIR)$(bindir)" "$(DESTDIR)$(mandir)/man4"
	mkdir -p "$(DESTDIR)$(srcdir)" "$(DESTDIR)$(mandir)/man8"
	cp $(TARGETS) "$(DESTDIR)$(bindir)"
	if test -n "$(PYLIB)" ; then \
		install -D -m 755 src/hwlatdetect/hwlatdetect.py $(DESTDIR)$(PYLIB)/hwlatdetect.py ; \
		ln -s $(PYLIB)/hwlatdetect.py "$(DESTDIR)$(bindir)/hwlatdetect" ; \
	fi
	install -D -m 644 src/backfire/backfire.c "$(DESTDIR)$(srcdir)/backfire/backfire.c"
	install -m 644 src/backfire/Makefile "$(DESTDIR)$(srcdir)/backfire/Makefile"
	gzip src/backfire/backfire.4 -c >"$(DESTDIR)$(mandir)/man4/backfire.4.gz"
	gzip src/cyclictest/cyclictest.8 -c >"$(DESTDIR)$(mandir)/man8/cyclictest.8.gz"
	gzip src/pi_tests/pi_stress.8 -c >"$(DESTDIR)$(mandir)/man8/pi_stress.8.gz"
	gzip src/hwlatdetect/hwlatdetect.8 -c >"$(DESTDIR)$(mandir)/man8/hwlatdetect.8.gz"
	gzip src/ptsematest/ptsematest.8 -c >"$(DESTDIR)$(mandir)/man8/ptsematest.8.gz"
	gzip src/sigwaittest/sigwaittest.8 -c >"$(DESTDIR)$(mandir)/man8/sigwaittest.8.gz"
	gzip src/svsematest/svsematest.8 -c >"$(DESTDIR)$(mandir)/man8/svsematest.8.gz"
	gzip src/pmqtest/pmqtest.8 -c >"$(DESTDIR)$(mandir)/man8/pmqtest.8.gz"
	gzip src/backfire/sendme.8 -c >"$(DESTDIR)$(mandir)/man8/sendme.8.gz"
	gzip src/hackbench/hackbench.8 -c >"$(DESTDIR)$(mandir)/man8/hackbench.8.gz"

.PHONY: release
release: clean changelog
	mkdir -p releases
	rm -rf tmp && mkdir -p tmp/rt-tests
	cp -r Makefile COPYING ChangeLog src tmp/rt-tests
	tar -C tmp -czf rt-tests-$(VERSION_STRING).tar.gz rt-tests
	rm -f ChangeLog
	cp rt-tests-$(VERSION_STRING).tar.gz releases

.PHONY: push
push:	release
	scripts/do-git-push $(VERSION_STRING)

.PHONY: pushtest
pushtest: release
	scripts/do-git-push --test $(VERSION_STRING)

rt-tests.spec: Makefile rt-tests.spec-in
	sed s/__VERSION__/$(VERSION_STRING)/ <$@-in >$@

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
	ctags -R --extra=+f --c-kinds=+p *
