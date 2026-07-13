#include "aggregator.h"

#include <math.h>
#include <string.h>

void aggregator_reset(aggregator_t *a)
{
    memset(a, 0, sizeof(*a));
}

void aggregator_add_sample(aggregator_t *a, double value)
{
    a->count++;
    double delta = value - a->mean;
    a->mean += delta / (double)a->count;
    double delta2 = value - a->mean;
    a->m2 += delta * delta2;

    if (!a->has_data) {
        a->min = value;
        a->max = value;
        a->has_data = true;
    } else {
        if (value < a->min) {
            a->min = value;
        }
        if (value > a->max) {
            a->max = value;
        }
    }
    a->last_raw = value;
}

double aggregator_stddev(const aggregator_t *a)
{
    if (a->count < 2) {
        return 0.0;
    }
    return sqrt(a->m2 / (double)(a->count - 1));
}
