// SpoutDX header compile check: proves the SDK headers + lib link cleanly
// with the project's build flags before the real sender integration.
#include "spout/SpoutDX.h"
#include <cstdio>

int main()
{
    spoutDX spout;
    spout.SetSenderName("NeuralScreenTest");
    printf("spoutDX object OK\n");
    return 0;
}
