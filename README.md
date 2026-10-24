# Smart Water Bottle Reminder: Edge AI on RT-Spark

BCA182 Embedded Systems Programming, Laboratory Activity No. 4 (Design and Development of an Edge AI Consumer Product), MSU-IIT College of Computer Studies.

A water bottle that knows what you are doing with it. A small machine learning model runs **on the microcontroller itself, with no internet and no cloud**. It tells apart four states from motion data: **resting, carried, drinking, shaken**. If you have not drunk for 30 seconds, it beeps and shows "DRINK WATER NOW!" on the screen.

## Hardware

| Item | Detail |
|---|---|
| Board | RT-Thread RT-Spark (Spark-1), STM32F407ZGT6 (Cortex-M4F, 168 MHz from the 8 MHz crystal through the PLL) |
| Motion sensor | ICM20608 IMU (accelerometer ±4 g, gyroscope ±500 dps) sampled at 50 Hz |
| Light / proximity | AP3216C |
| Temperature / humidity | AHT20/21 family |
| Display | ST7789 240×240 LCD over the FSMC 8-bit bus |
| Output | Buzzer on PB0, LEDs on PF11 / PF12 |
| Debug | USART1 (PA9 / PA10, 115200 8N1) through the ST-LINK virtual COM port |

Pins used: IMU and AP3216C on software I2C bus A (SCL PF1, SDA PF0); AHT sensor on bus B (SCL PE0, SDA PE1); LCD reset PD3, backlight PF9, FSMC bank 3; buttons PC0, PC1, PC4, PC5.

Other fitted modules, checked on the diagnostics screen but not used by the product (it works offline): external SPI flash (SPI2: SCK PB13, MISO PC2, MOSI PC3, CS PB12; JEDEC ID EF4017, a Winbond W25Q64), RW007 Wi-Fi module (SPI2, CS PF10, RST PG15, INT PG11), SD card slot (card-detect PF3; no card was fitted) and the audio codec (I2C 0x10, address probe only).

### Controls

| Button | Action |
|---|---|
| LEFT | Acknowledge: resets the 30-second reminder timer and beeps once |
| RIGHT | Switch between the normal screen and the diagnostics screen |
| DOWN | On the diagnostics screen: re-run the flash, Wi-Fi and SD checks. On the normal screen: start a 10-second sensor-fault test (see below) |
| UP | Buzzer test |

The diagnostics screen shows the live status of the IMU, light/proximity sensor, temperature/humidity sensor, codec, flash, Wi-Fi module, SD slot, RTC, LCD and the on-board model self-test.

Board file: `boards/rtspark.json` (copied from the PlatformIO `black_f407zg` manifest for the same STM32F407ZGT6 and renamed for the RT-Spark). The pin mapping is defined in `src/main.c`, and the oscillator setup (8 MHz crystal, PLL to 168 MHz, APB1 /4, APB2 /2, flash latency 5) is in `src/clock.c`.

## How it works

### Water Bottle Reminder Demo

[▶ Watch the Water Bottle Reminder Demo](docs/water_bottle_reminder_demo.mp4)

1. The **sensor task** reads the IMU and light/proximity sensor every 20 ms (50 Hz) and sends samples into a queue.
2. The **inference task** keeps a 2-second window (100 samples), computes 28 features (mean, standard deviation, min and max of the six IMU axes, accelerometer magnitude statistics, proximity maximum, mean light) and runs a decision tree once per second.
3. The **reminder task** tracks the time since the last "drinking" prediction. After 30 seconds without drinking it beeps, then repeats every 10 seconds until you drink.
4. The **LCD task** shows the detected state, the time since the last drink, and live sensor readings, or the diagnostics screen.
5. The **sensor task** rejects invalid IMU reads (failed read, or a stuck bus where every axis reads the same 0 or -1). Rejected samples never reach the model. The LCD shows `SENSOR FAULT` and the reminder timer keeps running. Press DOWN on the normal screen to trigger a 10-second test of this.

### FreeRTOS tasks

| Task | Priority | Stack (words) | Job |
|---|---|---|---|
| infer | 4 | 512 | Features and decision tree, once per second |
| sensors | 3 | 512 | IMU, light and humidity reads, sample validation, queue producer |
| buzzer | 2 | 384 | Beeps when notified |
| buttons | 2 | 384 | Polls the four buttons every 20 ms (acknowledge, diagnostics, re-run / fault test, buzzer test) |
| reminder | 2 | 384 | Reminder logic; timer keeps running even if no prediction arrives |
| lcd | 1 | 640 | Display updates, diagnostics screen |
| led | 1 | 192 | Heartbeat LEDs |
| monitor | 1 | 384 | Prints RTC time, free heap and stack headroom every 10 s |

