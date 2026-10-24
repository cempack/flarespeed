/* Terminal color profile detection, following colorprofile.Detect. */
#ifndef FLARESPEED_PROFILE_H
#define FLARESPEED_PROFILE_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
	PROFILE_UNKNOWN,
	PROFILE_NOTTY,
	PROFILE_ASCII,
	PROFILE_ANSI,
	PROFILE_ANSI256,
	PROFILE_TRUECOLOR,
} profile_t;

/* Looks up an environment variable, returning NULL when it is unset. */
typedef const char *(*env_lookup_fn)(const char *name);

/* Picks the color profile for output going to a terminal (or not). */
profile_t profile_detect(bool output_is_tty, env_lookup_fn env);

/* The environment-only part of detection, colorprofile's colorProfile. */
profile_t profile_from_env(bool isatty, env_lookup_fn env);

/* Decodes a compiled terminfo entry. Returns 1 if it declares the Tc or RGB
 * extended capability, 0 if not, and -1 if the data is malformed. */
int terminfo_declares_truecolor(const unsigned char *data, size_t len);

/* strconv.ParseBool, with anything unparsable counting as false. */
bool env_bool(const char *value);

#endif
