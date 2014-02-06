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
#include <sys/types.h>
#include <unistd.h>
#include <fnmatch.h>
#include <string.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>

#include "main.h"
#include "loadindex.h"
#include "mytar.h"
#include "readtar.h"
#include "filters.h"
#include "block.h"
#include "rsync.h"
#include "extract.h"

static char *blocks;
static int allocated_blocks = 0;

/* From main.c */
extern struct filter *defilter;

/* From traverse.c */
extern const char rdiff_extension[];
extern const size_t rdiff_extension_len;

static void
set_block_seen(int block)
{
    if (block >= allocated_blocks)
    {
        int before = allocated_blocks;
        int after = block+1;
        allocated_blocks = after;
        blocks = realloc(blocks, after);
        if (!blocks)
            fatal_error("Cannot realloc");
        memset(blocks + before, 0, after - before);
    }
}

static void
set_blocks(int block, int nblocks)
{
    int i;

    set_block_seen(block+nblocks-1);

    for(i=0; i < nblocks; ++i)
    {
        blocks[block+i] = 1;
    }
}

static int
should_process_block(int block)
{
    if (allocated_blocks == 0)
        return 1;

    if (block >= allocated_blocks)
        return 1;

    return blocks[block];
}

static int
matches_paths(const char *path)
{
    if (command_line.paths)
    {
        int i = 0;
        while(command_line.paths[i] != 0)
        {
            if (command_line.debug > 1)
                fprintf(stderr,
                        "Check traverse path: \"%s\" matches \"%s\"?\n",
                        path, command_line.paths[i]);
            if(fnmatch(command_line.paths[i],
                        path, 0) == 0)
                return 1;
            ++i;
        }
        return 0;
    }
    /* In case of no traverse path, extract all */
    return 1;
}

struct intar_state
{
    unsigned long long nread;
    unsigned long long expected_size;
    int writing;
    int mtime;
    int atime;
    char *name;
    struct mytar *tar;
    int fd;
    struct rsync_patch *rsync_patch;
};

static void
set_mtime(const struct intar_state *is)
{
    int res;
    struct timeval t[2];

    assert(is->name != NULL);

    t[0].tv_sec = is->atime;
    t[0].tv_usec = 0;
    t[1].tv_sec = is->mtime;
    t[1].tv_usec = 0;
    res = lutimes(is->name, t);
    if (res == -1)
        fprintf(stderr, "Cannot set times to %s: %s\n", is->name,
                strerror(errno));
}

static void
intar_new_data_cb(const char *data, size_t len, void *userdata)
{
    struct intar_state *is = (struct intar_state *) userdata;

    if (is->writing)
    {
        ssize_t res;
        if (command_line.action == EXTRACT_TO_TAR)
        {
            res = mytar_write_data(is->tar, data, len);
            if ((size_t) res != len)
                fatal_errno("Cannot write to extraction tar");

            is->nread += len;
            if (is->nread == is->expected_size)
            {
                res = mytar_write_end(is->tar);
                if (res == -1)
                    fatal_errno("Cannot write to extraction tar");
            }
        }
        else if (command_line.action == EXTRACT)
        {
            assert(is->fd >= 0);
            if (is->rsync_patch)
            {
                rsync_patch_work(is->rsync_patch, data, len);
            }
            else
            {
                res = write_all(is->fd, data, len);
                if ((size_t) res != len)
                    fatal_errno("Cannot write to file");
            }

            is->nread += len;
            if (is->nread == is->expected_size)
            {
                if (is->rsync_patch)
                {
                    rsync_patch_work(is->rsync_patch, 0, 0);
                    assert(is->rsync_patch->basename != NULL);
                    res = unlink(is->rsync_patch->basename);
                    if (res != 0)
                        fprintf(stderr, "Cannot unlink tmp base file for rsync patch %s",
                                is->rsync_patch->basename);
                    rsync_patch_free(is->rsync_patch);
                    is->rsync_patch = 0;
                }
                close(is->fd);
                set_mtime(is);
                is->fd = -1;
            }
        }
    }
}

static int
matches_rdiff_extension(const char *path, char *base)
{
#ifndef WITH_LIBRSYNC
    path = path;
    base = base;
#else
    size_t len = strlen(path);
    if (len > rdiff_extension_len)
    {
        int start = len - rdiff_extension_len;
        if (strcmp(path+start, rdiff_extension) == 0)
        {
            if (base)
            {
                strncpy(base, path, start);
                base[start] = '\0';
            }
            return 1;
        }
    }
#endif
    return 0;
}

