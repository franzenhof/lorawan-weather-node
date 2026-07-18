#pragma once

#include <Print.h>

// Tiny ring-buffer "tee": every write is mirrored to the real hardware
// Serial (so the USB/UART monitor keeps working exactly as before) and also
// appended to a fixed-size ring buffer, so the web UI can show a rolling
// view of what this device's own code has logged. Library-internal prints
// (WiFiManager, RadioLib, ...) aren't included here — they call the real
// Serial object directly, not through this wrapper. Our own code uses
// dbgSerial instead of Serial for exactly this reason.
class SerialLog : public Print
{
public:
    size_t write(uint8_t c) override;
    size_t write(const uint8_t* buffer, size_t size) override;

    // Copies the buffered log (oldest byte first) into out as a NUL-terminated
    // string. Returns the number of bytes written, excluding the terminator.
    size_t snapshot(char* out, size_t outSize) const;

    void clear();
};

extern SerialLog dbgSerial;
