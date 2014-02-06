/*
    btar - no-tape archiver.
    Copyright (C) 2011  Lluis Batlle i Rossell

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include "mytar.h"
#include "main.h"
#include "loadindex.h"
#include "readtar.h"

static struct Index
{
    struct IndexElem *ptr;
    size_t nelem;
    size_t allocated;
    size_t search_until;
} myindex;

static int
compare_sort(const void *p1, const void *p2)
{
    const struct IndexElem *e1 = p1;
    const struct IndexElem *e2 = p2;

    return strcmp(e1->name, e2->name);
}

static int
compare_sort_inverse(const void *p1, const void *p2)
{
    return -compare_sort(p1, p2);
}

static int
compare_search(const void *p1, const void *p2)
{
    const char *s1 = p1;
    const struct IndexElem *e2 = p2;

    return strcmp(s1, e2->name);
}

int
block_name_to_int(const char *str)
{
    int b;
    int nread;
    nread = sscanf(str, "block%i", &b);
    if (nread != 1)
        fatal_error("Could not read block number from name \"%s\"", str);
    return b;
}

static void
set_block(int block, struct IndexElem *e)
{
    /* We care on the blocks only on extraction. And there
     * we don't combine indices, so this call should work fine,
     * considering 'myindex.nelem' the previous element read from
     * the index. */

    e->block = block;
    e->nblocks = -1; /* Until the end */
    if (myindex.nelem > 0)
    {
        int prev = myindex.nelem - 1;
        int prevblock = myindex.ptr[prev].block;
        myindex.ptr[prev].nblocks = block - prevblock + 1;
    }
}

static void
update_entry(struct IndexElem *new_e)
{
    struct IndexElem *e = index_find_element(new_e->name);

    if (command_line.debug > 1)
        fprintf(stderr, "index: %s (block %i, mtime %u%s)\n",
                new_e->name,
                new_e->block,
                (unsigned int) new_e->mtime,
                new_e->signature ? ", signature":"");

    if (e)
    {
        free(new_e->name);
        e->mtime = new_e->mtime;

        /* We only need the blocks at extraction time,
         * and then, we will not be joining indices. So
         * it's of little use to update them here. */
        e->block = new_e->block;
        e->nblocks = new_e->nblocks;

        free(e->signature);
        e->signature = new_e->signature;
        e->signaturelen = new_e->signaturelen;
    }
    else
    {
        const size_t allocstep = 10000;
        if (myindex.nelem == myindex.allocated)
        {
            myindex.ptr = realloc(myindex.ptr,
                    (myindex.allocated + allocstep) * sizeof(*myindex.ptr));
            if (!myindex.ptr)
                fatal_error("Cannot realloc");
            memset(myindex.ptr + myindex.allocated, 0, allocstep * sizeof(*myindex.ptr));
            myindex.allocated += allocstep;
        }
        myindex.ptr[myindex.nelem++] = *new_e;
    }
}

struct loadindex_state
{
    char *data;
    size_t nread;
    unsigned long long expected_size;
    int should_read;
    int last_block;
    struct IndexElem e;
};

static void
loadindex_new_data_cb(const char *data, size_t len, void *userdata)
{
    struct loadindex_state *ls = (struct loadindex_state *) userdata;

    if (ls->should_read)
    {
        memcpy(ls->data + ls->nread, data, len);
        ls->nread += len;


        if (ls->nread == ls->expected_size)
        {
            /* End of file */

            /* First, at the data part, there will be
             * the block name string, ending in \0. After the \0,
             * all the rest is a rsync signature */
            set_block(block_name_to_int(ls->data), &ls->e);

            {
                int offset = strlen(ls->data) + 1;
                int finalsize = ls->expected_size - offset;
                int i;
                for(i = 0; i < finalsize; ++i)
                    ls->data[i] = ls->data[i+offset];
                /* Take less memory, although it may be cpu overhead */
                ls->data = realloc(ls->data, finalsize);
                if (!ls->data)
                    fatal_error("Cannot realloc");

                ls->e.signaturelen = finalsize;
            }
            ls->e.signature = ls->data;
            ls->data = 0;

            update_entry(&ls->e);
        }
    }
}

