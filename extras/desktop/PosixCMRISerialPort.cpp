// PosixCMRISerialPort.cpp — POSIX termios byte port for the desktop
// Host harness.

#include "PosixCMRISerialPort.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#ifdef __APPLE__
// IOSSIOSPEED: set an arbitrary baud rate. 28800 — the fielded cpNode
// rate — is not a POSIX Bxxxx constant, so cfsetspeed() alone cannot
// select it on macOS.
#include <IOKit/serial/ioss.h>
#endif

namespace CMRInet {

namespace {

/// Standard termios constant for `baud`, or 0 when the rate has no
/// constant (e.g. the fielded 28800) and needs a platform ioctl.
speed_t standardSpeed(uint32_t baud) {
  switch (baud) {
    case 1200: return B1200;
    case 2400: return B2400;
    case 4800: return B4800;
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    default: return 0;
  }
}

}  // namespace

PosixCMRISerialPort::PosixCMRISerialPort(const char* device, uint32_t baud,
                                         bool stopBits2)
    : device_(device), baud_(baud), stopBits2_(stopBits2) {}

void PosixCMRISerialPort::begin() {
  if (fd_ >= 0) {
    return;  // idempotent
  }
  lastError_ = "";

  fd_ = ::open(device_, O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd_ < 0) {
    lastError_ = "open() failed";
    return;
  }

  struct termios tio;
  if (tcgetattr(fd_, &tio) != 0) {
    lastError_ = "tcgetattr() failed";
    close();
    return;
  }

  // Fully raw. cfmakeraw() clears the cooked-mode processing; the
  // explicit IXON/IXOFF/IXANY clear is belt-and-braces against any
  // platform that leaves flow control outside cfmakeraw's remit.
  // VALIDATION: review-CMRI-Controller-host.md Finding 7: no software
  // flow control, ever — a raw 0x13 in an R body must be data.
  cfmakeraw(&tio);
  tio.c_iflag &= ~static_cast<tcflag_t>(IXON | IXOFF | IXANY);
  tio.c_cflag |= static_cast<tcflag_t>(CLOCAL | CREAD | CS8);
  tio.c_cflag &= ~static_cast<tcflag_t>(PARENB | CRTSCTS);
  if (stopBits2_) {
    tio.c_cflag |= static_cast<tcflag_t>(CSTOPB);  // 8N2
  } else {
    tio.c_cflag &= ~static_cast<tcflag_t>(CSTOPB);  // 8N1
  }
  // Non-blocking reads: return immediately with whatever is buffered.
  tio.c_cc[VMIN] = 0;
  tio.c_cc[VTIME] = 0;

  // Standard rates go through termios; nonstandard rates (28800, the
  // fielded cpNode rate) are set by platform ioctl after tcsetattr.
  const speed_t standard = standardSpeed(baud_);
  (void)cfsetspeed(&tio, (standard != 0) ? standard : B9600);

  if (tcsetattr(fd_, TCSANOW, &tio) != 0) {
    lastError_ = "tcsetattr() failed";
    close();
    return;
  }

  if (standard == 0) {
#ifdef __APPLE__
    speed_t speed = baud_;
    if (ioctl(fd_, IOSSIOSPEED, &speed) != 0) {
      lastError_ = "IOSSIOSPEED failed (baud not supported?)";
      close();
      return;
    }
#else
    // Linux: a nonstandard rate needs termios2/BOTHER. Add it when a
    // Linux bench exists; refuse rather than run at the wrong rate.
    lastError_ = "nonstandard baud unsupported on this platform";
    close();
    return;
#endif
  }

  // Start from a clean slate: drop stale bytes buffered by the OS.
  tcflush(fd_, TCIOFLUSH);
}

int PosixCMRISerialPort::readByte() {
  if (fd_ < 0) {
    return -1;
  }
  uint8_t byte = 0;
  const ssize_t n = ::read(fd_, &byte, 1);
  return (n == 1) ? byte : -1;
}

size_t PosixCMRISerialPort::writeBytes(const uint8_t* bytes, size_t length) {
  if (fd_ < 0 || bytes == nullptr || length == 0) {
    return 0;
  }
  const ssize_t n = ::write(fd_, bytes, length);
  if (n < 0) {
    // EAGAIN — kernel TX buffer full. The transport retries the
    // remainder on a later tick.
    return 0;
  }
  return static_cast<size_t>(n);
}

bool PosixCMRISerialPort::transmitDrained() const {
  if (fd_ < 0) {
    return true;
  }
  // Kernel output-queue occupancy. This answers for the OS buffer and
  // the USB pipeline only; the transport's wire-time estimate covers
  // the adapter's shift register.
  int pending = 0;
  if (ioctl(fd_, TIOCOUTQ, &pending) != 0) {
    return true;  // no visibility: defer to the wire-time estimate
  }
  return pending == 0;
}

void PosixCMRISerialPort::setTransmitEnable(bool enabled) {
  (void)enabled;  // auto-direction / permanently-driven adapter
}

uint32_t PosixCMRISerialPort::byteDurationMicros() const {
  // 8N2 = start + 8 data + 2 stop = 11 bit times; 8N1 = 10.
  const uint32_t bits = stopBits2_ ? 11u : 10u;
  const uint32_t baud = (baud_ == 0) ? 1u : baud_;
  const uint32_t micros = (bits * 1000000u) / baud;
  return (micros == 0) ? 1u : micros;
}

void PosixCMRISerialPort::close() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

}  // namespace CMRInet
