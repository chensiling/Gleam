// ArgvTarget: prints argc and each argv element verbatim, for
// command-line quoting verification.
#include <cstdio>

int main(int argc, char** argv)
{
    printf("ARGC=%d\n", argc);
    for(int i = 1; i < argc; i++)
        printf("ARGV[%d]=[%s]\n", i, argv[i]);
    fflush(stdout);
    return 0;
}
