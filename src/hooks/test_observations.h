#ifndef DEFJAM_TEST_OBSERVATIONS_H
#define DEFJAM_TEST_OBSERVATIONS_H
#include <stdint.h>
int defjam_test_observations_enabled(void);
void defjam_test_update(void);
void defjam_test_hit(uint32_t attacker, uint32_t defender, uint32_t caller);
void defjam_test_health_begin(uint32_t actor, uint32_t caller);
void defjam_test_health_notify(uint32_t actor, uint32_t code);
/* test_telemetry.c: match state and combat events ([TEST-STATE], [TEST-EVENT]). */
void defjam_test_match_start(void);
void defjam_test_rng(uint32_t stack);
void defjam_test_step(void);
void defjam_test_attack(int attacker_slot, int defender_slot);
void defjam_test_damage(int defender_slot, float before, float after);
void defjam_test_result(uint32_t winners, uint32_t losers, uint32_t code);
void defjam_test_input(uint32_t actor);
void defjam_test_ai_seed(uint32_t stack);
long defjam_test_shot_due(void);
void defjam_test_generator_seed(uint32_t generator, uint32_t stack);
void defjam_test_draw(uint32_t generator, uint32_t caller);
void defjam_test_crowd_update(void);
#endif
