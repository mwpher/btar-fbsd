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
#ifdef WITH_LIBRSYNC
#include <librsync.h>
#endif
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include "rsync.h"
#include "main.h"

#ifndef WITH_LIBRSYNC
struct rsync_signature *
rsync_signature_new()
{
    return 0;
}

void
rsync_signature_work(struct rsync_signature *rs, const char *data, size_t len)
{
    rs = rs;
    data = data;
    len = len;
}

void
rsync_signature_free(struct rsync_signature *rs)
{
    rs = rs;
}

size_t
rsync_signature_size(const struct rsync_signature *rs)
{
    rs = rs;
    return 0;
}

struct rsync_delta *
rsync_delta_new(char *signature, int len)
{
    signature = signature;
    len = len;
    return 0;
}

int
rsync_delta_work(struct rsync_delta *rd, const char *data, size_t len)
{
    rd = rd;
    data = data;
    len = len;
    return 0;
}

void
rsync_delta_free(struct rsync_delta *rd)
{
    rd = rd;
}

size_t
rsync_delta_size(const struct rsync_delta *rd)
{
    rd = rd;
    return 0;
}

struct rsync_patch *
rsync_patch_new(char *basefile, int outfd)
{
    basefile = basefile;
    outfd = outfd;
    return 0;
}

void
rsync_patch_work(struct rsync_patch *rp, const char *data, size_t len)
{
    rp = rp;
    data = data;
    len = len;
}

void
rsync_patch_free(struct rsync_patch *rp)
{
    rp = rp;
}

#else

struct rsync_signature *
rsync_signature_new()
{
    struct rsync_signature *rs = malloc(sizeof(*rs));
    if (!rs)
        fatal_error("Cannot allocate");

    rs->allocated_output = 4096;
    rs->output = malloc(rs->allocated_output);
    if (!rs->output)
        fatal_error("Cannot allocate");
    rs->buffers.next_out = rs->output;
    rs->buffers.avail_out = rs->allocated_output;
    rs->buffers.avail_in = 0;
    rs->buffers.eof_in = 0;
    rs->job = rs_sig_begin(command_line.rsync_block_size, RS_DEFAULT_STRONG_LEN);
    assert(rs->job != 0);

    return rs;
}

void
rsync_signature_work(struct rsync_signature *rs, const char *data, size_t len)
{
    rs_result res;

    assert(rs->buffers.avail_in == 0);
    rs->buffers.next_in = (char *) data;
    rs->buffers.avail_in = len;
    if(len == 0)
        rs->buffers.eof_in = 1;

    do
    {
        res = rs_job_iter(rs->job, &rs->buffers);
        assert(res == RS_DONE || res == RS_BLOCKED);
        if (rs->buffers.avail_out == 0)
        {
            int written = rs->buffers.next_out - rs->output;
            const int increment = 4096;
            rs->allocated_output += increment;
            rs->output = realloc(rs->output, rs->allocated_output);
            if (!rs->output)
                fatal_error("Cannot realloc");
            rs->buffers.next_out = rs->output + written;
            rs->buffers.avail_out = increment;
        }
    } while (rs->buffers.avail_in > 0 || (rs->buffers.eof_in && res != RS_DONE));
}

void
rsync_signature_free(struct rsync_signature *rs)
{
    free(rs->output);
    rs_job_free(rs->job);
    free(rs);
}

size_t
rsync_signature_size(const struct rsync_signature *rs)
{
    return rs->buffers.next_out - rs->output;
}

struct rsync_delta *
rsync_delta_new(char *signature, int len)
{
    struct rsync_delta *rd = malloc(sizeof(*rd));
    struct rs_buffers_s sbuffers;
    rs_job_t *sjob;
    rs_result res;

    if (!rd)
        fatal_error("Cannot allocate");

    if (0)
        rs_trace_set_level(RS_LOG_DEBUG);

    sjob = rs_loadsig_begin(&rd->signature);
    assert(sjob != 0);

    sbuffers.next_in = signature;
    sbuffers.avail_in = len;
    sbuffers.avail_out = 0;
    sbuffers.next_out = 0;
    sbuffers.eof_in = 1;

    do
    {
        res = rs_job_iter(sjob, &sbuffers);
    } while (sbuffers.avail_in > 0);

    if (res != RS_DONE)
        fatal_error("Error creating rsync delta loading the signature");

    rs_job_free(sjob);

    res = rs_build_hash_table(rd->signature);
    assert(res == RS_DONE);

    rd->allocated_output = 1024*1024;
    rd->output = malloc(rd->allocated_output);
    if (!rd->output)
        fatal_error("Cannot allocate");
    rd->buffers.avail_out = rd->allocated_output;
    rd->buffers.next_out = rd->output;
    rd->buffers.next_in = 0;
    rd->buffers.avail_in = 0;
    rd->buffers.eof_in = 0;
    rd->job = rs_delta_begin(rd->signature);

    return rd;
}

