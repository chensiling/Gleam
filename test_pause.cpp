// Simple test program for pause command
#include <windows.h>

int main() {
    // Just loop forever so we can test pause
    volatile int x = 0;
    while(true) {
        x++;
        Sleep(100);
    }
    return 0;
}
