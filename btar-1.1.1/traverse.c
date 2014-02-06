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
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <assert.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#ifdef USE_MMAP
#include <sys/mman.h>
#endif
#include <errno.h>
#include <limits.h>
#include <fnmatch.h>
#include "main.h"
#include "filters.h"
#include "traverse.h"
#include "mytar.h"
#include "loadindex.h"
#include "rsync.h"

struct traverse
{
    int dirfd;
    int filefd;
    DIR *dir;
    struct traverse *up;
    char *name;
    char *displayname;
    int is_dir;
    int emitted;
    int in_reference;
    struct stat dirstat;
};

static struct traverse *mytraverse;
static int npath; /* pointer to the list of paths */

static struct mytar *intar;
static struct mytar *indextar;
static struct mytar *deletedtar;

static char *buffer;
static int inbuffer;
static int bufferoffset;

static int current_block = 0;

static struct rsync_signature *rsync_signature = 0;
static struct rsync_delta *rsync_delta = 0;

static char filename[PATH_MAX]; /* As has to be open()ed */
static char display_filename[PATH_MAX]; /* Without initial slashes */
static char block_filename[PATH_MAX];
static char delta_filename[PATH_MAX];

static int creating_delta = 0;

static unsigned long long size_expected;
static time_t mtime_expected;

const char rdiff_extension[] = ".btar_rdiff";
const size_t rdiff_extension_len = sizeof(rdiff_extension)-1;


static int
matches_extensions(const char *path)
{
    size_t len = strlen(path);
    if (len > rdiff_extension_len)
    {
        int start = len - rdiff_extension_len;
        if (strcmp(path+start, rdiff_extension) == 0)
        {
            fprintf(stderr, "Omiting %s due to conflicting extension with btar\n",
                    path);
            return 1;
        }
    }
    return 0;
}

static int
matches_exclude_pattern(const char *path)
{
    if (command_line.exclude_patterns)
    {
        int i = 0;
        while(command_line.exclude_patterns[i] != 0)
        {
            if (command_line.debug > 1)
                fprintf(stderr,
                        "Check exclude pattern: \"%s\" matches \"%s\"?\n",
                        path, command_line.exclude_patterns[i]);
            if(fnmatch(command_line.exclude_patterns[i],
                        path, 0) == 0)
                return 1;
            ++i;
        }
    }
    return 0;
}

static void
emit_until_this(struct traverse *t)
{
    /* Although it is a recursive function, we care
     * that the variable will not be used by more than one function
     * body */
    static char dirname_with_slash[PATH_MAX];
    int res;

    if (!t || t->emitted)
        return;

    if (t && !t->emitted)
        emit_until_this(t->up);

    mytar_new_file(intar);
    if (indextar)
        mytar_new_file(indextar);

    if(strlen(t->displayname) + 1 /* / */ + 1 /* \0 */>= PATH_MAX)
        fatal_error("Directory name too long");

    strcpy(dirname_with_slash, t->displayname);
    strcat(dirname_with_slash, "/");

    if (command_line.verbose)
    {
        fprintf(stderr, "%s\n", dirname_with_slash);
    }

    mytar_set_filename(intar, dirname_with_slash);

    if (indextar)
        mytar_set_filename(indextar, dirname_with_slash);

    mytar_set_from_stat(intar, &t->dirstat);
    if (indextar)
        mytar_set_from_stat(indextar, &t->dirstat);

    res = mytar_write_header(intar);
    if (res == -1)
        error("Cannot write index tar");

    if (indextar)
    {
        res = mytar_write_header(indextar);
        if (res == -1)
            error("Cannot write index tar - mytar_write_header");
    }

    /* We don't need to call mytar_write_end, as it will convey zero data */

    t->emitted = 1;
}

static void
emit_directories()
{
    emit_until_this(mytraverse);
}

