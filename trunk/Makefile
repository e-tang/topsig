#gcc doesn't seem to have -march under Darwin, but this option is necessary for other
#architectures that don't support atomic fetch and increment properly unless the -march
#is changed
ifeq "${shell uname -s}" "Darwin"
  CCFLAGS_EXTRA =
else
  CCFLAGS_EXTRA = -march=i486 -mtune=native 
endif

ifeq ($(strip $(DEBUG)),)
    BUILD = -O3
else 
    BUILD = -g3 -pg
endif

LDFLAGS = -lm -lz -lbz2 ${BUILD} -pthread -Wl,--large-address-aware
CCFLAGS = -W -Wall -std=c99 ${BUILD} ${CCFLAGS_EXTRA} -pthread

OBJS = src/topsig-main.o \
src/topsig-config.o \
src/topsig-index.o \
src/topsig-process.o \
src/topsig-stem.o \
src/topsig-porterstemmer.o \
src/topsig-stop.o \
src/topsig-signature.o \
src/topsig-query.o \
src/topsig-search.o \
src/topsig-topic.o \
src/topsig-filerw.o \
src/topsig-file.o \
src/topsig-thread.o \
src/topsig-progress.o \
src/topsig-semaphore.o \
src/topsig-stats.o \
src/topsig-document.o \
src/topsig-issl.o \
src/topsig-experimental-rf.o \
src/superfasthash.o \
src/ISAAC-rand.o

default:	topsig

%.o:		%.c
		gcc ${CCFLAGS} -c -o $@ $?

topsig:	${OBJS}
		gcc -o $@ $+ ${LDFLAGS}

all-at-once:		
		gcc ${CCFLAGS} -o topsig src/*.c -fwhole-program -flto ${LDFLAGS}

clean:		
		rm -f ${OBJS}

topcat:		
		gcc ${CCFLAGS} -o topcat src/tools/topcat.c

create-random-sigfile:		src/tools/create-random-sigfile.c
		gcc ${CCFLAGS} -o create-random-sigfile src/tools/create-random-sigfile.c

wsj-title-lookup:		src/tools/wsj-title-lookup.c
		gcc ${CCFLAGS} -o wsj-title-lookup src/tools/wsj-title-lookup.c

wiki-link-lookup:		src/tools/wiki-link-lookup.c
		gcc ${CCFLAGS} -o wiki-link-lookup src/tools/wiki-link-lookup.c

wsj-cosine-sim:		src/tools/wsj-cosine-sim.c src/topsig-porterstemmer.c
		gcc ${CCFLAGS} -o wsj-cosine-sim src/tools/wsj-cosine-sim.c src/topsig-porterstemmer.c -Wl,--large-address-aware
		
topcut:		src/tools/topcut.c
		gcc ${CCFLAGS} -o topcut src/tools/topcut.c

plagtest:		src/tools/plagtest.c
		gcc ${CCFLAGS} -o plagtest src/tools/plagtest.c