static enum readtar_newfile_result
intar_new_file_cb(const struct readtar_file *file, void *userdata)
{
    struct intar_state *is = (struct intar_state *) userdata;
    char basename[PATH_MAX];
    int res;

    int matches_rdiff;

    matches_rdiff = matches_rdiff_extension(file->name, basename);

    if (matches_paths(file->name) || (matches_rdiff &&
                matches_paths(basename)))
    {
        free(is->name);
        is->name = 0;

        if (command_line.verbose)
            fprintf(stderr, "%s\n", file->name);

        if (command_line.action == EXTRACT)
        {
            char newbase[PATH_MAX];

            const struct header_gnu_tar *h = file->header;

            int mode = read_octal_number(h->mode, sizeof(h->mode));

            if (h->typeflag[0] == '0')
            {
                if (matches_rdiff)
                {
                    if (command_line.debug)
                        fprintf(stderr, "Rsync patch found for %s\n", basename);

                    strcpyn(newbase, basename, sizeof newbase);
                    strcat(newbase, ".btar_rbase");

                    res = rename(basename, newbase);
                    if (res == -1)
                    {
                        fprintf(stderr, "Cannot rename %s to %s for rsync patch\n",
                                basename, newbase);
                        matches_rdiff = 0;
                    }

                    if (matches_rdiff)
                    {
                        is->fd = open(basename, O_CREAT | O_TRUNC | O_WRONLY, mode);
                        if (is->fd == -1)
                        {
                            fprintf(stderr, "Cannot create %s: %s\n", file->name,
                                    strerror(errno));
                            is->writing = 0;
                            return READTAR_SKIPDATA;
                        }

                        is->rsync_patch = rsync_patch_new(newbase, is->fd);

                        if (!is->rsync_patch)
                        {
                            fprintf(stderr, "Cannot open the base %s for the patch in %s: %s. "
                                    "Extracting raw file.",
                                    newbase, file->name, strerror(errno));
                            close(is->fd);
                            matches_rdiff = 0;
                        }
                    }
                }

                if (!matches_rdiff)
                {
                    if (command_line.debug)
                        fprintf(stderr, "Creating file %s\n",  file->name);
                    is->fd = open(file->name, O_CREAT | O_TRUNC | O_WRONLY, mode);
                    if (is->fd == -1)
                    {
                        fprintf(stderr, "Cannot create %s: %s\n", file->name,
                                strerror(errno));
                        is->writing = 0;
                        return READTAR_SKIPDATA;
                    }
                }

                res = chmod(matches_rdiff ? basename : file->name, mode);
                if (res == -1)
                    fprintf(stderr, "Cannot set mode to %s: %s\n",
                            matches_rdiff ? basename : file->name,
                            strerror(errno));

                is->writing = 1;
                is->expected_size = file->size;
                is->nread = 0;

                /* For later setting lutimes */
                is->name = strdup(matches_rdiff ? basename : file->name);

                if (is->expected_size == 0)
                {
                    close(is->fd);
                    assert(is->rsync_patch == 0);
                    set_mtime(is);
                    is->fd = -1;
                    is->writing = 0;
                }
            }
            else if (h->typeflag[0] == '2')
            {
                res = symlink(file->linkname, file->name);
                if (res == -1)
                    fprintf(stderr, "Could not create symlink %s: %s\n", file->name,
                            strerror(errno));

                is->writing = 0;
            }
            else if (h->typeflag[0] == '5')
            {
                res = mkdir(file->name, mode);
                if (command_line.debug > 1 && res == -1)
                    fprintf(stderr, "Could not create directory %s: %s\n", file->name,
                            strerror(errno));

                is->writing = 0;
            }
            else 
            {
                fprintf(stderr, "Unknown file type '%c' for file %s\n",
                        h->typeflag[0], file->name);
                is->writing = 0;
                return READTAR_SKIPDATA;
            }

            res = lchown(matches_rdiff ? basename : file->name,
                    read_octal_number(h->uid, sizeof(h->uid)),
                    read_octal_number(h->gid, sizeof(h->gid)));
            if (res == -1)
                fprintf(stderr, "Cannot set uid/gid to %s: %s\n",
                        matches_rdiff ? basename : file->name,
                        strerror(errno));

            {
                struct timeval t[2];
                is->atime = read_octal_number(h->atime, sizeof(h->atime));
                t[0].tv_sec = is->atime;
                t[0].tv_usec = 0;
                is->mtime = read_octal_number(h->mtime, sizeof(h->mtime));
                t[1].tv_sec = is->mtime;
                t[1].tv_usec = 0;
                res = lutimes(matches_rdiff ? basename : file->name, t);
                if (res == -1)
                    fprintf(stderr, "Cannot set times to %s: %s\n",
                            matches_rdiff ? basename : file->name,
                            strerror(errno));
            }
        }
        else if (command_line.action == EXTRACT_TO_TAR)
        {
            if (matches_rdiff)
                fprintf(stderr, "Outputting raw rdiff patch %s to tar\n",
                        file->name);
            memcpy(&is->tar->header, file->header, sizeof(*file->header));

            mytar_set_filename(is->tar, file->name);
            if (file->linkname)
                mytar_set_linkname(is->tar, file->linkname);

            res = mytar_write_header(is->tar);
            if (res == -1)
                fatal_errno("Cannot write header to extraction tar");
            is->writing = 1;
            is->expected_size = file->size;
            is->nread = 0;
        }
        return READTAR_NORMAL;
    }
    else
    {
        is->writing = 0;
        return READTAR_SKIPDATA;
    }
}

