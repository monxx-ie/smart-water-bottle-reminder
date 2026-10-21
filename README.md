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

Not used by this product: the Wi-Fi module, external flash, SD card slot (no card was fitted) and audio codec.

Board file: `boards/rtspark.json` (copied from the PlatformIO `black_f407zg` manifest for the same STM32F407ZGT6 and renamed for the RT-Spark). The pin mapping is defined in `src/main.c`, and the oscillator setup (8 MHz crystal, PLL to 168 MHz, APB1 /4, APB2 /2, flash latency 5) is in `src/clock.c`.

## How it works

1. The **sensor task** reads the IMU and light/proximity sensor every 20 ms (50 Hz) and sends samples into a queue.
2. The **inference task** keeps a 2-second window (100 samples), computes 28 features (mean, standard deviation, min and max of the six IMU axes, accelerometer magnitude statistics, proximity maximum, mean light) and runs a decision tree once per second.
3. The **reminder task** tracks the time since the last "drinking" prediction. After 30 seconds without drinking it beeps, then repeats every 10 seconds until you drink.
4. The **LCD task** shows the detected state, the time since the last drink, and live sensor readings.

### FreeRTOS tasks

| Task | Priority | Stack (words) | Job |
|---|---|---|---|
| infer | 4 | 512 | Features and decision tree, once per second |
| sensors | 3 | 512 | IMU, light and humidity reads, queue producer |
| buzzer | 2 | 384 | Beeps when notified |
| buttons | 2 | 384 | Polls the four buttons (UP beeps) |
| reminder | 2 | 384 | Reminder logic |
| lcd | 1 | 512 | Display updates |
| led | 1 | 192 | Heartbeat LEDs |
| monitor | 1 | 384 | Prints RTC time, free heap and stack headroom every 10 s |

Shared-data protection: two queues (samples, results), a mutex for the UART, a mutex for the shared snapshot used by the LCD, and a task notification to the buzzer.

## Results

### Model (offline, `ml/train.py`)
- Data: 24 recordings of 20 s each (6 per state, `data/`), cut into 2-second windows.
- Split by recording, so windows from one recording never appear in both training and test sets. Training uses sessions 1, 2, 4 and 6 (40 windows per class); testing uses sessions 3 and 5 (20 windows per class).
- Decision tree, depth 5. **Held-out accuracy 95%** (baseline that always guesses the most common class: 25%).

![Confusion matrix](ml/confusion_matrix.png)

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
- Flash: 22,096 B (2.1%). RAM: 51,660 B (39.4%), including the 48 KB FreeRTOS heap. Minimum free heap during the run: 33,744 B.
- 15-minute run (`tools/soak.py`, `logs/soak_run.txt`): 901 predictions, longest gap between predictions 1000 ms, 0 queue drops, 0 IMU errors, 0 reboots, 0 serial drops.

## Limitations

- Trained and tested by one person with one board, so it may be less accurate for other people.
- Several tree rules use the x-axis, so the result depends on how the bottle is held.
- Carried and shaken are sometimes mistaken for drinking, so a drink can be reported by mistake and reset the reminder timer.
- The live trials were run on the earlier 16 MHz build and not repeated at 168 MHz.

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
3. Train: `python ml/train.py` (needs numpy, pandas, scikit-learn, matplotlib). It prints the accuracy table and writes `include/model.h` and `ml/confusion_matrix.png`.
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

## Troubleshooting

| Problem | Fix |
|---|---|
| `LIBUSB_ERROR_NOT_FOUND` on upload | Install the ST-LINK driver (STSW-LINK009) on Windows. |
| `Failed to read memory` or `timed out while waiting for target halted` | The `upload_flags` in `platformio.ini` (connect under reset) fix this. Keep them. |
| Python tool says `Access is denied` on COM3 | Close the PlatformIO serial monitor first. Only one program can use the port. |
| Link error about floating-point registers | Build with `-mfloat-abi=softfp` as in `platformio.ini`. |
| FreeRTOS builds the wrong port | `lib/FreeRTOS/library.json` selects only the Cortex-M4F port and `heap_4.c`. |
| Serial text is garbled | The clock is wrong. Check `src/clock.c` and `-DHSE_VALUE=8000000U` in `platformio.ini`. |

## License and attribution

- Project code: see `LICENSE`.
- FreeRTOS kernel V11.1.0 (`lib/FreeRTOS`): MIT license, Amazon Web Services.
- STM32Cube HAL and CMSIS: STMicroelectronics, used through the PlatformIO `stm32cube` framework.
- LCD pin mapping and ST7789 start-up sequence: taken from the RT-Thread RT-Spark BSP (`drv_lcd.c`, Apache-2.0).
- The 5×7 bitmap font table is a standard public-domain font.
- Training uses numpy, pandas, scikit-learn and matplotlib.

## Repository layout

```
src/main.c        firmware (tasks, drivers, inference)
src/clock.c       168 MHz clock setup
boards/           RT-Spark board file for PlatformIO
include/          FreeRTOSConfig.h, model.h (generated by ml/train.py)
lib/FreeRTOS/     FreeRTOS kernel
ml/               training script and confusion matrix
tools/            collect.py, trial.py, soak.py
data/             recordings and trial results
logs/             15-minute run log
```

## Team

- Cassandra Khan P. Mendoza: firmware, data, training
- Sean Marie G. Rabago: lab report Introduction, product concept, block diagram, and task 1
- _: lab report
- _: demo video
