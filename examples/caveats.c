#include <stdint.h>

uint32_t worth_a_followup(uint32_t has_evidence, uint32_t sources)
{
    return has_evidence && sources >= 2u;
}

uint32_t enough_to_publish(uint32_t already_verified, uint32_t sources)
{
    return already_verified || sources >= 2u;
}
