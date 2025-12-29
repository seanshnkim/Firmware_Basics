## Dec 28
Build both bootloader and application project, and flash them.
The application should be flashed into the start address of 0x08010000, not 0x08000000 (default).
And then press reset button (black button - STM32F429I-DISC1)
This is the output message:

```
========================================
    BOOTLOADER v1.0                    
========================================
Running at address: 0x08000751

Bootloader running... (LED blinks 3 times)

Attempting to jump to application...
Preparing to jump to application at 0x08010000...
  App Stack Pointer: 0x20030000
  App Entry Point:   0x08011741
Jumping to application NOW!

4. UART_Init() completed
Application v1.0 started.
                         App started! Checking TIM6...
TIM6 CR1: 0x00000001
TIM6 DIER: 0x00000001
```

### Diagnosis
- HAL_Init() completes but doesn't print "1"
- SystemClock_Config() completes but doesn't print "2"
- MX_GPIO_Init() completes but doesn't print "3"
- Only after MX_USART1_UART_Init() do we see print "4"

So only 4. UART_Init() completed is printed out. Why?
-> printf() needs UART to be initialized. All those earlier printf() statements were called BEFORE UART was initialized, so they went nowhere.

But there is a bigger problem. After "4. UART_Init() completed", there should be:

```
"Application v1.0 started."
"App started! Checking TIM6..."
The TIM6 register values
```

It's not there. This means the code hangs somewhere between the UART init and those printf statements.
The application is hanging in MX_USB_HOST_Init().