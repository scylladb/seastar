extern "C" {
#include <signal.h>
#include <time.h>
}

int main() {
    timer_t td;
    struct sigevent sev;
    timer_create(CLOCK_MONOTONIC, &sev, &td);
}
