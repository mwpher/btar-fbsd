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
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <errno.h>
#include "main.h"
#include "mytar.h"
#include "readtar.h"

static int
process_part(struct readtar *readtar, const char *data, size_t len)
{
    size_t maxlen;
    size_t amount_read = 0;

    switch(readtar->state)
    {
        case IN_HEADER:
            maxlen = (sizeof readtar->header - readtar->data_read);
            if (maxlen > len)
                maxlen = len;
            memcpy(((char *)&readtar->header) + readtar->data_read, data, maxlen);
            readtar->data_read += maxlen;
            amount_read = maxlen;
            readtar->total_data_read += amount_read;
            break;
        case IN_LONG_FILENAME:
            maxlen = readtar->filedata_left;
            if (maxlen > len)
                maxlen = len;
            assert(readtar->longfilename);
            memcpy(readtar->longfilename + readtar->data_read, data, maxlen);
            readtar->data_read += maxlen;
            readtar->filedata_left -= maxlen;
            readtar->until_header_left -= maxlen;
            amount_read = maxlen;
            readtar->total_data_read += amount_read;
            if (readtar->filedata_left == 0)
            {
                readtar->state = IN_EXTRA_DATA;
                readtar->data_read = 0;
                /* Here we don't update the smblock! we want to
                 * keep it as the start of the long name header */
            }
            break;
        case IN_LONG_LINKNAME:
            maxlen = readtar->filedata_left;
            if (maxlen > len)
                maxlen = len;
            assert(readtar->longlinkname);
            memcpy(readtar->longlinkname + readtar->data_read, data, maxlen);
            readtar->data_read += maxlen;
            readtar->filedata_left -= maxlen;
            readtar->until_header_left -= maxlen;
            amount_read = maxlen;
            readtar->total_data_read += amount_read;
            if (readtar->filedata_left == 0)
            {
                readtar->state = IN_EXTRA_DATA;
                readtar->data_read = 0;
                /* Here we don't update the smblock! we want to
                 * keep it as the start of the long name header */
            }
            break;
        case IN_EXTRA_DATA:
            maxlen = readtar->until_header_left;
            if (maxlen > len)
                maxlen = len;
            readtar->data_read += maxlen;
            readtar->until_header_left -= maxlen;
            amount_read = maxlen;
            readtar->total_data_read += amount_read;
            if (readtar->until_header_left == 0)
            {
                readtar->state = IN_HEADER;
                readtar->data_read = 0;
            }
            break;
        case IN_DATA:
            maxlen = readtar->filedata_left;
            if (maxlen > len)
                maxlen = len;
            readtar->data_read += maxlen;
            readtar->filedata_left -= maxlen;
            readtar->until_header_left -= maxlen;
            amount_read = maxlen;
            readtar->cb.new_data(data, maxlen, readtar->cb.userdata);
            readtar->total_data_read += amount_read;
            if (readtar->filedata_left == 0)
            {
                readtar->data_read = 0;
                if (readtar->until_header_left > 0)
                    readtar->state = IN_EXTRA_DATA;
                else
                    readtar->state = IN_HEADER;
            }
            break;
    }

    if (readtar->state == IN_HEADER && readtar->data_read == sizeof readtar->header)
    {
        const struct header_gnu_tar *sh = &readtar->header;
        int checksum;
        struct readtar_file file;

        readtar->data_read = 0;

        if (sh->checksum[0] == '\0')
        {
            /* Skip block, it should be final zero block. We could check it. */
            readtar->state = IN_HEADER;
            return amount_read;
        }

        checksum = read_octal_number(sh->checksum, sizeof sh->checksum);
        if (calc_checksum(sh) != checksum || strncmp(sh->magic, "ustar", 5))
        {
            /* Just to print the message only once until the next header */
            if (command_line.debug && readtar->good_header)
                fprintf(stderr, "readtar: failed checksum interpreting tar, at pos %llu. "
                        "Searching header...\n", readtar->total_data_read);
            readtar->good_header = 0;
            readtar->state = IN_HEADER;
            return amount_read;
        }

        if (!readtar->good_header)
            if (command_line.debug)
                fprintf(stderr, "readtar: found new header at %llu.\n",
                        readtar->total_data_read);
        readtar->good_header = 1;

        file.size = read_size(readtar->header.size);
        readtar->filedata_left = file.size;
        readtar->until_header_left = file.size;

        /* Clamp at 512 bytes */
        if (readtar->until_header_left % 512 > 0)
            readtar->until_header_left += 512 - readtar->until_header_left % 512;

        if (sh->typeflag[0] == 'L')
        {
            readtar->longfilename = malloc(readtar->filedata_left + 1);
            if (!readtar->longfilename)
                fatal_error("Cannot allocate");
            readtar->longfilename[readtar->filedata_left] = '\0';
            readtar->state = IN_LONG_FILENAME;
            readtar->data_read = 0;
            return amount_read;
        }

        if (sh->typeflag[0] == 'K') /* Long link name. We ignore it */
        {
            readtar->longlinkname = malloc(readtar->filedata_left + 1);
            if (!readtar->longlinkname)
                fatal_error("Cannot allocate");
            readtar->longlinkname[readtar->filedata_left] = '\0';
            readtar->state = IN_LONG_LINKNAME;
            readtar->data_read = 0;
            return amount_read;
        }

        file.name = readtar->longfilename ? readtar->longfilename :
            read_fixed_size_string(readtar->header.name, sizeof readtar->header.name);
        file.linkname = readtar->longlinkname ? readtar->longlinkname :
            read_fixed_size_string(readtar->header.linkname, sizeof readtar->header.linkname);
        file.size = read_size(readtar->header.size);
        file.header = &readtar->header;

        if (command_line.debug > 1)
            fprintf(stderr, "readtar: found header for file %s.\n", file.name);

        if (file.size > 0)
        {
            readtar->filedata_left = file.size;
            readtar->until_header_left = file.size;
            readtar->state = IN_DATA;
        }

        /* Clamp at 512 bytes */
        if (readtar->until_header_left % 512 > 0)
            readtar->until_header_left += 512 - readtar->until_header_left % 512;

        {
            enum readtar_newfile_result rres;
            rres = readtar->cb.new_file(&file, readtar->cb.userdata);
            if (rres == READTAR_SKIPDATA && readtar->fd >= 0)
            {
                int res;

                /* Attempt seeking in case of skip, to go faster */
                res = lseek(readtar->fd, readtar->until_header_left, SEEK_CUR);
                if (res != -1)
                {
                    readtar->state = IN_HEADER;
                    readtar->filedata_left = 0;
                    readtar->until_header_left = 0;
                }
            }
        }

        free(file.name);
        readtar->longfilename = 0;

        free(file.linkname);
        readtar->longlinkname = 0;
    }

    return amount_read;
}

