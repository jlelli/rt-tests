CFLAGS += -Wall -O2
LDFLAGS += -lpthread

all:	ptsematest
	@echo Done

ptsematest.o: ptsematest.c

ptsematest:

clean:
	@rm -f *.o

tar:	clean
	@rm -f ptsematest
	$(shell bn=`basename $$PWD`; cd ..; tar -zcf $$bn.tgz $$bn)
