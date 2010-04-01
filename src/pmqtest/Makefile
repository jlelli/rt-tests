CFLAGS += -Wall -O2
LDFLAGS += -lpthread

all:	pmqtest
	@echo Done

pmqtest.o: pmqtest.c

pmqtest:

clean:
	@rm -f *.o

tar:	clean
	@rm -f pmqtest
	$(shell bn=`basename $$PWD`; cd ..; tar -zcf $$bn.tgz $$bn)
