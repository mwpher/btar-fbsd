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
#include <fcntl.h>
#include <signal.h>
#include <limits.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <time.h>

#include "main.h"
#include "mytar.h"
#include "traverse.h"
#include "loadindex.h"
#include "filters.h"
#include "index_from_tar.h"
#include "block.h"
#include "blockprocess.h"
#include "filememory.h"
#include "listindex.h"
#include "extract.h"

#define STRVERSION_(x) #x
#define STRVERSION(x) STRVERSION_(x)

/* Global */
struct command_line command_line;
struct filter *filter;
struct filter *filterindex;
struct filter *defilter;

/* Here, so the usr1 can get it */
static struct main_archive
{
    struct mytar *archive;
    int nextblock;
    char *data;
    size_t insize;
} main_archive;
static struct file_memory *im = 0; /* index.tar memory, received from the filters */
static struct file_memory *dm = 0; /* deleted.tar memory, received from the filters */
static struct block_process *ref_reading_bp; /* Just for USR1 convenience */
unsigned long long total_read_in_full_blocks = 0;

void
mainarchive_open(struct main_archive *ma, int outfd)
{
    ma->archive = mytar_new();

    mytar_open_fd(ma->archive, outfd);

    ma->nextblock = 0;
}

void
mainarchive_close(struct main_archive *ma)
{
    int res = mytar_write_archive_end(ma->archive);
    if (res == -1)
        error("Could not write archive end");
    close(1);
}

void addfd(fd_set *set, int fd, int *nfds)
{
    if (fd + 1 > *nfds)
        *nfds = fd+1;
    FD_SET(fd, set);
}

void usr1_handler(int s)
{
    static time_t t = 0;
    time_t newt;
    const char *action = "unknown";

    /* Just to avoid a warning */
    s ^= s;

    if (command_line.action != CREATE && command_line.action != FILTER)
        return;

    newt = time(NULL);

    if (command_line.action == CREATE && command_line.references == 0)
        action = "creating";
    else if (command_line.action == CREATE)
        action = "differentiating";
    else if (command_line.action == FILTER)
        action = "filtering";

    fprintf(stderr, "USR1 stats (%s)", action);
    if (ref_reading_bp)
    {
        static unsigned long long last_total = 0;
        unsigned long long total = total_read_in_full_blocks;
        if (!ref_reading_bp->finished_read_ack)
                total += block_process_total_read(ref_reading_bp);
        fprintf(stderr, " bytes_in=%llu", total);

        if (t != 0 && newt > t)
        {
            const char *units = "B/s";
            unsigned long long d = (total - last_total) / (newt - t);
            if (d > 1024)
            {
                d /= 1024;
                units = "KiB/s";
            }
            if (d > 1024)
            {
                d /= 1024;
                units = "MiB/s";
            }
            fprintf(stderr, " (%llu%s)", d, units);
        }
        last_total = total;
    }

    if (main_archive.archive)
    {
        static unsigned long long last_total = 0;
        unsigned long long total = main_archive.archive->total_written;
        fprintf(stderr, " bytes_out=%llu", total);
        if (t != 0 && newt > t)
        {
            const char *units = "B/s";
            unsigned long long d = (total - last_total) / (newt - t);
            if (d > 1024)
            {
                d /= 1024;
                units = "KiB/s";
            }
            if (d > 1024)
            {
                d /= 1024;
                units = "MiB/s";
            }
            fprintf(stderr, " (%llu%s)", d, units);
        }
        last_total = total;
        fprintf(stderr, " (block %i)", main_archive.nextblock);
    }

    t = newt;

    if (im)
        fprintf(stderr, " index_size=%zu", im->bo->total_written);

    fputc('\n', stderr);
}

