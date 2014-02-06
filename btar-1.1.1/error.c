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
#include <stdarg.h>
#include <errno.h>
#include <string.h>
#include "main.h"

static void
end()
{
    abort();
}

static void
end_no_core()
{
    exit(1);
}

void
error(const char *str)
{
    perror(str);
    end();
}

void
fatal_error(const char *str, ...)
{
    va_list ap;

    va_start(ap, str);

    vfprintf(stderr, str, ap);
    fputc('\n', stderr);
    end();
    va_end(ap);
}

void
fatal_error_no_core(const char *str, ...)
{
    va_list ap;

    va_start(ap, str);

    vfprintf(stderr, str, ap);
    fputc('\n', stderr);
    end_no_core();
    va_end(ap);
}

void
fatal_errno(const char *str, ...)
{
    va_list ap;
    int e;

    va_start(ap, str);
    e = errno;

    vfprintf(stderr, str, ap);
    fprintf(stderr, ": %s\n", strerror(e));
    end();
    va_end(ap);
}
