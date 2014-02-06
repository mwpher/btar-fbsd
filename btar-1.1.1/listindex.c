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
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <stdlib.h>

#include "main.h"
#include "mytar.h"
#include "readtar.h"
#include "loadindex.h"
#include "filters.h"
#include "listindex.h"

extern struct filter *defilter;

static void
list_new_data_cb(const char *data, size_t len, void *userdata)
{
    /* Silent warnings */
    data = data;
    len = len;
    userdata = userdata;
}

static enum readtar_newfile_result
list_new_file_cb(const struct readtar_file *file, void *userdata)
{
    /* Silent warnings */
    userdata = userdata;

    printf("%s\n", file->name);
    return READTAR_SKIPDATA;
}

static void
list_tar(int fd)
{
    struct readtar rt;
    struct readtar_callbacks cb = { list_new_file_cb, list_new_data_cb, 0};

    if (command_line.action == LIST_INDEX)
    {
        init_readtar(&rt, &cb);

        read_full_tar(fd, &rt);
    }
    else if (command_line.action == EXTRACT_INDEX)
    {
        /* It may look a bit stupid to read and write what we have; we could have
         * sent the filters output directly to stdout, but I found it
         * harder to implement, given how we have the filters functions. */
        char *buffer = malloc(buffersize);
        if (!buffer)
            fatal_error("Cannot allocate");
        while (1)
        {
            ssize_t res;
            ssize_t nread = read(fd, buffer, buffersize);

            if (nread == -1)
            {
               if (errno == EINTR)
                   continue;
               fatal_errno("Cannot read");
            }
            if (nread == 0)
                break;
            res = write_all(1, buffer, nread);
            if (res == -1 && errno != EINTR)
                fatal_errno("Cannot write to stdout");
            if (res != nread)
                fatal_error("Cannot write to stdout as much as wanted");
        }
        free(buffer);
    }
}

static void
run_index_reader(int fdin, int *fdout, int closechild1)
{
    int pid;
    int res;
    int mypipe[2];

    res = pipe(mypipe);
    if (res == -1)
        error("Cannot pipe");

    pid = fork();

    if (pid == -1)
        error("Cannot fork");
    if (pid == 0)
    {
        close(0);
        close(mypipe[0]);
        close(closechild1);
        list_tar(fdin);
        exit(0);
    }
    else /* Parent */
    {
        close(mypipe[1]);
        *fdout = mypipe[0];
        close(fdin);
    }
}

void listindex(int fd)
{
    char *name;
    int filterin, filterout;
    unsigned long long indexsize;
    struct filter *mydefilter;
    char *buffer;
    int wait_fd;

    name = index_find_in_tar(fd, &indexsize);
    if (!name)
    {
        fatal_error_no_core("Cannot find index in btar");
    }

    if (command_line.debug)
        fprintf(stderr, "Index file name in main btar: %s\n", name);

    if (defilter)
        mydefilter = defilter;
    else
        mydefilter = defilters_from_extensions(name);

    free(name);

    run_filters(mydefilter, &filterin, &filterout);

    run_index_reader(filterout, &wait_fd, /*close child */filterin);

    buffer = malloc(buffersize);
    if (!buffer)
        fatal_error("Cannot allocate");

    while(indexsize > 0)
    {
        ssize_t res;
        unsigned long long toread = indexsize;
        int towrite;
        int written;

        if (toread > buffersize)
            toread = buffersize;

        res = read(fd, buffer, toread);
        if (res == -1)
            error("Cannot read index file from the btar");

        written = 0;
        towrite = res;
        while(towrite > 0)
        {
            res = write(filterin, buffer + written, towrite);
            if (res == -1)
            {
                if (errno != EINTR)
                    error("Cannot write");
                continue;
            }
            towrite -= res;
            written += res;
        }

        indexsize -= written;
    }

    close(filterin);

    if (mydefilter != defilter)
        free(mydefilter);

    do
    {
        ssize_t res;
        res = read(wait_fd, buffer, 1);
        if (res == -1)
        {
            if(errno == EINTR)
                continue;
            fatal_errno("Failed to read from the wait_fd");
        }
        assert(res == 0);
        if (res == 0)
            break;
    } while(1);

    free(buffer);
}