Shared-data protection: two queues (samples, results), a mutex for the UART, a mutex for the shared snapshot used by the LCD, and task notifications to the buzzer and from the LEFT button to the reminder task.

## Results

### Model (offline, `ml/train.py`)
- Data: 24 recordings of 20 s each (6 per state, `data/`), cut into 2-second windows.
Split by recording, so windows from one recording never appear in both training and test sets. Training uses sessions 1, 2, 4 and 6 (40 windows per class); testing uses sessions 3 and 5 (20 windows per class).
- Decision tree, depth 5. **Held-out accuracy 95%**. The tree uses 5 of the 28 features (`ax_mean`, `ax_max`, `ay_std`, `az_max`, `mag_std`); it does not use proximity or light.
- Baselines on the same test windows (`ml/baseline.py`, output in `ml/results.txt`):

| Method | Accuracy |
|---|---|
| Always guess the most common class | 25.0% |
| Simple threshold rules (no learning, thresholds set from the training sessions only) | 97.5% |
| Decision tree (deployed) | 95.0% |

On this small test set (80 windows) the simple rule does about as well as the tree. We kept the tree because it is trained from data and does not need hand-picked thresholds.

Per-class results of the tree:

| Class | Precision | Recall |
|---|---|---|
| Resting | 1.00 | 1.00 |
| Carried | 0.94 | 0.85 |
| Drinking | 0.86 | 0.95 |
| Shaken | 1.00 | 1.00 |

### On-board check against Python
At boot the board replays all 80 test windows (`include/selftest.h`, made by `tools/make_selftest.py`) through its own C feature code and tree. Result: 80 of 80 predicted classes match Python, with a largest feature difference of 2 parts per million. It also appears on the diagnostics screen as `SELFTEST 80/80`.

### Controlled failure test
Pressing DOWN on the normal screen forces the IMU reads to be invalid for 10 s. The log shows `IMU FAULT: samples rejected`, the LCD shows `SENSOR FAULT`, LEFT still acknowledges, the LEDs keep blinking, and 2 s after `IMU recovered` predictions resume. Evidence is in `docs/evidence/`.

### Confusion matrix

![Confusion matrix](docs/confusion_matrix.png)

### Live trials on the board (`tools/trial.py`, `data/trials.csv`)
80 trials, 20 per state, 6 seconds each. These ran on the earlier 16 MHz build; the model, features and window length are unchanged in the final 168 MHz build.

| State | Correct |
|---|---|
| Resting | 20 / 20 |
| Carried | 16 / 20 |
| Drinking | 20 / 20 |
| Shaken | 17 / 20 |
| **Overall** | **73 / 80 (91.2%)** |

All 7 mistakes were predicted as "drinking". An earlier carried run scored 15 / 20.

### Performance and resources (final firmware)
- Inference time (50 runs, features plus tree): median 154 µs, P95 154 µs, max 154 µs. (On the earlier 16 MHz build it was 1638 µs.)
- Final firmware: Flash 162,064 B (15.5%), RAM 51,712 B (39.5%), including the 48 KB FreeRTOS heap. About 137 KB of the Flash is the embedded self-test data, which can be switched off with `#define SELFTEST 0` in `src/main.c`; without it the application and model take roughly 24 KB.
The firmware used for the 15-minute run measured 22,096 B Flash and 51,660 B RAM. Minimum free heap during that run: 33,744 B.
- Sampling-to-response delay (estimated, not measured with a timer): a prediction is made every 1 s from the last 2 s of data and the LCD refreshes every 500 ms, so a new motion shows on screen about 2 to 3.5 s after it starts. The LEFT button takes effect within about 120 ms.
- 15-minute run (`tools/soak.py`, `logs/soak_run.txt`): 901 predictions, longest gap between predictions 1000 ms, 0 queue drops, 0 IMU errors, 0 reboots, 0 serial drops.

## Limitations

- Trained and tested by one person with one board, so it may be less accurate for other people.
- Several tree rules use the x-axis, so the result depends on how the bottle is held.
- Carried and shaken are sometimes mistaken for drinking, so a drink can be reported by mistake and reset the reminder timer.
- The live trials were run on the earlier 16 MHz build and not repeated at 168 MHz.
- A simple rule matches the tree on our small test set, so machine learning has not clearly added value here yet. We did not test other people or other bottles.
- Pressing RST in the middle of an IMU read can leave the IMU holding the I2C bus (`init: IMU FAIL`). Unplug and replug USB to recover.

## Build and run

Requirements: VS Code with the PlatformIO extension, an RT-Spark board, and the ST-LINK driver on Windows.

1. Clone this repo and open the folder in VS Code.
2. Plug in the board through the **USB-DBG** port.
3. Click **Build** (✓), then **Upload** (→) in the PlatformIO bar.
4. Open the serial monitor (115200). You should see `state=... infer=... us` once per second.

