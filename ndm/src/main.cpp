#include <stdio.h>
#include "spork.h"

int main(int argc, const char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Usage: spork script.js\n");
        return 1;
    } else {
        spork::init(argv[1]);
        return 0;
    }
}
