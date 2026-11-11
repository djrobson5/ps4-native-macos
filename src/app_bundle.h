#pragma once
/* A Mac app's start (app_bundle.c). When the executable sits in an app's Contents/MacOS and its
 * arguments are only flags (or none): makes the data folder (S13_APP_DATA_DIR, else
 * ~/Library/Application Support/13 Sentinels), reads its wayfarer.cfg (renderer=metal|moltenvk,
 * fullscreen=0|1), sets what scripts/run-13s.sh sets where the environment doesn't already, sends
 * the output to logs/<date>.log (S13_APP_LOG=- keeps it on the terminal), changes to the data
 * folder and replaces the arguments with `- --app0 <app's folder>/CUSA19610 --user <data>/user
 * --timeout 0` plus the flags given. S13_APP_DRY_RUN=1 prints the result and exits. Anything else
 * (a developer's `bb-probe - --app0 …`) is left alone. */
#ifdef __APPLE__
void bb_app_bundle_bootstrap(int *argc, char ***argv);
#endif