void child_handler(int s)
{
    int status;
    int pid;

    /* Just to avoid a warning */
    s ^= s;

    while(1)
    {
        pid = waitpid(-1, &status, WNOHANG);

        if (pid == 0)
            return;

        if (pid == -1 && errno == ECHILD)
            return;

        if (pid == -1)
            error("Error on waitpid");

        if (WIFEXITED(status))
        {
            if (WEXITSTATUS(status) != 0)
                fatal_error_no_core("The child %i exited with return value %i",
                        pid, WEXITSTATUS(status));
            if(command_line.debug > 1)
                fprintf(stderr, "Child %i finished\n", pid);
        }
        else if (WIFSIGNALED(status))
            fatal_error_no_core("The child was terminated by signal %i",
                    WTERMSIG(status));
    }
}

void register_child_handler()
{
    struct sigaction act;

    act.sa_handler = child_handler;
    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &act, 0);
}

void register_usr1_handler()
{
    struct sigaction act;

    act.sa_handler = usr1_handler;
    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &act, 0);
}

void usage()
{
    printf("btar %s Copyright (C) 2011-2012  Lluis Batlle i Rossell\n", STRVERSION(VERSION));
    printf("usage: btar [options] [actions]\n");
    printf("actions:\n");
    printf("   -c       Create a btar file. Non-options mean directories or files to add.\n");
    printf("   -x       Extract the btar contents to disk.\n"
           "              In this case, non-options mean glob patterns to extract.\n");
    printf("   -T       Extract the btar contents as a tar to stdout.\n"
           "              In this case, non-options mean glob patterns to extract.\n");
    printf("   -l       List the btar index contents.\n");
    printf("   -L       Output the btar index as tar.\n");
    printf("   -m       Mangle filters and block size from stdin to output btar (-f or stdout)).\n");
    printf("   (none)   Make btar file from the standard input data (filter mode).\n");
    printf("options only meaningful when creating or filtering:\n");
    printf("   -b <blocksize>   Set the block size in megabytes (default 10MiB)\n");
    printf("   -d <file>        Take the index in the btar file as files already stored\n");
    printf("   -D <file>        Take the index file as files already stored\n");
    printf("   -f <file>        Output file while creating, input while extracting, \n"
           "                      or stdin/out if ommitted.\n");
    printf("   -F <filter>      Filter each block through program named 'filter'.\n");
    printf("   -H               Delete files as noted in diff backups, when extracting.\n");
    printf("   -j <n>           Number of blocks to filter in parallel.\n");
    printf("   -N               Skip making an index in the btar, make only blocks.\n");
    printf("   -R               Add a XOR redundancy block.\n");
    printf("   -U <filter>      Filters for the index and deleted list.\n");
    printf("   -v               Output the file names on stderr (on action 'c').\n");
    printf("   -X <pattern>     Add glob exclude pattern.\n");
#ifdef WITH_LIBRSYNC
    printf("   -Y               Create and use rsync signatures in indices for binary diff.\n");
#endif
    printf("other options:\n");
    printf("   -G <defilter>    Defilter each input block through program named 'filter'.\n"
           "                      May be relevant for '-d' when creating/filtering, or extracting.\n");
    printf("   -V               Show traces of what goes on. More V mean more traces.\n");
    printf("examples:\n");
    printf("   tar c /home | btar -b 50 -F xz > /tmp/homebackup.btar\n");
    printf("   btar -F xz -c -f mydir.btar mydir\n");
    printf("   btar -d mydir.btar -v -c mydir > mydir2.btar\n");
    printf("   btar -T 'mydir/m*' < mydir.btar | tar x\n");
    printf(" This program comes with ABSOLUTELY NO WARRANTY.\n");
    printf(" This is free software, and you are welcome to redistribute it\n");
    printf(" under certain conditions. See the source distribution for details.\n");
    exit(0);
}

void set_default_command_line()
{
    command_line.verbose = 0;
    command_line.blocksize = 10*1024*1024;
    command_line.add_create_index = 1;
    command_line.action = FILTER;
    command_line.input_files = 0;
    command_line.paths = 0;
    command_line.parallelism = 1;
    command_line.xorblock = 0;
    command_line.should_rsync = 0;
    command_line.should_delete = 0;
    command_line.rsync_block_size = 128*1024;
    command_line.rsync_minimal_size = 2 * command_line.rsync_block_size;
    command_line.rsync_max_delta = 100*1024*1024;
}

