/* The interactive mode: a live view of the test in the terminal. */
#ifndef FLARESPEED_APP_H
#define FLARESPEED_APP_H

#include "model.h"
#include "profile.h"

/* Runs tests until the user quits; returns the process exit status. */
int app_run(config_t cfg, const char *base_url, profile_t profile, int signal_fd);

#endif
