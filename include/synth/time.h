#ifndef SYNTH_TIME_H
#define SYNTH_TIME_H

#include <synth/hal.h>

uint32_t time_now(void);
void time_local(struct rtc_time *t);
int time_zone(void);
void time_zone_format(int minutes, char *buf, size_t cap);
uint32_t time_from_rtc(const struct rtc_time *t);
void time_to_rtc(uint32_t epoch, struct rtc_time *t);
int time_weekday(const struct rtc_time *t);
const char *time_weekday_name(int day);
const char *time_month_name(int month);
void time_format(uint32_t epoch, char *buf, size_t cap);

#endif
