# Manual FreeRTOS Setup on Raspberry Pi Pico 2 W (VS Code + Pico Extension)

This document records what was wrong with a hand-assembled FreeRTOS + Pico 2 W (`pico2_w`,
RP2350) project in the Raspberry Pi Pico VS Code extension, the reasoning that led to each fix,
and the extension's own template-based path that avoids all of it. It also documents the two
low-level debugging techniques (reading CPU fault registers, inspecting the exception vector
table) used to find the root cause, as a reusable reference for future firmware bring-up issues.

Project used throughout: `blink_freertos`, board `pico2_w`, Pico SDK 2.3.0, FreeRTOS-Kernel
(RP2350_ARM_NTZ SMP port), toolchain 15_2_Rel1, Raspberry Pi Pico VS Code extension 0.21.0.

---

## 1. Does the extension offer a built-in FreeRTOS template? — No, verified directly

This section was originally written assuming the extension's **"New Project From Example"**
wizard could generate a working FreeRTOS project. That claim was **wrong** and has been corrected
here after checking the extension's actual example-list source rather than its documentation.

### 1.1 What's actually in the wizard's dropdown

The "New Project From Example" panel does **not** read the live `pico-examples` GitHub repository
or its `README.md` catalog (which does list FreeRTOS examples — that's the file the original,
incorrect version of this section was based on). It reads a curated, versioned manifest bundled
inside the extension itself: `data/<version>/examples.json`. Checking every version bundled with
extension 0.21.0 (`0.10.0` through `0.18.0`) directly:

```
$ python3 -c "import json,glob
for f in sorted(glob.glob('data/*/examples.json')):
    d = json.load(open(f))
    print(f, [k for k in d if 'freertos' in k.lower()])"
data/0.10.0/examples.json []
data/0.15.0/examples.json []
data/0.16.0/examples.json []
data/0.17.0/examples.json []
data/0.18.0/examples.json []
```

Zero FreeRTOS entries in any version — confirming what you observed directly in the dropdown.
The general **"New Pico Project"** wizard doesn't offer one either: its generator script
(`scripts/pico_project.py`) has a `picow_freertos` entry present in source but explicitly
commented out, so it's dead code in this version, not a hidden option.

**Conclusion: as of extension 0.21.0, there is no wizard-driven way to generate a FreeRTOS
project.** Manual setup (or manually pulling an upstream example, below) is the only path.

### 1.2 The upstream examples do exist — just not reachable from the wizard

The `freertos/hello_freertos` example genuinely exists in the
[`raspberrypi/pico-examples`](https://github.com/raspberrypi/pico-examples) repository (verified
by fetching it directly, not inferred from local docs). Pulling it manually is still the fastest
way to get a **correct, tested** `FreeRTOSConfig.h` for RP2350 rather than reverse-engineering one
by hand as this document had to:

```
git clone --depth 1 https://github.com/raspberrypi/pico-examples.git
```

The relevant files are under `freertos/`:

- `freertos/hello_freertos/` — the example project itself (one_core / two_cores /
  static_allocation variants, selected via CMake options inside its `CMakeLists.txt`)
- `freertos/FreeRTOSConfig.h` — a thin wrapper that just does
  `#include "FreeRTOSConfig_examples_common.h"`
- `freertos/FreeRTOSConfig_examples_common.h` — the real, tested configuration, shared across all
  FreeRTOS examples, RP2040 and RP2350 alike. Fetched and confirmed directly, it contains
  (gated behind `#if PICO_RP2350`):

  ```c
  #define configRUN_FREERTOS_SECURE_ONLY          1
  #define configENABLE_FPU                        1
  #define configENABLE_MPU                        0
  #define configENABLE_TRUSTZONE                  0
  #define configMAX_SYSCALL_INTERRUPT_PRIORITY    16
  #define configASSERT(x)                         assert(x)
  ```

  This **exactly matches** the fix independently derived via fault-register/vector-table
  debugging in §5 below — useful confirmation that the root cause and fix were correct, from an
  authoritative source. Note also what's *absent*: no `configKERNEL_INTERRUPT_PRIORITY` and no
  `configPRIO_BITS` — consistent with §5's finding that this port never references either.