static void
start_path(const char *path)
{
    if (command_line.debug)
        fprintf(stderr, "Starting traverse of the path %s\n", path);

    mytraverse = malloc(sizeof(*mytraverse));
    if (!mytraverse)
        fatal_error("Cannot allocate");
    mytraverse->up = 0;
    mytraverse->filefd = -1;
    mytraverse->dirfd = -1;
    mytraverse->dir = 0;
    mytraverse->emitted = 0;
    mytraverse->name = malloc(PATH_MAX);
    if (!mytraverse->name)
        fatal_error("Cannot allocate");
    mytraverse->displayname = malloc(PATH_MAX);
    if (!mytraverse->displayname)
        fatal_error("Cannot allocate");

    /* We should have to search the index here, and decide based on that.
     * But an additional directory won't harm */
    mytraverse->in_reference = 0;

    assert(strlen(path) > 0);

    strcpyn(mytraverse->name, path, PATH_MAX);
    /* Remove final slashes */
    {
        int len;
        while ((len = strlen(mytraverse->name)) > 1 &&
                mytraverse->name[len-1] == '/')
        {
            mytraverse->name[len-1] = '\0';
        }
    }

    strcpyn(mytraverse->displayname, mytraverse->name,
            PATH_MAX);

    {
        /* Mark the element as seen, or it will appear in deleted.tar */
        struct IndexElem *e = index_find_element(mytraverse->displayname);
        if (e != 0)
            e->seen = 1;
    }

    /* Remove initial slashes in the name to write to the tar */
    {
        int len = strlen(mytraverse->displayname);
        int start = 0;
        int i;
        for (; mytraverse->displayname[start] == '/'; ++start);

        for(i = 0; i < len - start+1/*'\0'*/; ++i)
        {
            mytraverse->displayname[i] = mytraverse->displayname[i+start];
        }
    }

    mytraverse->is_dir = 0;

    {
        int res;
        struct stat s;
        res = lstat(mytraverse->name, &s);
        if (res == -1)
            fatal_errno("Cannot lstat %s", mytraverse->name);
        if (S_ISDIR(s.st_mode))
        {
            mytraverse->is_dir = 1;
            memcpy(&mytraverse->dirstat, &s, sizeof s);
        }
        else
            return;
    }


    mytraverse->dir = opendir(mytraverse->name);
    if (mytraverse->dir == NULL)
        error("Cannot open directory");
    set_cloexec(dirfd(mytraverse->dir));
}


