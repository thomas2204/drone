#include "uwb_range.h"
#include <assert.h>
#include <stdio.h>

static bool running;
static unsigned nesting, entries;
void UWB_TestEnterCritical(void)
{
    /* The embedded port leaves interrupts masked when used before startup. */
    assert(running);
    nesting++;
    entries++;
}
void UWB_TestExitCritical(void) { assert(nesting != 0); nesting--; }

int main(void)
{
    UWB_RangeSnapshot snapshot;
    UWB_RangeInit();
    assert(entries == 0); /* Initialization must not touch task critical state. */
    running = true;
    assert(UWB_RangeGetSnapshot(&snapshot));
    assert(snapshot.init_result == 1 && !snapshot.valid && snapshot.polls == 0);
    assert(nesting == 0 && entries == 1);
    running = false;
    UWB_RangeTaskUnavailable();
    assert(entries == 1); /* Allocation-failure reporting is also pre-scheduler. */
    running = true;
    assert(UWB_RangeGetSnapshot(&snapshot));
    assert(snapshot.init_result == -5 && !snapshot.valid);
    assert(!UWB_RangeGetSnapshot(NULL));
    assert(nesting == 0);
    puts("UWB startup tests passed (pre-scheduler init and allocation failure).");
}
