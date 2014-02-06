#include <stdio.h>
#include <string.h>
#include <stdlib.h>

char *block;
size_t allocated;

int main(int argc, char *argv[])
{
    int i;
    size_t pos;

    if (argc == 1)
    {
        fprintf(stderr, "usage: xortest <b1> [b2] [b3] [...] > other");
        return 1;
    }

    block = malloc(1);
    block[0] = 0;
    allocated = 1;

    for(i=1; i < argc; ++i)
    {
        FILE *f = fopen(argv[i], "rb");
        unsigned long long size;
        size_t nread;
        size_t towrite;

        size_t j;

        if (!f)
        {
            fprintf(stderr, "Error! Can't open file %s", argv[i]);
            return 1;
        }

        fprintf(stderr, "Opened file %s\n", argv[i]);

        fseek(f, 0, SEEK_END);
        size = ftell(f);
        rewind(f);

        fprintf(stderr, " size %llu, last %hhi\n", size, block[allocated-1]);

        if (size + 1 > allocated)
        {
            block = realloc(block, size+1);
            memset(block + allocated, block[allocated-1], size + 1 - allocated);
            allocated = size + 1;
        }

        pos = 0;
        towrite = size;
        while(!feof(f))
        {
            char b[4096];

            nread = fread(b, 1, sizeof b, f);

            for(j = 0; j < nread; ++j)
                block[j+pos] ^= b[j];

            towrite -= nread;
            pos += nread;
        }

        for(j = pos; j < allocated; ++j)
            block[j] ^= 0;

        fclose(f);
    }

    fprintf(stderr, "Writing XOR of size %zu\n", allocated);

    pos = 0;
    while(allocated > 0)
    {
        size_t nwritten;
        nwritten = fwrite(block + pos, 1, allocated, stdout);
        pos += nwritten;
        allocated -= nwritten;
    };

    return 0;
}
