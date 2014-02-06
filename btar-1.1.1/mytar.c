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
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include "main.h"
#include "mytar.h"

ssize_t
write_all(int fd, const void *buf, size_t n)
{
    size_t left = n;
    const char *ptr = buf;

    do
    {
        int res;
        res = write(fd, ptr, left);
        if (res == -1)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        left -= res;
        ptr += res;
    } while(left != 0);

    return n;
}

struct mytar *
mytar_new()
{
    struct mytar *t = malloc(sizeof(*t));
    if (!t)
        fatal_error("Cannot allocate");
    memset(t, 0, sizeof(*t));
    return t;
}

void
mytar_open_fd(struct mytar *t, int fd)
{
    t->fd = fd;
}

void
mytar_new_file(struct mytar *t)
{
    memset(&t->header, 0, sizeof(t->header));
    /* GNU tar */
    strncpy(t->header.magic, "ustar ", sizeof t->header.magic);
    memcpy(t->header.version, " \0", 2);
    memcpy(t->header.uname, "unknown", sizeof "unknown");
    memcpy(t->header.gname, "unknown", sizeof "unknown");
    t->file_size = 0;
    t->longname = 0;
    t->longlinkname = 0;
}

void
mytar_set_filename(struct mytar *t, char *name)
{
    size_t len = strlen(name);
    if(len >= sizeof t->header.name)
    {
        t->longname = realloc(t->longname, len+1);
        if (!t->longname)
            error("Cannot realloc");
        strcpyn(t->longname, name, len+1);
    }
    strcpyn(t->header.name, name, sizeof t->header.name);
}

void
mytar_set_uid(struct mytar *t, int uid)
{
    snprintf(t->header.uid, sizeof(t->header.uid), "%07o ", uid);
}

void
mytar_set_size(struct mytar *t, unsigned long long size)
{
    if (size > 077777777777ULL)
    {
        const int limit = sizeof(t->header.size)-1;
        int i;

        t->header.size[0] = 0x80;

        /* Binary */
        for(i = 0; i < limit; ++i)
        {
            t->header.size[limit - i] = size & 0xff;
            size = size >> 8;
        }
    }
    else
        snprintf(t->header.size, sizeof(t->header.size), "%011Lo ", size);
}

void
mytar_set_gid(struct mytar *t, int gid)
{
    snprintf(t->header.gid, sizeof(t->header.gid), "%07o ", gid);
}

void
mytar_set_mode(struct mytar *t, int mode)
{
    snprintf(t->header.mode, sizeof(t->header.mode), "%07o ", mode);
}

int
mytar_set_filetype(struct mytar *t, int type)
{
    char ctype;
    switch(type & S_IFMT)
    {
        case S_IFREG:
            ctype = '0';
            break;
        case S_IFDIR:
            ctype = '5';
            break;
        case S_IFLNK:
            ctype = '2';
            break;
        default:
            /* Unsupported type */
            return -1;
    }

    t->header.typeflag[0] = ctype;

    return 0;
}

void
mytar_set_linkname(struct mytar *t, const char *linkname)
{
    size_t len = strlen(linkname);
    if(len >= sizeof t->header.name)
    {
        t->longlinkname = realloc(t->longlinkname, len+1);
        if (!t->longlinkname)
            error("Cannot realloc");
        strcpyn(t->longlinkname, linkname, len+1);
    }
    strcpyn(t->header.linkname, linkname,
            sizeof(t->header.linkname));
}

void
mytar_set_mtime(struct mytar *t, time_t time)
{
    snprintf(t->header.mtime, sizeof(t->header.mtime), "%011o ", (unsigned int) time);
}

void
mytar_set_atime(struct mytar *t, time_t time)
{
    snprintf(t->header.atime, sizeof(t->header.atime), "%011o ", (unsigned int) time);
}

void
mytar_set_uname(struct mytar *t, const char *uname)
{
    strcpyn(t->header.uname, uname,
            sizeof(t->header.uname));
}

void
mytar_set_gname(struct mytar *t, const char *gname)
{
    strcpyn(t->header.gname, gname,
            sizeof(t->header.gname));
}

