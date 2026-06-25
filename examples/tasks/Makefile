SRCS=$(wildcard *.cpp)

TARGETS=$(patsubst %.cpp,%,$(SRCS))

all: $(TARGETS)

clean:
	rm -f $(TARGETS)

.PHONY: distclean maintainer-clean

distclean: clean

maintainer-clean: distclean

.cpp:
	g++ -O0 -g -Wall -Werror `pkg-config --cflags sword` $< -o $@ `pkg-config --libs sword`
.c:
	gcc -O0 -g -Wall -Werror `pkg-config --cflags sword` $< -o $@ `pkg-config --libs sword` -lstdc++