static void
deletedtar_new_data_cb(const char *data, size_t len, void *userdata)
{
}

static enum readtar_newfile_result
deletedtar_new_file_cb(const struct readtar_file *file, void *userdata)
{
    int res;
    int should_delete;

    if (command_line.paths == 0)
        should_delete = 1;
    else
    {
        int j = 0;
        should_delete = 0;
        while(command_line.paths[j] != 0)
        {
            should_delete = 0;
            if(fnmatch(command_line.paths[j], file->name, 0) == 0)
            {
                should_delete = 1;
                break;
            }
            ++j;
        }
    }

    if (should_delete)
    {
        if (command_line.verbose)
            fprintf(stderr, "Deleting: %s\n", file->name);

        if (file->header->typeflag[0] == '5')
        {
            res = rmdir(file->name);
            if (res == -1 && errno != ENOENT)
            {
                fprintf(stderr, "Error in rmdir \"%s\": %s\n", file->name,
                        strerror(errno));
            }
        }
        else
        {
            res = unlink(file->name);
            if (res == -1 && errno != ENOENT)
            {
                fprintf(stderr, "Error in unlink \"%s\": %s\n", file->name,
                        strerror(errno));
            }
        }
    }

    return READTAR_SKIPDATA;
}

struct block_extraction_state
{
    unsigned long long nread;
    unsigned long long expected_size;
    int should_read;
    int filter_in;
    int filter_out;
    struct block *to_filterin;
    int close_filter_in;
    struct readtar intar;
    struct intar_state intar_state;
    enum {
        BES_BLOCK,
        BES_INDEX,
        BES_DELETER
    } blocktype;
    int outindex;
    int outdeleted;
};

static void
block_extraction_new_data_cb(const char *data, size_t len, void *userdata)
{
    struct block_extraction_state *bes = (struct block_extraction_state *) userdata;
    if (bes->should_read)
    {
        struct block *b = bes->to_filterin;
        size_t res;

        res = block_fill_from_memory(b, data, len);
        assert(res == len);

        bes->nread += len;

        if (bes->nread == bes->expected_size)
        {
            /* This will make the select() loop not fill the to_filter_in block
             * until this is cleared */
            bes->close_filter_in = 1;
        }
    }
}

