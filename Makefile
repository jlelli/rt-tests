TARGETS=cyclictest signaltest
FLAGS= -Wall -Wno-nonnull -O2
LIBS = -lpthread -lrt

all: cyclictest signaltest

cyclictest: src/cyclictest/cyclictest.c
	$(CROSS_COMPILE)gcc $(FLAGS) $^ -o $@ $(LIBS)

signaltest: src/signaltest/signaltest.c
	$(CROSS_COMPILE)gcc $(FLAGS) $^ -o $@ $(LIBS)

clean:
	rm -f $(TARGETS) *.o .depend *.*~

