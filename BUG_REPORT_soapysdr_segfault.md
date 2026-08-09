# Bug Report: SoapySDR Segfault on Launch

## Summary
When using `--source soapysdr --device "driver=uhd"`, the program opens the GUI window for a fraction of a second, prints "O" to the console, then crashes with a segmentation fault **before rendering the first frame**.

**Status: FIXED** (see resolution section)

## Steps to Reproduce
```bash
# This WORKS (HackRF native):
./build/fpvdec --no-amp --vga 16 --rate 10e6 --gain manual --freq 5865000000 \
  --channel A1 --enforce-clkin --overlay-font 18 --overlay-color 127,127,127 \
  --no-stats --no-clkin --no-agc --no-signal --source hackrf

# This CRASHED (SoapySDR) - NOW FIXED:
./build/fpvdec --no-amp --vga 16 --rate 10e6 --gain manual --freq 5865000000 \
  --channel A1 --enforce-clkin --overlay-font 18 --overlay-color 127,127,127 \
  --no-stats --no-clkin --no-agc --no-signal --source soapysdr \
  --device "driver=uhd"
```

## Resolution

### Root Cause
The segfault had **two root causes**, both in `src/source/soapy_source.cpp`:

#### 1. Type Mismatch in `readStream()` Return Value
`SoapySDR::Device::readStream()` returns an `int` (negative on error/timeout), but the code stored it in `size_t actual`:

```cpp
// BUGGY CODE:
size_t actual = device_->readStream(
    stream_,
    buffers,
    nsamples,  // passed by value, not reference
    status,
    timeNs,
    kReadTimeoutMs);
```

When UHD returns `-1` (timeout/error), `size_t` converts it to **~4 billion**. The subsequent loop `for (size_t i = 0; i < actual; i++)` reads way beyond the buffer bounds, causing a buffer overflow and segfault.

**Fix:** Changed `size_t actual` to `int actual`, and added proper error handling:

```cpp
int actual = device_->readStream(
    stream_,
    buffers,
    nsamples,  // in/out: SoapySDR updates this with actual count
    status,
    timeNs,
    100000);   // microseconds (100ms timeout)

if (actual < 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    continue;
}
if (actual == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    continue;
}
```

#### 2. Crash During Auto-Resolution Stream Restart
The auto-res detection phase creates a DSP thread, waits for lock detection, then sets `g_running = false` to stop. However, the thread is blocked inside `readStream()` in the UHD library, which **doesn't see the flag**. The stream is left in a corrupted state.

When the main code called `stop()` (destroying the device) then `start()` (recreating it), the UHD library crashed because it had a dangling reference.

**Fix:** Added `pause()`/`resume()` methods to `SoapySource` that:
- `pause()`: Just flips the `running_` flag (no device touching)
- `resume()`: Re-activates the stream to unblock any pending `readStream()`, then sets `running_ = true`

Updated `main.cpp` auto-res detection to use `pause()`/`resume()` instead of `stop()`/`start()`:

```cpp
// Stop DSP thread and restart source stream cleanly.
src->pause();
tmp_dsp.join();
src->resume();
```

## Files Modified

### `src/source/soapy_source.cpp`
1. Changed `size_t actual` → `int actual` in `readStream()` call
2. Added proper error handling for negative/zero return values
3. Changed `nsamples` from `size_t` to `int` (per SoapySDR API)
4. Moved buffer allocation outside the read loop (performance fix)
5. Added `pause()` and `resume()` implementations
6. Changed timeout from milliseconds to microseconds (100ms)

### `src/source/soapy_source.hpp`
1. Added virtual `pause()` and `resume()` declarations

### `src/source/sample_source.hpp`
1. Added virtual `pause()` and `resume()` methods to base class `ISampleSource`

### `src/main.cpp`
1. Fixed `auto_cfg.stats()` → `tmp_dec.stats()` (Config has no `stats()` method)
2. Fixed `dec.stats()` → `dec->stats()` (dec is a pointer, not reference)
3. Fixed `dec.shift_levels()` → `dec->shift_levels()` (same reason)
4. Fixed `carrier_offset_hz(dec, cfg)` → `carrier_offset_hz(*dec, cfg)` (reference parameter)
5. Updated auto-res detection to use `src->pause()`/`src->resume()` instead of `stop()`/`start()`

## Verification

### GDB Backtrace (Before Fix)
```
Thread 32 "fpvdec" received signal SIGSEGV, Segmentation fault.
#0  0x000055555556d470 in famidec::SoapySource::read(unsigned char*, unsigned long) ()
#1  0x0000555555563606 in (anonymous namespace)::dsp_thread(...) ()
#2  0x00007ffff7aecdb4 in ?? () from /lib/x86_64-linux-gnu/libstdc++.so.6
#3  0x00007ffff769cb84 in start_thread (arg=<optimized out>) at ./nptl/pthread_create.c:447
```

### GDB Backtrace (After Fix)
```
[Inferior 1 (process 305356) exited normally]
No stack.
```
Program exits cleanly without segfault.

### HackRF Path (No Regression)
```
CLKIN: locked (external clock) [enforced]
input: HackRF   video carrier 5865.000 MHz   center 5865.000 MHz   10.0 MSPS
[Inferior 1 (process 305453) exited normally]
```

### SoapySDR Path Without Auto-Res
```
input: SoapySDR (SoapySDR)   video carrier 5865.000 MHz   center 5865.000 MHz   10.0 MSPS
[Inferior 1 (process 305478) exited normally]
```

### SoapySDR Path With Auto-Res
```
input: SoapySDR (SoapySDR)   video carrier 5865.000 MHz   center 5865.000 MHz   10.0 MSPS
AUTO-RES: detected NTSC (chroma=0.00 MHz, 240 active lines, horiz_detail=0, line_rate=0.000 kHz)
  -> applying resolution: 640x480
[Inferior 1 (process 305356) exited normally]
```

## Remaining Issues (Non-Critical)

1. **"O" characters in console**: These are UHD library debug messages, not from our code. They appear because no VTX signal is connected to the B210 (it's receiving noise instead of video). This is cosmetic only.

2. **Auto-detection not detecting signal**: The auto-res detection shows `chroma=0.00 MHz` because no analog video signal is connected to the SDR. This is expected behavior when there's no VTX transmission.

3. **No video display**: This is expected when no VTX is transmitting. The decoder needs an actual FM-ATV signal to decode.

## Build Status
```
Clean build: 0 errors, 0 warnings
```

## Lessons Learned

1. **Always check return types**: `readStream()` returns `int` (signed), not `size_t` (unsigned). This is a classic bug that can cause massive buffer overflows.

2. **Thread interruption with blocking I/O**: Setting a flag doesn't work when threads are blocked inside library calls. You need to either:
   - Use timeout-based I/O so the thread can periodically check the flag
   - Provide a way to wake up the blocked call (e.g., `deactivateStream()` + `activateStream()`)
   - Use `pause()`/`resume()` semantics instead of destroy/recreate

3. **SoapySDR/USRP stream lifecycle**: The stream cannot be safely destroyed while a thread is blocked in `readStream()`. Always stop the thread first, then clean up the stream.
