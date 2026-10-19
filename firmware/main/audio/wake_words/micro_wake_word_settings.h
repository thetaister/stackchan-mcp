#ifndef MICRO_WAKE_WORD_SETTINGS_H
#define MICRO_WAKE_WORD_SETTINGS_H

// Mochi build: live microWakeWord sensitivity, saved in NVS ("mww" namespace) so it survives reboots.
// cutoff_pct: detection fires when the window-average probability exceeds this (50-99).
// window: number of model outputs averaged (1-10; each is ~30 ms). Lower values catch shorter, softer wakes.
void MwwSetSensitivity(int cutoff_pct, int window);
void MwwGetSensitivity(int* cutoff_pct, int* window);
// Restores the build defaults (CONFIG_MICRO_WAKE_WORD_CUTOFF / _WINDOW) and clears the saved values.
void MwwResetSensitivity();

#endif
