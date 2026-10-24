# Report addendum: sections to add to `docs/LAB_REPORT.md`

Items marked **[CONFIRM]** need the team to check or fill in before submitting. Everything else comes from our own data and code.

---

## 2.3 Alternative Ideas Considered  **[CONFIRM: edit if the team considered different ones]**

| Alternative | Why not chosen |
|---|---|
| **Fall / drop detector** for a bottle or small object | Only two useful states (still vs. dropped). A drop is hard to repeat safely and often, so we could not collect enough test data. |
| **Desk posture / sedentary reminder** (IMU + light) | The board sits on a desk, not on the body, so the sensors cannot observe posture. |
| **Smart Water Bottle Reminder (chosen)** | Four states we can repeat safely 20+ times each, uses the IMU and the proximity/light sensor directly, and the output (a reminder) is easy to observe. |

## 2.4 Component-to-Function Matrix

| Component | Interface | Category | Used for |
|---|---|---|---|
| ICM20608 IMU | Soft I2C bus A, 0x68 | **Primary sensing** | Motion for the 4-state classifier (50 Hz) |
| AP3216C (proximity + light) | Soft I2C bus A, 0x1E | **Primary sensing / context** | Proximity peak helps detect drinking; light shown on the LCD |
| AHT20/21 (temp, humidity) | Soft I2C bus B, 0x38 | Context sensing | Shown on the LCD |
| ST7789 LCD 240x240 | FSMC bank 3 | User interaction | State, timer, reminder, diagnostics screen |
| Buzzer | PB0 | User interaction | Reminder beep, button feedback |
| Status LEDs | PF11, PF12 | User interaction | Heartbeat |
| Buttons (LEFT/RIGHT/DOWN/UP) | PC0, PC4, PC1, PC5 | User interaction | LEFT: acknowledge/reset reminder. RIGHT: diagnostics on/off. DOWN: re-run checks (diagnostics screen) or start the 10 s sensor-fault test (normal screen). UP: buzzer test |
| RTC (LSE 32.768 kHz) | On-chip | Logging / communication | Timestamp in serial log, shown on the diagnostics screen |
| USART1 via ST-LINK | PA9/PA10, 115200 | Logging / communication | Inference log, health report, data collection |
| External SPI flash | SPI2: SCK PB13, MISO PC2, MOSI PC3, CS PB12 | Diagnostic / service | JEDEC ID read on the diagnostics screen: `EF4017` (Winbond W25Q64, 8 MB) |
| Wi-Fi module RW007 | SPI2, CS PF10, RST PG15, INT PG11 | Diagnostic / service | Reset + INT response check: "READY". Not used for communication (offline by design) |
| SD card slot | SDIO, card-detect PF3 | Diagnostic / service | Card-detect read: "NO CARD" (no card fitted) |
| Audio codec | Soft I2C bus A, 0x10 | Diagnostic / service | I2C address probe: "OK". Audio not used |

Notes:
- The schematic labels the flash W25Q128, but the chip on our board reports ID `EF4017` (W25Q64). We report what the hardware returned.
- No SD card is fitted. The slot itself is checked, and the diagnostics screen shows "NO CARD".

## 2.5 Acceptance Criteria and Risks

| Item | Criterion | Result |
|---|---|---|
| Behavior | Show one of 4 states about once per second; remind after 30 s without drinking; repeat every 10 s | Met (LCD + 15-minute run) |
| Accuracy | At least 90% on 80 live trials (20 per state) | 91.2% (73/80). Met, by a narrow margin |
| Cost of errors | Most costly error: a non-drinking motion read as "drinking", because it resets the timer and delays a reminder | All 7 live errors were of this kind (4 carried and 3 shaken read as drinking) |
| Speed | Inference under 1 ms | 154 µs median and P95 (50 runs) |
| Stability | No crash, hang, or dropped sample in 15 minutes | 901 predictions, 0 drops, 0 IMU errors, 0 reboots |
| Offline | Works with no network for 5 minutes or more | Demo video **[CONFIRM: link]** |
| Measurement plan | 24 labeled recordings split by session; 80 live trials; 15-minute soak; 50 timed inferences | Done |

**Privacy.** No microphone or camera is used. The device records only motion, light, and proximity numbers. Nothing leaves the device (no network). Training data are motion CSVs with no names or images, so they can be shared in the repo.

**Safety.** The product is an experimental prototype, not a medical device. It does not measure how much water is drunk and gives no health advice. A wrong reminder is a minor inconvenience, not a safety risk.

**Accessibility.** Reminders use both sound (buzzer) and sight (red "DRINK WATER NOW!" on the LCD), so a user who cannot hear or cannot see one still gets the other. Limitations: the LCD text is small, there is no vibration output, and the buzzer volume cannot be adjusted.

## 4.5 Engineering Details Required by the Lab

**Sampling interval.** All 24 recordings have a mean interval of 20.0 ms (min 19 ms, max 21 ms), so the effective rate is 50 Hz.

**Calibration and offsets.** No calibration is applied. From the 6 resting recordings the gyroscope offset is about +0.3 dps (X), +0.16 dps (Y) and -0.1 dps (Z). The resting gyro standard deviation is 0.04 to 0.13 dps, while the carried state is at least 2.75 dps. So the offset is too small to affect the features. The accelerometer reads about 1 g on whichever axis points up (0.97 g, 1.01 g, 0.96 g), matching its 8192 LSB/g scale.

**Absent or unusable hardware.** No SD card is fitted. The Wi-Fi module is present but unused, because the product is offline by design.

