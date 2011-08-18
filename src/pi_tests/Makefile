PACKAGE	:=	pi_tests
SPEC	:=	$(PACKAGE).spec
VERSION	:=	$(shell awk '/^Version:/ { print $$2 }' $(SPEC))
RELEASE	:=	$(shell awk '/Release:/ { print $$2 }' ${PACKAGE}.spec)
SRPM	:=	$(PACKAGE)-$(VERSION)-$(RELEASE).src.rpm
HERE	:= 	$(shell pwd)
TARGET	:=	$(shell uname -p)
RPMARGS	:=	--define "_sourcedir $(HERE)" 	\
		--define "_builddir $(HERE)/BUILD"	\
		--define "_rpmdir $(HERE)/RPMS" 	\
		--define "_srcrpmdir $(HERE)/SRPMS"
CC	:= gcc
OPT	:= -g -O
CFLAGS 	:= $(OPT) -Wall -D_GNU_SOURCE -DVERSION=\"$(VERSION)\"
LIBS	:= -lpthread -lrt

SRC	:= classic_pi.c allprios.c tst-mutexpi10.c pi_stress.c sigtest.c
DOC	:= pi_stress.8 README COPYING
TARGETS	:= $(subst .c,,$(SRC))

all: classic_pi pi_stress

classic_pi: classic_pi.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

pi_stress: pi_stress.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

tst-mutexpi10:  tst-mutexpi10.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

#tst-mutexpi10.c:	classic_pi.c
#	indent --gnu-style classic_pi.c -o tst-mutexpi10.c

allprios:	allprios.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

tarball:  pi_tests-$(VERSION).tar.gz

libctest: tst-mutexpi10

sigtest: sigtest.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

pi_tests-$(VERSION).tar.gz:	clean $(SRC) $(DOC) Makefile pi_tests.spec
	rm -rf $@ pi_tests-$(VERSION)
	mkdir pi_tests-$(VERSION)
	cp $(SRC) $(DOC) Makefile pi_tests.spec pi_tests-$(VERSION)
	tar -czvf pi_tests-$(VERSION).tar.gz pi_tests-$(VERSION)

clean:
	rm -rf *~ *.[oi] $(TARGETS) *.tar.gz pi_tests-*

rpm:  rpmdirs $(SPEC) tarball
	rm -rf RPMS/$(TARGET)
	rpmbuild -ba --target $(TARGET) $(RPMARGS) $(SPEC)

rpmdirs:
	@[ -d BUILD ]  || mkdir BUILD
	@[ -d RPMS ]   || mkdir RPMS
	@[ -d SRPMS ]  || mkdir SRPMS

rpmlint lint: rpm
	rpmlint -vi RPMS/*/*
	rpmlint -vi SRPMS/*

DESTINATION	:= 	people.redhat.com:~williams/public_html/tests

push: rpm
	scp pi_tests-$(VERSION).tar.gz $(DESTINATION)
	scp SRPMS/* $(DESTINATION)