static
int find_next_file()
{
    struct dirent * dirent = NULL; /* Silent a warning */
    int res;
    int dir_in_reference = 0;
    struct stat bufstat;

    if (mytraverse && !mytraverse->is_dir)
    {
        free(mytraverse->name);
        free(mytraverse);
        mytraverse = 0;
    }

    while (!mytraverse && command_line.paths[npath])
        start_path(command_line.paths[npath++]);

    if (!mytraverse)
        return -2;

    if (mytraverse->filefd != -1)
    {
        close(mytraverse->filefd);
        mytraverse->filefd = -1;
    }

    while (mytraverse->is_dir)
    {
        dirent = readdir(mytraverse->dir);
        if (dirent == NULL)
        {
            struct traverse *newt = mytraverse->up;

            /* Keep new void directories */
            if (!mytraverse->in_reference)
                emit_directories();

            closedir(mytraverse->dir);
            free(mytraverse->name);
            free(mytraverse->displayname);
            free(mytraverse);
            mytraverse = newt;
            if (mytraverse == 0)
            {
                while (mytraverse == 0 && command_line.paths[npath])
                {
                    start_path(command_line.paths[npath++]);
                }
                if (!mytraverse)
                    return -2; /* End of traverse */
                if (!mytraverse->is_dir)
                    break;
            }
        }
        else
        {
            if (strcmp(dirent->d_name, ".") &&
                    strcmp(dirent->d_name, ".."))
            {
                break;
            }
        }
    }

    {
        int left = sizeof filename;
        strcpyn(filename, mytraverse->name, left);
        left -= strlen(mytraverse->name);
        if (mytraverse->is_dir)
        {
            strncat(filename, "/", left-1);
            left -= 1;
            strncat(filename, dirent->d_name, left-1);
        }

        strcpyn(display_filename, filename, sizeof display_filename);

        /* Remove initial slashes in the name to write to the tar */
        {
            int len = strlen(display_filename);
            int start = 0;
            int i;
            for (; display_filename[start] == '/'; ++start);

            for(i = 0; i < len - start+1/*'\0'*/; ++i)
            {
                display_filename[i] = display_filename[i+start];
            }
        }
    }

    if (matches_exclude_pattern(display_filename))
        return 1; /* Go for the next */

    if (matches_extensions(filename))
        return 1; /* Go for the next */


    res = lstat(filename, &bufstat);
    if (res == -1)
    {
        error("Cannot stat file");
    }

    {
        if (!S_ISDIR(bufstat.st_mode)
                && !S_ISREG(bufstat.st_mode)
                && !S_ISLNK(bufstat.st_mode))
        {
            fprintf(stderr, "Type not implemented. Ignoring %s\n", filename);
            return 1;
        }
    }

    if (S_ISREG(bufstat.st_mode))
    {
        /* There are other exit points of find_next, but then the next call to
         * find_next will close the handle. But we have to test soon if we can
         * open it, or the rest of the work will be useless. access() is not
         * enough in case of cygwin 'Device or resource busy' */
        mytraverse->filefd = open(filename, O_RDONLY);
        if (mytraverse->filefd == -1)
        {
            fprintf(stderr, "Cannot open file: %s. Ignoring %s\n",
                    strerror(errno), filename);
            return 1;
        }
        set_cloexec(mytraverse->filefd);
    }

    mtime_expected = bufstat.st_mtime;
    size_expected = bufstat.st_size;

    if (S_ISDIR(bufstat.st_mode) ||
            S_ISREG(bufstat.st_mode) || S_ISLNK(bufstat.st_mode))
    {
        struct IndexElem *e = index_find_element(display_filename);
        if (e != 0)
        {
            e->seen = 1;
            if (S_ISDIR(bufstat.st_mode))
            {
                dir_in_reference = 1;
            }
            else 
            {
                if (bufstat.st_mtime <= e->mtime)
                {
                    if (command_line.debug > 1)
                        fprintf(stderr, "traverse: skipping file %s based on mtime\n",
                                display_filename);
                    return 1;
                }

                if (command_line.debug)
                    fprintf(stderr, "%s included based on mtime\n", display_filename);

                if (e->signature)
                {
                    assert(S_ISREG(bufstat.st_mode));

                    if (strlen(display_filename) + strlen(rdiff_extension) + 1 < PATH_MAX)
                    {
                        if (command_line.debug > 1)
                            fprintf(stderr, "traverse: preparing delta for %s\n",
                                    display_filename);
                        creating_delta = 1;
                        rsync_delta = rsync_delta_new(e->signature, e->signaturelen);
                        strcpy(delta_filename, display_filename);
                        strcat(delta_filename, rdiff_extension);
                    }
                }
            }
        }
    }

    /* access() tests dereferencing symlinks. */
    if (!S_ISLNK(bufstat.st_mode))
    {
        int perm = R_OK;
        if (S_ISDIR(bufstat.st_mode))
            perm = perm | X_OK;
        if (access(filename, perm) == -1)
        {
            if (creating_delta)
            {
                rsync_delta_free(rsync_delta);
                rsync_delta = 0;
            }
            fprintf(stderr, "Cannot read %s: %s\n", filename, strerror(errno));
            return 1;
        }
    }

    if (S_ISDIR(bufstat.st_mode))
    {
        struct traverse *t;

        t = malloc(sizeof(*mytraverse));
        if (!t)
            fatal_error("Cannot allocate");
        t->up = mytraverse;
        t->filefd = -1;
        t->name = strdup(filename);
        t->displayname = strdup(display_filename);
        t->is_dir = 1;
        t->emitted = 0;
        t->in_reference = dir_in_reference;
        memcpy(&t->dirstat, &bufstat, sizeof bufstat);

        t->dir = opendir(t->name);
        if (t->dir == NULL)
            error("Cannot open directory");
        set_cloexec(dirfd(t->dir));

        mytraverse = t;

        return 1; /* Go for the next */
    }

    /* Emit previous directories, if we find files to add */
    emit_directories();

    mytar_new_file(intar);
    if (indextar)
    {
        mytar_new_file(indextar);
    }

    if (command_line.verbose)
    {
        fprintf(stderr, "%s\n", display_filename);
    }

    mytar_set_filename(intar, display_filename);
    if (indextar)
        mytar_set_filename(indextar, display_filename);

    mytar_set_from_stat(intar, &bufstat);
    if (indextar)
        mytar_set_from_stat(indextar, &bufstat);

    if (S_ISLNK(bufstat.st_mode))
    {
        char linkname[PATH_MAX];
        res = readlink(filename, linkname, sizeof(linkname));
        if (res == -1)
            error("Error in readlinkat");
        linkname[res] = 0;

        mytar_set_linkname(intar, linkname);
    }

    /* Just before the write_header of intar, let's calculate
     * what block we are in */
    while (intar->total_written > (current_block+1) * command_line.blocksize)
    {
        ++current_block;
    }

    if (!creating_delta)
    {
        res = mytar_write_header(intar);
        if (res == -1)
            error("Cannot write internal tar");
    }

    /* Prepare the index */
    if (indextar)
    {
        int should_rsync = 0;

        if (command_line.should_rsync &&
                S_ISREG(bufstat.st_mode) &&
                bufstat.st_size > (off_t) command_line.rsync_minimal_size)
        {
            should_rsync = 1;
        }

        assert(!S_ISDIR(bufstat.st_mode));

        snprintf(block_filename, sizeof block_filename,
                "block%i.tar%s_%lli", current_block,
                get_filter_extensions(filter),
                (long long int) bufstat.st_size);

        /* In case of non-regular file, we write the header directly,
         * as we are going to reenter find_next. */
        if (!should_rsync)
        {
            mytar_set_filetype(indextar, S_IFLNK);
            mytar_set_linkname(indextar, block_filename);

            mytar_set_size(indextar, 0);
            res = mytar_write_header(indextar);
            if (res == -1)
                error("Cannot write index tar");
        }
        else
        {
            mytar_set_filetype(indextar, S_IFREG);
            rsync_signature = rsync_signature_new();
        }
    }

    inbuffer = 0;
    bufferoffset = 0;

    if (S_ISLNK(bufstat.st_mode))
    {
        res = mytar_write_end(intar);
        if (res == -1)
            error("Cannot write internal tar - mytar_write_end");
        return 1; /* Go for the next */
    }
    return 0;
}