static enum readtar_newfile_result
loadindex_new_file_cb(const struct readtar_file *file, void *userdata)
{
    struct loadindex_state *ls = (struct loadindex_state *) userdata;
    const struct header_gnu_tar *h = file->header;

    /* Symlinks or the directory elements */
    if (h->typeflag[0] != '2' && h->typeflag[0] != '5'
            && h->typeflag[0] != '0')
    {
        fprintf(stderr, "Type not implemented in the index: %c\n", h->typeflag[0]);
        return READTAR_SKIPDATA;
    }

    ls->e.seen = 0;
    ls->e.is_dir = (h->typeflag[0] == '5');
    ls->e.signature = 0;
    ls->e.signaturelen = 0;
    ls->e.name = strdup(file->name);

    ls->should_read = 0;
    ls->nread = 0;
    ls->expected_size = file->size;

    if (h->typeflag[0] == '5') /* Directory */
    {
        int lenname = strlen(ls->e.name);

        /* Remove last slash, as this is how works on traverse,
         * looking for directories. */
        if (ls->e.name[lenname-1] == '/')
            ls->e.name[lenname-1] = '\0';

        /* To make valgrind happy, when sending the index through a fd.
         * It has to be processed properly also in the search at extract.c. */
        ls->e.block = -1;
        ls->e.nblocks = 1;
    }
    else if (h->typeflag[0] == '0') /* regular file, like for rdiff signatures */
    {
        ls->data = malloc(file->size);
        if (!ls->data)
            fatal_error("Cannot allocate");
        ls->should_read = 1;
    }
    else
    {
        set_block(block_name_to_int(file->linkname), &ls->e);
    }

    ls->e.mtime = read_octal_number(h->mtime, sizeof(h->mtime));

    if (ls->should_read)
    {
        return READTAR_NORMAL;
    }
    else
    {
        update_entry(&ls->e);
        return READTAR_SKIPDATA;
    }
}

void
index_load_from_fd(int fd)
{
    const size_t allocstep = 10000;
    struct readtar rt;
    struct loadindex_state li_state;
    struct readtar_callbacks cb = { loadindex_new_file_cb, loadindex_new_data_cb, &li_state};
    init_readtar(&rt, &cb);

    li_state.data = 0;
    li_state.nread = 0;
    li_state.expected_size = 0;

    myindex.ptr = malloc(allocstep * sizeof(*myindex.ptr));
    if (!myindex.ptr)
        fatal_error("Cannot allocate");
    myindex.nelem = 0;
    myindex.allocated = allocstep;
    memset(myindex.ptr, 0, myindex.allocated * sizeof(*myindex.ptr));

    read_full_tar(fd, &rt);
}

void
index_sort()
{
    qsort(myindex.ptr, myindex.nelem, sizeof(myindex.ptr[0]),
            compare_sort);
    myindex.search_until = myindex.nelem;
}

void
index_sort_inverse()
{
    qsort(myindex.ptr, myindex.nelem, sizeof(myindex.ptr[0]),
            compare_sort_inverse);
    myindex.search_until = myindex.nelem;
}

struct IndexElem *
index_find_element(const char *name)
{
    void *e;

    /* Case of no index loaded */
    if (myindex.ptr == 0 || myindex.search_until == 0)
        return 0;

    e = bsearch(name, myindex.ptr, myindex.search_until,
            sizeof(myindex.ptr[0]), compare_search);

    return (struct IndexElem *) e;
}

const struct IndexElem *
index_get_elements(size_t *nelems)
{
    *nelems = myindex.nelem;
    return myindex.ptr;
}

char *
index_find_in_tar(int fd, unsigned long long *size)
{
    while(1)
    {
        struct header_gnu_tar h;
        int skip = 0;
        int res;
        res = read(fd, &h, sizeof h);
        if (res == 0 || strlen(h.name) == 0)
            break;
        if (res != sizeof(h))
            error("Failed read() reading index");

        if (command_line.debug > 1)
            fprintf(stderr, "index_find_in_tar: seen %s\n", h.name);
        if (!strncmp(h.name,"index", 5))
        {
            *size = read_size(h.size);
            return strdup(h.name);
        }

        skip = read_size(h.size);

        if (skip % 512)
            skip += 512 - (skip % 512);

        res = lseek(fd, skip, SEEK_CUR);
        if (res == -1)
            error("Error seeking in the index file");
    }
    return 0;
}

