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

all: bench

bench: $(OBJS)
	$(CC) -o $@ $< $(LIBS_ALL)

clean:
	$(RM) bench $(OBJS)

.c.o:
	$(CC) -c $(CFLAGS_ALL) $<

.PHONY: all clean
.SUFFIXES: .c .o