static
int find_next()
{
    int res;
    while((res = find_next_file()) == 1);
    return res;
}

static void
dump_deleted()
{
    size_t nelems;
    const struct IndexElem *ptr = index_get_elements(&nelems);
    size_t i;
    int res;

    /* We reverse the list, this way files always go before directories,
     * and we can then delete properly the directories if possible,
     * simply traversing the list as is at extraction time. */
    index_sort_inverse();

    for(i = 0; i < nelems; ++i)
    {
        const struct IndexElem *e = &ptr[i];

        if (!e->seen)
        {
            mytar_new_file(deletedtar);
            mytar_set_filename(deletedtar, e->name);
            mytar_set_mtime(deletedtar, time(NULL));
            mytar_set_gid(deletedtar, getgid());
            mytar_set_uid(deletedtar, getuid());
            mytar_set_mode(deletedtar, 0644 | S_IFREG);
            if(e->is_dir)
                mytar_set_filetype(deletedtar, S_IFDIR);
            else
            {
                mytar_set_filetype(deletedtar, S_IFREG);
                mytar_set_size(deletedtar, 0);
            }
            res = mytar_write_header(deletedtar);
            if (res == -1)
                error("Cannot write to 'deleted' tar");
        }
    }

    res = mytar_write_archive_end(deletedtar);
    if (res == -1)
        error("Cannot write internal tar - mytar_write_archive_end");
}