static enum readtar_newfile_result
block_extraction_new_file_cb(const struct readtar_file *file, void *userdata)
{
    struct block_extraction_state *bes = (struct block_extraction_state *) userdata;

    int block;
    int should_read = 0;
   
    bes->expected_size = file->size;
    bes->nread = 0;

    if (strncmp(file->name, "block", 5) == 0)
    {
        block = block_name_to_int(file->name);
        bes->blocktype = BES_BLOCK;

        if (should_process_block(block))
        {
            int res;
            struct filter *mydefilter;

            if (command_line.debug)
            {
                fprintf(stderr, "Processing block %i\n", block);
            }

            if (!bes->should_read)
            {
                struct readtar_callbacks icb = { intar_new_file_cb,
                    intar_new_data_cb,
                    &bes->intar_state};
                if (command_line.debug)
                    fprintf(stderr, "Restart internal tar due to block change\n");
                init_readtar(&bes->intar, &icb);
            }
            should_read = 1;

            if (defilter)
                mydefilter = defilter;
            else
                mydefilter = defilters_from_extensions(file->name);

            run_filters(mydefilter, &bes->filter_in, &bes->filter_out);
            res = fcntl(bes->filter_in, F_SETFL, O_NONBLOCK);
            if (res == -1)
                error("Cannot fcntl");

            if (mydefilter != defilter)
                free_filters(mydefilter);
        }
        else
        {
            if(command_line.debug)
                fprintf(stderr, "Skipping block %i\n", block);
        }
    }
    /* Check the prefix */
    else if (strncmp(file->name, "index.tar", sizeof("index.tar")-1) == 0)
    {
        if (bes->outindex >= 0)
        {
            int res;
            struct filter *mydefilter;

            should_read = 1;
            bes->blocktype = BES_INDEX;

            if (defilter)
                mydefilter = defilter;
            else
                mydefilter = defilters_from_extensions(file->name);

            run_filters(mydefilter, &bes->filter_in, &bes->filter_out);
            res = fcntl(bes->filter_in, F_SETFL, O_NONBLOCK);
            if (res == -1)
                error("Cannot fcntl");

            if (mydefilter != defilter)
                free_filters(mydefilter);
        }
    }
    else if (strncmp(file->name, "deleted.tar", sizeof("deleted.tar")-1) == 0)
    {
        /* We should not delete, if not told so, and when extracting to TAR */
        if ((command_line.should_delete && command_line.action == EXTRACT) ||
                bes->outdeleted >= 0)
        {
            int res;
            struct filter *mydefilter;

            should_read = 1;
            bes->blocktype = BES_DELETER;

            if (defilter)
                mydefilter = defilter;
            else
                mydefilter = defilters_from_extensions(file->name);

            run_filters(mydefilter, &bes->filter_in, &bes->filter_out);
            res = fcntl(bes->filter_in, F_SETFL, O_NONBLOCK);
            if (res == -1)
                error("Cannot fcntl");

            if (mydefilter != defilter)
                free_filters(mydefilter);

            /* Keep the callbacks, restart the intar */
            static const struct readtar_callbacks dcb = { deletedtar_new_file_cb,
                deletedtar_new_data_cb, 0};
            init_readtar(&bes->intar, &dcb);
        }
    }

    bes->should_read = should_read;
    if (should_read)
        return READTAR_NORMAL;
    else
        return READTAR_SKIPDATA;
}

static void
process_internal_tar_data(struct block_extraction_state *bes,
        char *data, size_t len)
{
    process_this_tar_data(&bes->intar, data, len);
}