void
mytar_set_from_stat(struct mytar *t, struct stat *s)
{
    mytar_set_mode(t, s->st_mode & ~S_IFMT);
    if (S_ISDIR(s->st_mode) || S_ISLNK(s->st_mode))
        mytar_set_size(t, 0);
    else
        mytar_set_size(t, s->st_size);
    mytar_set_uid(t, s->st_uid);
    {
        struct passwd *p = getpwuid(s->st_uid);
        if (p)
            mytar_set_uname(t, p->pw_name);
    }
    mytar_set_gid(t, s->st_gid);
    {
        struct group *p = getgrgid(s->st_gid);
        if (p)
            mytar_set_gname(t ,p->gr_name);
    }
    mytar_set_filetype(t, s->st_mode & S_IFMT);
    mytar_set_mtime(t, s->st_mtime);
    mytar_set_atime(t, s->st_atime);
}

int
calc_checksum(const struct header_gnu_tar *h)
{
    int sum = 0;
    size_t i;
    struct header_gnu_tar th;
    memcpy(&th, h, sizeof th);

    memset(th.checksum, ' ', sizeof(th.checksum));
    for(i=0; i < sizeof(th); ++i)
    {
        sum += ((unsigned char *)&th)[i];
    }
    return sum;
}

static void
set_checksum(struct header_gnu_tar *h)
{
    memset(h->checksum, ' ', sizeof(h->checksum));
    snprintf(h->checksum, sizeof h->checksum, "%06o", calc_checksum(h));
}

ssize_t
mytar_write_header(struct mytar *t)
{
    ssize_t res;

    if (t->longname)
    {
        struct header_gnu_tar h2;
        memset(&h2, 0, sizeof h2);
        strncpy(h2.magic, "ustar ", sizeof h2.magic);
        memcpy(h2.version, " \0", 2);

        const char n[] = "././@LongLink";
        const char u[] = "root";
        strcpyn(h2.name, n, sizeof(h2.name));
        strcpyn(h2.uname, u, sizeof(h2.uname));
        strcpyn(h2.gname, u, sizeof(h2.uname));
        snprintf(h2.size, sizeof(h2.size), "%011o", (unsigned int) strlen(t->longname));
        snprintf(h2.mode, sizeof h2.mode, "%07o", 0);
        snprintf(h2.uid, sizeof h2.uid, "%07o", 0);
        snprintf(h2.gid, sizeof h2.gid, "%07o", 0);
        snprintf(h2.mtime, sizeof(h2.mtime), "%011o ", 0);
        h2.typeflag[0] = 'L'; /* GNU Long name coming as data */

        set_checksum(&h2);

        res = write_all(t->fd, &h2, sizeof(h2));
        if (res != 0)
            t->total_written += res;
        if (res == -1)
            error("Cannot write tar");

        t->file_data_written = 0;
        res = mytar_write_data(t, t->longname, strlen(t->longname));
        if (res == -1)
            error("Cannot write tar long name data");
        res = mytar_write_end(t);
        if (res == -1)
            error("Cannot write tar long name end");
    }

    if (t->longlinkname)
    {
        struct header_gnu_tar h2;
        memset(&h2, 0, sizeof h2);
        strncpy(h2.magic, "ustar ", sizeof h2.magic);
        memcpy(h2.version, " \0", 2);

        const char n[] = "././@LongLink";
        const char u[] = "root";
        strcpyn(h2.name, n, sizeof(h2.name));
        strcpyn(h2.uname, u, sizeof(h2.uname));
        strcpyn(h2.gname, u, sizeof(h2.uname));
        snprintf(h2.size, sizeof(h2.size), "%011o", (unsigned int) strlen(t->longlinkname));
        snprintf(h2.mode, sizeof h2.mode, "%07o", 0);
        snprintf(h2.uid, sizeof h2.uid, "%07o", 0);
        snprintf(h2.gid, sizeof h2.gid, "%07o", 0);
        snprintf(h2.mtime, sizeof(h2.mtime), "%011o ", 0);
        h2.typeflag[0] = 'K'; /* GNU Long link name coming as data */

        set_checksum(&h2);

        res = write_all(t->fd, &h2, sizeof(h2));
        if (res != 0)
            t->total_written += res;
        if (res == -1)
            error("Cannot write tar");

        t->file_data_written = 0;
        res = mytar_write_data(t, t->longlinkname, strlen(t->longlinkname));
        if (res == -1)
            error("Cannot write tar long name data");
        res = mytar_write_end(t);
        if (res == -1)
            error("Cannot write tar long name end");
    }
    set_checksum(&t->header);

    res = write_all(t->fd, &t->header, sizeof(t->header));
    if (res != 0)
        t->total_written += res;

    t->file_data_written = 0;

    free(t->longname);
    t->longname = 0;
    free(t->longlinkname);
    t->longlinkname = 0;

    return res;
}

