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
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>

#include "mytar.h"
#include "main.h"
#include "filters.h"
#include "index_from_tar.h"
#include "rsync.h"

static
struct {
    enum {
        IN_HEADER,
        IN_DATA,
        IN_EXTRA_DATA,
        IN_LONG_FILENAME
    } state;
    char *longfilename;
    struct header_gnu_tar header;
    unsigned long long filedata_left;
    unsigned long long until_header_left;
    unsigned long long data_read;
    unsigned long long total_data_read;
    size_t block;
} sm;

static struct mytar *indextar;

static struct rsync_signature *rsync_signature = 0;

char index_filename[PATH_MAX];

void advance_block()
{
    while (sm.total_data_read > (sm.block+1) * command_line.blocksize)
        ++sm.block;
}

static int
process_part(const char *data, size_t len)
{
    size_t maxlen;
    size_t amount_read = 0;

    switch(sm.state)
    {
        case IN_HEADER:
            maxlen = (sizeof sm.header - sm.data_read);
            if (maxlen > len)
                maxlen = len;
            memcpy(((char *)&sm.header) + sm.data_read, data, maxlen);
            sm.data_read += maxlen;
            amount_read = maxlen;
            sm.total_data_read += amount_read;
            break;
        case IN_LONG_FILENAME:
            maxlen = sm.filedata_left;
            if (maxlen > len)
                maxlen = len;
            assert(sm.longfilename);
            memcpy(sm.longfilename + sm.data_read, data, maxlen);
            sm.data_read += maxlen;
            sm.filedata_left -= maxlen;
            sm.until_header_left -= maxlen;
            amount_read = maxlen;
            sm.total_data_read += amount_read;
            if (sm.filedata_left == 0)
            {
                if (strlen(sm.longfilename) > sm.data_read)
                    fatal_error("index_from_tar: Wrong long-filename length");
                sm.state = IN_EXTRA_DATA;
                sm.data_read = 0;
                /* Here we don't update the smblock! we want to
                 * keep it as the start of the long name header */
            }
            break;
        case IN_EXTRA_DATA:
            maxlen = sm.until_header_left;
            if (maxlen > len)
                maxlen = len;
            sm.data_read += maxlen;
            sm.until_header_left -= maxlen;
            amount_read = maxlen;
            sm.total_data_read += amount_read;
            if (sm.until_header_left == 0)
            {
                sm.state = IN_HEADER;
                sm.data_read = 0;
                advance_block();
            }
            break;
        case IN_DATA:
            maxlen = sm.filedata_left;
            if (maxlen > len)
                maxlen = len;
            sm.data_read += maxlen;
            sm.filedata_left -= maxlen;
            sm.until_header_left -= maxlen;
            amount_read = maxlen;

            if (rsync_signature && maxlen > 0)
                rsync_signature_work(rsync_signature, data, maxlen);

            sm.total_data_read += amount_read;
            if (command_line.debug > 2)
                fprintf(stderr, "index_from_tar: still %lli to skip\n", sm.filedata_left);
            if (sm.filedata_left == 0)
            {
                sm.state = IN_EXTRA_DATA;
                sm.data_read = 0;
                advance_block();

                if (rsync_signature)
                {
                    size_t siglen;
                    size_t len;
                    int res;

                    /* Mark end of file to rsync */
                    rsync_signature_work(rsync_signature, 0, 0);

                    /* Should output the block name too */
                    siglen = rsync_signature_size(rsync_signature);
                    len = siglen + strlen(index_filename) + 1 /*\0*/;

                    mytar_set_size(indextar, len);
                    mytar_write_header(indextar);

                    /* The name, and \0 */
                    res = mytar_write_data(indextar, index_filename,
                            strlen(index_filename) + 1);
                    if (res == -1)
                        error("Cannot write index tar 1 - mytar_write_data");
                    /* Then the rsync signature */
                    res = mytar_write_data(indextar, rsync_signature->output,
                            siglen);
                    if (res == -1)
                        error("Cannot write index tar 2 - mytar_write_data");

                    rsync_signature_free(rsync_signature);
                    rsync_signature = 0;

                    res = mytar_write_end(indextar);
                    if (res == -1)
                        error("Cannot write index tar - mytar_write_end");
                }
            }
            break;
    }

    if (sm.state == IN_HEADER && sm.data_read == sizeof sm.header)
    {
        const struct header_gnu_tar *sh = &sm.header;
        int checksum;
        unsigned long long size;
        int should_rsync = 0;

        /* Process the header */
        if (sh->checksum[0] == '\0')
        {
            /* Skip block, it should be final zero block. We could check it. */
            sm.data_read = 0;
            sm.state = IN_HEADER;
            return amount_read;
        }
        checksum = read_octal_number(sh->checksum, sizeof sh->checksum);
        if (calc_checksum(sh) != checksum)
            error("Failed checksum interpreting tar");

        size = read_size(sh->size);

        sm.filedata_left = size;
        sm.until_header_left = size;

        /* Clamp at 512 bytes */
        if (sm.until_header_left % 512 > 0)
            sm.until_header_left += 512 - sm.until_header_left % 512;

        if (sh->typeflag[0] == 'L')
        {
            sm.longfilename = malloc(sm.filedata_left+1);
            if (!sm.longfilename)
                fatal_error("Cannot allocate");
            sm.longfilename[sm.filedata_left] = '\0';
            sm.state = IN_LONG_FILENAME;
            sm.data_read = 0;
            return amount_read;
        }

        if (sh->typeflag[0] == 'K') /* Long link name. We ignore it */
        {
            sm.state = IN_DATA;
            sm.data_read = 0;
            return amount_read;
        }

        /* Check that we parse a GNU tar archive,
         * and that we know the type. */
        if (strncmp(sh->magic, "ustar ", 6) != 0 ||
                strncmp(sh->version, " \0", 2) != 0 ||
                (sh->typeflag[0] != '0' &&
                sh->typeflag[0] != '5' &&
                sh->typeflag[0] != '2' &&
                sh->typeflag[0] != '1'
            ))
        {
            /* Unknown file. Skip data. */
            char *fname;
            fname = read_fixed_size_string(sh->name, sizeof sh->name);
            fprintf(stderr, "Unknown header type, skipping %llu bytes: %s\n",
                    sm.until_header_left, fname);
            free(fname);
            sm.data_read = 0;
            if (sm.filedata_left > 0)
                sm.state = IN_DATA;
            else
            {
                sm.state = IN_HEADER;
                advance_block();
            }
            return amount_read;
        }


        mytar_new_file(indextar);
        if (sm.longfilename)
        {
            if (command_line.debug > 1)
                fprintf(stderr, "Writing index entry for the file %s\n", sm.longfilename);
            mytar_set_filename(indextar, sm.longfilename);
            free(sm.longfilename);
            sm.longfilename = 0;
        }
        else
        {
            char name[sizeof sh->name + 1];
            strcpyn(name, sh->name, sizeof sh->name);
            if (command_line.debug > 1)
                fprintf(stderr, "Writing index entry for the file %s\n", name);
            mytar_set_filename(indextar, name);
        }

        mytar_set_mode(indextar, read_octal_number(sh->mode, sizeof sh->mode));
        mytar_set_size(indextar, 0);
        mytar_set_uid(indextar, read_octal_number(sh->uid, sizeof sh->uid));
        mytar_set_gid(indextar, read_octal_number(sh->gid, sizeof sh->gid));
        mytar_set_uname(indextar, sh->uname); /* internal strncopy will save harm */
        mytar_set_gname(indextar, sh->gname); /* internal strncopy will save harm */
        switch(sh->typeflag[0])
        {
            case '5':
                mytar_set_filetype(indextar, S_IFDIR);
                break;
            case '0':
                if (command_line.should_rsync &&
                        sm.filedata_left > command_line.rsync_minimal_size)
                    should_rsync = 1;

                if (should_rsync)
                    mytar_set_filetype(indextar, S_IFREG);
                else
                    mytar_set_filetype(indextar, S_IFLNK);
                break;
            case '1':
            case '2':
                mytar_set_filetype(indextar, S_IFLNK);
                break;
            default:
                error("Indexing unknown file type");
        }
        mytar_set_mtime(indextar, read_octal_number(sh->mtime, sizeof sh->mtime));
        mytar_set_atime(indextar, read_octal_number(sh->mtime, sizeof sh->mtime));

        {
            /* Start of block file - header */
            snprintf(index_filename, sizeof index_filename,
                    "block%zu.tar%s_%llu", sm.block,
                    get_filter_extensions(filter),
                    (unsigned long long) size);
            if (sh->typeflag[0] != '5' && !should_rsync)
                mytar_set_linkname(indextar, index_filename);
        }

        if (should_rsync)
        {
            assert(rsync_signature == 0);
            rsync_signature = rsync_signature_new();
        }
        else
            mytar_write_header(indextar);

        sm.data_read = 0;
        if (sm.filedata_left > 0)
        {
            if (command_line.debug > 1)
                fprintf(stderr, "index_from_tar: Going to skip %llu bytes\n", sm.filedata_left);
            sm.state = IN_DATA;
        }
        else
        {
            sm.state = IN_HEADER;
            advance_block();
        }
    }

    return amount_read;
}

static void
index_from_tar_data(const char *data, size_t len)
{
    const char *p = data;
    while(len > 0)
    {
        size_t res;
        res = process_part(p, len);
        if (command_line.debug > 2)
            fprintf(stderr, "index_from_tar, processed %zu bytes\n", res);
        len -= res;
        p += res;
    }
}

void
index_from_tar(int infd, int outfd)
{
    const int buffersize = 1*1024*1024;
    char *buffer = malloc(buffersize);
    if (!buffer)
        error("Cannot allocate memory");

    indextar = mytar_new();
    mytar_open_fd(indextar, outfd);

    while(1)
    {
        int res;
        res = read(infd, buffer, buffersize);
        if (res == -1 && errno == EINTR)
            continue;
        if (res == -1)
            error("Error reading while index_from_tar");

        if (res == 0)
            break;
        if (command_line.debug > 2)
            fprintf(stderr, "index_from_tar, read %i bytes\n", res);
        index_from_tar_data(buffer, res);
    }

    if (command_line.debug > 1)
        fprintf(stderr, "index_from_tar: finishing index\n");

    mytar_write_archive_end(indextar);

    free(buffer);
}