static void
do_block_extraction(int fd, int outindex, int outdeleted)
{
    struct readtar rt;
    struct block_extraction_state bes;
    struct readtar_callbacks cb = { block_extraction_new_file_cb,
        block_extraction_new_data_cb,
        &bes};

    struct readtar_callbacks icb = { intar_new_file_cb,
        intar_new_data_cb,
        &bes.intar_state};

    struct block_reader *br_to_filterin;
    int closed_in = 0;

    char *bufferout = 0;

    bufferout = malloc(buffersize);
    if (!bufferout)
        fatal_error("Cannot allocate");

    bes.to_filterin = block_new(buffersize);
    br_to_filterin = block_reader_new(bes.to_filterin);

    init_readtar(&rt, &cb);
    rt.fd = fd; /* For skip to work */

    bes.nread = 0;
    bes.expected_size = 0;
    bes.filter_in = -1;
    bes.filter_out = -1;
    bes.close_filter_in = 0;
    bes.intar_state.tar = 0;
    bes.intar_state.fd = -1;
    bes.intar_state.name = 0;
    bes.intar_state.rsync_patch = 0;
    bes.outindex = outindex;
    bes.outdeleted = outdeleted;
    init_readtar(&bes.intar, &icb);

    /* We don't want to create the tar, if we run with
     * -N without paths. That should extract the blocks raw,
     * as the btar filter input could have been not a tar. */
    if (command_line.action == EXTRACT_TO_TAR &&
            (command_line.add_create_index || command_line.paths))
    {
        bes.intar_state.tar = mytar_new();
        mytar_open_fd(bes.intar_state.tar, 1); /* stdout */
    }
    
    /* Llegint del fitxer btar, hem d'enviar als filtres.
     * El que llegim dels filtres, ho hem de processar i extreure.
     * Hem d'evitar deadlock dels filtres. O sigui:
     *  * No podem esperar que les escriptures al filtres no-bloquegin
     *  * Hem d'estar sempre a punt de llegir dels filtres.
     *
     * Si X bytes del btar base mai donaran més de X bytes per enviar
     * als filtres, podem jugar amb un buffer intermig, on hi
     * posem tot el que s'ha d'enviar als filtres, i fer-lo anar dins
     * el select.
     *
     * Hem de vigilar de no llegir més enllà del fitxer, per a gestionar
     * els filtres que van alhora.
     * */

    while(1)
    {
        int nfds = 0;
        fd_set readfds;
        fd_set writefds;
        int res;
        size_t can_send_to_filterin;
        size_t bytes_to_next_readtar_change;
        
        FD_ZERO(&readfds);
        FD_ZERO(&writefds);

        /* Main tar fd. Whatever read, will fill the to_filterin block */
        can_send_to_filterin = bes.to_filterin->allocated
            - bes.to_filterin->writer_pos;

        if (closed_in || bes.close_filter_in)
            can_send_to_filterin = 0;

        if (can_send_to_filterin > buffersize)
            can_send_to_filterin = buffersize;

        bytes_to_next_readtar_change = readtar_bytes_to_next_change(&rt);
        if (can_send_to_filterin > bytes_to_next_readtar_change)
            can_send_to_filterin = bytes_to_next_readtar_change;

        if (fd >= 0 && can_send_to_filterin > 0)
            addfd(&readfds, fd, &nfds);

        /* From filter. To be processed by inner tar. 
         * That will send data to stdout. */
        if (bes.filter_out >= 0)
            addfd(&readfds, bes.filter_out, &nfds);

        /* From the buffer to the filter */
        if (block_reader_can_read(br_to_filterin))
            addfd(&writefds, bes.filter_in, &nfds);

        res = select(nfds, &readfds, &writefds, 0, 0);
        if (res == -1 && errno == EINTR)
            continue;

        if (fd >= 0 && FD_ISSET(fd, &readfds))
        {
            ssize_t nread;
            nread = read(fd, bufferout, can_send_to_filterin);
            if (nread == -1 && errno != EINTR)
                fatal_errno("Cannot read from filters");
            if (nread > 0)
            {
                if (command_line.debug > 2)
                    fprintf(stderr, "extract: %zi data to process_this_tar_data\n",
                            nread);
                /* This may block, but it's final btar output. */
                process_this_tar_data(&rt, bufferout, nread);
            }
            else if (nread == 0)
            {
                closed_in = 1;

                if (bes.filter_in == -1)
                    break;
            }
        }

        if (bes.filter_out >= 0 && FD_ISSET(bes.filter_out, &readfds))
        {
            ssize_t nread;
            nread = read(bes.filter_out, bufferout, buffersize);
            if (nread == -1 && errno != EINTR)
                fatal_errno("Cannot read from filters");
            if (nread > 0)
            {
                if (command_line.debug > 2)
                    fprintf(stderr, "extract: %zi data to process_internal_tar_data\n",
                            nread);

                /* This may block, but it's final btar output. */
                if (bes.blocktype == BES_BLOCK)
                {
                    if (command_line.action != EXTRACT_TO_TAR ||
                            command_line.paths || command_line.add_create_index)
                        process_internal_tar_data(&bes, bufferout, nread);
                    else
                    {
                        int res;
                        /* Directly to stdout in case of EXTRACT_TO_TAR */
                        assert(command_line.action == EXTRACT_TO_TAR);

                        res = write_all(1, bufferout, nread);
                        if (res == -1)
                            fatal_errno("Can't write to stdout");
                    }
                }
                else if (bes.blocktype == BES_INDEX && outindex >= 0)
                {
                    int res;
                    /* Directly to stdout in case of EXTRACT_TO_TAR */
                    assert(command_line.action == EXTRACT_TO_TAR);

                    res = write_all(outindex, bufferout, nread);
                    if (res == -1)
                        fatal_errno("Can't write to outindex fd=%i", outindex);
                }
                else if (bes.blocktype == BES_DELETER && outdeleted >= 0)
                {
                    int res;
                    /* Directly to stdout in case of EXTRACT_TO_TAR */
                    assert(command_line.action == EXTRACT_TO_TAR);

                    res = write_all(outdeleted, bufferout, nread);
                    if (res == -1)
                        fatal_errno("Can't write to outdeleted fd=%i", outdeleted);
                }
                else
                {
                    /* To process index and deleter. For simple extractions,
                     * outindex and outdeleted will be -1, and we have to
                     * process that data. */
                    process_internal_tar_data(&bes, bufferout, nread);
                }
            }
            if (nread == 0)
            {
                if (command_line.debug > 1)
                    fprintf(stderr, "extract: end on filter_out (%i)\n", bes.filter_out);
                close(bes.filter_out);
                bes.filter_out = -1;
                bes.close_filter_in = 0; /* This should make read more from fd */

                if (closed_in)
                    break;
            }
        }

        if (bes.filter_in >= 0 && FD_ISSET(bes.filter_in, &writefds))
        {
            ssize_t nwritten;
            nwritten = block_reader_to_fd(br_to_filterin, bes.filter_in);
            if (nwritten == -1 && errno != EINTR)
                fatal_errno("Cannot write to filters");
            
            if (command_line.debug > 2)
                fprintf(stderr, "extract: %zi data sent to filterin\n", nwritten);

            if (!block_reader_can_read(br_to_filterin) && bes.close_filter_in)
            {
                if (command_line.debug > 1)
                    fprintf(stderr, "extract: close(%i) filter_in\n", bes.filter_in);
                close(bes.filter_in);
                bes.filter_in = -1;
            }

        }
    }

    if (bes.intar_state.tar)
        mytar_write_archive_end(bes.intar_state.tar);

    block_reader_free(br_to_filterin);
    block_free(bes.to_filterin);
    free(bufferout);
}

