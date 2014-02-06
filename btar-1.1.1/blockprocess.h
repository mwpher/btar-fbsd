struct block_process
{
    struct block *bi;
    struct block_reader *br_to_filter;
    int fd_filterin;
    int fd_filterout;
    struct block *bo;
    size_t left;
    int closed_in;
    int block_finished;
    int nblock;
    int has_read;
    int finished_read_ack; /* to keep track of the caller having acked */
};

struct block_process * block_process_new(int nblock);
int block_process_can_read(const struct block_process *bp);
size_t block_process_total_read(const struct block_process *bp);
void prepare_readfds(struct block_process *bp, fd_set *fdset, int *nfds, int should_read);
void prepare_writefds(struct block_process *bp, fd_set *fdset, int *nfds);
void check_read_fds(struct block_process *bp, fd_set *readfds, int should_read);
void check_write_fds(struct block_process *bp, fd_set *writefds);
void dump_block_to_tar(struct block_process *bp, struct mytar *tar);

struct block_reader * block_process_new_input_reader(struct block_process *bp);
void block_process_reset(struct block_process *bp, int nblock);
int block_process_finished(struct block_process *bp);
int block_process_finished_reading(struct block_process *bp);
int block_process_has_read(struct block_process *bp);
void xor_to_xorblock(struct block_process *bp, struct block *xorblock);
