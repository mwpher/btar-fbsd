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
#include <fcntl.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "filters.h"
#include "main.h"

void
add_filter(char * const *args, int fdin, int *fdout, int alsoclose)
{
    int filter_output[2];

    int res;

    res = pipe(filter_output);
    if (res == -1)
        error("Error making pipe");

    int pid = fork();

    if (pid == 0)
    {
        int res;
        /* child*/

        if (alsoclose != -1)
            close(alsoclose);
        close(0);
        res = dup(fdin);
        if (res == -1)
            error("Cannot dup");
        assert(res == 0);
        close(fdin);

        close(1);
        res = dup(filter_output[1]);
        if (res == -1)
            error("Cannot dup");
        assert(res == 1);
        close(filter_output[1]);

        close(filter_output[0]);
        res = execvp(args[0], args);
        error("Cannot exec filter");
    }
    else
    {
        if (command_line.debug)
        {
            char * const *ptr = args;
            fprintf(stderr, "Running filter PID %i: ", pid);
            while(*ptr)
            {
                fprintf(stderr, "%s ", *ptr);
                ++ptr;
            }
            fprintf(stderr, "\n");
        }
        close(fdin);
        close(filter_output[1]);
        *fdout = filter_output[0];
    }
}

void
first_filter(char * const *args, int *fdin, int *fdout)
{
    int filter_input[2];

    int res;

    res = pipe(filter_input);
    if (res == -1)
        error("Error making pipe");

    *fdin = filter_input[1];

    add_filter(args, filter_input[0], fdout, *fdin);
}

void
run_filters_given_fdin(struct filter *filter, int fdin, int *fdout)
{
    struct filter *f = filter;
    if (f != 0)
    {
        add_filter(f->args, fdin, fdout, -1);
        f = f->next;

        while(f)
        {
            add_filter(f->args, *fdout, fdout, fdin);
            f = f->next;
        }
    }
    else
    {
        *fdout = fdin;
    }

    if (command_line.debug)
        fprintf(stderr, "  Filter started, reading from given fd %i, outputing on fd %i.\n",
                fdin, *fdout);
}

void
run_filters(struct filter *filter, int *fdin, int *fdout)
{
    struct filter *f = filter;
    if (f != 0)
    {
        first_filter(f->args, fdin, fdout);
        f = f->next;

        while(f)
        {
            add_filter(f->args, *fdout, fdout, *fdin);
            f = f->next;
        }
    }
    else
    {
        /* Simply pipe in case of no filter */
        int mypipe[2];
        int res;

        res = pipe(mypipe);
        if (res == -1)
            error("Cannot create pipe");

        *fdin = mypipe[1];
        *fdout = mypipe[0];
    }

    if (command_line.debug)
        fprintf(stderr, "  Filter started, accepting on fd %i, outputing on fd %i.\n",
                *fdin, *fdout);
}

struct filter *
append_filter(struct filter *f, char **args)
{
    struct filter *fnew;

    fnew = malloc(sizeof(*fnew));
    if (!fnew)
        fatal_error("Cannot allocate");

    fnew->next = 0;
    fnew->args = args;

    {
        const char *pos, *npos;
        pos = args[0];
        if ((npos = strrchr(pos, '/')) != 0)
            pos = npos+1;

        if(f)
        {
            char *tmp;
            tmp = strdup(f->extensions);
            snprintf(fnew->extensions, sizeof fnew->extensions,
                    "%s.%s", tmp, pos);
            free(tmp);
        }
        else
        {
            if (!strcmp(pos, "cat"))
                fnew->extensions[0] = '\0';
            else
                snprintf(fnew->extensions, sizeof fnew->extensions,
                        ".%s",  pos);
        }
    }

    if (f)
    {
        f->next = fnew;
        return f;
    }

    return fnew;
}

struct filter *
append_filter_spaces(struct filter *f, const char *arg)
{
    /*Find out the number of spaces*/
    int spaces = 0;
    char **args;
    char *tmp = strdup(arg); /* We need this allocation forever, as the filters point here */
    char *pos = tmp;
    char *nextpos;

    while(*pos == ' ')
        ++pos;
    while((pos = strchr(pos, ' ')) != 0)
    {
        do
            ++pos;
        while(*pos == ' ' && *pos != '\0');
        ++spaces;
    }
    ++spaces; /* final string */
    ++spaces; /* NULL at the end */

    args = malloc(sizeof(*args) * spaces);
    if (!args)
        fatal_error("Cannot allocate");

    spaces = 0;
    pos = tmp;
    /* We don't skip initial spaces, because we are going to call 'free()'
     * to this pointer */
    while((nextpos = strchr(pos,' ')) != 0)
    {
        *nextpos = '\0';
        args[spaces++] = pos;

        do
            ++nextpos;
        while(*nextpos == ' ' && *pos != '\0');
        pos = nextpos;
    }
    args[spaces++] = pos; /* Final string */

    args[spaces] = 0; /* Null end of the list, for execvp */

    return append_filter(f, args);
}

const char *
get_filter_extensions(struct filter *f)
{
    while(f != 0)
    {
        if (!f->next)
            return f->extensions;
        f = f->next;
    }

    return "";
}

struct filter *
defilters_from_extensions(const char *filename)
{
    struct filter *f = 0;
    char *tmp = strdup(filename);
    char *pos = tmp;

    while((pos = strrchr(tmp, '.')) != 0)
    {
        /*next char*/
        pos = pos+1;
        if (!strcmp(pos, "tar"))
            break;
        else
        {
            char command[PATH_MAX];
            snprintf(command, sizeof command, "%s -d", pos);
            f = append_filter_spaces(f, command);
        }

        --pos;
        /* It will be valid, because it found a dot, and
         * we did pos+1 */
        *pos = '\0';
        --pos;
    }

    free(tmp);

    return f;
}

void
free_filters(struct filter *f)
{
    while(f)
    {
        struct filter *next = f->next;
        free(f->args[0]);
        free(f->args);
        free(f);

        f = next;
    }
}
