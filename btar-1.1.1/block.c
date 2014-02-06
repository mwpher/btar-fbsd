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
#include <unistd.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include "main.h"
#include "block.h"

struct block *
block_new(size_t allocate)
{
    struct block *b = malloc(sizeof(*b));;
    if (!b)
        fatal_error("Cannot allocate");
    b->writer_pos = 0;
    b->readers = 0;
    b->nreaders = 0;
    b->writer_pos = 0;
    b->data = malloc(allocate);
    b->allocated = allocate;
    b->nextblock = 0;
    b->lastblock = b;
    b->total_written = 0;
    b->go_back = 1;

    return b;
}

void
block_free(struct block *b)
{
    while(b)
    {
        struct block *next;
        next = b->nextblock;
        free(b->data);
        free(b);
        b = next;
    }
}

void
block_realloc_set(struct block *b, size_t len, char c)
{
    size_t lastallocated = b->allocated;

    if (len > b->allocated)
    {
        b->data = realloc(b->data, len);
        if (!b->data)
            fatal_error("Cannot realloc");
        b->allocated = len;

        memset(b->data + lastallocated, c, len - lastallocated);
        b->writer_pos = len;
        b->total_written = len;
    }
}

struct block *
block_new_never_back(size_t allocate)
{
    struct block *b = block_new(allocate);
    b->go_back = 0;
    return b;
}

size_t
block_fill_from_fd(struct block *b, int fd, size_t maxbytes)
{
    size_t left = b->allocated - b->writer_pos;
    ssize_t nread;

    if (maxbytes > left)
        maxbytes = left;

    nread = read(fd, b->data + b->writer_pos, maxbytes);
    if (nread > 0)
    {
        b->writer_pos += nread;
        b->total_written += nread;
    }
    return nread;
}

size_t
block_fill_from_memory(struct block *b, const char *data, size_t len)
{
    size_t left = b->allocated - b->writer_pos;

    if (len > left)
        len = left;

    memcpy(b->data + b->writer_pos, data, len);
    b->writer_pos += len;
    return len;
}

ssize_t
block_fill_from_fd_multi(struct block *b, int fd, size_t maxbytes)
{
    size_t left = b->lastblock->allocated - b->lastblock->writer_pos;
    ssize_t nread;
    if (left == 0)
    {
        b->lastblock->nextblock = block_new(b->lastblock->allocated);
        b->lastblock = b->lastblock->nextblock;
        left = b->lastblock->allocated;
    }

    if (maxbytes > left)
        maxbytes = left;

    nread = read(fd, b->lastblock->data + b->lastblock->writer_pos, maxbytes);
    if (nread > 0)
    {
        b->lastblock->writer_pos += nread;
        b->total_written += nread;
    }
    return nread;
}

int
block_can_accept(struct block *b)
{
    return (b->writer_pos < b->allocated);
}

struct block_reader *
block_reader_new(struct block *b)
{
    struct block_reader *r = malloc(sizeof(*r));;
    r->b = b;
    r->pos = 0;
    b->nreaders++;
    b->readers = realloc(b->readers, sizeof(*b->readers)*b->nreaders);
    if (!b->readers)
        fatal_error("Cannot realloc");
    b->readers[b->nreaders-1] = r;

    return r;
}

void
block_reader_free(struct block_reader *br)
{
    size_t i;
    struct block *b = br->b;

    for(i = 0; i < b->nreaders; ++i)
    {
        if (b->readers[i] == br)
            break;
    }
    assert(i < b->nreaders);
    for(; i < b->nreaders-1; ++i)
    {
        b->readers[i] = b->readers[i+1];
    }
    b->nreaders--;
    /* I think we don't have to realloc necessarily */
    b->readers = realloc(b->readers, sizeof(*b->readers)*b->nreaders);
    if (b->nreaders > 0 && !b->readers)
        fatal_error("Cannot realloc");
    free(br);
}

int
block_reader_can_read(struct block_reader *r)
{
    size_t left = r->b->writer_pos - r->pos;
    if (left > 0)
        return 1;
    return 0;
}

int
block_are_readers_done(struct block *b)
{
    size_t i;
    int allequal = 1;

    for(i=0; i < b->nreaders; ++i)
        if (b->readers[i]->pos != b->writer_pos)
        {
            allequal = 0;
            break;
        }

    return allequal;
}

void
block_reset_pos_if_possible(struct block *b)
{
    size_t i;

    if (!b->go_back)
        return;

    if (block_are_readers_done(b))
    {
        b->writer_pos = 0;
        for(i=0; i < b->nreaders; ++i)
            b->readers[i]->pos = 0;
    }
}

int
block_reader_to_fd(struct block_reader *r, int fd)
{
    size_t left = r->b->writer_pos - r->pos;
    ssize_t nwritten;

    if (left == 0)
        return 0;

    nwritten = write(fd, r->b->data + r->pos, left);
    if (nwritten > 0)
    {
        r->pos += nwritten;
        block_reset_pos_if_possible(r->b);
    }

    return nwritten;
}
