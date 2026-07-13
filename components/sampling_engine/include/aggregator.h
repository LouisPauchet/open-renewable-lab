#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Welford running mean/variance + min/max, reset at the start of every
 * logging interval. Numerically stable for long-running accumulation,
 * unlike a naive sum-of-squares approach. */
typedef struct {
    uint32_t count;
    double mean;
    double m2; /* sum of squared deviations from the running mean */
    double min;
    double max;
    double last_raw;
    bool has_data;
} aggregator_t;

void aggregator_reset(aggregator_t *a);
void aggregator_add_sample(aggregator_t *a, double value);

/* Sample standard deviation (n-1 denominator); 0 if count < 2. */
double aggregator_stddev(const aggregator_t *a);

#ifdef __cplusplus
}
#endif
