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
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <assert.h>
#include "block.h"
#include "mytar.h"
#include "main.h"
#include "filters.h"
#include "blockprocess.h"

extern struct filter *filter;

struct block_process *
block_process_new(int nblock)
{
    struct block_process *bp = malloc(sizeof(*bp));
    if (!bp)
        fatal_error("Cannot allocate");
    bp->fd_filterin = -1;
    bp->fd_filterout = -1;
    bp->bo = block_new_never_back(
            command_line.blocksize + /*margin for block*/ 1*1024*1024);
    bp->bi = 0;
    if (filter)
    {
        size_t readblocksize;
        /* In case of parallelism, we want to be able to read as much as
         * needed, regardless of how much the filter accepts. Then
         * we should read a full block into RAM */
        if (command_line.parallelism > 1)
            readblocksize = command_line.blocksize;
        else
            readblocksize = buffersize;
        bp->bi = block_new(readblocksize);
        bp->br_to_filter = block_reader_new(bp->bi);
    }
    bp->closed_in = 0;
    bp->block_finished = 1;
    bp->nblock = nblock;
    bp->has_read = 0;
    bp->finished_read_ack = 0;
    return bp;
}

int
block_process_can_read(const struct block_process *bp)
{
    struct block *b = bp->bo;
    if (filter)
        b = bp->bi;

    if (block_can_accept(b) &&
            b->total_written < command_line.blocksize)
        return 1;
    return 0;
}

size_t
block_process_total_read(const struct block_process *bp)
{
    struct block *b = bp->bo;
    if (filter)
        b = bp->bi;

    return b->total_written;
}

int
block_process_finished_reading(struct block_process *bp)
{
    /* Check that all input readers are happy */
    struct block *b = bp->bo;
    if (filter)
        b = bp->bi;

    if ((bp->closed_in || b->total_written == command_line.blocksize))
        return 1;
    return 0;
}

int
block_process_finished(struct block_process *bp)
{
    /* Check that all input readers are happy */
    struct block *b = bp->bo;
    if (filter)
        b = bp->bi;

    return bp->block_finished && block_are_readers_done(b);
}

void
block_process_reset(struct block_process *bp, int nblock)
{
    if (filter)
    {
        bp->bi->total_written = 0;
        assert(bp->bi->writer_pos == 0);
        assert(bp->br_to_filter->pos == 0);
    }

    assert(bp->bo->writer_pos == 0);
    bp->bo->total_written = 0;
    bp->nblock = nblock;
    bp->block_finished = 1;
    bp->has_read = 0;
    bp->finished_read_ack = 0;
}

void
prepare_readfds(struct block_process *bp, fd_set *fdset, int *nfds, int should_read)
{
    struct block *b = bp->bo;

    if (filter)
    {
        if (bp->fd_filterout >= 0 && block_can_accept(b))
        {
            /* Read from the filter output */
            addfd(fdset, bp->fd_filterout, nfds);
        }
        b = bp->bi;
    }

    if (should_read && !bp->closed_in && block_process_can_read(bp))
        addfd(fdset, 0, nfds); /* read from stdin */
}

void
prepare_writefds(struct block_process *bp, fd_set *fdset, int *nfds)
{
    if (bp->fd_filterin >= 0 && block_reader_can_read(bp->br_to_filter))
    {
        addfd(fdset, bp->fd_filterin, nfds); /* write to filter */
    }
}

void
check_read_fds(struct block_process *bp, fd_set *readfds, int should_read)
{
    struct block *b = bp->bo;
    if (filter)
        b = bp->bi;

    if (should_read && FD_ISSET(0, readfds))
    {
        ssize_t nread;
        size_t max_to_read = command_line.blocksize - b->total_written;
        nread = block_fill_from_fd(b, 0, max_to_read);
        if (nread == -1)
        {
            if (errno == EINTR)
                return;
            error("Failed read from stdin");
        }

        if (nread == 0)
        {
            bp->closed_in = 1;
            if (command_line.debug)
                fprintf(stderr, "End of input to the btar blocker %p\n", bp);
            if (!filter)
                bp->block_finished = 1;
            if (filter && b->writer_pos == 0)
            {
                /* We can close the filter input, because we are not
                 * going to write anything more there */
                if (bp->fd_filterin >= 0)
                {
                    close(bp->fd_filterin);
                    bp->fd_filterin = -1;
                }
            }
        }
        else
        {
            bp->block_finished = 0;
            bp->has_read = 1;
            if (filter && bp->fd_filterin == -1)
            {
                assert(bp->fd_filterout == -1);
                if (command_line.debug)
                    fprintf(stderr, "Starting block %i\n", bp->nblock);
                run_filters(filter, &bp->fd_filterin, &bp->fd_filterout);
                /* Otherwise the next execing filters will get them */
		set_cloexec(bp->fd_filterin);
		set_cloexec(bp->fd_filterout);

                int res;
                res = fcntl(bp->fd_filterin, F_SETFL, O_NONBLOCK);
                if (res == -1)
                    error("Cannot fcntl");
            }
        }

        if (b->total_written == command_line.blocksize)
        {
            if (command_line.debug)
                fprintf(stderr, "BlockProcess %p finished reading\n", bp);

            if (!filter)
                bp->block_finished = 1;
        }
    }

    if (bp->fd_filterout >= 0 && FD_ISSET(bp->fd_filterout, readfds))
    {
        int nread;
        /* We need to read until we get all the block in RAM, because
         * we need to know its size */
        nread = block_fill_from_fd(bp->bo, bp->fd_filterout, bp->bo->allocated);
        if (nread == -1)
            fatal_errno("Failed read from filter");

        if (nread == 0)
        {
            close(bp->fd_filterout);
            bp->fd_filterout = -1;

            bp->block_finished = 1;
        }
    }
}

