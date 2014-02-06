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
#include <sys/select.h>
#include <limits.h>
#include <assert.h>
#include <stdio.h>
#include <time.h>
#include "main.h"
#include "block.h"
#include "mytar.h"
#include "filters.h"
#include "filememory.h"

extern struct filter *filter;

struct file_memory *
file_memory_new(int fd)
{
    struct file_memory *im = malloc(sizeof(*im));
    if (!im)
        fatal_error("Cannot allocate");

    im->fd = fd;
    im->bo = block_new(buffersize);
    if (command_line.debug)
        fprintf(stderr, "Starting file memory from fd %i\n", im->fd);

    return im;
}

int
file_memory_finished(struct file_memory *im)
{
    if (im->fd >= 0)
        return 0;
    return 1;
}

void
file_memory_prepare_readfds(struct file_memory *im, fd_set *fdset, int *nfds)
{
    if (im->fd >= 0)
        addfd(fdset, im->fd, nfds);
}

void
file_memory_check_readfds(struct file_memory *im, fd_set *fdset)
{
    if (im->fd >= 0 && FD_ISSET(im->fd, fdset))
    {
        int nread = block_fill_from_fd_multi(im->bo, im->fd,
                im->bo->allocated);
        if (nread == -1)
            fatal_errno("Failed read from filememory filter, fd %i", im->fd);
        else if (nread == 0)
        {
            close(im->fd);
            im->fd = -1;
        }
    }
}

void
file_memory_to_tar(struct file_memory *im, const char *namepattern,
        const char *filter_extensions, struct mytar *tar)
{
    int res;
    char filename[PATH_MAX];
    struct block *indexb;

    if (command_line.debug)
        fprintf(stderr, "Writing the %s to the btar stream\n", namepattern);

    /* Start of block file - header */
    snprintf(filename, sizeof filename, namepattern, filter_extensions);

    mytar_new_file(tar);
    mytar_set_filename(tar, filename);
    mytar_set_gid(tar, getgid());
    mytar_set_uid(tar, getuid());
    mytar_set_size(tar, im->bo->total_written);
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

    /* Index data */
    indexb = im->bo;
    do
    {
        int res;
        /* The block body - all in bo */
        res = mytar_write_data(tar, indexb->data, indexb->writer_pos);
        if (res == -1)
            error("Could not write index mytar data");
        assert((size_t) res == indexb->writer_pos);
    } while((indexb = indexb->nextblock) != 0);

    /* The block end - tar padding */
    res = mytar_write_end(tar);
    if (res == -1)
        error("Could not write mytar file end");

    if (command_line.debug)
        fprintf(stderr, "File Memory %s written\n", namepattern);
}
