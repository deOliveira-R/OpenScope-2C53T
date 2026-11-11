/*
 * OpenScope 2C53T - experimental continuity buzzer drive.
 *
 * The stock firmware audibly beeps in continuity mode, but this clean-room
 * tree does not yet have a confirmed buzzer pin/timer map. PB9 is stock
 * AF-push-pull and is one of the unresolved auxiliary pins, so this driver
 * tries it as a bounded bench probe: output toggles only in continuity mode
 * while the parser reports an actual short.
 */
#ifndef CONTINUITY_BUZZER_H
#define CONTINUITY_BUZZER_H

#include <stdbool.h>
#include <stdint.h>

void continuity_buzzer_init(void);
void continuity_buzzer_create_task(void);
void continuity_buzzer_force_ms(uint32_t duration_ms);
void continuity_buzzer_snapshot(bool *task_started, bool *active,
                                uint32_t *toggle_count,
                                uint32_t *create_fail_count);


/* M4: sound the buzzer the way stock does, from the SoC's buzzer line on
 * EXTI3, gated to Auto and Continuity, one fixed pulse per edge. Off by default
 * (the software shorted-lead verdict stays in charge) until the bench has
 * chosen the EXTI3 source pin: port 2 = C, 3 = D, 4 = E (A/B are refused: USART2/SPI3), -1 = line off; edge
 * 0 rising / 1 falling / 2 both. */
bool continuity_buzzer_soc_edge_config(int8_t port, uint8_t edge, bool pull_up);
void continuity_buzzer_soc_edge_mode_set(bool on);
void continuity_buzzer_soc_tick_ms_set(uint32_t ms);
void continuity_buzzer_soc_status(bool *mode_on, int8_t *port, uint32_t *edges,
                                  uint32_t *pulses, uint32_t *gated,
                                  bool *pulse_active, uint32_t *tick_ms);

#endif /* CONTINUITY_BUZZER_H */
