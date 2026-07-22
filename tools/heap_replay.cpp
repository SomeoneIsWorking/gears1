// Replays a recorded guest heap trace through the REAL GuestHeap, so the
// allocator's behaviour on the title's actual request stream can be measured
// without a 13-minute game run.
//
// Input is a lucent log captured with the "heap" debug channel enabled:
//   [heap] allocated 0xa0010000+0x10000 (type 0x20001000, 64 KiB pages)
//   [heap] freed 0xa0010000+0x10000
// The recorded address selects the heap (0x4... title, 0xA... physical) and
// identifies the region across the alloc/free pair; the replay reallocates
// from scratch and keeps its own mapping from recorded address to replayed
// address, because a working allocator will not reproduce the recorded
// addresses -- that is the entire point.
//
//   heap_replay <trace.log>
//
// It reports the peak of each heap and whether the request stream fits.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include "guest_heap.h"
#include "guest_memory.h"
#include "ppc_config.h"
#include "ppc_context.h"

PPCFuncMapping PPCFuncMappings[] = { { 0, nullptr } };

namespace
{
struct Replay
{
    gears::GuestHeap& heap;
    std::map<uint32_t, uint32_t> mapping; // recorded address -> replayed address
    uint64_t allocs = 0;
    uint64_t frees = 0;
    uint64_t failures = 0;
};
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: heap_replay <trace.log>\n");
        return 2;
    }

    FILE* f = fopen(argv[1], "r");
    if (!f)
    {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }

    gears::GuestMemory memory;
    if (!memory.Reserve())
        return 2;

    static gears::GuestHeap titleHeap(memory, 0x40000000, 0x20000000);
    static gears::GuestHeap physicalHeap(memory, 0xA0000000, 0x20000000);
    Replay title{ titleHeap };
    Replay physical{ physicalHeap };

    char line[512];
    uint64_t lineNumber = 0;
    while (fgets(line, sizeof(line), f))
    {
        ++lineNumber;
        const char* p = strstr(line, "[heap] allocated ");
        bool freeing = false;
        if (!p)
        {
            p = strstr(line, "[heap] freed ");
            freeing = p != nullptr;
        }
        if (!p)
            continue;

        uint32_t address = 0, size = 0, type = 0;
        if (freeing)
        {
            if (sscanf(p, "[heap] freed %x+%x", &address, &size) != 2)
                continue;
        }
        else if (sscanf(p, "[heap] allocated %x+%x (type %x", &address, &size, &type) != 3)
        {
            continue;
        }

        Replay& r = (address >= 0xA0000000) ? physical : title;

        if (freeing)
        {
            auto it = r.mapping.find(address);
            if (it == r.mapping.end())
                continue; // freed something allocated before the trace started
            r.heap.Free(it->second);
            r.mapping.erase(it);
            ++r.frees;
            continue;
        }

        // Newer traces record the alignment argument. Older ones do not, and it
        // must NOT be inferred from the recorded address: a bump allocator's
        // addresses are incidentally aligned to far more than was asked for
        // (0x40000000 looks 1 GiB-aligned), which makes the replay reject
        // blocks the real allocator would have used. Fall back to 0, which is
        // what every title-heap caller passes.
        uint32_t alignment = 0;
        const char* alignField = strstr(p, ", align ");
        if (alignField)
            sscanf(alignField, ", align %x", &alignment);

        uint32_t requested = size;
        const uint32_t got = r.heap.Allocate(0, requested, type, alignment);
        ++r.allocs;
        if (got == 0)
        {
            ++r.failures;
            if (r.failures <= 3)
                fprintf(stderr, "line %llu: allocation of %#x failed\n",
                    (unsigned long long)lineNumber, size);
            continue;
        }
        r.mapping[address] = got;
    }
    fclose(f);

    auto report = [](const char* name, Replay& r) {
        const gears::GuestHeap::Usage u = r.heap.GetUsage();
        printf("%s: %llu allocs, %llu frees, %llu FAILURES\n", name,
            (unsigned long long)r.allocs, (unsigned long long)r.frees,
            (unsigned long long)r.failures);
        printf("  peak %.2f MiB, live %.2f MiB in %u regions\n",
            u.peak / 1048576.0, u.allocated / 1048576.0, u.regions);
        printf("  free %.2f MiB in %u blocks, largest %.2f MiB\n",
            u.free / 1048576.0, u.freeBlocks, u.largestFree / 1048576.0);
    };
    report("title heap", title);
    report("physical heap", physical);
    return (title.failures || physical.failures) ? 1 : 0;
}
