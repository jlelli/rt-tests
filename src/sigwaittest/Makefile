CFLAGS += -Wall -O2
LDFLAGS += -lpthread -lrt

all:	sigwaittest
	@echo Done

sigwaittest.o: sigwaittest.c

sigwaittest:

clean:
	@rm -f *.o

tar:	clean
	@rm -f sigwaittest
	$(shell bn=`basename $$PWD`; cd ..; tar -zcf $$bn.tgz $$bn)