### 1.3 Using a manually-pulled example in this extension

1. Clone `pico-examples` (above) somewhere outside your project directory.
2. Copy `freertos/hello_freertos/`, `freertos/FreeRTOSConfig.h`,
   `freertos/FreeRTOSConfig_examples_common.h`, and `freertos/FreeRTOS_Kernel_import.cmake` into
   your own project.
3. Open the copied project folder in VS Code and run **Raspberry Pi Pico: Import Pico Project**
   (Command Palette) to have the extension generate/regenerate the `.vscode/*` scaffolding
   (`launch.json`, `tasks.json`, `c_cpp_properties.json`, `cmake-kits.json`) for your chosen board
   and SDK version — the same scaffolding the wizard produces for a plain project.
4. Confirm `FREERTOS_KERNEL_PATH` resolves as described in §3.1 (it will, automatically, if
   `~/.pico-sdk/FreeRTOS-Kernel` already exists from prior use of this extension).
5. Build and flash as normal (**Compile Project**, then **Run Project**).

This is the closest thing to a "wizard path" that actually exists today — extension-assisted
scaffolding, but a manually-sourced, upstream-tested `FreeRTOSConfig.h` rather than a
wizard-generated one.

---

## 2. What was wrong with the manual setup

The project directory already contained `.vscode/*` scaffolding generated by the plain **New Pico
Project** wizard (for board `pico2_w`, no FreeRTOS), onto which `FreeRTOSConfig.h`,
`FreeRTOS_Kernel_import.cmake`, and a stock `main.c` were copied in by hand afterward. That manual
splice — instead of §1's example-project path — is the root of every issue that follows.

### 2.1 `FREERTOS_KERNEL_PATH` could not be resolved (configure failed)

`FreeRTOS_Kernel_import.cmake`'s auto-detection logic checks
`${PICO_SDK_PATH}/../FreeRTOS-Kernel`. The extension actually installs the kernel one directory
further up the tree — at `~/.pico-sdk/FreeRTOS-Kernel`, a sibling of `sdk/`, not a sibling of
`sdk/2.3.0/`. `cmake` failed immediately with:

```
CMake Error at FreeRTOS_Kernel_import.cmake:56 (message):
  FreeRTOS location was not specified.  Please set FREERTOS_KERNEL_PATH.
```

**How this was found**: reproduced the configure step directly with the real toolchain
(`cmake -G Ninja -S ... -B ...`) rather than trusting the IDE's cached state, which surfaced the
exact CMake error instead of a vague "it doesn't build."

**Fix** — added a fallback in `CMakeLists.txt`, placed after the `picoVscode` include block so
`USERHOME` is already resolved:

```cmake
# Location of the FreeRTOS kernel managed by the Pico VS Code extension
if (NOT DEFINED FREERTOS_KERNEL_PATH AND NOT DEFINED ENV{FREERTOS_KERNEL_PATH})
    set(FREERTOS_KERNEL_PATH ${USERHOME}/.pico-sdk/FreeRTOS-Kernel)
endif()
```

### 2.2 `FreeRTOSConfig.h` was missing macros the RP2350 port requires

Once configure succeeded, compilation failed with explicit `#error` directives from the port
headers (`portmacrocommon.h`, `FreeRTOS.h`):

```
#error Missing definition: One of configUSE_16_BIT_TICKS and configTICK_TYPE_WIDTH_IN_BITS ...
#error configENABLE_FPU must be defined in FreeRTOSConfig.h ...
#error configENABLE_MPU must be defined in FreeRTOSConfig.h ...
#error configENABLE_TRUSTZONE must be defined in FreeRTOSConfig.h ...
```

The original `FreeRTOSConfig.h` was written for the older RP2040 port (Cortex-M0+), which has no
MPU/TrustZone/FPU to configure and therefore never needed these macros. RP2350 uses Cortex-M33 and
needs all of them defined explicitly.

**Fix** — added:

