/* Number formatting for the readouts, matching the Go version digit for
 * digit. */
#ifndef FLARESPEED_FORMAT_H
#define FLARESPEED_FORMAT_H

#include <stddef.h>
#include <stdint.h>

/* Room for any double in fixed notation ("%.2f" of -DBL_MAX is 313
 * characters), with space left for a unit after it. */
#define FMT_LEN 352

/* Formats v to about three significant digits: 4.87, 48.7, 487. */
void fmt_sig3(char out[FMT_LEN], double v);

/* Splits a bit rate into a display number and its unit. */
void fmt_rate(char value[FMT_LEN], const char **unit, double bps);

/* "487 Mbps". */
void fmt_rate_text(char out[FMT_LEN], double bps);

/* "21.8 ms". */
void fmt_ms(char out[FMT_LEN], int64_t ns);

/* "812 MB". */
void fmt_bytes(char out[FMT_LEN], int64_t n);

/* "4.2s". */
void fmt_seconds(char out[FMT_LEN], int64_t ns);

/* Nanoseconds to milliseconds, like float64(d) / float64(time.Millisecond). */
double ns_to_ms(int64_t ns);

/* time.Duration.Seconds(). */
double ns_to_seconds(int64_t ns);

#endif
