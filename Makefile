VERSION_STRING = "0.21"

TARGETS	= cyclictest signaltest classic_pi pi_stress
FLAGS	= -Wall -Wno-nonnull -O2
LIBS 	= -lpthread -lrt
DESTDIR	= /usr/local
INSTDIR = $(DESTDIR)/bin
DOCDIR	= $(DESTDIR)/share/man/man8/

all: $(TARGETS)

cyclictest: src/cyclictest/cyclictest.c
	$(CC) $(FLAGS) -D VERSION_STRING=$(VERSION_STRING) $^ -o $@ $(LIBS)

signaltest: src/signaltest/signaltest.c
	$(CC) $(FLAGS) -D VERSION_STRING=$(VERSION_STRING) $^ -o $@ $(LIBS)

classic_pi: src/pi_tests/classic_pi.c
	$(CC) $(FLAGS) -D_GNU_SOURCE -D VERSION_STRING=\"$(VERSION_STRING)\" $^ -o $@ $(LIBS)

pi_stress:  src/pi_tests/pi_stress.c
	$(CC) $(FLAGS) -D_GNU_SOURCE -D VERSION_STRING=\"$(VERSION_STRING)\" $^ -o $@ $(LIBS)

CLEANUP = $(TARGETS) *.o .depend *.*~ ChangeLog *.orig *.rej

clean:
	for F in $(CLEANUP); do find -type f -iname $$F | xargs rm -f; done

distclean: clean
	rm -rf BUILD RPMS SRPMS releases

changelog:
	git log >ChangeLog

install: all
	cp $(TARGETS) $(DESTDIR)/bin
	gzip src/cyclictest/cyclictest.8 -c >$(DOCDIR)cyclictest.8.gz
	gzip src/pi_tests/pi_stress.8 -c >$(DOCDIR)pi_stress.8.gz

release: clean changelog
	mkdir -p releases
	tar -C ".." --exclude ".git" --exclude "patches" --exclude "releases" -c rt-tests | gzip >releases/rt-tests-$(VERSION_STRING).tar.gz
	rm -f ChangeLog

HERE	:=	$(shell pwd)
RPMARGS	:=	--define "_topdir $(HERE)" 	\
		--define "_sourcedir $(HERE)/releases" 	\
		--define "_builddir $(HERE)/BUILD" 	\

rpm:	rpmdirs release
	rpmbuild -ba $(RPMARGS) rt-tests.spec

rpmdirs:
	@[ -d BUILD ]  || mkdir BUILD
	@[ -d RPMS ]   || mkdir RPMS
	@[ -d SRPMS ]  || mkdir SRPMS
