VERSION=1.1.1 [$(shell if test -f manifest.uuid; then cat manifest.uuid | cut -c -10 ; else echo undefined; fi)]
CFLAGS+=-DVERSION="$(VERSION)" -D_FILE_OFFSET_BITS=64 -g -Wall -Wextra

PREFIX?=/usr/local

# Comment the following lines if you don't have librsync
LIBRSYNC_CFLAGS=-DWITH_LIBRSYNC
LIBRSYNC_LDFLAGS=-lrsync


# ----------------------
CFLAGS+=$(LIBRSYNC_CFLAGS)
LDFLAGS+=$(LIBRSYNC_LDFLAGS)

OBJECTS=main.o mytar.o traverse.o error.o loadindex.o filters.o \
	   	index_from_tar.o block.o blockprocess.o filememory.o \
		readtar.o extract.o listindex.o rsync.o string.o

btar: $(OBJECTS)
	$(CC)  -o $@ $^ $(LDFLAGS)

install: btar
	mkdir -p $(PREFIX)/bin
	cp btar $(PREFIX)/bin
	mkdir -p $(PREFIX)/share/man/man1
	cp btar.1 $(PREFIX)/share/man/man1

all: btar fnmatchtest rsynctest loadindextest

clean:
	rm -f $(OBJECTS) btar fnmatchtest loadindextest rsynctest

main.o: main.c main.h traverse.h mytar.h loadindex.h filters.h block.h blockprocess.h
traverse.o: traverse.c main.h traverse.h mytar.h
mytar.o: mytar.c main.h mytar.h
error.o: error.c main.h
loadindex.o: loadindex.c mytar.h main.h loadindex.h
filters.o: filters.c filters.h main.h
index_from_tar.o: index_from_tar.c filters.h mytar.h main.h
block.o: block.c block.h
blockprocess.o: blockprocess.c blockprocess.h block.h main.h mytar.h
filememory.o: filememory.c filememory.h block.h main.h mytar.h
rsync.o: rsync.c rsync.h main.h
rsynctest.o: rsynctest.c rsync.h main.h
readtar.o: readtar.c readtar.h main.h mytar.h
extract.o: extract.c extract.h main.h readtar.h mytar.h
listindex.o: listindex.c listindex.h main.h readtar.h mytar.h
string.o: string.c main.h

loadindextest: loadindextest.o error.o mytar.o readtar.o

rsynctest: rsynctest.o rsync.o error.o

loadindextest.o: loadindex.c mytar.h main.h loadindex.h
	$(CC) $(CFLAGS) -DINDEXTEST -c -o $@ $<

fnmatchtest: fnmatchtest.o

xortest: xortest.o
