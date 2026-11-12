#pragma once
/* A Mac app's start (app_bundle.c). When the executable sits in an app's Contents/MacOS and its
 * arguments are only flags (or none), the launch is prepared from the Info.plist's BBTitleID
 * (CUSA19610: 13 Sentinels, the default; CUSA03173: Bloodborne). Both: the game's folder is
 * <the app's folder>/<title id>, the data folder is ~/Library/Application Support/<name>
 * (S13_APP_DATA_DIR / BB_APP_DATA_DIR), wayfarer.cfg there is read (renderer, fullscreen, display,
 * vsync, fps_mode, fps_limit, metal_hud, frame_log), the output goes to logs/<date>.log
 * (BB_APP_LOG=- keeps it on the terminal), and BB_APP_DRY_RUN=1 prints the result and exits.
 * 13 Sentinels: sets what scripts/run-13s.sh sets where the environment doesn't already and the
 * arguments `- --app0 <game> --user <data>/user --timeout 0` plus the flags given
 * (S13_APP_DRY_RUN=1 is its older dry run). Bloodborne: runs the bundle's run.sh once, which the
 * probe answers with its arguments, environment and directory instead of starting the game
 * (bb_app_launch_dump), and takes those over. Anything else (a developer's `bb-probe - --app0 …`)
 * is left alone.
 * Bloodborne also: on the Metal renderer BB_SHADER_NOTE=1 is set unless the environment has it (play
 * records the pipeline states the shader compile step builds from). The argument --compile-shaders
 * (Bloodborne.app only, taken out of the arguments the game gets) sets BB_SHADER_COMPILE=1,
 * BB_SHADER_COMPILE_EXIT=1 and BB_SHADER_COMPILE_FORCE=1 and otherwise starts as usual, so the window shows
 * the step and the process ends after it. Refused with a message on stderr and nothing started: exit 3
 * (this app holds 13 Sentinels), exit 4 (the renderer is MoltenVK, or not Metal for another reason). */
#ifdef __APPLE__
void bb_app_bundle_bootstrap(int *argc, char ***argv);
/* Called before anything else: with BB_LAUNCH_DUMP set and `-` as the first argument, writes the
 * launch to that file and exits. */
void bb_app_launch_dump(int argc, char **argv);
/* 1 when this executable holds Bloodborne: an app whose Info.plist's BBTitleID is CUSA03173, or a
 * program that is not in an app at all (a developer's bb-probe). 0 for 13 Sentinels' app (and
 * any other title's). Reads the Info.plist only (--compile-status). */
int bb_app_holds_bloodborne(void);
#endif
