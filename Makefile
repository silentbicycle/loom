PROJECT =	loom
OPTIMIZE =	-O3
WARN =		-Wall -pedantic
CSTD +=		-std=c99 #-D_POSIX_C_SOURCE=1 -D_C99_SOURCE

CFLAGS +=	${CSTD} -g ${WARN} ${CDEFS} ${CINCS} ${OPTIMIZE}
LDFLAGS +=	-lpthread

TEST_CFLAGS = 	${CFLAGS}
TEST_LDFLAGS += -L. -lloom
BENCH_LDFLAGS += -L. -lloom

all: test_${PROJECT} lib${PROJECT}.a benchmarks

OBJS=	loom.o

TEST_OBJS=	

# Basic targets

${PROJECT}: main.o ${OBJS}
	${CC} -o $@ main.o ${OBJS} ${LDFLAGS}

lib${PROJECT}.a: ${OBJS}
	ar -rcs lib${PROJECT}.a ${OBJS}

test_${PROJECT}: test_${PROJECT}.o ${TEST_OBJS} lib${PROJECT}.a
	${CC} -o $@ test_${PROJECT}.o \
		${TEST_OBJS} ${TEST_CFLAGS} ${TEST_LDFLAGS}

test: ./test_${PROJECT}
	./test_${PROJECT}

bench: benchmarks
	./benchmarks

benchmarks: benchmarks.o lib${PROJECT}.a
	${CC} -o $@ $< ${BENCH_LDFLAGS}

ci: test bench

clean:
	rm -f ${PROJECT} test_${PROJECT} benchmarks *.o *.a *.core

${OBJS}: Makefile

loom.o: loom.h loom_internal.h

# Installation
PREFIX ?=	/usr/local
INSTALL ?=	install
RM ?=		rm

install:
	${INSTALL} -c lib${PROJECT}.a ${PREFIX}/lib/
	${INSTALL} -c ${PROJECT}.h ${PREFIX}/include/

uninstall:
	${RM} -f ${PREFIX}/lib/lib${PROJECT}.a
	${RM} -f ${PREFIX}/include/${PROJECT}.h
