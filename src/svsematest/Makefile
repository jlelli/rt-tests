CFLAGS += -Wall -O2
LDFLAGS += -lpthread -lrt

all:	svsematest
	@echo Done

svsematest.o: svsematest.c

svsematest:

clean:
	@rm -f *.o

tar:	clean
	@rm -f svsematest
	$(shell bn=`basename $$PWD`; cd ..; tar -zcf $$bn.tgz $$bn)
