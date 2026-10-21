1. Introduction

Maintaining proper hydration throughout the day is a common challenge for students and professionals, yet most existing hydration trackers rely either on manual smartphone logging or simple fixed-interval timers that go off regardless of whether the user has actually taken a sip. Other smart bottles require constant Bluetooth pairing and cloud connectivity to process sensor data, which drains battery life, adds latency, and introduces privacy and connectivity dependencies.

This laboratory activity presents the Smart Water Bottle Reminder, an autonomous Edge AI consumer product implemented on the RT-Thread RT-Spark (STM32F407ZGT6) development board running FreeRTOS V11.1.0. By executing a lightweight, depth-5 Decision Tree classifier directly on the ARM Cortex-M4F microcontroller at 168 MHz, the system processes real-time motion and optical sensor data locally—requiring zero internet or cloud connectivity. The device continuously distinguishes between four physical states (resting, carried, drinking, and shaken) and triggers audiovisual reminders only when the user genuinely has not taken a drink within the target window.

2. Product Concept

2.1 Overview & Value Proposition

The Smart Water Bottle Reminder transforms an ordinary water bottle into a context-aware hydration assistant. Rather than nagging the user on a blind timer, the embedded Edge AI model understands how the bottle is being handled in real time:

-Context-Aware Tracking: Differentiates between a bottle sitting on a desk (resting), walking in a bag or hand (carried), being actively sipped (drinking), or being mixed/fidgeted with (shaken).

-100% On-Device Edge AI: Inference runs locally in 154 µs using only 2.1% Flash (22,096 B) and 39.4% RAM (51,660 B), proving that responsive machine learning can live entirely on a resource-constrained microcontroller.

-Smart Escalating Alerts: If 30 seconds pass without a detected drinking event, the system flashes "DRINK WATER NOW!" on the onboard 240×240 LCD and sounds the buzzer, repeating every 10 seconds until the user takes a drink.

2.2 Target User Workflow

Continuous Sensing: As the user goes about their day, the bottle samples 6-axis motion (accelerometer and gyroscope) and ambient light/proximity at 50 Hz.

Real-Time Classification: Once per second, the system evaluates the past 2 seconds of motion (100 samples) and displays the live activity state, sensor readings, and elapsed time since the last sip on the ST7789 LCD.

Hydration Alert & Reset: When the user drinks from the bottle, the classifier detects the characteristic tilt and proximity profile of the drinking state and automatically resets the reminder timer to zero.