```c
#define configTICK_TYPE_WIDTH_IN_BITS  TICK_TYPE_WIDTH_32_BITS
#define configENABLE_MPU               0
#define configENABLE_TRUSTZONE         0
#define configENABLE_FPU               1   /* see §4 for why this must be 1 here */
```

A further compile error (`implicit declaration of function 'xTimerPendFunctionCallFromISR'`) —
triggered from inside `event_groups.h`, called by the RP2350 port's cross-core doorbell IRQ
handler — required also adding:

```c
#define INCLUDE_xTimerPendFunctionCall  1
```

### 2.3 `configASSERT` referenced a macro not yet defined at its call site

```c
#define configASSERT(x) if((x)==0) { taskDISABLE_INTERRUPTS(); for(;;); }
```

`taskDISABLE_INTERRUPTS()` is defined in `task.h`. But the RP2350 port's `portmacro.h` invokes
`configASSERT()` from an inline function that is compiled **before** `task.h` has been included in
that translation unit — an ordering quirk specific to this port. The build failed with
`implicit declaration of function 'taskDISABLE_INTERRUPTS'`.

**Fix** — switched to `portDISABLE_INTERRUPTS()`, defined earlier in `portmacro.h` itself:

```c
#define configASSERT(x) if((x)==0) { portDISABLE_INTERRUPTS(); for(;;); }
```

### 2.4 Missing FreeRTOS application hook functions (link errors)

With `configSUPPORT_STATIC_ALLOCATION`, `configUSE_MALLOC_FAILED_HOOK`, and
`configCHECK_FOR_STACK_OVERFLOW` all enabled, the linker demanded four functions the stock
`main.c` never provided:

```
undefined reference to `vApplicationMallocFailedHook'
undefined reference to `vApplicationGetIdleTaskMemory'
undefined reference to `vApplicationStackOverflowHook'
undefined reference to `vApplicationGetTimerTaskMemory'
```

**Fix** — implemented directly from the linker's undefined-symbol list; standard, mechanical
boilerplate (see the final `main.c` in the project for the implementations).

At this point the project **built and flashed successfully** — but the LED did not blink. That
began a longer investigation, split into environmental noise that blocked getting any diagnostic
signal, and then a genuine firmware bug underneath.

### 2.5 Environmental noise (blocked diagnosis, not a code bug)

These cost the most wall-clock time even though none of them were firmware bugs — worth listing
so they're recognized immediately next time:

- **Charge-only USB cable.** `picotool` reported `No accessible RP-series devices in BOOTSEL mode
  were found`; `lsusb` and `ls /dev/sd*` showed nothing at all when the board was plugged in.
  Swapping to a data-capable cable fixed BOOTSEL enumeration immediately.
- **Missing `dialout` group membership.** The serial monitor failed with `Failed to open the
  serial port /dev/ttyACM0`, even though the device node existed
  (`crw-rw---- root dialout /dev/ttyACM0`) — the user's account just wasn't a member of the
  `dialout` group that Debian/Ubuntu gates serial device access behind. Fixed with:
  ```
  sudo usermod -aG dialout $USER
  ```
  followed by a full logout/login (group membership is cached per login session, so a mid-session
  `newgrp` in one terminal isn't enough for the VS Code serial monitor extension to pick it up).
- **Debug Probe vs. the board's own native USB.** `/dev/ttyACM0` turned out to belong to the
  Raspberry Pi Debug Probe's UART bridge (its own USB-to-serial chip, wired to the board's UART0
  TX/RX pins) — not the Pico's native USB CDC that `pico_enable_stdio_usb` enables. `lsusb` only
  ever showed one `2e8a:000c Raspberry Pi Debug Probe` entry, never a second device for the
  board's own USB port, because that port's cable was providing power only in one round of
  testing. This mattered because an early diagnostic attempt (enabling USB-CDC stdio) was
  initially pointed at a port nobody was listening on — UART (already enabled by default) was the
  channel that actually worked once the Debug Probe's own UART wiring was accounted for.

### 2.6 Isolating the real bug: LED silent, no crash message, no clear cause

Rather than keep patching `main.c` on hunches, each round removed exactly one variable and
re-measured, narrowing the search space:

