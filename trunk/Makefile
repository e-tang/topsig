#gcc doesn't seem to have -march under Darwin, but this option is necessary for other
#architectures that don't support atomic fetch and increment properly unless the -march
#is changed
ifeq "${shell uname -s}" "Darwin"
  CCFLAGS_EXTRA =
else
  CCFLAGS_EXTRA = -march=native -mtune=native 
endif

LDFLAGS = -lm -lz -lbz2 -lpthread -g -O3
CCFLAGS = -Wall -std=c99 -g -O3 -fno-strict-aliasing -finline-functions -funswitch-loops -fpredictive-commoning -fgcse-after-reload ${CCFLAGS_EXTRA}
#-ftree-vectorize is not included as this program does not satisfy the alignment requirement when searching

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
src/topsig-thread.o \
src/topsig-tmalloc.o \
src/topsig-progress.o \
src/topsig-semaphore.o \
src/ISAAC-rand.o

default:	topsig

%.o:		%.c
		gcc -c -o $@ $? ${CCFLAGS}

topsig:	${OBJS}
		gcc -o $@ $+ ${LDFLAGS}

all-at-once:		
		gcc -o topsig src/*.c -fwhole-program ${CCFLAGS} ${LDFLAGS}

clean:		
		rm ${OBJS}