static int
fnmatch_with_rdiff_extension(const char *pattern, const char *name, int flags)
{
#ifndef WITH_LIBRSYNC
    pattern = pattern;
    name = name;
    flags = flags;
    return -1;
#else
    char p[PATH_MAX];
    strcpyn(p, pattern, sizeof p);
    strncat(p, rdiff_extension, PATH_MAX - strlen(pattern) - 1);

    return fnmatch(p, name, flags);
#endif
}

void extract(int fd, int outindex, int outdeleted)
{
    int res;
    int can_lseek = 1;

    /* Load the index if possible */
    res = lseek(fd, 0, SEEK_CUR);
    if (res == -1)
        can_lseek = 0;

    /* We are interested in the index in the case of
     * having set traverse paths (what to extract).
     *
     * We also accept -N, on not processing the indices. */
    if (command_line.paths && can_lseek && command_line.add_create_index)
    {
        load_index_from_tar(fd);

        /* Prepare what files we have to extract. Traverse paths in command line. */
        size_t nelems;
        size_t i;
        const struct IndexElem *ptr = index_get_elements(&nelems);

        for(i = 0; i < nelems; ++i)
        {
            const struct IndexElem *e = &ptr[i];
            int j = 0;

            /* Directories have block -1; trick as we can't store any block in their
             * tar header */
            if (e->block == -1)
                continue;

            while(command_line.paths[j] != 0)
            {
                if (command_line.debug > 1)
                    fprintf(stderr,
                            "Check future extraction: \"%s\" matches \"%s\"?\n",
                            e->name, command_line.paths[j]);

                if(fnmatch(command_line.paths[j],
                            e->name, 0) == 0 ||
                        fnmatch_with_rdiff_extension(command_line.paths[j],
                            e->name, 0) == 0)
                {
                    if (command_line.debug)
                    {
                        fprintf(stderr,
                                "  Will extract \"%s\", from block %i to %i\n",
                                e->name, e->block, e->block+e->nblocks-1);
                    }
                    set_blocks(e->block, e->nblocks);
                }
                else
                    set_block_seen(e->block);
                ++j;
            }
        }

        res = lseek(fd, 0, SEEK_SET);
        if (res == -1)
            fatal_errno("Cannot lseek stdin, while a while ago we could");
    }

    do_block_extraction(fd, outindex, outdeleted);

    free(blocks);
    blocks = 0;
    allocated_blocks = 0;

    /* This is specially important, or the next call to extract will combine
     * indices */
    free_index();
}
