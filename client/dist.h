#ifndef MY_CLIENT_DIST_H
#define MY_CLIENT_DIST_H

#include <random>
#include <stdint.h>
#include <cmath>

class ExpDist {
private:
    std::default_random_engine g;
    std::exponential_distribution<double> d;
    uint64_t curNs;

public:
    ExpDist(double lambda, uint64_t seed, uint64_t startNs)
        : g(static_cast<unsigned>(seed & 0xFFFFFFFFu)), d(lambda), curNs(startNs) {}

    uint64_t nextArrivalNs() {
        curNs += static_cast<uint64_t>(d(g));
        return curNs;
    }
};

class BimodalDist {
private:
    std::default_random_engine g;
    std::uniform_real_distribution<double> d;
    uint64_t work1, work2;
    double ratio;

public:
    BimodalDist(uint64_t seed, uint64_t work1_ns, uint64_t work2_ns, double ratio_)
        : g(static_cast<unsigned>(seed & 0xFFFFFFFFu)), d(0.0, 1.0),
          work1(work1_ns), work2(work2_ns), ratio(ratio_) {}

    uint64_t workNs() {
        if (d(g) < ratio)
            return work1;
        else
            return work2;
    }
};

#endif