size_t
readtar_bytes_to_next_change(const struct readtar *readtar)
{
    size_t maxlen;
    switch(readtar->state)
    {
        case IN_HEADER:
            maxlen = (sizeof readtar->header - readtar->data_read);
            break;
        case IN_LONG_FILENAME:
            maxlen = readtar->filedata_left;
            break;
        case IN_LONG_LINKNAME:
            maxlen = readtar->filedata_left;
            break;
        case IN_EXTRA_DATA:
            maxlen = readtar->until_header_left;
            break;
        case IN_DATA:
            maxlen = readtar->filedata_left;
            break;
        default:
            fatal_error("Unknown readtar state");
            maxlen = 0; /* Silent warning */
            break;
    }
    return maxlen;
}

void
process_this_tar_data(struct readtar *readtar, const char *data, size_t len)
{
    const char *p = data;
    while(len > 0)
    {
        size_t res;
        res = process_part(readtar, p, len);
        if (command_line.debug > 2)
            fprintf(stderr, "readtar, processed %zu bytes\n", res);
        len -= res;
        p += res;
    }
}

void
init_readtar(struct readtar *readtar, const struct readtar_callbacks *cb)
{
    readtar->cb = *cb;
    readtar->longfilename = 0;
    readtar->longlinkname = 0;
    readtar->filedata_left = 0;
    readtar->until_header_left = 0;
    readtar->data_read = 0;
    readtar->total_data_read = 0;
    readtar->state = IN_HEADER;
    readtar->good_header = 1;
    readtar->fd = -1; /* For skip to work */
}

void
read_full_tar(int infd, struct readtar *readtar)
{
    const int buffersize = 1*1024*1024;

    char *buffer = malloc(buffersize);
    if (!buffer)
        fatal_error("Cannot allocate memory");

    while(1)
    {
        int res;
        res = read(infd, buffer, buffersize);
        if (res == -1 && errno == EINTR)
            continue;
        if (res == -1)
            error("Error reading while readtar");

        if (res == 0)
            break;
        if (command_line.debug > 2)
            fprintf(stderr, "readtar, read %i bytes\n", res);
        process_this_tar_data(readtar, buffer, res);
    }

    if (command_line.debug > 1)
        fprintf(stderr, "readtar: finishing tar\n");

    free(buffer);
}
