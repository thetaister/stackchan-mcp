# microWakeWord models (Mochi build)

- `okay_nabu.tflite` / `.json`: stock model from esphome/micro-wake-word-models (v2, Apache-2.0),
  used to prove the engine before the custom "hey mochi" model is trained.
- The model in use is set by `CONFIG_MICRO_WAKE_WORD_MODEL` (with cutoff, window and arena size
  from the model's .json) in `main/boards/stackchan/config.json`.