ssize_t
mytar_write_data(struct mytar *t, const char *buffer, size_t n)
{
    int res;

    res = write_all(t->fd, buffer, n);
    if (res != -1)
    {
        t->file_data_written += res;
        t->total_written += res;
    }

    return res;
}

ssize_t
mytar_write_end(struct mytar *t)
{
    static const char c[512]; /* Will be zero */
    int over;

    /* Could be the case, of zero data */
    over = (t->file_data_written % 512);
    if (over > 0)
    {
        int tail;
        int res;
        tail = 512 - over;
        res = write_all(t->fd, c, tail);
        if (res != -1)
            t->total_written += res;
        return res;
    }
    return 0;
}

ssize_t
mytar_write_archive_end(struct mytar *t)
{
    static const char c[1024]; /* Will be zero */
    ssize_t res;

    res = write_all(t->fd, c, sizeof(c));
    if (res != -1)
        t->total_written += res;
    return res;
}

unsigned long long
read_octal_number(const char *str, int len)
{
    char pattern[15];
    long long unsigned v;
    int res;
    int i;

    for(i=0; i < len; ++i)
    {
        if(str[i] == 0)
            break;
        if ((str[i] < '0' || str[i] > '7'))
            return 0;
    }

    snprintf(pattern, sizeof(pattern), "%%%iLo", len);

    if (str[0] == '\0')
        return 0;

    res = sscanf(str, pattern, &v);
    if (res != 1)
        error("Failed number parsing");

    return v;
}

unsigned long long
read_size(const char *str)
{
    unsigned long long v;
    int res;
    const int sizeofstr = sizeof(((struct header_gnu_tar *)0)->size);

    if ((str[0] & 0x80) == 0x80)
    {
        int i;
        const unsigned char *val = (unsigned char *) str;
        const int limit = sizeofstr;

        v = val[0] & ~0x80;

        /* Binary */
        for(i = 1; i < limit; ++i)
        {
            v = (v << 8) + val[i];
        }
    }
    else
    {
        char pattern[50];
        snprintf(pattern, sizeof(pattern), "%%%iLo",
                sizeofstr-1);

        if (str[0] == '\0')
            v = 0;
        else
        {
            res = sscanf(str, pattern, &v);
            if (res != 1)
                fatal_error("Failed number parsing on \"%s\"", str);
        }
    }

    return v;
}

char *
read_fixed_size_string(const char *str, int len)
{
    char *news;

    news = malloc(len+1);
    if (!news)
        fatal_error("Cannot allocate");
    strcpyn(news, str, len);
    news[len] = '\0';
    return news;
}

/* GNU tar files don't have prefix. I keep this code
 * for the day we may process non-gnu tar files. */
#if 0
char *
read_filename(const struct header_gnu_tar *h)
{
    int finallen = 0;
    int lenprefix;
    char myprefix[sizeof h->prefix + 1];
    char myname[sizeof h->name + 1];
    char *news;

    /* Prepare a zero-limited prefix string */
    strncpy(myprefix, h->prefix, sizeof h->prefix);
    myprefix[sizeof h->prefix] = '\0';
    strncpy(myname, h->name, sizeof h->name);
    myname[sizeof h->name] = '\0';

    lenprefix = strlen(myprefix);

    finallen += sizeof h->name + 1 /* '\0' */;

    if (lenprefix > 0)
    {
        finallen += lenprefix + 1 /* '/' */;
        news = malloc(finallen);
        strcpy(news, myprefix);
        strcat(news, "/");
        strcat(news, myname);
    }
    else
    {
        news = malloc(finallen);
        strcat(news, myname);
    }

    return news;
}
#endif