The firmware is built with PlatformIO (`ststm32`, `stm32cube`) and FreeRTOS V11.1.0 in `lib/FreeRTOS`.

## Reproduce the data and model

1. Set `#define STREAM_CSV 1` in `src/main.c` and upload. The board now streams raw samples.
2. Record a state: `python tools/collect.py <resting|carried|drinking|shaken> 20` (close the serial monitor first). Files appear in `data/`.
3. Train: `python ml/train.py` (needs numpy, pandas, scikit-learn, matplotlib). It prints the accuracy table and writes `include/model.h` and `ml/confusion_matrix.png`. Then compare with the rule baseline: `python ml/baseline.py`. Regenerate the on-board self-test data: `python tools/make_selftest.py` (writes `include/selftest.h`).
4. Set `STREAM_CSV` back to 0, then build and upload.
5. Test on the board: `python tools/trial.py`, and for the stability run `python tools/soak.py 15`.

The Python tools use COM3. Change `PORT` at the top of each script if your board uses another port.

## Data manifest

| Item | Detail |
|---|---|
| Files | `data/<state>_<nn>.csv`, 6 recordings for each of resting, carried, drinking and shaken |
| Length | 20 s each, about 1006 samples at 50 Hz |
| Columns | `ms, ax, ay, az, gx, gy, gz, ps, als` |
| Units | Raw sensor counts. Accelerometer 8192 LSB/g, gyroscope 65.5 LSB/dps, proximity and light as read from the AP3216C |
| Subject | One person, the author. No personal data or identifiers |
| Labels | Taken from the file name (the state that was performed) |
| Split | By recording: sessions 1, 2, 4, 6 for training and 3, 5 for testing |
| Trial results | `data/trials.csv`: `true, trial, predicted, correct` |
| Model | `include/model.h` (generated), a decision tree of depth 5 on 28 features |
| Baseline results | `ml/results.txt` (majority-class, threshold rules and tree, with per-class precision and recall) |
| Self-test data | `include/selftest.h` (generated from the 80 held-out windows by `tools/make_selftest.py`) |
| Evidence | `docs/evidence/` (screenshots and photos) |

## Troubleshooting

| Problem | Fix |
|---|---|
| `LIBUSB_ERROR_NOT_FOUND` on upload | Install the ST-LINK driver (STSW-LINK009) on Windows. |
| `Failed to read memory` or `timed out while waiting for target halted` | The `upload_flags` in `platformio.ini` (connect under reset) fix this. Keep them. |
| Python tool says `Access is denied` on COM3 | Close the PlatformIO serial monitor first. Only one program can use the port. |
| Link error about floating-point registers | Build with `-mfloat-abi=softfp` as in `platformio.ini`. |
| FreeRTOS builds the wrong port | `lib/FreeRTOS/library.json` selects only the Cortex-M4F port and `heap_4.c`. |
| Serial text is garbled | The clock is wrong. Check `src/clock.c` and `-DHSE_VALUE=8000000U` in `platformio.ini`. |
| OpenOCD `init failed` on upload | The board is not plugged in, or the USB cable is in the wrong port. Plug it into the ST-LINK port and try again. |
| `init: IMU FAIL` after pressing RST | The IMU is stuck. Unplug the USB cable, wait 5 s, and plug it in again. |

## License and attribution

- Project code: see `LICENSE`.
- FreeRTOS kernel V11.1.0 (`lib/FreeRTOS`): MIT license, Amazon Web Services.
- STM32Cube HAL and CMSIS: STMicroelectronics, used through the PlatformIO `stm32cube` framework.
- LCD pin mapping and ST7789 start-up sequence: taken from the RT-Thread RT-Spark BSP (`drv_lcd.c`, Apache-2.0).
- The 5×7 bitmap font table is a standard public-domain font.
- Training uses numpy, pandas, scikit-learn and matplotlib.

## Repository layout

```text
src/main.c        firmware (tasks, drivers, inference)
src/clock.c       168 MHz clock setup
boards/           RT-Spark board file for PlatformIO
include/          FreeRTOSConfig.h, model.h and selftest.h (generated)
lib/FreeRTOS/     FreeRTOS kernel
ml/               train.py, baseline.py, results.txt
tools/            collect.py, trial.py, soak.py, make_selftest.py
docs/             lab report, confusion matrix, demo video, evidence/
data/             recordings and trial results
logs/             15-minute run log
```
## Team

- Cassandra Khan P. Mendoza: firmware, data, training
- Sean Marie G. Rabago: lab report Introduction, product concept, block diagram, and task 1
- Carl Hovan B. Chan: lab report Results (Data Collection, Model Training, 80-Trial Test, 15-Min Run)
- Shaqkobe Dos P. Tejada: demo video