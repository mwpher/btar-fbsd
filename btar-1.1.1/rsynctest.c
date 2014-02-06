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
#include <assert.h>
#include <string.h>
#include "rsync.h"
#include "main.h"

/* as rsync.c will depend on it */
struct command_line command_line;

void
signature()
{
    FILE *fin = stdin;
    char buffer[4096];
    int len;

    struct rsync_signature *rs;

    rs = rsync_signature_new();

    while((len = fread(buffer, 1, sizeof buffer, fin)) > 0)
    {
        rsync_signature_work(rs, buffer, len);
    }

    rsync_signature_work(rs, buffer, 0);

    fwrite(rs->output, 1, (rs->buffers.next_out - rs->output), stdout);

    rsync_signature_free(rs);
}

void
delta(const char *signame)
{
    FILE *sig;
    FILE *fin = stdin;

    char buffer[4096];
    int len;
    struct rsync_delta *rd;

    /* Load the signature */
    sig = fopen(signame, "rb");
    assert(sig != 0);

    len = fread(buffer, 1, sizeof(buffer), sig);

    rd = rsync_delta_new(buffer, len);

    fclose(sig);

    while((len = fread(buffer, 1, sizeof buffer, fin)) > 0)
    {
        rsync_delta_work(rd, buffer, len);
    }
    rsync_delta_work(rd, buffer, 0);

    fwrite(rd->output, 1, (rd->buffers.next_out - rd->output), stdout);

    rsync_delta_free(rd);
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        printf("usage: rsynctest [s | d signature]\n");
        return 1;
    }

    command_line.rsync_block_size = 128*1024;
    command_line.rsync_max_delta = 100*1024*1024;

    if (strcmp(argv[1], "s") == 0)
        signature();
    else if (strcmp(argv[1], "d") == 0)
        delta(argv[2]);

    return 0;
}
