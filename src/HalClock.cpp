#include "HalClock.h"

#include <cstdio>
#include <ctime>

HalClock halClock;

namespace {
constexpr const char *kMonthNames[] = {"Jan", "Feb", "Mar", "Apr",
                                       "May", "Jun", "Jul", "Aug",
                                       "Sep", "Oct", "Nov", "Dec"};
constexpr char kFullMonthNames[][10] = {
    "January", "February", "March",     "April",   "May",      "June",
    "July",    "August",   "September", "October", "November", "December"};
} // namespace

void HalClock::begin() {
#if defined(SIMULATOR_DEVICE_X3) || defined(SIMULATOR_DEVICE_STICKY)
  _available = true;
#else
  _available = false;
#endif
}

bool HalClock::getTime(uint8_t &hour, uint8_t &minute) const {
  if (!_available)
    return false;

  const std::time_t now = std::time(nullptr);
  std::tm utcTime{};
#if defined(_WIN32)
  gmtime_s(&utcTime, &now);
#else
  gmtime_r(&now, &utcTime);
#endif
  hour = static_cast<uint8_t>(utcTime.tm_hour);
  minute = static_cast<uint8_t>(utcTime.tm_min);
  return true;
}

bool HalClock::getDateTime(uint16_t &year, uint8_t &month, uint8_t &day,
                           uint8_t &hour, uint8_t &minute) const {
  if (!_available)
    return false;

  const std::time_t now = std::time(nullptr);
  std::tm utcTime{};
#if defined(_WIN32)
  gmtime_s(&utcTime, &now);
#else
  gmtime_r(&now, &utcTime);
#endif
  year = static_cast<uint16_t>(utcTime.tm_year + 1900);
  month = static_cast<uint8_t>(utcTime.tm_mon + 1);
  day = static_cast<uint8_t>(utcTime.tm_mday);
  hour = static_cast<uint8_t>(utcTime.tm_hour);
  minute = static_cast<uint8_t>(utcTime.tm_min);
  return true;
}

bool HalClock::formatTime(char *buf, size_t bufSize,
                          uint8_t utcOffsetQuarterHoursBiased,
                          bool use12Hour) const {
  if (bufSize < (use12Hour ? 9u : 6u))
    return false;

  uint8_t hour;
  uint8_t minute;
  if (!getTime(hour, minute))
    return false;

  if (utcOffsetQuarterHoursBiased > 104)
    utcOffsetQuarterHoursBiased = 104;
  const int offsetQuarterHours =
      static_cast<int>(utcOffsetQuarterHoursBiased) - 48;
  int totalMinutes = static_cast<int>(hour) * 60 + static_cast<int>(minute) +
                     offsetQuarterHours * 15;
  totalMinutes = ((totalMinutes % 1440) + 1440) % 1440;

  const int hour24 = totalMinutes / 60;
  const int min = totalMinutes % 60;
  if (use12Hour) {
    const bool pm = hour24 >= 12;
    int hour12 = hour24 % 12;
    if (hour12 == 0)
      hour12 = 12;
    std::snprintf(buf, bufSize, "%d:%02d %s", hour12, min, pm ? "PM" : "AM");
  } else {
    std::snprintf(buf, bufSize, "%02d:%02d", hour24, min);
  }
  return true;
}

bool HalClock::formatDate(char *buf, size_t bufSize,
                          uint8_t utcOffsetQuarterHoursBiased,
                          const DateFormat dateFormat,
                          const char numericSeparator) const {
  if (bufSize < 13u || !_available)
    return false;

  if (utcOffsetQuarterHoursBiased > 104)
    utcOffsetQuarterHoursBiased = 104;
  const int offsetQuarterHours =
      static_cast<int>(utcOffsetQuarterHoursBiased) - 48;
  const std::time_t now =
      std::time(nullptr) +
      static_cast<std::time_t>(offsetQuarterHours) * 15 * 60;
  std::tm utcTime{};
#if defined(_WIN32)
  gmtime_s(&utcTime, &now);
#else
  gmtime_r(&now, &utcTime);
#endif
  const unsigned int year = static_cast<unsigned int>(utcTime.tm_year + 1900);
  const unsigned int month = static_cast<unsigned int>(utcTime.tm_mon + 1);
  const unsigned int day = static_cast<unsigned int>(utcTime.tm_mday);
  const char separator = numericSeparator == '.' || numericSeparator == '-'
                             ? numericSeparator
                             : '/';
  switch (dateFormat) {
  case DAY_MONTH_YEAR_LONG:
    std::snprintf(buf, bufSize, "%02u %s %u", day, kMonthNames[month - 1],
                  year);
    break;
  case MONTH_DAY_YEAR_NUMERIC:
    std::snprintf(buf, bufSize, "%02u%c%02u%c%u", month, separator, day,
                  separator, year);
    break;
  case DAY_MONTH_YEAR_NUMERIC:
    std::snprintf(buf, bufSize, "%02u%c%02u%c%u", day, separator, month,
                  separator, year);
    break;
  case YEAR_MONTH_DAY_NUMERIC:
    std::snprintf(buf, bufSize, "%u%c%02u%c%02u", year, separator, month,
                  separator, day);
    break;
  case MONTH_DAY_NUMERIC:
    std::snprintf(buf, bufSize, "%02u%c%02u", month, separator, day);
    break;
  case DAY_MONTH_NUMERIC:
    std::snprintf(buf, bufSize, "%02u%c%02u", day, separator, month);
    break;
  case MONTH_DAY_LONG:
    std::snprintf(buf, bufSize, "%s %02u", kFullMonthNames[month - 1], day);
    break;
  case DAY_MONTH_LONG:
    std::snprintf(buf, bufSize, "%02u %s", day, kFullMonthNames[month - 1]);
    break;
  case MONTH_DAY_YEAR_LONG:
  default:
    std::snprintf(buf, bufSize, "%s %02u, %u", kMonthNames[month - 1], day,
                  year);
    break;
  }
  return true;
}

bool HalClock::syncFromNTP() { return _available; }