1. **Hypothesis: `cyw43_arch_init()` overflows `blink_task`'s tiny stack** (it was called inside a
   task created with only `configMINIMAL_STACK_SIZE` = 256 words = 1KB). Moved the call to
   `main()`, which has a much larger boot-time stack — no change in behavior. This hypothesis was
   later disproven by reading the SDK source directly: `cyw43_arch_init()` spins up its own
   dedicated ~4KB-stack FreeRTOS worker task internally (`async_context_freertos_init()`), so
   where you call it from was never actually the constraint.

2. **Bisection: remove all CYW43/LED code, keep only a `printf` heartbeat inside `blink_task`.**
   Still completely silent. This ruled out CYW43 as the cause of "nothing happens at all" — the
   bug was more fundamental than the LED driver.

3. **Bisection: a bare-metal `printf` loop directly in `main()`, before `vTaskStartScheduler()`
   even runs.** This printed fine, 20/20 heartbeats every time. This proved UART wiring,
   permissions, and cabling were **not** the remaining issue — the failure was specifically
   post-scheduler-start.

4. **Re-ran the full `blink_task` build with the heartbeat loop still in `main()`.** The 20
   heartbeats printed, then printed **again from 0** — i.e. the board was **crash-resetting in a
   loop**, not hanging. This ruled out a simple deadlock and pointed squarely at an unhandled CPU
   fault happening somewhere around scheduler startup.

5. **Attached the Debug Probe as an actual debugger** (printf-only diagnostics had reached their
   ceiling) and caught the reset live: execution landed in `isr_hardfault`, a placeholder
   breakpoint-trap stub in the SDK's `crt0.S`. See §5 for exactly how the fault was decoded from
   here — this is where CPU fault-register inspection replaced guesswork.

---

## 3. How to read CPU fault registers directly with a debug probe

This is the general technique that broke the "guess and recompile" cycle. It applies to **any**
unexplained crash/reset on a Cortex-M target, not just this bug.

### 3.1 Prerequisites

