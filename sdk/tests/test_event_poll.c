#include "win32_compat.h"
#include <assert.h>
#include <stdio.h>
#include <time.h>

int main(void)
{
    HANDLE events[56];
    for (int i = 0; i < 56; ++i)
        events[i] = CreateEventA(NULL, FALSE, FALSE, NULL);
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    assert(WaitForMultipleObjects(56, events, FALSE, 0) == WAIT_TIMEOUT);
    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = end.tv_sec - start.tv_sec + (end.tv_nsec - start.tv_nsec) / 1e9;
    assert(elapsed < 0.2);
    assert(SetEvent(events[37]));
    assert(WaitForMultipleObjects(56, events, FALSE, 0) == WAIT_OBJECT_0 + 37);
    assert(WaitForSingleObject(events[37], 0) == WAIT_TIMEOUT);
    HANDLE manual = CreateEventA(NULL, TRUE, TRUE, NULL);
    assert(WaitForSingleObject(manual, 0) == WAIT_OBJECT_0);
    assert(WaitForSingleObject(manual, 0) == WAIT_OBJECT_0);
    assert(ResetEvent(manual));
    assert(WaitForSingleObject(manual, 0) == WAIT_TIMEOUT);
    assert(CloseHandle(manual));
    for (int i = 0; i < 56; ++i) assert(CloseHandle(events[i]));
    printf("Event poll checks passed (56 idle events: %.6f s)\n", elapsed);
    return 0;
}
