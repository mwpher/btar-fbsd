struct file_memory
{
    struct block *bo;
    int fd;
};

struct file_memory * file_memory_new(int fd);
void file_memory_prepare_readfds(struct file_memory *im, fd_set *fdset, int *nfds);
void file_memory_check_readfds(struct file_memory *im, fd_set *fdset);
void file_memory_to_tar(struct file_memory *im, const char *namepattern,
        const char *filter_extensions, struct mytar *tar);
int file_memory_finished(struct file_memory *im);
