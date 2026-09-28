#include "spsc_ring.h"

#include <algorithm>
#include <cstdio>
#include <thread>

#define CHECK(cond)                                              \
    do {                                                         \
        if (!(cond)) {                                           \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);  \
            return 1;                                            \
        }                                                        \
    } while (0)

int main() {
    // basic push / pop
    {
        SpscRing<int> r(8);
        int in[5] = {1, 2, 3, 4, 5};
        CHECK(r.push(in, 5) == 5);
        int out[8] = {};
        CHECK(r.pop(out, 8) == 5);
        for (int i = 0; i < 5; ++i) CHECK(out[i] == i + 1);
        CHECK(r.size() == 0);
    }

    // full buffer only accepts what fits
    {
        SpscRing<int> r(8);
        int in[10] = {};
        CHECK(r.push(in, 10) == 8);
        CHECK(r.push(in, 1) == 0);
    }

    // two threads, 5 million ints, order must be perfect
    {
        constexpr int N = 5'000'000;
        SpscRing<int> ring(1024);
        std::thread producer([&] {
            int next = 0;
            int chunk[64];
            while (next < N) {
                const int m = std::min(64, N - next);
                for (int k = 0; k < m; ++k) chunk[k] = next + k;
                next += static_cast<int>(ring.push(chunk, m));
            }
        });

        int expected = 0;
        bool ok = true;
        int buf[64];
        while (expected < N) {
            const size_t got = ring.pop(buf, 64);
            for (size_t k = 0; k < got; ++k)
                if (buf[k] != expected++) ok = false;
        }
        producer.join();
        CHECK(ok);
    }

    std::printf("ring_test: all passed\n");
    return 0;
}
