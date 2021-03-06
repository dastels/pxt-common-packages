#include "pxt.h"
#include "devpins.h"

namespace pins {
    /**
     * Read `size` bytes from a 7-bit I2C `address`.
     */
    //%
    Buffer i2cReadBuffer(int address, int size, bool repeat = false)
    {
      Buffer buf = createBuffer(size);
      int ok = io->i2c.read(address << 1, (char*)buf->payload, size, repeat);
      if (!ok) {
        free(buf);
        buf = 0;
      }
      return buf;
    }

    /**
     * Write bytes to a 7-bit I2C `address`.
     */
    //%
    int i2cWriteBuffer(int address, Buffer buf, bool repeat = false)
    {
      return io->i2c.write(address << 1, (char*)buf->payload, buf->length, repeat);
    }
}