**Interrupt priorities and shared state.**
- FreeRTOS port: `configLIBRARY_LOWEST_INTERRUPT_PRIORITY` = 15 and `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` = 5. SysTick and PendSV run at the lowest priority (15), and the HAL tick is set to its default priority.
- No peripheral interrupts are enabled. The UART transmits by polling, the sensors use bit-banged I2C, the buttons are polled, and the LCD is memory-mapped. So no interrupt handler calls a FreeRTOS API, and there are no priority-inversion paths other than the mutexes.
- Shared data: the `snap` snapshot and the serial port are protected by mutexes. The two diagnostics flags (`diag_mode`, `diag_rerun`) are single-word flags written by the button task and read by the LCD task.

**Sampling-to-response delay (estimated, not measured with a timer).**
- A prediction is made every 1 s from the latest 2 s of data. Inference takes 154 µs. The LCD refreshes every 500 ms.
- So a new motion appears on screen about 2 s to 3.5 s after it starts, and no more than about 1.5 s after the window containing it closes.
- Button response: the buttons are polled every 20 ms and the reminder task checks for the acknowledge signal every 100 ms, so the acknowledge takes effect within about 120 ms. It beeps at the same time.

**Versions.** FreeRTOS V11.1.0; PlatformIO platform `ststm32` 20.0.0 with the `stm32cube` framework (`framework-stm32cubef4` 1.28.3, STM32 HAL); compiler package `toolchain-gccarmnoneeabi` 1.70201.0 (GNU Arm Embedded GCC 7.2.1); OpenOCD `tool-openocd` 3.1200.7. Training: Python 3.11.7, scikit-learn 1.9.1, NumPy 2.4.6, pandas 3.0.6.

## 5.6 Per-Class Precision and Recall (held-out test, 80 windows)

| Class | Precision | Recall | F1 | Support |
|---|---|---|---|---|
| Resting | 1.00 | 1.00 | 1.00 | 20 |
| Carried | 0.94 | 0.85 | 0.89 | 20 |
| Drinking | 0.86 | 0.95 | 0.90 | 20 |
| Shaken | 1.00 | 1.00 | 1.00 | 20 |

All classes are balanced (20 windows each), so class imbalance does not affect these numbers. The only errors are between carried and drinking: 3 carried windows were read as drinking, and 1 drinking window as carried. Both involve a bottle being lifted and tilted, which looks similar in motion data.

## 5.7 Baseline Comparison

| Method | Accuracy | Notes |
|---|---|---|
| Majority-class guess | 25.0% | Always predicts one class |
| Threshold rules (no learning) | 97.5% | `ml/baseline.py`. Thresholds set from the training sessions only |
| Decision tree, depth 5 (deployed) | 95.0% | Trained automatically from the same data |

The rule uses three thresholds: average gyro standard deviation below 1.0 dps for resting; above 95 dps for shaken; and proximity peak above 60 or tilt (mean az below 0.7 g) for drinking, otherwise carried.

**Honest reading.** On our small test set (80 windows), the simple rule does as well as the tree, and the 2.5-point gap is only 2 windows. Machine learning has not clearly added value on this dataset. We still deployed the tree because it is trained from data, needs no hand-picked thresholds, and gives a baseline we can retrain when more users and bottles are added. A rule tuned on only a few sessions may fail on a different person or bottle, and we have not tested that.

## 5.8 Explaining the Live Errors

In the 80 live trials, 7 of 80 were wrong: 4 carried and 3 shaken, all read as drinking. Carried and drinking share a lifted-and-tilted motion. Shaken windows that contain a tilt can also look like drinking. These errors are the harmful kind (they reset the reminder timer).

## 5.9 Controlled Failure Case: Invalid Sensor Read  **[CONFIRM: fill in the results after running the test]**

**Firmware behavior.** The sensor task rejects an IMU read if the bus read fails (after one retry), if every axis returns the same value of 0 or -1 (a stuck bus), or while the fault test is active. A rejected sample is never sent to the classifier. When the IMU goes bad, the inference window is cleared, so old data cannot be mixed with new data after recovery. The reminder timer keeps running during the fault, so a missing sensor never silences the reminder.

**Test.** Pressing DOWN on the normal screen forces every IMU read to be invalid for 10 seconds.

| Check | Expected | Observed |
|---|---|---|
| Serial log | `IMU FAULT: samples rejected`, then after 10 s `IMU recovered: inference restarts` | **[CONFIRM]** |
| LCD during the fault | State line shows red "SENSOR FAULT" and "ACC ERR" | **[CONFIRM]** |
| Scheduler still responsive | LED heartbeat keeps blinking; LEFT still beeps and resets the timer | **[CONFIRM]** |
| Reminder during the fault | Timer keeps counting; "DRINK WATER NOW!" still appears after 30 s | **[CONFIRM]** |
| Recovery | After about 2 to 3 s, new `state=` lines appear and the LCD shows a state again | **[CONFIRM]** |

## 5.10 Limitations and Improvements Supported by Testing

**Limitations**
- Small dataset: 24 recordings of 20 s, by one person. Not tested on other people or other bottles.
- Live trials ran on the earlier 16 MHz build. Offline accuracy and the rule baseline are on the same recorded data.
- Test set: 80 windows, so one wrong window changes accuracy by 1.25 points.

**Improvements**
1. **Require two "drinking" predictions in a row before resetting the timer.** Evidence: all 7 live errors were false "drinking", which delays the reminder. A single wrong prediction would no longer reset the timer. (Not yet implemented or tested.)
2. **Add more recordings from different people and bottles, and add the rule-based features to the tree.** Evidence: the rule baseline (97.5%) using gyro spread and proximity separates the classes about as well as the tree, and the only tree errors are carried versus drinking.