void
send_index_to_fd(int fd)
{
    ssize_t res;
    size_t i;

    do
        res = write(fd, &myindex.nelem, sizeof myindex.nelem);
    while(res == -1 && errno == EINTR);
    if (res != sizeof myindex.nelem)
        error("Could not serialize index 0");

    for(i=0; i < myindex.nelem; ++i)
    {
        size_t tosend;

        tosend = strlen(myindex.ptr[i].name) + 1 /*null*/;

        do
            res = write(fd, &tosend , sizeof tosend);
        while(res == -1 && errno == EINTR);
        if (res != sizeof tosend)
            error("Could not serialize index 1");

        do
            res = write(fd, myindex.ptr[i].name, tosend);
        while(res == -1 && errno == EINTR);
        if ((size_t) res != tosend)
            error("Could not serialize index 2");

        do
            res = write(fd, &myindex.ptr[i].mtime, sizeof myindex.ptr[i].mtime);
        while(res == -1 && errno == EINTR);
        if (res != sizeof myindex.ptr[i].mtime)
            error("Could not serialize index 3");

        do
            res = write(fd, &myindex.ptr[i].block, sizeof myindex.ptr[i].block);
        while(res == -1 && errno == EINTR);
        if ((size_t)res != sizeof myindex.ptr[i].block)
            error("Could not serialize index 4");

        do
            res = write(fd, &myindex.ptr[i].nblocks, sizeof myindex.ptr[i].nblocks);
        while(res == -1 && errno == EINTR);
        if ((size_t)res != sizeof myindex.ptr[i].nblocks)
            error("Could not serialize index 4");

        do
            res = write(fd, &myindex.ptr[i].is_dir, sizeof myindex.ptr[i].is_dir);
        while(res == -1 && errno == EINTR);
        if ((size_t)res != sizeof myindex.ptr[i].is_dir)
            error("Could not serialize index 4");

        tosend = myindex.ptr[i].signaturelen;

        do
            res = write(fd, &tosend, sizeof tosend);
        while(res == -1 && errno == EINTR);
        if (res != sizeof tosend)
            error("Could not serialize index 5");

        do
            res = write(fd, myindex.ptr[i].signature, tosend);
        while(res == -1 && errno == EINTR);
        if ((size_t)res != tosend)
            error("Could not serialize index 6");
    }
}

void
recv_index_from_fd(int fd)
{
    ssize_t res;
    size_t i;
    size_t nelem;

    do
        res = read(fd, &nelem, sizeof nelem);
    while(res == -1 && errno == EINTR);
    if (res != sizeof nelem)
        error("Could not deserialize index 0");

    for(i=0; i < nelem; ++i)
    {
        size_t len;
        struct IndexElem e;

        do
            res = read(fd, &len , sizeof len);
        while(res == -1 && errno == EINTR);
        if (res != sizeof len)
            error("Could not deserialize index (namelen)");

        e.name = malloc(len);
        if (!e.name)
            fatal_error("Cannot allocate");

        do
            res = read(fd, e.name, len);
        while(res == -1 && errno == EINTR);
        if ((size_t) res != len)
            error("Could not deserialize index (name)");

        do
            res = read(fd, &e.mtime, sizeof e.mtime);
        while(res == -1 && errno == EINTR);
        if (res != sizeof e.mtime)
            error("Could not deserialize index (mtime)");

        do
            res = read(fd, &e.block, sizeof e.block);
        while(res == -1 && errno == EINTR);
        if (res != sizeof e.block)
            error("Could not deserialize index (block)");

        do
            res = read(fd, &e.nblocks, sizeof e.nblocks);
        while(res == -1 && errno == EINTR);
        if (res != sizeof e.nblocks)
            error("Could not deserialize index (nblocks)");

        do
            res = read(fd, &e.is_dir , sizeof e.is_dir);
        while(res == -1 && errno == EINTR);
        if (res != sizeof e.is_dir)
            error("Could not deserialize index (is_dir)");

        do
            res = read(fd, &len , sizeof len);
        while(res == -1 && errno == EINTR);
        if (res != sizeof len)
            error("Could not deserialize index (siglen)");

        e.signaturelen = len;
        e.signature = 0;
        if (len > 0)
        {
            e.signature = malloc(len);
            if (!e.signature)
                fatal_error("Cannot allocate");

            do
                res = read(fd, e.signature, len);
            while(res == -1 && errno == EINTR);
            if ((size_t)res != len)
                error("Could not deserialize index (sig)");
        }
        
        e.seen = 0;
        update_entry(&e);
    }

    /* Just to be certain on end of fd */
    res = read(fd, &i, sizeof i);
    if (res != 0)
        fatal_error("Too much data deserialising the index");
}

void
free_index()
{
    size_t i;
    for(i=0; i < myindex.nelem; ++i)
    {
        free(myindex.ptr[i].name);
        free(myindex.ptr[i].signature);
    }
    free(myindex.ptr);
    myindex.ptr = 0;
    myindex.allocated = 0;
    myindex.nelem = 0;
    myindex.search_until = 0;
}

#ifdef INDEXTEST

struct command_line command_line;

int main(int argc, char **argv)
{
    int fd;

    command_line.debug = 1;

    if (argc < 2)
    {
        printf("usage: loadindextest <index.tar>\n");
        return 1;
    }
    fd = open(argv[1], O_RDONLY);
    if (fd == -1)
        error("Cannot open");

    index_load_from_fd(fd);

    /* Calculate memory used */
    {
        size_t mem = myindex.allocated * sizeof(*myindex.ptr);
        size_t i;
        for(i=0; i < myindex.nelem; ++i)
        {
            mem += strlen(myindex.ptr[i].name);
            if (myindex.ptr[i].signature)
                mem += myindex.ptr[i].signaturelen;
        }
        fprintf(stderr, "Memory used: %zu (%zu elements)\n", mem, myindex.nelem);
    }

    close(fd);

    free_index();
    return 0;
}
#endif
