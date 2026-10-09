#pragma once

/* TEMPORARY: jumper GPIO a <-> b, logs GPIO and UART loopback results. */
void pin_diag_run(int a, int b);

/* Drive each pin 0/1 on its own and read the pad back — no jumper needed. */
void pin_diag_selftest(const int *pins, int count);

/* With the sensor wired: check UART TX reaches the pad, count RX edges after a command. */
void pin_diag_sensor_lines(int tx, int rx);

/* Background task logging every level change on `pin`. */
void pin_diag_watch(int pin);
