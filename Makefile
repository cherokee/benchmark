CFLAGS_ALL=\
$(CFLAGS) \
`curl-config --cflags` \
`cherokee-config --cflags` \
-DCHEROKEE_COMPILATION

LIBS_ALL=\
$(LIBS) \
-lpthread \
`curl-config --libs` \
`cherokee-config --libs`

OBJS=\
main.o

all: cherokee-benchmark

cherokee-benchmark: $(OBJS)
	$(CC) -o $@ $< $(LIBS_ALL)

clean:
	$(RM) cherokee-benchmark $(OBJS)

.c.o:
	$(CC) -c $(CFLAGS_ALL) $<

.PHONY: all clean
.SUFFIXES: .c .o