static void
add_to_string_vector(const char ***vector, const char *c)
{
    int nelems = 0;
    if (*vector == 0)
    {
        nelems = 1;
    }
    else
    {
        while((*vector)[nelems++] != 0);
    }

    (*vector) = realloc(
            (*vector),
            (nelems+1)*sizeof(**vector));
    if (!(*vector))
        fatal_error("Cannot realloc");

    (*vector)[nelems-1] = c;
    (*vector)[nelems] = 0;
}

static void
add_input_file(const char *c)
{
    add_to_string_vector(&command_line.input_files, c);
}

static void
add_path(const char *c)
{
    add_to_string_vector(&command_line.paths, c);
}

static void
add_exclude_pattern(const char *c)
{
    /* Remove initial slashes in the name to write to the tar */
    char *str = strdup(c);
    int len = strlen(str);
    int start = 0;
    int i;
    for (; str[start] == '/'; ++start);

    for(i = 0; i < len - start+1/*'\0'*/; ++i)
        str[i] = str[i+start];
    add_to_string_vector(&command_line.exclude_patterns, str);
}

static void
add_reference_type(enum reftype t)
{
    int nelems = 0;
    if (command_line.reference_types == 0)
    {
        nelems = 1;
    }
    else
    {
        while(command_line.reference_types[nelems++] != 0);
    }

    command_line.reference_types = realloc(
            command_line.reference_types,
            (nelems+1)*sizeof(*command_line.reference_types));
    if (!command_line.reference_types)
        fatal_error("Cannot realloc");

    command_line.reference_types[nelems-1] = t;
    command_line.reference_types[nelems] = 0;
}

static void
add_reference(enum reftype t, char *c)
{
    add_reference_type(t);
    add_to_string_vector(&command_line.references, c);
}

void parse_command_line(int argc, char *argv[])
{
    int c;

    set_default_command_line();

    /* Parse options */
    while(1) {
        c = getopt(argc, argv, "b:f:F:U:G:HNvVX:D:d:cxTlLj:Rhm"
#ifdef WITH_LIBRSYNC
                "Y"
#endif
                );

        if (c == -1)
            break;

        switch(c)
        {
            case 'b':
                command_line.blocksize = (size_t) 1024 * 1024 * atoi(optarg);
                break;
            case 'F':
                filter = append_filter_spaces(filter, optarg);
                break;
            case 'U':
                filterindex = append_filter_spaces(filterindex, optarg);
                break;
            case 'G':
                defilter = append_filter_spaces(defilter, optarg);
                break;
            case 'f':
                add_input_file(optarg);
                break;
            case 'N':
                command_line.add_create_index = 0;
                break;
            case 'v':
                command_line.verbose++;
                break;
            case 'V':
                command_line.debug++;
                break;
            case 'h':
                usage();
                break;
            case 'H':
                command_line.should_delete = 1;
                break;
            case 'X':
                add_exclude_pattern(optarg);
                break;
            case 'D':
                add_reference(REF_INDEX, optarg);
                break;
            case 'd':
                add_reference(REF_NOTAR, optarg);
                break;
            case 'c':
                command_line.action = CREATE;
                break;
            case 'x':
                command_line.action = EXTRACT;
                break;
            case 'T':
                command_line.action = EXTRACT_TO_TAR;
                break;
            case 'l':
                command_line.action = LIST_INDEX;
                break;
            case 'L':
                command_line.action = EXTRACT_INDEX;
                break;
            case 'm':
                command_line.action = MANGLE;
                break;
            case 'j':
                command_line.parallelism = atoi(optarg);
                break;
            case 'R':
                command_line.xorblock = 1;
                break;
            case 'Y':
                command_line.should_rsync = 1;
                break;
            case '?':
                fprintf(stderr, "Wrong option %c.\n", optopt);
                exit(-1);
        }
    }

    if (!filterindex)
        filterindex = filter;

    if (optind < argc)
    {
        if (command_line.action != EXTRACT &&
                command_line.action != EXTRACT_TO_TAR &&
                command_line.action != CREATE)
        {
            fatal_error_no_core("Paths not accepted unless -c or -x");
        }
        while (optind < argc)
        {
            add_path(argv[optind]);
            ++optind;
        }
    }
}

