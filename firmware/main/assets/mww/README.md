# microWakeWord models (Mochi build)

- `okay_nabu.tflite` / `.json`: stock model from esphome/micro-wake-word-models (v2, Apache-2.0),
  used to prove the engine before the custom "hey mochi" model is trained.
- The model in use is set by `CONFIG_MICRO_WAKE_WORD_MODEL` (with cutoff, window and arena size
  from the model's .json) in `main/boards/stackchan/config.json`.
- `hey_mochi.tflite` / `.json`: Mochi's own model, trained 2026-10-20 on Tai's PC with
  JohnnyPrimus/MicroWakeWordV2Trainer (Piper TTS, spellings "hey mochi" + "hey moe chee", 2000 samples,
  10k steps). Test set: cutoff 0.97 -> 9% missed, 0 false/h; 0.88 -> 7% missed, 0.19 false/h.
- `hey_mochi_v2.tflite` / `.json`: retrained 2026-10-20 with 40 of Tai's own clips recorded through Mochi's mic
  (x10 each) on top of the synthetic set. Held-out real clips: 10/10 (v1: 8/10). Trainer test set: cutoff 0.75
  -> 4.6% missed, 0 false/h. Build default cutoff 0.80, window 3.