/* 0 ok, 1 max exceeded */
int
rsync_delta_work(struct rsync_delta *rd, const char *data, size_t len)
{
    rs_result res;

    assert(rd->buffers.avail_in == 0);
    rd->buffers.next_in = (char *) data;
    rd->buffers.avail_in = len;
    if(len == 0)
        rd->buffers.eof_in = 1;

    do
    {
        res = rs_job_iter(rd->job, &rd->buffers);
        assert(res == RS_DONE || res == RS_BLOCKED);
        if (rd->buffers.avail_out == 0)
        {
            size_t written = rd->buffers.next_out - rd->output;
            size_t increment = 10*1024*1024;
            if (rd->allocated_output + increment > command_line.rsync_max_delta)
                increment = command_line.rsync_max_delta - rd->allocated_output;

            /* If we need more memory than allowed, break returning 1 */
            if (increment == 0)
                return 1;

            rd->allocated_output += increment;
            rd->output = realloc(rd->output, rd->allocated_output);
            if (!rd->output)
                fatal_error("Cannot realloc");
            rd->buffers.next_out = rd->output + written;
            rd->buffers.avail_out = increment;
        }
    } while (rd->buffers.avail_in > 0 || (rd->buffers.eof_in && res != RS_DONE));

    if (command_line.debug > 2)
        fprintf(stderr, "rsync patch write len %zu, current size %zu, res %i\n",
                len, rsync_delta_size(rd), res);

    /* All ok */
    return 0;
}

void
rsync_delta_free(struct rsync_delta *rd)
{
    free(rd->output);
    rs_free_sumset(rd->signature);
    rs_job_free(rd->job);
    free(rd);
}

size_t
rsync_delta_size(const struct rsync_delta *rd)
{
    return rd->buffers.next_out - rd->output;
}

static
rs_result
copy_cb(void *opaque, rs_long_t pos, size_t *len, void **buf)
{
    struct copy_state *s = (struct copy_state *) opaque;
    size_t nread = 0;
    off_t res = lseek(s->fd, pos, SEEK_SET);
    if (res == -1)
        fatal_errno("Cannot lseek on rsync base file");

    if (command_line.debug > 2)
    {
        fprintf(stderr, "rsync patch copy_cb, pos %llu, len %zu\n",
                (unsigned long long) pos, *len);
    }

    while(1)
    {
        ssize_t tmp;
        tmp = read(s->fd, *buf + nread, *len - nread);
        if (tmp == -1)
        {
            if (errno == EINTR)
                continue;
            fatal_errno("Cannot read on rsync base file");
        }
        if (tmp == 0)
        {
            *len = nread;
            return RS_INPUT_ENDED;
        }

        nread += tmp;
        if (nread == *len)
            break;
    }
    return RS_DONE;
}

struct rsync_patch *
rsync_patch_new(char *basefile, int outfd)
{
    struct rsync_patch *rp = malloc(sizeof(*rp));

    if (!rp)
        fatal_error("Cannot allocate");

    rp->copy_state.fd = open(basefile, O_RDONLY);
    if (rp->copy_state.fd == -1)
    {
        free(rp);
        return 0;
    }

    rp->outfd = outfd;
    rp->basename = strdup(basefile);

    rp->buffers.next_out = 0;
    rp->buffers.avail_out = 0;
    rp->buffers.avail_in = 0;
    rp->buffers.eof_in = 0;

    rp->allocated_output = buffersize;
    rp->output = malloc(rp->allocated_output);
    if (!rp->output)
        fatal_error("Cannot allocate");

    rp->job = rs_patch_begin(copy_cb, &rp->copy_state);
    assert(rp->job != 0);

    return rp;
}

void
rsync_patch_work(struct rsync_patch *rp, const char *data, size_t len)
{
    rs_result res;

    assert(rp->buffers.avail_in == 0);
    rp->buffers.next_in = (char *) data;
    rp->buffers.avail_in = len;
    if(len == 0)
        rp->buffers.eof_in = 1;
    rp->buffers.next_out = rp->output;
    rp->buffers.avail_out = rp->allocated_output;

    if (command_line.debug > 2)
        fprintf(stderr, "rsync patch incoming len %zu\n", len);

    do
    {
        size_t written = 0;
        size_t left;
        res = rs_job_iter(rp->job, &rp->buffers);
        assert(res == RS_DONE || res == RS_BLOCKED);

        left = rp->buffers.next_out - rp->output;

        if (command_line.debug > 2)
            fprintf(stderr, "rsync patch write len %zu, job res %i\n",
                    left, res);

        while(left > 0)
        {
            ssize_t tmp;
            tmp = write(rp->outfd, rp->output+written, left);
            if (tmp == -1)
            {
                if (errno == EINTR)
                    continue;
                fatal_errno("Cannot write on rsync patch work");
            }
            left -= tmp;
            written += tmp;
        }

        rp->buffers.next_out = rp->output;
        rp->buffers.avail_out = rp->allocated_output;
    } while (rp->buffers.avail_in > 0 || (rp->buffers.eof_in && res != RS_DONE));
}

void
rsync_patch_free(struct rsync_patch *rp)
{
    close(rp->copy_state.fd);
    free(rp->basename);
    free(rp->output);
    rs_job_free(rp->job);
    free(rp);
}
#endif