- A debug probe wired via SWD to the target (here: a Raspberry Pi Debug Probe, `cmsis-dap.cfg`).
- The project's `.vscode/launch.json` already has a working `"Pico Debug (Cortex-Debug)"`
  configuration (generated by the Pico extension's wizard) — no extra setup needed.
- `configASSERT` should not be a silent infinite loop if you can help it (see §2.3's final form) —
  ours prints the failing file/line before halting, which sometimes makes the debugger step
  unnecessary. But for a *hardware* fault (as opposed to a FreeRTOS-level `configASSERT`), no
  amount of `printf` will fire, because the CPU is faulting before your code's fault handler ever
  gets meaningful control — hence the need for direct register inspection.

### 3.2 Catching the fault

1. Set a breakpoint at the suspect call (here, `vTaskStartScheduler();` in `main.c`).
2. Start the debug session: Run and Debug panel (`Ctrl+Shift+D`) → **Pico Debug (Cortex-Debug)** →
   F5.
3. When the breakpoint hits, press **Continue (F5)** and let it run past.
4. If it crash-resets, the debugger will typically catch the fault handler as a breakpoint trap
   (the SDK's default unhandled-exception stubs in `crt0.S` are literally a `bkpt` instruction —
   `decl_isr_bkpt`) and report something like:
   ```
   Thread 1 "rp2350.cm0" received signal SIGTRAP, Trace/breakpoint trap.
   isr_hardfault () at .../pico_crt0/crt0.S:349
   349    decl_isr_bkpt isr_hardfault
   warning: Could not fetch required XPSR content.  Further unwinding is impossible.
   ```
   Landing in `isr_hardfault` confirms a genuine unhandled HardFault — not a FreeRTOS-level
   assertion, and not a benign hang.

### 3.3 Reading CFSR and HFSR

**Important**: in the cortex-debug Debug Console, type raw GDB commands with **no** `-exec`
prefix (that prefix is a cpptools/GDB convention, not cortex-debug's).

Two memory-mapped System Control Block registers decode the fault precisely:

| Register | Address | Meaning |
|---|---|---|
| **CFSR** (Configurable Fault Status Register) | `0xE000ED28` | Combines MemManage (bits 7:0), Bus (bits 15:8), and Usage (bits 31:16) fault status |
| **HFSR** (HardFault Status Register) | `0xE000ED2C` | Why a *HardFault specifically* occurred (as opposed to a lower-tier fault being handled directly) |

```
x/1xw 0xE000ED28
x/1xw 0xE000ED2C
```

In this investigation these read `CFSR = 0x00040000` and `HFSR = 0x40000000`. Decoding:

- **CFSR bit 18 set** (`0x00040000` = `1 << 18`) → within the UsageFault Status byte (bits
  31:16, i.e. UFSR), bit 2 of that byte is **INVPC**: *invalid PC load UsageFault* — the CPU
  attempted an exception return (or `bx`/`ldr pc` load) with an `EXC_RETURN`/PC value it can't
  accept. This is the single most useful clue: it means an exception-return sequence used a bad
  return-address encoding, not (say) a null-pointer dereference or bad array index.
- **HFSR bit 30 set** (`0x40000000`) → **FORCED**: a fault that would normally be handled by its
  own dedicated handler (here, UsageFault) was escalated to HardFault because that lower-tier
  handler isn't separately enabled. This is a downstream *consequence* of the UsageFault above,
  not an independent problem — always check CFSR first, HFSR second.

Relevant UFSR bits (add 16 to get the CFSR bit number), for reference next time:

| UFSR bit (+16 for CFSR) | Name | Meaning |
|---|---|---|
| 0 (16) | UNDEFINSTR | Undefined instruction executed |
| 1 (17) | INVSTATE | Invalid EPSR/Thumb-state on branch |
| 2 (18) | **INVPC** | Invalid PC/`EXC_RETURN` on exception return |
| 3 (19) | NOCP | Coprocessor (FPU) access attempted while disabled |
| 8 (24) | UNALIGNED | Unaligned access (if trapping enabled) |
| 9 (25) | DIVBYZERO | Integer divide by zero (if trapping enabled) |

### 3.4 Reading the actual faulting instruction address

`$lr` at the point the debugger stops inside `isr_hardfault` is **HardFault's own** return
address — not the original faulting code's location. To find where the fault actually happened,
dump the exception stack frame the CPU automatically pushed on entry:

```
p/x $sp
x/8xw $sp
```

This prints 8 words in order: **R0, R1, R2, R3, R12, LR, PC, xPSR** — the 7th word (byte offset
24, i.e. the value at `$sp + 0x18`) is the address of the instruction that was executing (or about
to execute) when the fault fired. Cross-reference that address against your `.elf`'s disassembly
(`info line *0x<address>` in GDB, or `arm-none-eabi-addr2line -e blink_freertos.elf 0x<address>`
from a shell) to identify the exact source line.

In this investigation, the frame values were inconsistent with a normal 8-word layout (R3 held
what looked like a stale `EXC_RETURN` pattern), which was itself a clue that the corruption was in
the *exception-return mechanism* rather than in ordinary application code — consistent with the
INVPC diagnosis above, and ultimately traced to the wrong `EXC_RETURN` constant being baked into
every new task's initial stack frame by `pxPortInitialiseStack()` (root cause: §4).

---

## 4. How to inspect the exception vector table (VTOR + SVCall/PendSV/SysTick)

Before concluding the fault was in FreeRTOS's own context-switch sequence, it was necessary to
rule out a simpler explanation: that FreeRTOS's `SVC_Handler`/`PendSV_Handler`/`SysTick_Handler`
were never actually installed into the live vector table at all (a real possibility on Pico SDK
targets, which relocate to a RAM-based, runtime-writable vector table rather than a fixed
flash-based one).

### 4.1 Background: exception numbers and their vector-table index

The Pico SDK's `hardware/exception.h` documents the ARM Cortex-M exception numbers used as vector
table indices (this numbering is architectural, not SDK-specific):

| Exception | Vector table index |
|---|---|
| HardFault | 3 |
| **SVCall** | **11** |
| **PendSV** | **14** |
| **SysTick** | **15** |

### 4.2 Step-by-step

1. Read **VTOR** (Vector Table Offset Register) — this is the base address the CPU actually
   fetches vectors from:
   ```
   p/x *(uint32_t*)0xE000ED08
   ```
   In this project this read `0x20000000` — SRAM, confirming the SDK's RAM-relocated vector
   table (`PICO_NO_RAM_VECTOR_TABLE` was `0`, the default) was active, as expected.

2. Get the addresses of both the "real" handlers you expect FreeRTOS to have installed, and the
   SDK's default placeholder stubs, by name — GDB resolves these directly from debug symbols:
   ```
   p SVC_Handler
   p PendSV_Handler
   p SysTick_Handler
   p isr_svcall
   p isr_pendsv
   p isr_systick
   ```
   (`SVC_Handler`/`PendSV_Handler`/`SysTick_Handler` are FreeRTOS's real port handlers;
   `isr_svcall`/`isr_pendsv`/`isr_systick` are the Pico SDK's default `crt0.S` stubs, each just a
   single `bkpt` instruction meant to trap in a debugger if nothing overrides them.)

3. Dereference the live vector table at the SVCall/PendSV/SysTick indices (11, 14, 15), each slot
   being one 32-bit word, i.e. at `VTOR + index*4`:
   ```
   p/x *(uint32_t*)(0x20000000 + 11*4)
   p/x *(uint32_t*)(0x20000000 + 14*4)
   p/x *(uint32_t*)(0x20000000 + 15*4)
   ```
   Compare the results against the addresses from step 2. Two outcomes:
   - **Values match the default stubs** (`isr_svcall`/`isr_pendsv`/`isr_systick`, plus the Thumb
     bit) → the real handlers were never installed; check `configUSE_DYNAMIC_EXCEPTION_HANDLERS`
     and whether `exception_set_exclusive_handler()` is actually being called (e.g. set a
     breakpoint right after that call site and re-read the table).
   - **Values match the real handlers** (`SVC_Handler`/`PendSV_Handler`/`SysTick_Handler`, plus
     the Thumb bit) → installation is correct; the fault is elsewhere (as it turned out to be
     here — see §5).

   Note the Thumb bit: function-pointer values for Thumb code always have bit 0 set (e.g.
   `SVC_Handler` at `0x10006cb0` appears in the vector table as `0x10006cb1`). Don't be thrown by
   the off-by-one-looking difference between a symbol's plain address and its vector-table value.

### 4.3 What this ruled in/out here

In this investigation, breaking at `vStartFirstTask` (the FreeRTOS port function that issues the
actual `SVC` instruction to start the first task — set a breakpoint on it by name, same as any
other symbol: `break vStartFirstTask`) and re-running the three vector-table reads showed the
**real** FreeRTOS handler addresses correctly installed, ruling out an installation-order or
`configUSE_DYNAMIC_EXCEPTION_HANDLERS` problem entirely. That meant the fault had to be happening
*inside* the context-switch mechanics themselves, which redirected the investigation toward the
`EXC_RETURN` value baked into each task's initial stack — and from there, to the actual root
cause below.

---

## 5. Root cause: `configRUN_FREERTOS_SECURE_ONLY` was never set

Tracing the corrupted `EXC_RETURN` value back through the port source (`port.c`,
`pxPortInitialiseStack()`) led to a compile-time constant, `portINITIAL_EXC_RETURN`, baked into
every new task's initial stack frame — the exact value the CPU uses to return into that task for
the first time:

```c
#if ( configRUN_FREERTOS_SECURE_ONLY == 1 )
    #define portINITIAL_EXC_RETURN    ( 0xfffffffd )   /* correct for this NTZ (No TrustZone) port */
#else
    #define portINITIAL_EXC_RETURN    ( 0xffffffbc )   /* TrustZone-partitioned variant — wrong here */
#endif
```

`configRUN_FREERTOS_SECURE_ONLY` was never defined anywhere in this project's
`FreeRTOSConfig.h`. Left undefined, the C preprocessor treats it as `0` inside `#if`, so the build
silently selected the TrustZone-partitioned `EXC_RETURN` constant — wrong for the
**RP2350_ARM_NTZ** ("No TrustZone") port, which runs entirely in a single, non-partitioned state
and needs the plain constant instead. That single wrong bit pattern corrupted the very first
context switch, producing the INVPC HardFault chased through §3–§4.

This exact requirement is documented in the port's own `README.md`
(`FreeRTOS-Kernel/portable/ThirdParty/GCC/RP2350_ARM_NTZ/README.md`, section "FreeRTOS
configuration for Armv8-M"), which lists three macros as required and one more as strongly
recommended:

```c
#define configENABLE_MPU                        0
#define configENABLE_TRUSTZONE                  0
#define configRUN_FREERTOS_SECURE_ONLY          1

/* set this if using floating point (this build does, via -march=...+fp -mfloat-abi=softfp) */
#define configENABLE_FPU                        1

/* the only value of configMAX_SYSCALL_INTERRUPT_PRIORITY tested by this port */
#define configMAX_SYSCALL_INTERRUPT_PRIORITY    16
```

Two things worth noting about this:

- **`configENABLE_FPU`** was fixed earlier in the investigation (§2.2) for an unrelated but
  related-looking reason (hardware FPU instructions were enabled at the compiler level via
  `-march=armv8-m.main+fp -mfloat-abi=softfp`, but the port config said `0`). That fix was
  correct and necessary, but insufficient alone — it did stop the *crash-reset loop* (the board
  stopped resetting), but the scheduler still silently hung rather than actually running any
  task, because `configRUN_FREERTOS_SECURE_ONLY` was still missing.
- **`configMAX_SYSCALL_INTERRUPT_PRIORITY = 16`** replaced an earlier hand-derived value (`64`,
  computed via a generic Cortex-M "shift by implemented priority bits" formula:
  `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS)`). That formula-derived
  value happened to pass this port's internal sanity `configASSERT`s, which is why it didn't
  surface as a separate bug — but it wasn't the port's documented/tested value, so it was
  replaced with the literal `16` once the README was found. `configKERNEL_INTERRUPT_PRIORITY`
  and `configPRIO_BITS`, which the earlier formula also introduced, turned out to be **not
  referenced anywhere in this port's source at all** (confirmed by grepping `port.c`/
  `portmacro.h`) and were removed as dead configuration.

### Final fix

```c
#define configENABLE_MPU                        0
#define configENABLE_TRUSTZONE                  0
#define configRUN_FREERTOS_SECURE_ONLY          1
#define configENABLE_FPU                        1
#define configMAX_SYSCALL_INTERRUPT_PRIORITY    16
```

With this in place, `blink_task` ran continuously (confirmed via serial output incrementing
indefinitely, no further resets), and re-enabling the CYW43 LED code produced a correctly
blinking LED.

---

## 6. General debugging logic, summarized

Each step traded a guess for a verified fact before moving on:

1. **Reproduce locally** with the real toolchain rather than trusting the IDE's cached state —
   this is what surfaced the exact `FREERTOS_KERNEL_PATH` and missing-macro compile errors
   immediately instead of a vague "it doesn't build."
2. **Bisect by removing subsystems** (CYW43, then FreeRTOS's scheduler itself via a bare-metal
   `printf` loop) to localize which layer owned the bug before touching any FreeRTOS-internal
   configuration.
3. **Escalate to hardware-level tools once software-level diagnostics hit their ceiling.**
   `printf` cannot fire during a fault that corrupts the exception-return mechanism itself — at
   that point, a debug probe and direct fault-register/vector-table inspection (§3–§4) are the
   only way to get real signal, and they gave an exact, unambiguous answer (`INVPC`) instead of
   another round of hypotheses.
4. **Read the port's own documentation last, verify first.** The root cause was, in the end,
   explicitly documented in the port's `README.md` — but confirming it via CFSR/HFSR and the
   vector table meant the fix was verified against hardware ground truth, not just "the docs said
   so," which also correctly ruled out the plausible-looking `configENABLE_FPU`-only fix as
   sufficient on its own.
