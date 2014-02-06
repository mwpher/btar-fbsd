#include <limits.h>

struct filter {
    char **args;
    struct filter *next;
    char extensions[PATH_MAX];
};

void
run_filters(struct filter *filter, int *fdin, int *fdout);

void
run_filters_given_fdin(struct filter *filter, int fdin, int *fdout);

struct filter *
append_filter(struct filter *f, char **args);

struct filter *
append_filter_spaces(struct filter *f, const char *arg);

struct filter *
defilters_from_extensions(const char *filename);

void
free_filters(struct filter *f);

const char *
get_filter_extensions(struct filter *f);

extern struct filter *filter; /* For traverse.c */