void
check_write_fds(struct block_process *bp, fd_set *writefds)
{
    if (bp->fd_filterin >= 0 && FD_ISSET(bp->fd_filterin, writefds))
    {
        int nwritten = block_reader_to_fd(bp->br_to_filter, bp->fd_filterin);
        if (nwritten == -1 && errno != EINTR)
            fatal_errno("Failed write to filter");

        if (bp->bi->total_written == command_line.blocksize &&
                !block_reader_can_read(bp->br_to_filter))
        {
            close(bp->fd_filterin);
            bp->fd_filterin = -1;
        }
        else if(bp->closed_in && !block_reader_can_read(bp->br_to_filter))
        {
            close(bp->fd_filterin);
            bp->fd_filterin = -1;
        }
    }
}

void
xor_to_xorblock(struct block_process *bp, struct block *xorblock)
{
    size_t mysize = bp->bo->writer_pos;
    size_t xorsize = mysize + 1 /* next expand byte */;
    size_t oldsize = xorblock->allocated;
    size_t i;
    char expand_byte;

    assert(oldsize > 0);

    expand_byte = xorblock->data[oldsize-1];

    block_realloc_set(xorblock, xorsize, expand_byte);

    for(i=0; i < mysize; ++i)
        xorblock->data[i] ^= bp->bo->data[i];

    /* Xor the last byte (expand_byte), that works for
     * any next byte there may be. */
    for(i=mysize; i < xorblock->allocated; ++i)
        xorblock->data[i] ^= 0;
}

void
dump_block_to_tar(struct block_process *bp, struct mytar *tar)
{
    ssize_t res;
    char filename[PATH_MAX];

    if (command_line.debug)
        fprintf(stderr, "Writing block %i to the btar stream\n", bp->nblock);

    /* Start of block file - header */
    snprintf(filename, sizeof filename, "block%i.tar%s", bp->nblock,
            get_filter_extensions(filter));

    mytar_new_file(tar);
    mytar_set_filename(tar, filename);
    mytar_set_gid(tar, getgid());
    mytar_set_uid(tar, getuid());
    mytar_set_size(tar, bp->bo->writer_pos);
    mytar_set_mode(tar, 0644 | S_IFREG);

    /* I'm not sure what's better, to save a mtime like this or not.
     * It would be nice that the hash of the final tar does not depend
     * on the date when it runs, but it's also fine to have the date somewhere.
     * Let's go with setting mtime by now */
    mytar_set_mtime(tar, time(NULL));

    mytar_set_filetype(tar, S_IFREG);
    res = mytar_write_header(tar);
    if (res == -1)
        error("Failed to write header");

    /* The block body - all in bo */
    res = mytar_write_data(tar, bp->bo->data, bp->bo->writer_pos);
    if (res == -1)
        error("Could not write mytar data");
    assert((size_t) res == bp->bo->writer_pos);

    /* The block end - tar padding */
    res = mytar_write_end(tar);
    if (res == -1)
        error("Could not write mytar file end");

    /* As it has no reader, we reset it here */
    /* Manual reset */
    bp->bo->writer_pos = 0;
    {
        size_t i;

        for(i=0; i < bp->bo->nreaders; ++i)
            bp->bo->readers[i]->pos = 0;
    }

    if (command_line.debug)
        fprintf(stderr, "Block %i written to the btar stream\n", bp->nblock);
}

struct block_reader * block_process_new_input_reader(struct block_process *bp)
{
    struct block *b = bp->bo;
    if (filter)
        b = bp->bi;

    return block_reader_new(b);
}

int block_process_has_read(struct block_process *bp)
{
    return bp->has_read;
}