void
set_cloexec(int fd)
{
    int res;
    res = fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (res == -1)
        error("Cannot fcntl for cloexec");
}

void
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
        free_index();
        index_load_from_fd(fdin);
        send_index_to_fd(mypipe[1]);
        exit(0);
    }
    else /* Parent */
    {
        close(fdin);
        close(mypipe[1]);
        *fdout = mypipe[0];
        if (command_line.debug)
            fprintf(stderr, "Starting index reader PID %i, reading from fd %i \n", pid,
                    *fdout);
    }
}

void
load_index_from_tar(int fd)
{
    char *name;
    int filterin, filterout;
    int indexin;
    unsigned long long indexsize;
    struct filter *mydefilter;
    char *buffer;

    name = index_find_in_tar(fd, &indexsize);
    if (!name)
    {
        if (command_line.debug)
            fprintf(stderr, "Cannot find index in btar\n");
        return;
    }

    if (command_line.debug)
        fprintf(stderr, "Index file name in main btar: %s\n", name);

    if (defilter)
        mydefilter = defilter;
    else
        mydefilter = defilters_from_extensions(name);

    free(name);

    run_filters(mydefilter, &filterin, &filterout);
    set_cloexec(filterin);
    set_cloexec(filterout);

    run_index_reader(filterout, &indexin, filterin);

    buffer = malloc(buffersize);
    if (!buffer)
        fatal_error("Cannot allocate");

    while(indexsize > 0)
    {
        int res;
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
    free(buffer);

    recv_index_from_fd(indexin);

    if (mydefilter != defilter)
        free(mydefilter);

    close(indexin);
}

static void
create_or_filter(int outfd)
{
    struct block_reader *br_to_index_tar = 0;
    int index_from_tar_fd = -1;
    int doing_index = 0;
    int doing_deleted = 0;
    int i;

    struct block_process **bp;
    int reading_bp;
    int writing_bp;
    struct block *xorblock = 0;

    assert(main_archive.archive == 0);
    mainarchive_open(&main_archive, outfd);
    set_cloexec(outfd);

    if (command_line.action == CREATE)
    {
        int mypipe[2];
        int res;
        int pid;
        int index_filterin = -1;
        int index_filterout = -1;
        int deleted_filterin = -1;
        int deleted_filterout = -1;

        if (command_line.references)
        {
            int i;
            for(i=0; command_line.references[i] != 0; ++i)
            {
                int fd;
                if (command_line.debug)
                    fprintf(stderr, "Loading reference %s\n",
                            command_line.references[i]);
                if (command_line.reference_types[i] == REF_INDEX)
                {
                    struct filter *mydefilter = 0;
                    int fdout;

                    if (strcmp(command_line.references[i], "-") == 0)
                        fd = dup(0);
                    else
                    {
                        fd = open(command_line.references[i], O_RDONLY);

                        if (fd == -1)
                            fatal_errno("Cannot open the index file %s",
                                    command_line.references[i]);

                        if (defilter)
                            mydefilter = defilter;
                        else
                            mydefilter = defilters_from_extensions(command_line.references[i]);
                    }

                    run_filters_given_fdin(mydefilter, fd, &fdout);

                    index_load_from_fd(fdout);

                    index_sort();

                    close(fdout);

                    if (mydefilter != defilter)
                        free_filters(mydefilter);
                }
                else if (command_line.reference_types[i] == REF_NOTAR)
                {
                    if (command_line.debug)
                        fprintf(stderr, "Opening tar looking for the index... ");

                    fd = open(command_line.references[i], O_RDONLY);
                    if (fd == -1)
                        fatal_errno("Cannot open the reference btar file %s",
                                command_line.references[i]);

                    set_cloexec(fd);

                    load_index_from_tar(fd);

                    index_sort();

                    close(fd);
                }
            }
        }

        if (command_line.add_create_index)
        {
            if (command_line.debug)
                fprintf(stderr, "Starting index creation\n");
            run_filters(filterindex, &index_filterin, &index_filterout);
            set_cloexec(index_filterin);
            set_cloexec(index_filterout);

            doing_index = 1;
        }

        if (command_line.reference_types != 0)
        {
            if (command_line.debug)
                fprintf(stderr, "Starting 'deleted' creation\n");
            run_filters(filterindex, &deleted_filterin, &deleted_filterout);
            set_cloexec(deleted_filterin);
            set_cloexec(deleted_filterout);

            doing_deleted = 1;
        }

        /* Traverse pipe */
        res = pipe(mypipe);
        if (res == -1)
            error("Error creating traverse pipe");

        if (command_line.debug)
            fprintf(stderr, "Starting to traverse directories...\n");

        pid = fork();
        if (pid == -1)
            error("Cannot fork");
        else if (pid == 0)
        {
            int res;
            /* Child */
            close(0);
            close(mypipe[0]);

            close(index_filterout);
            close(deleted_filterout);

            res = traverse(mypipe[1], index_filterin, deleted_filterin);
            if (res == -1)
                error("Cannot traverse");

            close(index_filterin);
            close(mypipe[1]);
            exit(0);
        }
        else
        {
            /* Parent */
            close(mypipe[1]);
            close(0);
            dup(mypipe[0]);
            close(mypipe[0]);
            close(index_filterin);
            close(deleted_filterin);
            if (command_line.debug)
                fprintf(stderr, "Starting traverse PID %i, outputing to fd 0\n", pid);

            if (doing_index)
                im = file_memory_new(index_filterout);
            if (doing_deleted)
                dm = file_memory_new(deleted_filterout);
        }
    }
    else if (command_line.action == FILTER && command_line.add_create_index)
    {
        int mypipe[2];
        int res;
        int pid;
        int index_filterin;
        int index_filterout;

        doing_index = 1;

        run_filters(filter, &index_filterin, &index_filterout);
        set_cloexec(index_filterin);
        set_cloexec(index_filterout);

        /* Pipe for the index_from_tar */
        res = pipe(mypipe);
        if (res == -1)
            error("Error creating index_from_tar pipe");

        if (command_line.debug)
            fprintf(stderr, "Starting to index from incoming tar...\n");

        pid = fork();
        if (pid == -1)
            error("Cannot fork");
        else if (pid == 0)
        {
            /* Child */
            close(0);
            close(mypipe[1]);

            close(index_filterout);

            index_from_tar(mypipe[0], index_filterin);

            close(index_filterin);
            close(mypipe[0]);
            exit(0);
        }
        else
        {
            /* Parent */
            int res;
            index_from_tar_fd = mypipe[1];
            res = fcntl(index_from_tar_fd, F_SETFL, O_NONBLOCK);
            if (res == -1)
                error("Cannot fcntl");

            close(mypipe[0]);
            close(index_filterin);

            if (command_line.debug)
                fprintf(stderr, "Starting index_from_tar PID %i, we'll write to filter fd %i"
                        " and read the index from fd %i\n", pid,
                        index_from_tar_fd, index_filterout);

            im = file_memory_new(index_filterout);
        }
    }
    else if (command_line.action == MANGLE)
    {
        int pipeextract[2];
        int res;
        int pid;
        int index_filterin = -1;
        int index_filterout = -1;
        int deleted_filterin = -1;
        int deleted_filterout = -1;

        if (command_line.paths)
            fatal_error_no_core("Cannot support paths in mangle (-m) mode");

        /* Run the filters for the index */
        run_filters(filter, &index_filterin, &index_filterout);
        set_cloexec(index_filterin);
        set_cloexec(index_filterout);

        /* Run the filters for the deleter */
        run_filters(filter, &deleted_filterin, &deleted_filterout);
        set_cloexec(deleted_filterin);
        set_cloexec(deleted_filterout);

        /* Pipe for the extraction */
        res = pipe(pipeextract);
        if (res == -1)
            error("Error creating extract pipe");

        if (command_line.debug)
            fprintf(stderr, "Starting to extract to tar from incoming tar...\n");

        pid = fork();
        if (pid == -1)
            error("Cannot fork");
        else if (pid == 0)
        {
            /* Child */
            close(1);
            dup(pipeextract[1]);
            close(pipeextract[1]);
            close(index_filterout);
            close(deleted_filterout);

            command_line.action = EXTRACT_TO_TAR;
            /* We force the extractor not to recreate the tar, as mangle should
             * not change the block data at all. At least as I understand
             * mangle now. */
            command_line.add_create_index = 0;

            extract(0, index_filterin, deleted_filterin);

            exit(0);
        }
        else
        {
            /* Parent */
            close(pipeextract[1]);
            close(0);
            dup(pipeextract[0]);
            close(pipeextract[0]);
            close(index_filterin);
            close(deleted_filterin);

            if (command_line.debug)
                fprintf(stderr, "Starting extract_to_tar PID %i\n", pid);
        }

        doing_index = 1;
        im = file_memory_new(index_filterout);

        doing_deleted = 1;
        dm = file_memory_new(deleted_filterout);
    }

    bp = malloc(sizeof(*bp)*command_line.parallelism);
    if (!bp)
        fatal_error("Cannot allocate");

    for(i=0; i < command_line.parallelism; ++i)
    {
        bp[i] = block_process_new(main_archive.nextblock++);
        if (command_line.debug)
            fprintf(stderr, "BlockProcess[%i] = %p\n", i, bp[i]);
    }
    reading_bp = 0;
    writing_bp = 0;
    ref_reading_bp = bp[reading_bp];

    if (command_line.xorblock)
    {
        /* It will be reallocated if needed */
        xorblock = block_new(1);
        /* We start a block with a single byte to zero. The last byte in the xor block
         * counts as any other byte there may come in the future. So, when making
         * the block bigger, we expand repeating the last byte value, that will
         * be properly xored since the beggining starting at zero. */
        xorblock->data[0] = 0;
    }

    if (index_from_tar_fd >= 0)
    {
        br_to_index_tar = block_process_new_input_reader(bp[reading_bp]);
    }

    while(1)
    {
        int nfds = 0;
        fd_set readfds;
        fd_set writefds;
        int res;

        /* For the stdin read */
        FD_ZERO(&readfds);
        FD_ZERO(&writefds);

        /* Input */
        for(i=0; i < command_line.parallelism; ++i)
        {
            prepare_readfds(bp[i], &readfds, &nfds, /* should_read */ i == reading_bp);
            prepare_writefds(bp[i], &writefds, &nfds);
        }
        if (im)
            file_memory_prepare_readfds(im, &readfds, &nfds);
        if (dm)
            file_memory_prepare_readfds(dm, &readfds, &nfds);

        if (index_from_tar_fd >= 0 && block_reader_can_read(br_to_index_tar))
            addfd(&writefds, index_from_tar_fd, &nfds);

        res = select(nfds, &readfds, &writefds, 0, 0);
        if (res == -1 && errno == EINTR)
            continue;

        if (res == -1)
            error("error in select()");

        for(i=0; i < command_line.parallelism; ++i)
        {
            check_read_fds(bp[i], &readfds, /* should_read */i == reading_bp);
            check_write_fds(bp[i], &writefds);
        }
        if (im)
            file_memory_check_readfds(im, &readfds);
        if (dm)
            file_memory_check_readfds(dm, &readfds);

        if (index_from_tar_fd >= 0 && FD_ISSET(index_from_tar_fd, &writefds))
        {
            int nwritten = block_reader_to_fd(br_to_index_tar, index_from_tar_fd);
            if (nwritten == -1 && errno != EINTR)
                error("Failed write to index_from_tar");
            assert(nwritten != 0);
        }

        if (bp[reading_bp]->closed_in && index_from_tar_fd >= 0
                && !block_reader_can_read(br_to_index_tar))
        {
            if (command_line.debug)
                fprintf(stderr, "Closing the index input fd %i\n", index_from_tar_fd);
            close(index_from_tar_fd);
            index_from_tar_fd = -1;
        }

        /* If the block finished reading, and
         * in case of indexing_from_tar, also that reader is done.
         * And in case of closed in, we don't want a new reader either. */
        if (!bp[reading_bp]->closed_in && block_process_finished_reading(bp[reading_bp])
                && !bp[reading_bp]->finished_read_ack /* to avoid reentering this */
                && (index_from_tar_fd == -1 || !block_reader_can_read(br_to_index_tar)))
        {
            int newbp;
            newbp = (reading_bp + 1) % command_line.parallelism;

            if (command_line.debug)
                fprintf(stderr, "Finished reading from file input, reading_bp=%i, bytes=%zu\n",
                        reading_bp, block_process_total_read(bp[reading_bp]));
            /* Just to no reenter this condition */
            bp[reading_bp]->finished_read_ack = 1;
            total_read_in_full_blocks += block_process_total_read(bp[reading_bp]);
            ref_reading_bp = bp[reading_bp];

            if (newbp != writing_bp)
            {
                reading_bp = newbp;
                if (index_from_tar_fd >= 0)
                {
                    block_reader_free(br_to_index_tar);
                    br_to_index_tar = block_process_new_input_reader(bp[reading_bp]);
                }
                /* If the current writing_bp has not read anything,
                 * it can only be in the just-reset state */
                if (!block_process_has_read(bp[writing_bp]))
                {
                    /* This above means that the block has been reseted */
                    writing_bp = (writing_bp + 1) % command_line.parallelism;
                }
                if (command_line.debug)
                    fprintf(stderr, "Parallelism: reading_bp = %i writing_bp = %i\n",
                            reading_bp, writing_bp);
            }
        }

        /* Check finishing conditions */
        while (block_process_finished(bp[writing_bp]) && bp[writing_bp]->bo->writer_pos > 0)
        {
            int newbp;
            if (command_line.debug)
                fprintf(stderr, "Parallelism: Finished reading from filter, writing_bp=%i\n",
                        writing_bp);

            if (xorblock)
                xor_to_xorblock(bp[writing_bp], xorblock);

            dump_block_to_tar(bp[writing_bp], main_archive.archive);
            block_process_reset(bp[writing_bp], main_archive.nextblock++);

            /* Go for the next, unless we override something */
            if (writing_bp != reading_bp || bp[reading_bp]->closed_in)
            {
                newbp = (writing_bp + 1) % command_line.parallelism;
                writing_bp = newbp;
                if (!bp[reading_bp]->closed_in
                        && block_process_finished_reading(bp[reading_bp]))
                {
                    total_read_in_full_blocks += block_process_total_read(bp[reading_bp]);
                    reading_bp = (reading_bp + 1) % command_line.parallelism;
                    ref_reading_bp = bp[reading_bp];
                    if (index_from_tar_fd >= 0)
                    {
                        block_reader_free(br_to_index_tar);
                        br_to_index_tar = block_process_new_input_reader(bp[reading_bp]);
                    }
                }
            }
            if (command_line.debug)
                fprintf(stderr, "Parallelism: reading_bp = %i writing_bp = %i\n",
                        reading_bp, writing_bp);
        }

        if (bp[reading_bp]->closed_in
                && (!im || file_memory_finished(im))
                && (!dm || file_memory_finished(dm))
                && block_process_finished(bp[writing_bp]))
            break;
    }

    /* Write the xorblock */;
    if (xorblock)
    {
        ssize_t res;

        if (command_line.debug)
            fprintf(stderr, "Writing xor block to the btar stream\n");

        mytar_new_file(main_archive.archive);
        mytar_set_filename(main_archive.archive, "xorblock");
        mytar_set_gid(main_archive.archive, getgid());
        mytar_set_uid(main_archive.archive, getuid());
        mytar_set_size(main_archive.archive, xorblock->writer_pos);
        mytar_set_mode(main_archive.archive, 0644 | S_IFREG);

        /* I'm not sure what's better, to save a mtime like this or not.
         * It would be nice that the hash of the final tar does not depend
         * on the date when it runs, but it's also fine to have the date somewhere.
         * Let's go with setting mtime by now */
        mytar_set_mtime(main_archive.archive, time(NULL));

        mytar_set_filetype(main_archive.archive, S_IFREG);
        res = mytar_write_header(main_archive.archive);
        if (res == -1)
            error("Failed to write header");

        res = mytar_write_data(main_archive.archive, xorblock->data, xorblock->writer_pos);
        if (res == -1)
            error("Could not write mytar data");
        assert((size_t) res == xorblock->writer_pos);

        /* The block end - main_archive.archive padding */
        res = mytar_write_end(main_archive.archive);
        if (res == -1)
            error("Could not write mytar file end");
    }

    /* Write the index */;
    if (doing_index)
    {
        assert(im != 0 && file_memory_finished(im));
        file_memory_to_tar(im, "index.tar%s", get_filter_extensions(filterindex),
                main_archive.archive);
    }

    if (doing_deleted)
    {
        assert(dm != 0 && file_memory_finished(dm));
        file_memory_to_tar(dm, "deleted.tar%s", get_filter_extensions(filterindex),
                main_archive.archive);
    }

    mainarchive_close(&main_archive);
}

int main(int argc, char *argv[])
{
    /* Filters */
    parse_command_line(argc, argv);

    if (command_line.action == CREATE && !command_line.paths)
    {
        fatal_error_no_core("error: please specify what paths to traverse");
    }

    register_child_handler();
    register_usr1_handler();

    switch(command_line.action)
    {
        case CREATE:
        case FILTER:
        case MANGLE:
            if (command_line.input_files)
            {
                int fd;
                fd = open(command_line.input_files[0], O_CREAT | O_WRONLY | O_TRUNC, 0666);
                if (fd == -1)
                    fatal_errno("Cannot open the btar file %s",
                            command_line.input_files[0]);
                create_or_filter(fd);
                close(fd);
            }
            else
                create_or_filter(1/*stdout*/);
            break;
        case EXTRACT:
        case EXTRACT_TO_TAR:
            if (command_line.input_files)
            {
                int i;
                for(i=0; command_line.input_files[i]; ++i)
                {
                    int fd;
                    if (command_line.debug)
                        fprintf(stderr, "Extracting from the btar file %s\n",
                                command_line.input_files[i]);
                    fd = open(command_line.input_files[i], O_RDONLY);
                    if (fd == -1)
                        fatal_errno("Cannot open the btar file %s",
                                command_line.input_files[i]);
                    set_cloexec(fd);
                    extract(fd, -1, -1);
                    close(fd);
                }
            }
            else
                extract(0, -1, -1);
            break;
        case EXTRACT_INDEX:
        case LIST_INDEX:
            if (command_line.input_files)
            {
                int fd;
                if (command_line.debug)
                    fprintf(stderr, "Listing the btar file %s\n",
                            command_line.input_files[0]);
                fd = open(command_line.input_files[0], O_RDONLY);
                if (fd == -1)
                    fatal_errno("Cannot open the btar file %s",
                            command_line.input_files[0]);
                listindex(fd);
                close(fd);
            }
            else
                listindex(0);
            break;
    }

    return 0;
}
