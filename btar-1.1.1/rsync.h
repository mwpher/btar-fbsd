#include <stdio.h>
#ifdef WITH_LIBRSYNC
#include <librsync.h>
#endif

struct rsync_signature
{
#ifdef WITH_LIBRSYNC
    struct rs_buffers_s buffers;
    struct rs_job *job;
#endif
    char *output;
    size_t allocated_output;
};

struct rsync_signature * rsync_signature_new();
void rsync_signature_work(struct rsync_signature *rs, const char *data, size_t len);
size_t rsync_signature_size(const struct rsync_signature *rs);
void rsync_signature_free(struct rsync_signature *rs);

struct rsync_delta
{
#ifdef WITH_LIBRSYNC
    struct rs_buffers_s buffers;
    struct rs_job *job;
    rs_signature_t *signature;
#endif
    char *output;
    size_t allocated_output;
};

struct rsync_delta * rsync_delta_new(char *signature, int len);
size_t rsync_delta_size(const struct rsync_delta *rd);

/* 0 ok, 1 max exceeded */
int rsync_delta_work(struct rsync_delta *rd, const char *data, size_t len);

void rsync_delta_free(struct rsync_delta *rd);

struct rsync_patch
{
#ifdef WITH_LIBRSYNC
    struct rs_buffers_s buffers;
    struct rs_job *job;
#endif
    struct copy_state
    {
        int fd;
    } copy_state;
    int outfd;
    char *basename;
    size_t allocated_output;
    char *output;
};

struct rsync_patch * rsync_patch_new(char *basefile, int outfd);
void rsync_patch_work(struct rsync_patch *rp, const char *data, size_t len);
void rsync_patch_free(struct rsync_patch *rp);