int
traverse(int datafd, int indexfd, int deletedfd)
{
    if (!mytraverse)
    {
        intar = mytar_new();
        mytar_open_fd(intar, datafd);
        if (indexfd != -1)
        {
            indextar = mytar_new();
            mytar_open_fd(indextar, indexfd);
        }
        if (deletedfd != -1)
        {
            deletedtar = mytar_new();
            mytar_open_fd(deletedtar, deletedfd);
        }
#ifndef USE_MMAP
        buffer = malloc(buffersize);
        if (!buffer)
            fatal_error("Cannot allocate");
#endif
    }

    while(1)
    {
        unsigned long long total_read;
        int res;
        int skipping_data;
        size_t skipped_data;

        res = find_next();
        if (res == -1)
            error("find_next");
        else if (res == -2)
        {
            /* End of tar */
            res = mytar_write_archive_end(intar);
            if (res == -1)
                error("Cannot write internal tar - mytar_write_archive_end");

            if (indextar)
            {
                res = mytar_write_archive_end(indextar);
                if (res == -1)
                    error("Cannot write internal tar - mytar_write_archive_end");
            }

            if (deletedtar)
                dump_deleted();

            free(buffer);
            return 0;
        }

        total_read = 0;
        skipping_data = 0;
        skipped_data = 0;

        while(1)
        {
            size_t max_to_read = buffersize;

            if (max_to_read > size_expected - total_read)
                max_to_read = size_expected - total_read;

            if (max_to_read == 0)
            {
                max_to_read = buffersize;
                skipping_data = 1;
            }

            ssize_t nread;
            nread = read(mytraverse->filefd, buffer, max_to_read);
            if (nread == -1 && errno == EINTR)
                continue;
            else if (nread == -1)
                error("Cannot read file");
            else if (nread == 0)
            {
                struct stat bufstat;
                res = fstat(mytraverse->filefd, &bufstat);
                if (res == -1)
                    error("Cannot stat file while reading it");
                if (bufstat.st_mtime != mtime_expected)
                    fprintf(stderr, "File %s changed while reading (mtime)\n", filename);
                if (skipped_data > 0)
                {
                    assert(total_read == size_expected);
                    fprintf(stderr, "File %s got bigger while reading (different size)\n",
                            filename);
                }
                else if (total_read < size_expected)
                {
                    assert(skipped_data == 0);

                    fprintf(stderr, "File %s got smaller while reading (different size)\n",
                            filename);

                    /* Fill the rest with zeros */
                    memset(buffer, 0, buffersize);

                    /* Here we use the total_read counter as the amount written
                     * to the tar */
                    while(total_read < size_expected)
                    {
                        size_t max_to_write = size_expected - total_read;
                        ssize_t res;
                        if (max_to_write > buffersize)
                            max_to_write = buffersize;

                        res = mytar_write_data(intar, buffer, max_to_write);
                        if (res == -1)
                            error("Cannot write internal tar - mytar_write_data");
                        total_read += res;
                    }
                }
                break;
            }

            if (skipping_data)
                skipped_data += nread;
            else
                total_read += nread;

            if (!creating_delta && !skipping_data)
            {
                res = mytar_write_data(intar, buffer, nread);
                if (res == -1)
                    error("Cannot write internal tar - mytar_write_data");
            }

            if (rsync_signature && !skipping_data)
                rsync_signature_work(rsync_signature, buffer, nread);

            if (creating_delta && !skipping_data)
            {
                int res;
                res = rsync_delta_work(rsync_delta, buffer, nread);
                if (res != 0)
                {
                    off_t off;

                    if (command_line.debug)
                        fprintf(stderr, "traverse: cannot do delta. Restarting file.\n");
                                

                    /* We can't calculate the delta, too much memory used.
                     * We have to go back all the file and start over, writing
                     * the tar header that the find_next_file prepared */
                    rsync_delta_free(rsync_delta);
                    rsync_delta = 0;
                    creating_delta = 0;
                    skipping_data = 0;
                    total_read = 0;
                    skipped_data = 0;

                    off = lseek(mytraverse->filefd, 0, SEEK_SET);
                    if (off != 0)
                        error("Can't lseek file at failure making delta");
                    if (rsync_signature)
                    {
                        rsync_signature_free(rsync_signature);
                        rsync_signature = rsync_signature_new();
                    }

                    res = mytar_write_header(intar);
                    if (res == -1)
                        error("Cannot write internal tar");

                    continue;
                }
            }

            if (!skipping_data && (size_expected - total_read) == 0)
            {
                if (creating_delta)
                {
                    size_t size;
                    int res;

                    /* Mark end of file to rsync */
                    res = rsync_delta_work(rsync_delta, 0, 0);
                    assert(res == 0);

                    size = rsync_delta_size(rsync_delta);

                    /* I've to set a clear different name */
                    mytar_set_size(intar, size);
                    mytar_set_filename(intar, delta_filename);

                    res = mytar_write_header(intar);
                    if (res == -1)
                        error("Cannot write internal tar delta header");

                    res = mytar_write_data(intar, rsync_delta->output,
                            size);
                    if (res == -1)
                        error("Cannot write internal tar delta - mytar_write_data");

                    creating_delta = 0;
                    rsync_delta_free(rsync_delta);
                    rsync_delta = 0;
                }

                if (rsync_signature)
                {
                    size_t siglen;
                    size_t len;

                    /* Mark end of file to rsync */
                    rsync_signature_work(rsync_signature, 0, 0);

                    /* Should output the block name too */
                    siglen = rsync_signature_size(rsync_signature);
                    len = siglen + strlen(block_filename) + 1 /*\0*/;

                    mytar_set_size(indextar, len);
                    mytar_write_header(indextar);

                    /* The name, and \0 */
                    res = mytar_write_data(indextar, block_filename,
                            strlen(block_filename) + 1);
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

        }

        res = mytar_write_end(intar);
        if (res == -1)
            error("Cannot write internal tar - mytar_write_end");
    }
    return 0;
}
