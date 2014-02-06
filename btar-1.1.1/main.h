#include <limits.h>
#include <stdlib.h>
#include <sys/select.h>

extern struct command_line {
    int verbose;
    int debug;
    unsigned long long blocksize;
    int add_create_index;
    int parallelism;
    int xorblock;
    int should_rsync;
    int should_delete;
    size_t rsync_minimal_size;
    size_t rsync_max_delta;
    size_t rsync_block_size;
    const char **paths;
    const char **input_files;
    const char **exclude_patterns;
    const char **references;
    enum reftype {REF_NOTAR, REF_INDEX} *reference_types;
    enum
    {
        FILTER,
        CREATE,
        EXTRACT,
        EXTRACT_TO_TAR,
        EXTRACT_INDEX,
        LIST_INDEX,
        MANGLE
    } action;
} command_line;

void set_cloexec(int fd);
void addfd(fd_set *set, int fd, int *nfds);

void load_index_from_tar(int fd);

enum {
    buffersize = 1*1024*1024
};

/* error.c */
void error(const char *str);
void fatal_errno(const char *str, ...);
void fatal_error(const char *str, ...);
void fatal_error_no_core(const char *str, ...);

/* string.c */
void strcpyn(char *dest, const char *src, size_t n);
