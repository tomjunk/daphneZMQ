#include "server_controller/handlers.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "Daphne.hpp"
#include "DevMem.hpp"
#include "FpgaRegDict.hpp"
#include "defines.hpp"
#include "daphneV3_low_level_confs.pb.h"
#include "reg.hpp"

namespace daphne_sc {
namespace {

using daphne::AFEConfig;
using daphne::ChannelConfig;
using daphne::ConfigureCLKsRequest;
using daphne::ConfigureCLKsResponse;
using daphne::ConfigureRequest;
using daphne::ConfigureResponse;
using daphne::Direction;
using daphne::DumpSpyBuffersChunkRequest;
using daphne::DumpSpyBuffersChunkResponse;
using daphne::DumpSpyBuffersRequest;
using daphne::DumpSpyBuffersResponse;
using daphne::GeneralInfo;
using daphne::InfoRequest;
using daphne::ReadTriggerCountersRequest;
using daphne::ReadTriggerCountersResponse;
using daphne::TestRegResponse;

using daphne::cmd_alignAFEs;
using daphne::cmd_alignAFEs_response;
using daphne::cmd_doAFEReset;
using daphne::cmd_doAFEReset_response;
using daphne::cmd_doSoftwareTrigger;
using daphne::cmd_doSoftwareTrigger_response;
using daphne::cmd_readAFEBiasSet;
using daphne::cmd_readAFEBiasSet_response;
using daphne::cmd_readAFEReg;
using daphne::cmd_readAFEReg_response;
using daphne::cmd_readAFEVgain;
using daphne::cmd_readAFEVgain_response;
using daphne::cmd_readBiasVoltageMonitor;
using daphne::cmd_readBiasVoltageMonitor_response;
using daphne::cmd_readCurrentMonitor;
using daphne::cmd_readCurrentMonitor_response;
using daphne::cmd_readOffset_allAFE;
using daphne::cmd_readOffset_allAFE_response;
using daphne::cmd_readOffset_allChannels;
using daphne::cmd_readOffset_allChannels_response;
using daphne::cmd_readOffset_singleChannel;
using daphne::cmd_readOffset_singleChannel_response;
using daphne::cmd_readTrim_allAFE;
using daphne::cmd_readTrim_allAFE_response;
using daphne::cmd_readTrim_allChannels;
using daphne::cmd_readTrim_allChannels_response;
using daphne::cmd_readTrim_singleChannel;
using daphne::cmd_readTrim_singleChannel_response;
using daphne::cmd_readVbiasControl;
using daphne::cmd_readVbiasControl_response;
using daphne::cmd_setAFEPowerState;
using daphne::cmd_setAFEPowerState_response;
using daphne::cmd_setAFEReset;
using daphne::cmd_setAFEReset_response;
using daphne::cmd_writeAFEBiasSet;
using daphne::cmd_writeAFEBiasSet_response;
using daphne::cmd_writeAFEAttenuation;
using daphne::cmd_writeAFEAttenuation_response;
using daphne::cmd_writeAFEFunction;
using daphne::cmd_writeAFEFunction_response;
using daphne::cmd_writeAFEVGAIN;
using daphne::cmd_writeAFEVGAIN_response;
using daphne::cmd_writeAFEReg;
using daphne::cmd_writeAFEReg_response;
using daphne::cmd_writeOFFSET_allAFE;
using daphne::cmd_writeOFFSET_allAFE_response;
using daphne::cmd_writeOFFSET_allChannels;
using daphne::cmd_writeOFFSET_allChannels_response;
using daphne::cmd_writeOFFSET_singleChannel;
using daphne::cmd_writeOFFSET_singleChannel_response;
using daphne::cmd_writeTRIM_allChannels;
using daphne::cmd_writeTRIM_allChannels_response;
using daphne::cmd_writeTrim_allAFE;
using daphne::cmd_writeTrim_allAFE_response;
using daphne::cmd_writeTrim_singleChannel;
using daphne::cmd_writeTrim_singleChannel_response;
using daphne::cmd_writeVbiasControl;
using daphne::cmd_writeVbiasControl_response;
using daphne::cmd_setHDMezzBlockEnable;
using daphne::cmd_setHDMezzBlockEnable_response;
using daphne::cmd_configureHDMezzBlock;
using daphne::cmd_configureHDMezzBlock_response;
using daphne::cmd_readHDMezzBlockConfig;
using daphne::cmd_readHDMezzBlockConfig_response;
using daphne::cmd_setHDMezzPowerStates;
using daphne::cmd_setHDMezzPowerStates_response;
using daphne::cmd_readHDMezzStatus;
using daphne::cmd_readHDMezzStatus_response;
using daphne::cmd_clearHDMezzAlertFlag;
using daphne::cmd_clearHDMezzAlertFlag_response;


struct I2C2BusGuard {
  Daphne& d;
  std::unique_lock<std::mutex> lock;

  explicit I2C2BusGuard(Daphne& daphne) : d(daphne), lock(daphne.i2c_2_mutex) {
    d.isI2C_2_device_configuring.store(true);
  }

  ~I2C2BusGuard() {
    d.isI2C_2_device_configuring.store(false);
  }
};


bool auto_align_enabled() {
  static const bool enabled = (std::getenv("DAPHNE_SKIP_ALIGN_AFTER_CONFIGURE") == nullptr);
  return enabled;
}

bool config_resets_enabled() {
  static const bool enabled = (std::getenv("DAPHNE_SKIP_CONFIG_RESET") == nullptr);
  return enabled;
}

size_t max_spybuffer_bytes() {
  size_t max_bytes = 64ULL * 1024 * 1024;
  if (const char* v = std::getenv("DAPHNE_MAX_SPYBUFFER_BYTES")) {
    try {
      max_bytes = static_cast<size_t>(std::stoull(v));
    } catch (...) {
    }
  }
  return max_bytes;
}

std::string decode_clk_status(uint32_t v) {
  const bool mmcm0 = (v & (1u << 0)) != 0;
  const bool mmcm1 = (v & (1u << 1)) != 0;
  std::ostringstream os;
  os << "MMCM0:" << (mmcm0 ? "LOCKED" : "UNLOCKED") << " MMCM1:" << (mmcm1 ? "LOCKED" : "UNLOCKED");
  return os.str();
}

namespace trigregs {
constexpr uint32_t PHYS_BASE = 0xA0010000u;
constexpr uint32_t STRIDE = 0x20u;
constexpr uint32_t NUM_CHANNELS = 40u;
constexpr uint32_t OFF_THR = 0x00u;
constexpr uint32_t OFF_REC_LO = 0x04u;
constexpr uint32_t OFF_REC_HI = 0x08u;
constexpr uint32_t OFF_BSY_LO = 0x0Cu;
constexpr uint32_t OFF_BSY_HI = 0x10u;
constexpr uint32_t OFF_FUL_LO = 0x14u;
constexpr uint32_t OFF_FUL_HI = 0x18u;
constexpr uint32_t MASK_28BIT = 0x0FFFFFFFu;
constexpr uint32_t MASK_REG_LOW = 0x94000020u;
constexpr uint32_t MASK_REG_HIGH = 0x94000024u;
}  // namespace trigregs

namespace stuffregs {
constexpr uint32_t PHYS_BASE = 0x94000000u;
constexpr uint32_t SPAN = 0x100u;
constexpr uint32_t OFF_ST_CONFIG = 0x2Cu;
constexpr uint32_t OFF_ST_DELAY = 0x30u;
constexpr uint32_t OFF_ST_FILTER_OUTPUT_SEL = 0x34u;
constexpr uint32_t OFF_ST_AFE_COMP_LO = 0x3Cu;
constexpr uint32_t OFF_ST_AFE_COMP_HI = 0x40u;
constexpr uint32_t OFF_ST_INVERT_LO = 0x44u;
constexpr uint32_t OFF_ST_INVERT_HI = 0x48u;
constexpr uint64_t MASK_40BIT = 0xFFFFFFFFFFULL;
}  // namespace stuffregs

namespace cmspi {
constexpr uint64_t DEFAULT_BASE = 0x9C020000ULL;
constexpr size_t SPAN = 0x100u;
constexpr uint32_t SRR = 0x40u;
constexpr uint32_t SPICR = 0x60u;
constexpr uint32_t SPISR = 0x64u;
constexpr uint32_t DTR = 0x68u;
constexpr uint32_t DRR = 0x6Cu;
constexpr uint32_t SPISSR = 0x70u;

constexpr uint32_t CR_SPE = 1u << 1;
constexpr uint32_t CR_MASTER = 1u << 2;
constexpr uint32_t CR_CPHA = 1u << 4;
constexpr uint32_t CR_TXFIFO_RESET = 1u << 5;
constexpr uint32_t CR_RXFIFO_RESET = 1u << 6;
constexpr uint32_t CR_MANUAL_SS = 1u << 7;
constexpr uint32_t CR_INHIBIT = 1u << 8;
constexpr uint32_t SR_RX_EMPTY = 1u << 0;

constexpr uint8_t ADS_RESET = 0x06u;
constexpr uint8_t ADS_START = 0x08u;
constexpr uint8_t ADS_STOP = 0x0Au;
constexpr uint8_t ADS_RDATA = 0x12u;
constexpr uint8_t ADS_RREG = 0x20u;
constexpr uint8_t ADS_WREG = 0x40u;
constexpr uint8_t REG_ID = 0x00u;
constexpr uint8_t REG_STATUS = 0x01u;
constexpr uint8_t REG_MODE0 = 0x02u;
constexpr uint8_t REG_MODE3 = 0x05u;
constexpr uint8_t REG_PGA = 0x10u;
constexpr uint8_t REG_INPMUX = 0x11u;
}  // namespace cmspi

uint64_t env_u64(const char* name, uint64_t fallback) {
  const char* text = std::getenv(name);
  if (text == nullptr || *text == '\0') return fallback;

  errno = 0;
  char* end = nullptr;
  const uint64_t value = std::strtoull(text, &end, 0);
  if (errno != 0 || end == text || (end != nullptr && *end != '\0')) return fallback;
  return value;
}

uint32_t lower32(uint64_t value) {
  return static_cast<uint32_t>(value & 0xFFFFFFFFULL);
}

uint32_t upper40(uint64_t value) {
  return static_cast<uint32_t>((value >> 32) & 0xFFULL);
}

std::string hex_u64(uint64_t value, int width = 0) {
  std::ostringstream os;
  os << "0x" << std::hex << std::uppercase;
  if (width > 0) os << std::setw(width) << std::setfill('0');
  os << value << std::dec;
  return os.str();
}

class AxiQuadSpi {
 public:
  explicit AxiQuadSpi(uint64_t base) : mem_(base) {
    mem_.map_memory(cmspi::SPAN);
    reset_core();
  }

  std::vector<uint8_t> transfer(const std::vector<uint8_t>& tx) {
    if (tx.empty()) return {};

    const uint32_t idle_cr = base_cr_ | cmspi::CR_INHIBIT;
    mem_.write_u32(cmspi::SPISSR, 0xFFFFFFFFu);
    mem_.write_u32(cmspi::SPICR, idle_cr | cmspi::CR_TXFIFO_RESET | cmspi::CR_RXFIFO_RESET);
    mem_.write_u32(cmspi::SPICR, idle_cr);

    drain_rx_fifo();
    for (const uint8_t b : tx) {
      mem_.write_u32(cmspi::DTR, b);
    }

    mem_.write_u32(cmspi::SPISSR, 0xFFFFFFFEu);
    mem_.write_u32(cmspi::SPICR, base_cr_);

    std::vector<uint8_t> rx;
    rx.reserve(tx.size());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    while (rx.size() < tx.size()) {
      const uint32_t status = mem_.read_u32(cmspi::SPISR);
      if ((status & cmspi::SR_RX_EMPTY) == 0) {
        rx.push_back(static_cast<uint8_t>(mem_.read_u32(cmspi::DRR) & 0xFFu));
        continue;
      }
      if (std::chrono::steady_clock::now() > deadline) {
        mem_.write_u32(cmspi::SPICR, idle_cr);
        mem_.write_u32(cmspi::SPISSR, 0xFFFFFFFFu);
        throw std::runtime_error("AXI Quad SPI transfer timed out waiting for RX data");
      }
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }

    mem_.write_u32(cmspi::SPICR, idle_cr);
    mem_.write_u32(cmspi::SPISSR, 0xFFFFFFFFu);
    return rx;
  }

 private:
  void reset_core() {
    mem_.write_u32(cmspi::SRR, 0x0Au);
    std::this_thread::sleep_for(std::chrono::microseconds(10));
    mem_.write_u32(cmspi::SPISSR, 0xFFFFFFFFu);
    mem_.write_u32(cmspi::SPICR,
                   base_cr_ | cmspi::CR_INHIBIT | cmspi::CR_TXFIFO_RESET | cmspi::CR_RXFIFO_RESET);
    mem_.write_u32(cmspi::SPICR, base_cr_ | cmspi::CR_INHIBIT);
  }

  void drain_rx_fifo() {
    for (int i = 0; i < 64; ++i) {
      const uint32_t status = mem_.read_u32(cmspi::SPISR);
      if ((status & cmspi::SR_RX_EMPTY) != 0) return;
      (void)mem_.read_u32(cmspi::DRR);
    }
  }

  DevMem mem_;
  const uint32_t base_cr_ = cmspi::CR_SPE | cmspi::CR_MASTER | cmspi::CR_CPHA | cmspi::CR_MANUAL_SS;
};

uint8_t ads_command(AxiQuadSpi& spi, uint8_t opcode) {
  const std::vector<uint8_t> rx = spi.transfer({opcode, 0x00u});
  return rx.size() > 1 ? rx[1] : 0;
}

uint8_t ads_read_reg(AxiQuadSpi& spi, uint8_t reg_addr) {
  const std::vector<uint8_t> rx = spi.transfer({static_cast<uint8_t>(cmspi::ADS_RREG | (reg_addr & 0x1Fu)),
                                                0x00u,
                                                0x00u});
  if (rx.size() < 3) throw std::runtime_error("short ADS126x RREG response");
  return rx[2];
}

void ads_write_reg(AxiQuadSpi& spi, uint8_t reg_addr, uint8_t value) {
  const std::vector<uint8_t> rx = spi.transfer({static_cast<uint8_t>(cmspi::ADS_WREG | (reg_addr & 0x1Fu)), value});
  if (rx.size() < 2) throw std::runtime_error("short ADS126x WREG response");
}

uint8_t current_monitor_mux_for_channel(uint32_t channel) {
  const std::string env_name = "DAPHNE_CURRENT_MONITOR_MUX_" + std::to_string(channel);
  if (std::getenv(env_name.c_str()) != nullptr) {
    return static_cast<uint8_t>(env_u64(env_name.c_str(), 0xFFu) & 0xFFu);
  }

  if (channel > 9) {
    throw std::invalid_argument("current monitor channel out of range (0..9): " + std::to_string(channel));
  }
  const uint8_t muxp = static_cast<uint8_t>(channel + 1u);
  const uint8_t muxn = static_cast<uint8_t>(env_u64("DAPHNE_CURRENT_MONITOR_NEG_MUX", 0x0u) & 0x0Fu);
  return static_cast<uint8_t>((muxp << 4) | muxn);
}

bool read_current_monitor_raw(const cmd_readCurrentMonitor& req,
                              uint32_t& current_value,
                              std::string& response_msg) {
  try {
    const uint64_t base = env_u64("DAPHNE_CURRENT_MONITOR_SPI_BASE", cmspi::DEFAULT_BASE);
    const uint32_t channel = req.currentmonitorchannel();
    const uint8_t mux = current_monitor_mux_for_channel(channel);

    AxiQuadSpi spi(base);
    (void)ads_command(spi, cmspi::ADS_RESET);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    const uint8_t id = ads_read_reg(spi, cmspi::REG_ID);
    const uint8_t dev_id = static_cast<uint8_t>((id >> 4) & 0x0Fu);
    if (dev_id != 0x8u && dev_id != 0xAu) {
      std::ostringstream os;
      os << "ADS126x current monitor did not return a valid device id: id=" << hex_u64(id, 2)
         << " dev_id=" << hex_u64(dev_id, 1);
      response_msg = os.str();
      return false;
    }

    ads_write_reg(spi, cmspi::REG_MODE3, 0x00u);  // Disable STATUS and CRC bytes for fixed-length reads.
    ads_write_reg(spi, cmspi::REG_MODE0, 0x6Cu);  // 14.4 kSPS, FIR filter, matching the legacy driver intent.
    ads_write_reg(spi, cmspi::REG_PGA, 0x05u);    // PGA enabled, gain code 5.
    ads_write_reg(spi, cmspi::REG_INPMUX, mux);

    const uint8_t mux_readback = ads_read_reg(spi, cmspi::REG_INPMUX);
    if (mux_readback != mux) {
      std::ostringstream os;
      os << "ADS126x INPMUX write/readback mismatch: wrote=" << hex_u64(mux, 2)
         << " read=" << hex_u64(mux_readback, 2);
      response_msg = os.str();
      return false;
    }

    (void)ads_command(spi, cmspi::ADS_START);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const std::vector<uint8_t> rx = spi.transfer({cmspi::ADS_RDATA, 0x00u, 0x00u, 0x00u, 0x00u});
    (void)ads_command(spi, cmspi::ADS_STOP);
    if (rx.size() < 5) throw std::runtime_error("short ADS126x RDATA response");

    const uint32_t raw24 = (static_cast<uint32_t>(rx[2]) << 16) |
                           (static_cast<uint32_t>(rx[3]) << 8) |
                           static_cast<uint32_t>(rx[4]);
    const int32_t signed_raw = (raw24 & 0x800000u) ? static_cast<int32_t>(raw24 | 0xFF000000u)
                                                   : static_cast<int32_t>(raw24);
    current_value = static_cast<uint32_t>(signed_raw);

    const uint8_t status = ads_read_reg(spi, cmspi::REG_STATUS);
    std::ostringstream os;
    os << "OK: ADS126x raw 24-bit conversion"
       << " channel=" << channel
       << " mux=" << hex_u64(mux, 2)
       << " id=" << hex_u64(id, 2)
       << " status=" << hex_u64(status, 2)
       << " raw24=" << hex_u64(raw24, 6)
       << " signed=" << signed_raw;
    response_msg = os.str();
    return true;
  } catch (const std::exception& e) {
    response_msg = std::string("Current monitor read failed: ") + e.what();
    current_value = 0;
    return false;
  }
}

bool write_self_trigger_controls(const ConfigureRequest& cfg, std::string& response_msg) {
  std::ostringstream out;
  bool ok = true;

  try {
    DevMem stuff(stuffregs::PHYS_BASE);
    stuff.map_memory(stuffregs::SPAN);

    if (cfg.tp_conf() != 0) {
      const uint64_t tp_conf = cfg.tp_conf();
      const uint32_t filter_output_sel = static_cast<uint32_t>(tp_conf & 0x3ULL);
      const uint32_t st_config = static_cast<uint32_t>((tp_conf >> 2) & 0x3FFFULL);
      const uint32_t signal_delay = static_cast<uint32_t>((tp_conf >> 16) & 0x1FULL);

      stuff.write_u32(stuffregs::OFF_ST_CONFIG, st_config);
      stuff.write_u32(stuffregs::OFF_ST_DELAY, signal_delay);
      stuff.write_u32(stuffregs::OFF_ST_FILTER_OUTPUT_SEL, filter_output_sel);

      out << "tp_conf " << hex_u64(tp_conf, 8)
          << " -> st_config=" << hex_u64(st_config, 4)
          << " signal_delay=" << signal_delay
          << " filter_output_selector=" << filter_output_sel << ".\n";
    } else {
      out << "tp_conf is zero; leaving descriptor config/delay/filter selector unchanged.\n";
    }

    const uint64_t comp = cfg.compensator() & stuffregs::MASK_40BIT;
    stuff.write_u32(stuffregs::OFF_ST_AFE_COMP_LO, lower32(comp));
    stuff.write_u32(stuffregs::OFF_ST_AFE_COMP_HI, upper40(comp));
    out << "compensator mask=" << hex_u64(comp, 10) << ".\n";

    const uint64_t inv = cfg.inverters() & stuffregs::MASK_40BIT;
    stuff.write_u32(stuffregs::OFF_ST_INVERT_LO, lower32(inv));
    stuff.write_u32(stuffregs::OFF_ST_INVERT_HI, upper40(inv));
    out << "inverter mask=" << hex_u64(inv, 10) << ".\n";

    if ((cfg.self_trigger_xcorr() >> 28) != 0) {
      out << "self_trigger_xcorr discrimination field="
          << hex_u64((cfg.self_trigger_xcorr() >> 28) & 0x3FFFULL, 4)
          << " has no separate register in this gateware; threshold_xc uses bits [27:0].\n";
    }
  } catch (const std::exception& e) {
    ok = false;
    out << "Failed to write self-trigger control registers via /dev/mem: " << e.what() << ".\n";
  }

  response_msg = out.str();
  return ok;
}

bool read_counters_raw(uint32_t base,
                       const std::vector<uint32_t>& chs,
                       ReadTriggerCountersResponse& resp,
                       std::string& err) {
  long pagesz = sysconf(_SC_PAGESIZE);
  if (pagesz <= 0) pagesz = 4096;
  const uint64_t pg = static_cast<uint64_t>(pagesz);

  const uint64_t map_base = (static_cast<uint64_t>(base)) & ~(pg - 1ULL);
  uint32_t maxch = 0;
  for (const auto c : chs) {
    if (c < trigregs::NUM_CHANNELS && c > maxch) maxch = c;
  }
  const uint32_t span = (maxch * trigregs::STRIDE) + trigregs::OFF_FUL_HI + 4u;
  const uint64_t tail = (static_cast<uint64_t>(base) - map_base) + static_cast<uint64_t>(span);
  const uint64_t map_len = (tail + (pg - 1ULL)) & ~(pg - 1ULL);

  int fd = open("/dev/mem", O_RDONLY | O_SYNC);
  if (fd < 0) {
    err = "open(/dev/mem) failed";
    return false;
  }
  void* p = mmap(nullptr, map_len, PROT_READ, MAP_SHARED, fd, map_base);
  if (p == MAP_FAILED) {
    close(fd);
    err = "mmap(/dev/mem) failed";
    return false;
  }
  auto* ptr = static_cast<uint8_t*>(p);

  auto rd32 = [&](uint32_t phys) -> uint32_t {
    const uint64_t off = static_cast<uint64_t>(phys) - map_base;
    uint32_t v = 0;
    std::memcpy(&v, ptr + off, 4);
    return v;
  };
  auto rd64 = [&](uint32_t lo, uint32_t hi) -> uint64_t {
    const uint32_t l = rd32(lo);
    const uint32_t h = rd32(hi);
    return (static_cast<uint64_t>(h) << 32) | l;
  };

  for (const auto ch : chs) {
    if (ch >= trigregs::NUM_CHANNELS) continue;
    const uint32_t b = base + ch * trigregs::STRIDE;
    auto* s = resp.add_snapshots();
    s->set_channel(ch);
    s->set_threshold(rd32(b + trigregs::OFF_THR) & trigregs::MASK_28BIT);
    s->set_record_count(rd64(b + trigregs::OFF_REC_LO, b + trigregs::OFF_REC_HI));
    s->set_busy_count(rd64(b + trigregs::OFF_BSY_LO, b + trigregs::OFF_BSY_HI));
    s->set_full_count(rd64(b + trigregs::OFF_FUL_LO, b + trigregs::OFF_FUL_HI));
  }

  munmap(ptr, map_len);
  close(fd);
  return true;
}

bool write_trigger_thresholds(const ConfigureRequest& cfg, std::string& response_msg) {
  std::ostringstream out;
  std::vector<uint32_t> channels;
  channels.reserve(static_cast<size_t>(cfg.channels_size()));
  for (const auto& ch_cfg : cfg.channels()) {
    channels.push_back(ch_cfg.id());
  }

  if (channels.empty()) {
    response_msg = "No channels provided; skipping trigger threshold programming.\n";
    return true;
  }

  std::sort(channels.begin(), channels.end());
  channels.erase(std::unique(channels.begin(), channels.end()), channels.end());
  for (const auto ch : channels) {
    if (ch >= trigregs::NUM_CHANNELS) {
      out << "Channel out of range for trigger threshold: " << ch << " (0.."
          << (trigregs::NUM_CHANNELS - 1) << ").\n";
      response_msg = out.str();
      return false;
    }
  }

  const bool use_xcorr_threshold = cfg.self_trigger_xcorr() != 0;
  const uint64_t raw_threshold = use_xcorr_threshold
                                     ? (cfg.self_trigger_xcorr() & trigregs::MASK_28BIT)
                                     : cfg.self_trigger_threshold();
  uint32_t thr_val = static_cast<uint32_t>(raw_threshold);
  if (raw_threshold > trigregs::MASK_28BIT) {
    out << "Requested threshold " << raw_threshold << " exceeds 28-bit range; clamping to "
        << trigregs::MASK_28BIT << ".\n";
    thr_val = trigregs::MASK_28BIT;
  }
  out << "Trigger threshold source: "
      << (use_xcorr_threshold ? "self_trigger_xcorr[27:0]" : "self_trigger_threshold")
      << ".\n";

  try {
    DevMem thr_mem(trigregs::PHYS_BASE);
    const size_t span = trigregs::STRIDE * trigregs::NUM_CHANNELS + 4u;
    thr_mem.map_memory(span);
    for (const auto ch : channels) {
      const size_t off = static_cast<size_t>(ch) * trigregs::STRIDE + trigregs::OFF_THR;
      thr_mem.write(off, std::vector<uint32_t>{thr_val});
    }
    out << "Trigger threshold 0x" << std::hex << thr_val << std::dec << " written to channels:";
    for (const auto ch : channels) out << " " << ch;
    out << ".\n";
  } catch (const std::exception& e) {
    out << "Failed to write trigger thresholds via /dev/mem: " << e.what() << ".\n";
    response_msg = out.str();
    return false;
  }

  uint32_t mask_low = 0;
  uint32_t mask_high = 0;
  for (const auto ch : channels) {
    if (ch < 32) {
      mask_low |= (1u << ch);
    } else {
      mask_high |= (1u << (ch - 32));
    }
  }

  bool ok = true;
  try {
    DevMem mlow(trigregs::MASK_REG_LOW);
    mlow.map_memory(4);
    mlow.write(0, std::vector<uint32_t>{mask_low});
    out << "Trigger enable LOW @ 0x" << std::hex << trigregs::MASK_REG_LOW << " = 0x" << mask_low
        << std::dec << ".\n";
  } catch (const std::exception& e) {
    out << "Failed to write LOW trigger mask via /dev/mem: " << e.what() << ".\n";
    ok = false;
  }

  try {
    DevMem mhigh(trigregs::MASK_REG_HIGH);
    mhigh.map_memory(4);
    mhigh.write(0, std::vector<uint32_t>{mask_high});
    out << "Trigger enable HIGH @ 0x" << std::hex << trigregs::MASK_REG_HIGH << " = 0x" << mask_high
        << std::dec << ".\n";
  } catch (const std::exception& e) {
    if (mask_high != 0) {
      out << "Failed to write HIGH trigger mask via /dev/mem: " << e.what() << ".\n";
      ok = false;
    } else {
      out << "High trigger mask register unavailable; skipped.\n";
    }
  }

  response_msg = out.str();
  return ok;
}

struct EpRegs {
  FpgaRegDict dict;
  reg r;
  EpRegs() : dict(), r(/*BaseAddr*/ 0x80000000ULL, /*MemLen*/ 0x6000000, dict) {}
};

bool set_clock_source_and_mmcm_reset(bool use_endpoint_clk,
                                    bool pulse_mmcm1_reset,
                                    std::string& msg,
                                    int timeout_ms = 500) {
  EpRegs ep;
  const uint32_t clk_src = use_endpoint_clk ? 1u : 0u;
  ep.r.WriteBits("endpointClockControl", "CLOCK_SOURCE", clk_src);
  if (pulse_mmcm1_reset) {
    ep.r.WriteBits("endpointClockControl", "MMCM_RESET", 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    ep.r.WriteBits("endpointClockControl", "MMCM_RESET", 0);
  }

  auto t0 = std::chrono::steady_clock::now();
  while (true) {
    const uint32_t s = ep.r.ReadRegister("endpointClockStatus");
    if ((s & 0x3u) == 0x3u) {
      msg += "Clock status: " + decode_clk_status(s) + "\n";
      return true;
    }
    if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() >
        timeout_ms) {
      msg += "Clock status (timeout): " + decode_clk_status(ep.r.ReadRegister("endpointClockStatus")) + "\n";
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

bool set_endpoint_addr_and_reset(uint16_t ep_addr,
                                bool pulse_ep_reset,
                                std::string& msg,
                                int timeout_ms = 800) {
  EpRegs ep;
  ep.r.WriteBits("endpointControl", "ADDRESS", ep_addr);

  if (pulse_ep_reset) {
    ep.r.WriteBits("endpointControl", "RESET", 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    ep.r.WriteBits("endpointControl", "RESET", 0);
  }

  auto t0 = std::chrono::steady_clock::now();
  while (true) {
    const uint32_t s = ep.r.ReadRegister("endpointStatus");
    const uint32_t fsm = (s & 0xF);
    const bool ts_ok = (s & (1u << 4)) != 0;
    if (fsm == 8 && ts_ok) {
      std::ostringstream os;
      os << "Endpoint READY; status=0x" << std::hex << s;
      msg += os.str() + "\n";
      return true;
    }
    if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() >
        timeout_ms) {
      std::ostringstream os;
      os << "Endpoint not ready (timeout); status=0x" << std::hex << s;
      msg += os.str() + "\n";
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

bool configureDaphne(const ConfigureRequest& requested_cfg, Daphne& daphne, std::string& response_str) {
  try {
    std::ostringstream out;
    bool ok_all = true;

    const bool requested_bias_for_any_afe =
        std::any_of(requested_cfg.afes().begin(),
                    requested_cfg.afes().end(),
                    [](const AFEConfig& afe_cfg) { return afe_cfg.v_bias() > 0; });
    bool bias_control_applied = false;

    if (config_resets_enabled()) {
      daphne.getAfe()->doReset();
      daphne.getAfe()->setPowerState(1);
    } else {
      out << "Config reset/powercycle skipped (DAPHNE_SKIP_CONFIG_RESET set).\n";
    }

    {
      std::string thr_msg;
      const bool thr_ok = write_trigger_thresholds(requested_cfg, thr_msg);
      ok_all = ok_all && thr_ok;
      out << "[TRIGGER_THRESHOLDS]\n" << thr_msg;
    }

    {
      std::string st_msg;
      const bool st_ok = write_self_trigger_controls(requested_cfg, st_msg);
      ok_all = ok_all && st_ok;
      out << "[SELF_TRIGGER_CONTROL]\n" << st_msg;
    }

    for (const ChannelConfig& ch_config : requested_cfg.channels()) {
      const uint32_t ch = ch_config.id();
      if (ch > 39) throw std::invalid_argument("Channel out of range (0..39): " + std::to_string(ch));

      const uint32_t afe_board = ch / 8;
      const uint32_t afe_pl = afe_definitions::AFE_board2PL_map.at(afe_board);
      const uint32_t idx = ch % 8;

      daphne.getDac()->setDacTrim(afe_pl, idx, ch_config.trim(), false, false);
      daphne.setChTrimDictValue(ch, ch_config.trim());
      out << "Trim value written successfully for Channel " << ch << ". Trim value: " << ch_config.trim()
          << ". Returned value: " << daphne.getChTrimDictValue(ch) << ".\n";

      daphne.getDac()->setDacOffset(afe_pl, idx, ch_config.offset(), false, false);
      daphne.setChOffsetDictValue(ch, ch_config.offset());
      out << "Offset value written successfully for Channel " << ch << ". Offset value: " << ch_config.offset()
          << ". Returned value: " << daphne.getChOffsetDictValue(ch) << ".\n";
    }

    {
      const uint32_t ctrl = requested_cfg.biasctrl();
      if (ctrl <= 4095) {
        const uint32_t returnedControlValue = daphne.getDac()->setDacHvBias(ctrl, false, false);
        const uint32_t returnedBiasEnable = daphne.getDac()->setBiasEnable(true);
        daphne.setBiasControlDictValue(ctrl);
        bias_control_applied = true;
        out << "Bias Control value written successfully. Bias Control value: " << ctrl << " and Enable: "
            << returnedBiasEnable << " Returned value: " << returnedControlValue << ".\n";
      } else {
        out << "Warning: Bias Control value " << ctrl << " out of range (0..4095). Skipping.\n";
      }
    }

    if (!bias_control_applied && requested_bias_for_any_afe) {
      const uint32_t ctrl = 4095;
      const uint32_t returnedControlValue = daphne.getDac()->setDacHvBias(ctrl, false, false);
      const uint32_t returnedBiasEnable = daphne.getDac()->setBiasEnable(true);
      daphne.setBiasControlDictValue(ctrl);
      out << "Bias Control was not set in request but AFE bias values are present. Defaulting Bias Control to " << ctrl
          << " and Enable: " << returnedBiasEnable << " Returned value: " << returnedControlValue << ".\n";
    }

    for (const AFEConfig& afe_config : requested_cfg.afes()) {
      const uint32_t afe_board = afe_config.id();
      const uint32_t afe_pl = afe_definitions::AFE_board2PL_map.at(afe_board);

      const uint32_t v = afe_config.attenuators();
      if (v > 4095) throw std::invalid_argument("VGAIN out of range for AFE " + std::to_string(afe_board));
      daphne.getDac()->setDacGain(afe_pl, v);
      daphne.setAfeAttenuationDictValue(afe_pl, v);
      out << "AFE VGAIN written successfully for AFE " << afe_board << ". VGAIN: " << v
          << ". Returned value: " << daphne.getAfeAttenuationDictValue(afe_pl) << ".\n";

      const uint32_t bias = afe_config.v_bias();
      if (bias > 4095) throw std::invalid_argument("BIAS out of range for AFE " + std::to_string(afe_board));
      if (bias != 0) {
        daphne.getDac()->setDacBias(afe_pl, bias);
        daphne.setBiasVoltageDictValue(afe_pl, bias);
        out << "AFE bias value written successfully for AFE " << afe_board << ". Bias value: " << bias
            << ". Returned value: " << daphne.getBiasVoltageDictValue(afe_pl) << ".\n";
      }

      const uint32_t adc_res = afe_config.adc().resolution() ? 1u : 0u;
      const uint32_t adc_out_fmt = afe_config.adc().output_format() ? 1u : 0u;
      const uint32_t adc_sb_first = afe_config.adc().sb_first() ? 1u : 0u;

      uint32_t r = daphne.getAfe()->setAFEFunction(afe_pl, "SERIALIZED_DATA_RATE", 1u);
      out << "Function SERIALIZED_DATA_RATE in AFE " << afe_board << " configured correctly.\nReturned value: " << r
          << "\n";
      r = daphne.getAfe()->setAFEFunction(afe_pl, "ADC_RESOLUTION_RESET", adc_res);
      out << "Function ADC_RESOLUTION_RESET in AFE " << afe_board << " configured correctly.\nReturned value: " << r
          << "\n";
      r = daphne.getAfe()->setAFEFunction(afe_pl, "ADC_OUTPUT_FORMAT", adc_out_fmt);
      out << "Function ADC_OUTPUT_FORMAT in AFE " << afe_board << " configured correctly.\nReturned value: " << r
          << "\n";
      r = daphne.getAfe()->setAFEFunction(afe_pl, "LSB_MSB_FIRST", adc_sb_first);
      out << "Function LSB_MSB_FIRST in AFE " << afe_board << " configured correctly.\nReturned value: " << r << "\n";

      r = daphne.getAfe()->setAFEFunction(afe_pl, "LPF_PROGRAMMABILITY", afe_config.pga().lpf_cut_frequency());
      out << "Function LPF_PROGRAMMABILITY in AFE " << afe_board << " configured correctly.\nReturned value: " << r
          << "\n";
      r = daphne.getAfe()->setAFEFunction(afe_pl,
                                          "PGA_INTEGRATOR_DISABLE",
                                          afe_config.pga().integrator_disable() ? 1u : 0u);
      out << "Function PGA_INTEGRATOR_DISABLE in AFE " << afe_board << " configured correctly.\nReturned value: " << r
          << "\n";
      r = daphne.getAfe()->setAFEFunction(afe_pl, "PGA_CLAMP_LEVEL", 2u);
      out << "Function PGA_CLAMP_LEVEL in AFE " << afe_board << " configured correctly.\nReturned value: " << r << "\n";
      r = daphne.getAfe()->setAFEFunction(afe_pl, "ACTIVE_TERMINATION_ENABLE", 0u);
      out << "Function ACTIVE_TERMINATION_ENABLE in AFE " << afe_board << " configured correctly.\nReturned value: " << r
          << "\n";

      r = daphne.getAfe()->setAFEFunction(afe_pl, "LNA_INPUT_CLAMP_SETTING", afe_config.lna().clamp());
      out << "Function LNA_INPUT_CLAMP_SETTING in AFE " << afe_board << " configured correctly.\nReturned value: " << r
          << "\n";
      r = daphne.getAfe()->setAFEFunction(afe_pl, "LNA_GAIN", afe_config.lna().gain());
      out << "Function LNA_GAIN in AFE " << afe_board << " configured correctly.\nReturned value: " << r << "\n";
      r = daphne.getAfe()->setAFEFunction(afe_pl,
                                          "LNA_INTEGRATOR_DISABLE",
                                          afe_config.lna().integrator_disable() ? 1u : 0u);
      out << "Function LNA_INTEGRATOR_DISABLE in AFE " << afe_board << " configured correctly.\nReturned value: " << r
          << "\n";
    }

    if (config_resets_enabled()) {
      daphne.getAfe()->setPowerState(1);
    }

    response_str = out.str();
    return ok_all;
  } catch (const std::exception& e) {
    response_str = std::string("Caught Exception:\n") + e.what();
    return false;
  }
}

bool writeAFERegister(const cmd_writeAFEReg& request,
                      Daphne& daphne,
                      std::string& response_str,
                      uint32_t& returned_value) {
  try {
    uint32_t afe_block = afe_definitions::AFE_board2PL_map.at(request.afeblock());
    const uint32_t reg_addr = request.regaddress();
    const uint32_t reg_value = request.regvalue();
    returned_value = daphne.getAfe()->setRegister(afe_block, reg_addr, reg_value);
    response_str = "AFE Register " + std::to_string(reg_addr) + " written with value " + std::to_string(reg_value) +
                   " for AFE " + std::to_string(afe_definitions::AFE_PL2board_map.at(afe_block)) +
                   ". Returned value: " + std::to_string(returned_value) + ".";
    daphne.setAfeRegDictValue(afe_block, reg_addr, returned_value);
    return true;
  } catch (const std::exception& e) {
    response_str = std::string("Error writing AFE Register: ") + e.what();
    return false;
  }
}

bool writeAFEVgain(const cmd_writeAFEVGAIN& request,
                   Daphne& daphne,
                   std::string& response_str,
                   uint32_t& returned_value) {
  try {
    const uint32_t afe_block = afe_definitions::AFE_board2PL_map.at(request.afeblock());
    const uint32_t vgain = request.vgainvalue();
    if (vgain > 4095) throw std::invalid_argument("VGAIN out of range (0..4095)");
    daphne.getDac()->setDacGain(afe_block, vgain);
    daphne.setAfeAttenuationDictValue(afe_block, vgain);
    returned_value = daphne.getAfeAttenuationDictValue(afe_block);
    response_str = "AFE VGAIN written successfully for AFE " + std::to_string(afe_definitions::AFE_PL2board_map.at(afe_block)) +
                   ". VGAIN: " + std::to_string(vgain) + ". Returned value: " + std::to_string(returned_value) + ".";
    return true;
  } catch (const std::exception& e) {
    response_str = std::string("Error writing AFE VGAIN: ") + e.what();
    return false;
  }
}

bool writeAFEAttenuation(const cmd_writeAFEAttenuation& request,
                         Daphne& daphne,
                         std::string& response_str,
                         uint32_t& returned_value) {
  try {
    const uint32_t afe_block = afe_definitions::AFE_board2PL_map.at(request.afeblock());
    const uint32_t attenuation = request.attenuation();
    if (attenuation > 4095) throw std::invalid_argument("attenuation out of range (0..4095)");
    daphne.getDac()->setDacGain(afe_block, attenuation);
    daphne.setAfeAttenuationDictValue(afe_block, attenuation);
    returned_value = daphne.getAfeAttenuationDictValue(afe_block);
    response_str = "AFE attenuation written successfully for AFE " +
                   std::to_string(afe_definitions::AFE_PL2board_map.at(afe_block)) +
                   ". Attenuation: " + std::to_string(attenuation) + ". Returned value: " +
                   std::to_string(returned_value) + ".";
    return true;
  } catch (const std::exception& e) {
    response_str = std::string("Error writing AFE attenuation: ") + e.what();
    return false;
  }
}

bool writeAFEBiasVoltage(const cmd_writeAFEBiasSet& request,
                         Daphne& daphne,
                         std::string& response_str,
                         uint32_t& returned_value) {
  try {
    const uint32_t afe_block = afe_definitions::AFE_board2PL_map.at(request.afeblock());
    const uint32_t bias_value = request.biasvalue();
    if (bias_value > 4095) throw std::invalid_argument("bias out of range (0..4095)");
    daphne.getDac()->setDacBias(afe_block, bias_value);
    daphne.setBiasVoltageDictValue(afe_block, bias_value);
    returned_value = daphne.getBiasVoltageDictValue(afe_block);
    response_str = "AFE bias value written successfully for AFE " +
                   std::to_string(afe_definitions::AFE_PL2board_map.at(afe_block)) + ". Bias value: " +
                   std::to_string(bias_value) + ". Returned value: " + std::to_string(returned_value) + ".";
    return true;
  } catch (const std::exception& e) {
    response_str = std::string("Error writing AFE bias value: ") + e.what();
    return false;
  }
}

bool writeChannelTrim(const cmd_writeTrim_singleChannel& request,
                      Daphne& daphne,
                      std::string& response_str,
                      uint32_t& returned_value) {
  try {
    const uint32_t trim_ch = request.trimchannel();
    const uint32_t trim_value = request.trimvalue();
    const bool trim_gain = request.trimgain();
    if (trim_value > 4095) throw std::invalid_argument("trimValue out of range (0..4095)");
    if (trim_ch > 39) throw std::invalid_argument("trimChannel out of range (0..39)");

    const uint32_t afe_block = afe_definitions::AFE_board2PL_map.at(trim_ch / 8);
    daphne.getDac()->setDacTrim(afe_block, trim_ch % 8, trim_value, trim_gain, false);
    daphne.setChTrimDictValue(trim_ch, trim_value);
    returned_value = daphne.getChTrimDictValue(trim_ch);
    response_str = "Trim value written successfully for Channel " + std::to_string(trim_ch) + ". Trim value: " +
                   std::to_string(trim_value) + ". Returned value: " + std::to_string(returned_value) + ".";
    return true;
  } catch (const std::exception& e) {
    response_str = std::string("Error writing Channel Trim value: ") + e.what();
    return false;
  }
}

bool writeChannelOffset(const cmd_writeOFFSET_singleChannel& request,
                        Daphne& daphne,
                        std::string& response_str,
                        uint32_t& returned_value) {
  try {
    const uint32_t offset_ch = request.offsetchannel();
    const uint32_t offset_value = request.offsetvalue();
    const bool offset_gain = request.offsetgain();
    if (offset_value > 4095) throw std::invalid_argument("offsetValue out of range (0..4095)");
    if (offset_ch > 39) throw std::invalid_argument("offsetChannel out of range (0..39)");

    const uint32_t afe_block = afe_definitions::AFE_board2PL_map.at(offset_ch / 8);
    daphne.getDac()->setDacOffset(afe_block, offset_ch % 8, offset_value, offset_gain, false);
    daphne.setChOffsetDictValue(offset_ch, offset_value);
    returned_value = daphne.getChOffsetDictValue(offset_ch);
    response_str = "Offset value written successfully for Channel " + std::to_string(offset_ch) + ". Offset value: " +
                   std::to_string(offset_value) + ". Returned value: " + std::to_string(returned_value) + ".";
    return true;
  } catch (const std::exception& e) {
    response_str = std::string("Error writing Channel Offset value: ") + e.what();
    return false;
  }
}

bool writeBiasVoltageControl(const cmd_writeVbiasControl& request,
                             Daphne& daphne,
                             std::string& response_str,
                             uint32_t& returned_value) {
  try {
    const uint32_t control_value = request.vbiascontrolvalue();
    const bool bias_enable = request.enable();
    if (control_value > 4095) throw std::invalid_argument("vbiasControlValue out of range (0..4095)");
    const uint32_t returnedControlValue = daphne.getDac()->setDacHvBias(control_value, false, false);
    const uint32_t returnedBiasEnable = daphne.getDac()->setBiasEnable(bias_enable);
    daphne.setBiasControlDictValue(control_value);
    returned_value = returnedControlValue;
    response_str = "Bias Control value written successfully. Bias Control value: " + std::to_string(control_value) +
                   " and Enable: " + std::to_string(returnedBiasEnable) +
                   " Returned value: " + std::to_string(returnedControlValue) + ".";
    return true;
  } catch (const std::exception& e) {
    response_str = std::string("Error writing Bias Control value: ") + e.what();
    return false;
  }
}

bool readAFEReg(const cmd_readAFEReg& request,
                cmd_readAFEReg_response& response,
                Daphne& daphne,
                std::string& response_str) {
  try {
    const uint32_t afe_block = afe_definitions::AFE_board2PL_map.at(request.afeblock());
    const uint32_t reg_addr = request.regaddress();
    const uint32_t reg_value = daphne.getAfeRegDictValue(afe_block, reg_addr);
    response.set_afeblock(request.afeblock());
    response.set_regaddress(reg_addr);
    response.set_regvalue(reg_value);
    response_str = "AFE Register " + std::to_string(reg_addr) + " read successfully. Value: " +
                   std::to_string(reg_value) + ".";
    return true;
  } catch (const std::exception& e) {
    response_str = std::string("Error reading AFE register: ") + e.what();
    return false;
  }
}

bool readAFEVgain(const cmd_readAFEVgain& request,
                  cmd_readAFEVgain_response& response,
                  Daphne& daphne,
                  std::string& response_str) {
  try {
    const uint32_t afe_block = afe_definitions::AFE_board2PL_map.at(request.afeblock());
    const uint32_t vgain = daphne.getAfeAttenuationDictValue(afe_block);
    response.set_afeblock(request.afeblock());
    response.set_vgainvalue(vgain);
    response_str = "AFE VGAIN read successfully. Value: " + std::to_string(vgain) + ".";
    return true;
  } catch (const std::exception& e) {
    response_str = std::string("Error reading AFE vgain: ") + e.what();
    return false;
  }
}

bool readAFEBiasSet(const cmd_readAFEBiasSet& request,
                    cmd_readAFEBiasSet_response& response,
                    Daphne& daphne,
                    std::string& response_str) {
  try {
    const uint32_t afe_block = afe_definitions::AFE_board2PL_map.at(request.afeblock());
    const uint32_t bias_value = daphne.getBiasVoltageDictValue(afe_block);
    response.set_afeblock(request.afeblock());
    response.set_biasvalue(bias_value);
    response_str = "AFE bias read successfully. Value: " + std::to_string(bias_value) + ".";
    return true;
  } catch (const std::exception& e) {
    response_str = std::string("Error reading AFE bias: ") + e.what();
    return false;
  }
}

bool readTrimAllChannels(const cmd_readTrim_allChannels&,
                         cmd_readTrim_allChannels_response& response,
                         Daphne& daphne,
                         std::string& response_str) {
  try {
    for (uint32_t ch = 0; ch < 40; ++ch) response.add_trimvalues(daphne.getChTrimDictValue(ch));
    response_str = "All channel trims read successfully.";
    return true;
  } catch (const std::exception& e) {
    response_str = std::string("Error reading all channel trims: ") + e.what();
    return false;
  }
}

bool readTrimAllAFE(const cmd_readTrim_allAFE& request,
                    cmd_readTrim_allAFE_response& response,
                    Daphne& daphne,
                    std::string& response_str) {
  try {
    const uint32_t afe_board = request.afeblock();
    if (afe_board > 4) throw std::out_of_range("AFE out of range (0..4)");
    response.set_afeblock(afe_board);
    const uint32_t base = afe_board * 8;
    for (uint32_t ch = base; ch < base + 8; ++ch) response.add_trimvalues(daphne.getChTrimDictValue(ch));
    response_str = "AFE trims read successfully for AFE " + std::to_string(afe_board) + ".";
    return true;
  } catch (const std::exception& e) {
    response_str = std::string("Error reading AFE trims: ") + e.what();
    return false;
  }
}

bool readTrimSingleChannel(const cmd_readTrim_singleChannel& request,
                           cmd_readTrim_singleChannel_response& response,
                           Daphne& daphne,
                           std::string& response_str) {
  try {
    const uint32_t ch = request.trimchannel();
    const uint32_t val = daphne.getChTrimDictValue(ch);
    response.set_trimchannel(ch);
    response.set_trimvalue(val);
    response_str = "Trim read successfully for Channel " + std::to_string(ch) + " value " + std::to_string(val) + ".";
    return true;
  } catch (const std::exception& e) {
    response_str = std::string("Error reading channel trim: ") + e.what();
    return false;
  }
}

bool readOffsetAllChannels(const cmd_readOffset_allChannels&,
                           cmd_readOffset_allChannels_response& response,
                           Daphne& daphne,
                           std::string& response_str) {
  try {
    for (uint32_t ch = 0; ch < 40; ++ch) response.add_offsetvalues(daphne.getChOffsetDictValue(ch));
    response_str = "All channel offsets read successfully.";
    return true;
  } catch (const std::exception& e) {
    response_str = std::string("Error reading all channel offsets: ") + e.what();
    return false;
  }
}

bool readOffsetAllAFE(const cmd_readOffset_allAFE& request,
                      cmd_readOffset_allAFE_response& response,
                      Daphne& daphne,
                      std::string& response_str) {
  try {
    const uint32_t afe_board = request.afeblock();
    if (afe_board > 4) throw std::out_of_range("AFE out of range (0..4)");
    response.set_afeblock(afe_board);
    const uint32_t base = afe_board * 8;
    for (uint32_t ch = base; ch < base + 8; ++ch) response.add_offsetvalues(daphne.getChOffsetDictValue(ch));
    response_str = "AFE offsets read successfully for AFE " + std::to_string(afe_board) + ".";
    return true;
  } catch (const std::exception& e) {
    response_str = std::string("Error reading AFE offsets: ") + e.what();
    return false;
  }
}

bool readOffsetSingleChannel(const cmd_readOffset_singleChannel& request,
                             cmd_readOffset_singleChannel_response& response,
                             Daphne& daphne,
                             std::string& response_str) {
  try {
    const uint32_t ch = request.offsetchannel();
    const uint32_t val = daphne.getChOffsetDictValue(ch);
    response.set_offsetchannel(ch);
    response.set_offsetvalue(val);
    response_str =
        "Offset read successfully for Channel " + std::to_string(ch) + " value " + std::to_string(val) + ".";
    return true;
  } catch (const std::exception& e) {
    response_str = std::string("Error reading channel offset: ") + e.what();
    return false;
  }
}

bool readVbiasControl(const cmd_readVbiasControl&,
                      cmd_readVbiasControl_response& response,
                      Daphne& daphne,
                      std::string& response_str) {
  try {
    const uint32_t val = daphne.getBiasControlDictValue();
    response.set_vbiascontrolvalue(val);
    response_str = "Vbias control read successfully. Value: " + std::to_string(val) + ".";
    return true;
  } catch (const std::exception& e) {
    response_str = std::string("Error reading Vbias control: ") + e.what();
    return false;
  }
}

bool dumpSpybuffer(const DumpSpyBuffersRequest& request,
                   DumpSpyBuffersResponse& response,
                   Daphne& daphne,
                   std::string& response_str) {
  try {
    const uint32_t number_of_samples = request.numberofsamples();
    const uint32_t number_of_waveforms = request.numberofwaveforms();
    const auto& channel_list = request.channellist();

    for (const auto ch : channel_list) {
      if (ch > 39) throw std::invalid_argument("channel out of range (0..39)");
    }

    const bool software_trigger = request.softwaretrigger();
    if (number_of_samples == 0 || number_of_samples > 2048)
      throw std::invalid_argument("numberOfSamples out of range (1..2048)");

    const size_t channels = static_cast<size_t>(channel_list.size());
    if (channels == 0) throw std::invalid_argument("channelList is empty");

    const size_t words_per_waveform = static_cast<size_t>(number_of_samples) * channels;
    const size_t total_words = words_per_waveform * static_cast<size_t>(number_of_waveforms);
    if (number_of_waveforms != 0 && total_words / static_cast<size_t>(number_of_waveforms) != words_per_waveform) {
      throw std::invalid_argument("Requested dump size overflow");
    }
    const size_t total_bytes = total_words * sizeof(uint32_t);
    if (total_words != 0 && total_bytes / sizeof(uint32_t) != total_words) {
      throw std::invalid_argument("Requested dump size overflow");
    }
    if (total_words > static_cast<size_t>(std::numeric_limits<int>::max())) {
      throw std::invalid_argument("Requested dump too large for protobuf RepeatedField");
    }
    if (total_bytes > max_spybuffer_bytes()) {
      throw std::invalid_argument("Requested dump exceeds DAPHNE_MAX_SPYBUFFER_BYTES; use chunked dump");
    }

    auto* spy_buffer = daphne.getSpyBuffer();
    auto* front_end = daphne.getFrontEnd();

    response.mutable_data()->Resize(static_cast<int>(total_words), 0);
    auto* data_field = response.mutable_data();
    uint32_t* data_ptr = data_field->mutable_data();

    if (channel_list.size() == 1) {
      uint32_t ch = channel_list[0];
      const uint32_t afe_block = afe_definitions::AFE_board2PL_map.at(ch / 8);
      const uint32_t afe_channel = ch % 8;
      const uint32_t mapped_channel = afe_block * 8 + afe_channel;
      for (uint32_t j = 0; j < number_of_waveforms; ++j) {
        if (software_trigger) front_end->doTrigger();
        uint32_t* waveform_start = data_ptr + number_of_samples * j;
        spy_buffer->extractMappedDataBulkSIMD(waveform_start, number_of_samples, mapped_channel);
      }
    } else {
      std::vector<uint32_t> mapped_channels;
      mapped_channels.reserve(static_cast<size_t>(channel_list.size()));
      for (const auto ch : channel_list) {
        const uint32_t afe_block = afe_definitions::AFE_board2PL_map.at(ch / 8);
        const uint32_t afe_channel = ch % 8;
        mapped_channels.push_back(afe_block * 8 + afe_channel);
      }
      for (uint32_t j = 0; j < number_of_waveforms; ++j) {
        if (software_trigger) front_end->doTrigger();
        for (size_t i = 0; i < mapped_channels.size(); ++i) {
          const uint32_t mapped_channel = mapped_channels[i];
          uint32_t* waveform_start = data_ptr + number_of_samples * (j * mapped_channels.size() + i);
          spy_buffer->extractMappedDataBulkSIMD(waveform_start, number_of_samples, mapped_channel);
        }
      }
    }

    auto* resp_channel_list = response.mutable_channellist();
    resp_channel_list->Clear();
    resp_channel_list->Reserve(channel_list.size());
    for (const auto ch : channel_list) resp_channel_list->Add(ch);

    response.set_numberofsamples(number_of_samples);
    response.set_numberofwaveforms(number_of_waveforms);
    response.set_softwaretrigger(software_trigger);
    response_str = "OK";
    return true;
  } catch (const std::exception& e) {
    response_str = std::string("Error dumping spybuffer: ") + e.what();
    return false;
  }
}

bool alignAFE(const cmd_alignAFEs&,
              cmd_alignAFEs_response& response,
              Daphne& daphne,
              std::string& response_str) {
  try {
    constexpr uint32_t kExpectedFclkWord = 0x00FF00FFu;
    constexpr uint32_t kVerificationReads = 4;
    constexpr uint32_t afe_num = 5;
    std::vector<uint32_t> delay(afe_num, 0);
    std::vector<uint32_t> bitslip(afe_num, 0);

    daphne.getFrontEnd()->resetDelayCtrlValues();
    daphne.getFrontEnd()->doResetDelayCtrl();
    daphne.getFrontEnd()->doResetSerDesCtrl();
    daphne.getFrontEnd()->setEnableDelayVtc(0);

    if (!daphne.getFrontEnd()->waitForDelayCtrlReady()) {
      throw std::runtime_error("DELAYCTRL_READY did not assert after reset; refusing alignment.");
    }

    std::string report;
    std::vector<std::string> failures;
    for (uint32_t afe_block = 0; afe_block < afe_num; ++afe_block) {
      std::string delay_dbg;
      std::string bitslip_dbg;
      bool matched = false;
      daphne.setBestDelay(afe_block, 512, &delay_dbg);
      const uint32_t aligned_word = daphne.setBestBitslip(afe_block, 16, &bitslip_dbg, &matched);
      report += delay_dbg + bitslip_dbg;

      bool verification_ok = matched;
      report += "  VERIFY_SCAN:";
      for (uint32_t i = 0; i < kVerificationReads; ++i) {
        daphne.getFrontEnd()->doTrigger();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const uint32_t verify_word = daphne.getSpyBuffer()->getFrameClock(afe_block, 0);
        report += " [" + std::to_string(i) + "]=0x" +
                  [&]() {
                    std::ostringstream os;
                    os << std::hex << std::uppercase << verify_word;
                    return os.str();
                  }();
        if (verify_word != kExpectedFclkWord) {
          verification_ok = false;
        }
      }
      report += "\n";

      if (!matched || aligned_word != kExpectedFclkWord || !verification_ok) {
        failures.push_back(
            "AFE_" + std::to_string(afe_block) +
            " did not converge to stable 0x00FF00FF alignment.");
      }
    }

    response.clear_delay();
    response.clear_bitslip();
    for (uint32_t afe_block = 0; afe_block < afe_num; ++afe_block) {
      delay[afe_block] = daphne.getFrontEnd()->getDelay(afe_block);
      bitslip[afe_block] = daphne.getFrontEnd()->getBitslip(afe_block);
      response.add_delay(delay[afe_block]);
      response.add_bitslip(bitslip[afe_block]);
      report += "AFE_" + std::to_string(afe_block) + "\nDELAY: " + std::to_string(delay[afe_block]) +
                "\nBITSLIP: " + std::to_string(bitslip[afe_block]) + "\n";
    }

    daphne.getFrontEnd()->setEnableDelayVtc(1);
    if (!failures.empty()) {
      response_str = "AFE alignment failed.\n";
      for (const auto& failure : failures) {
        response_str += failure + "\n";
      }
      response_str += "\n" + report;
      return false;
    }

    response_str = "AFEs aligned.\n" + report;
    return true;
  } catch (const std::exception& e) {
    response_str = std::string("Error aligning AFE: ") + e.what();
    return false;
  }
}

bool writeAFEFunction(const cmd_writeAFEFunction& request,
                      cmd_writeAFEFunction_response& response,
                      Daphne& daphne,
                      std::string& response_str) {
  try {
    uint32_t afe_block = afe_definitions::AFE_board2PL_map.at(request.afeblock());
    if (afe_block > 4) throw std::invalid_argument("AFE out of range (0..4)");
    const std::string afe_function_name = request.function();
    const uint32_t conf_value = request.configvalue();
    const uint32_t returned = daphne.getAfe()->setAFEFunction(afe_block, afe_function_name, conf_value);
    response.set_function(afe_function_name);
    response.set_configvalue(returned);
    response.set_afeblock(afe_block);
    response_str = "Function " + afe_function_name + " configured correctly for AFE " +
                   std::to_string(afe_definitions::AFE_PL2board_map.at(afe_block)) + ". Returned value: " +
                   std::to_string(returned) + ".";
    return true;
  } catch (const std::exception& e) {
    response_str = std::string("Error writing AFE function: ") + e.what();
    return false;
  }
}

bool setAFEReset(const cmd_setAFEReset& request,
                 cmd_setAFEReset_response& response,
                 Daphne& daphne,
                 std::string& response_str) {
  try {
    const bool reset_value = request.resetvalue();
    const uint32_t returned = daphne.getAfe()->setReset(static_cast<uint32_t>(reset_value));
    response.set_resetvalue(returned);
    response_str = "AFEs reset register written with value " + std::to_string(reset_value) +
                   ". Returned value: " + std::to_string(returned) + ".";
    return true;
  } catch (const std::exception& e) {
    response_str = std::string("Error resetting AFEs: ") + e.what();
    return false;
  }
}

bool doAFEReset(const cmd_doAFEReset&,
               cmd_doAFEReset_response&,
               Daphne& daphne,
               std::string& response_str) {
  try {
    (void)daphne.getAfe()->doReset();
    response_str = "AFEs doreset command successful.";
    return true;
  } catch (const std::exception& e) {
    response_str = std::string("Error resetting AFEs: ") + e.what();
    return false;
  }
}

bool setAFEPowerState(const cmd_setAFEPowerState& request,
                      cmd_setAFEPowerState_response& response,
                      Daphne& daphne,
                      std::string& response_str) {
  try {
    const bool power_state_value = request.powerstate();
    const uint32_t returned = daphne.getAfe()->setPowerState(static_cast<uint32_t>(power_state_value));
    response.set_powerstate(returned);
    response_str = "AFEs powerstate register written with value " + std::to_string(power_state_value) +
                   ". Returned value: " + std::to_string(returned) + ".";
    return true;
  } catch (const std::exception& e) {
    response_str = std::string("Error setting AFEs power state: ") + e.what();
    return false;
  }
}

bool doSoftwareTrigger(const cmd_doSoftwareTrigger&,
                       cmd_doSoftwareTrigger_response&,
                       Daphne& daphne,
                       std::string& response_str) {
  try {
    daphne.getFrontEnd()->doTrigger();
    response_str = "Software trigger executed.";
    return true;
  } catch (const std::exception& e) {
    response_str = std::string("Error executing software trigger: ") + e.what();
    return false;
  }
}

bool readBiasVoltageMonitor(const cmd_readBiasVoltageMonitor& request,
                            cmd_readBiasVoltageMonitor_response& response,
                            Daphne& daphne,
                            std::string& response_msg) {
  const uint32_t afe_block = request.afeblock();
  const std::array<double, 5> biases = {
      daphne._VBIAS_0_voltage.load(),
      daphne._VBIAS_1_voltage.load(),
      daphne._VBIAS_2_voltage.load(),
      daphne._VBIAS_3_voltage.load(),
      daphne._VBIAS_4_voltage.load(),
  };

  if (afe_block >= biases.size()) {
    response_msg = "AFE block out of range (0..4)";
    return false;
  }

  const double bias_volts = biases[afe_block];
  response.set_afeblock(afe_block);
  response.set_biasvoltagevalue(static_cast<uint32_t>(std::lround(bias_volts * 1000.0)));

  std::ostringstream oss;
  oss << std::fixed << std::setprecision(5);
  oss << "3V3PDS:" << daphne._3V3PDS_voltage.load() << " V, "
      << "1V8PDS:" << daphne._1V8PDS_voltage.load() << " V. "
      << "3V3A:" << daphne._3V3A_voltage.load() << " V, "
      << "1V8A:" << daphne._1V8A_voltage.load() << " V, "
      << "-5VA:" << daphne._n5VA_voltage.load() << " V. "
      << "BIAS0:" << biases[0] << " V, "
      << "BIAS1:" << biases[1] << " V, "
      << "BIAS2:" << biases[2] << " V, "
      << "BIAS3:" << biases[3] << " V, "
      << "BIAS4:" << biases[4] << " V.";

  response_msg = oss.str();
  return true;
}

// --------------HD Mezzanine helper functions-------------------------

bool setHDMezzBlockEnable(const cmd_setHDMezzBlockEnable& request,
                          cmd_setHDMezzBlockEnable_response& response,
                          Daphne& daphne,
                          std::string& response_str) {
  try {
    const uint32_t afeBlock = request.afeblock();
    const bool enable = request.enable();
    if (afeBlock > 4) throw std::invalid_argument("HD mezzanine block out of range (0..4)");
    if(!daphne.getHDMezzDriver()) throw std::runtime_error("HD mezzanine driver not initialized");
    I2C2BusGuard bus_guard(daphne);
    daphne.getHDMezzDriver()->enableAfeBlock(afeBlock, enable);
    response.set_afeblock(afeBlock);
    response.set_enable(enable);
    response_str = "HD mezzanine block " + std::to_string(afeBlock) + " enable state set to " +
                   std::to_string(enable) + ".";
    return true;
  } catch (const std::exception& e) {
    response_str = std::string("Error setting HD mezzanine block enable state: ") + e.what();
    return false;
  }
}

bool configureHDMezzBlock(const cmd_configureHDMezzBlock& request,
                         cmd_configureHDMezzBlock_response& response,
                         Daphne& daphne,
                         std::string& response_str) {
  try {
    const uint32_t afeBlock = request.afeblock();
    const float r_shunt_5V = request.r_shunt_5v();
    const float r_shunt_3V3 = request.r_shunt_3v3();
    const float max_current_5V_scale = request.max_current_5v_scale();
    const float max_current_3V3_scale = request.max_current_3v3_scale();
    const float max_current_5V_shutdown = request.max_current_5v_shutdown();
    const float max_current_3V3_shutdown = request.max_current_3v3_shutdown();
    if (afeBlock > 4) throw std::invalid_argument("HD mezzanine block out of range (0..4)");
    if(!daphne.getHDMezzDriver()) throw std::runtime_error("HD mezzanine driver not initialized");
    I2C2BusGuard bus_guard(daphne);
    daphne.getHDMezzDriver()->setRShunt(afeBlock, r_shunt_5V, "5V");
    daphne.getHDMezzDriver()->setRShunt(afeBlock, r_shunt_3V3, "3V3");
    daphne.getHDMezzDriver()->setMaxCurrentScale(afeBlock, max_current_5V_scale, "5V");
    daphne.getHDMezzDriver()->setMaxCurrentScale(afeBlock, max_current_3V3_scale, "3V3");
    daphne.getHDMezzDriver()->setMaxCurrentShutdown(afeBlock, max_current_5V_shutdown, "5V");
    daphne.getHDMezzDriver()->setMaxCurrentShutdown(afeBlock, max_current_3V3_shutdown, "3V3");
    daphne.getHDMezzDriver()->configureHdMezzAfeBlock(afeBlock);
    response.set_afeblock(afeBlock);
    response.set_r_shunt_5v(r_shunt_5V);
    response.set_r_shunt_3v3(r_shunt_3V3);
    response.set_max_current_5v_scale(max_current_5V_scale);
    response.set_max_current_3v3_scale(max_current_3V3_scale);
    response.set_max_current_5v_shutdown(max_current_5V_shutdown);
    response.set_max_current_3v3_shutdown(max_current_3V3_shutdown);
    response_str = "HD mezzanine block " + std::to_string(afeBlock) + " configured with values " +
                   std::to_string(r_shunt_5V) + ", " + std::to_string(r_shunt_3V3) + ", " +
                   std::to_string(max_current_5V_scale) + ", " + std::to_string(max_current_3V3_scale) + ", " +
                   std::to_string(max_current_5V_shutdown) + ", " + std::to_string(max_current_3V3_shutdown) + ".";
    return true;
  } catch (const std::exception& e) {
    response_str = std::string("Error configuring HD mezzanine block: ") + e.what();
    return false;
  }
}

bool readHDMezzBlockConfig(const cmd_readHDMezzBlockConfig& request,
                          cmd_readHDMezzBlockConfig_response& response,
                          Daphne& daphne,
                          std::string& response_str) {
  try {
    const uint32_t afeBlock = request.afeblock();
    if (afeBlock > 4) throw std::invalid_argument("HD mezzanine block out of range (0..4)");
    if(!daphne.getHDMezzDriver()) throw std::runtime_error("HD mezzanine driver not initialized");
    I2C2BusGuard bus_guard(daphne);
    response.set_afeblock(afeBlock);
    response.set_r_shunt_5v(daphne.getHDMezzDriver()->getRShunt(afeBlock, "5V"));
    response.set_r_shunt_3v3(daphne.getHDMezzDriver()->getRShunt(afeBlock, "3V3"));
    response.set_max_current_5v_scale(daphne.getHDMezzDriver()->getMaxCurrentScale(afeBlock, "5V"));
    response.set_max_current_3v3_scale(daphne.getHDMezzDriver()->getMaxCurrentScale(afeBlock, "3V3"));
    response.set_max_current_5v_shutdown(daphne.getHDMezzDriver()->getMaxCurrentShutdown(afeBlock, "5V"));
    response.set_max_current_3v3_shutdown(daphne.getHDMezzDriver()->getMaxCurrentShutdown(afeBlock, "3V3"));
    response.set_max_power_5v(daphne.getHDMezzDriver()->getMaxPower(afeBlock, "5V"));
    response.set_max_power_3v3(daphne.getHDMezzDriver()->getMaxPower(afeBlock, "3V3"));
    response.set_current_lsb_5v(daphne.getHDMezzDriver()->getCurrentLsb(afeBlock, "5V"));
    response.set_current_lsb_3v3(daphne.getHDMezzDriver()->getCurrentLsb(afeBlock, "3V3"));
    response.set_shunt_cal_5v(daphne.getHDMezzDriver()->getShuntCal(afeBlock, "5V"));
    response.set_shunt_cal_3v3(daphne.getHDMezzDriver()->getShuntCal(afeBlock, "3V3"));
    response_str = "HD mezzanine block " + std::to_string(afeBlock) + " configuration read successfully.";
    return true;
  } catch (const std::exception& e) {
    response_str = std::string("Error reading HD mezzanine block configuration: ") + e.what();
    return false;
  }
}

bool setHDMezzPowerStates(const cmd_setHDMezzPowerStates& request,
                        cmd_setHDMezzPowerStates_response& response,
                        Daphne& daphne,
                        std::string& response_str) {
  try {
    const uint32_t afeBlock = request.afeblock();
    if (afeBlock > 4) throw std::invalid_argument("HD mezzanine block out of range (0..4)");
    if(!daphne.getHDMezzDriver()) throw std::runtime_error("HD mezzanine driver not initialized");
    const bool power_5v = request.power5v();
    const bool power_3v3 = request.power3v3();
    if (afeBlock > 4) throw std::invalid_argument("HD mezzanine block out of range (0..4)");
    I2C2BusGuard bus_guard(daphne);
    daphne.getHDMezzDriver()->powerOn_HDMezzAfeBlock(afeBlock, power_5v, "5V");
    daphne.getHDMezzDriver()->powerOn_HDMezzAfeBlock(afeBlock, power_3v3, "3V3");
    response.set_afeblock(afeBlock);
    response.set_power5v(power_5v);
    response.set_power3v3(power_3v3);
    response_str = "HD mezzanine block " + std::to_string(afeBlock) + " power states set to 5V: " + std::to_string(power_5v) +
                   ", 3V3: " + std::to_string(power_3v3) + ".";
    return true;
  } catch (const std::exception& e) {
    response_str = std::string("Error setting HD mezzanine block power states: ") + e.what();
    return false;
  }
}

bool readHDMezzStatus(const cmd_readHDMezzStatus& request,
                    cmd_readHDMezzStatus_response& response,
                    Daphne& daphne,
                    std::string& response_str) {
  try {
    const uint32_t afeBlock = request.afeblock();
    if (afeBlock > 4) throw std::invalid_argument("HD mezzanine block out of range (0..4)");
    if(!daphne.getHDMezzDriver()) throw std::runtime_error("HD mezzanine driver not initialized");
    response.set_afeblock(afeBlock);
    response.set_power5v(daphne.HDMezz_5V_is_powered[afeBlock].load());
    response.set_power3v3(daphne.HDMezz_3V3_is_powered[afeBlock].load());
    response.set_measured_voltage5v(daphne.HDMezz_5V_voltage[afeBlock].load());
    response.set_measured_voltage3v3(daphne.HDMezz_3V3_voltage[afeBlock].load());
    response.set_measured_current5v(daphne.HDMezz_5V_current[afeBlock].load());
    response.set_measured_current3v3(daphne.HDMezz_3V3_current[afeBlock].load());
    response.set_measured_power5v(daphne.HDMezz_5V_power[afeBlock].load());
    response.set_measured_power3v3(daphne.HDMezz_3V3_power[afeBlock].load());
    response.set_alert_5v(daphne.HDMezz_5V_alert[afeBlock].load());
    response.set_alert_3v3(daphne.HDMezz_3V3_alert[afeBlock].load());
    response_str = "HD mezzanine block " + std::to_string(afeBlock) + " status read successfully.";
    return true;
  } catch (const std::exception& e) {
    response_str = std::string("Error reading HD mezzanine block status: ") + e.what();
    return false;
  }
}

bool clearHDMezzAlertFlag(const cmd_clearHDMezzAlertFlag& request,
                        cmd_clearHDMezzAlertFlag_response& response,
                        Daphne& daphne,
                        std::string& response_str) {
  try {
    const uint32_t afeBlock = request.afeblock();
    if (afeBlock > 4) throw std::invalid_argument("HD mezzanine block out of range (0..4)");
    if(!daphne.getHDMezzDriver()) throw std::runtime_error("HD mezzanine driver not initialized");
    I2C2BusGuard bus_guard(daphne);
    daphne.HDMezz_5V_alert[afeBlock].store(false);
    daphne.HDMezz_3V3_alert[afeBlock].store(false);
    response.set_afeblock(afeBlock);
    response_str = "HD mezzanine block " + std::to_string(afeBlock) + " alert flags cleared.";
    return true;
  } catch (const std::exception& e) {
    response_str = std::string("Error clearing HD mezzanine block alert flags: ") + e.what();
    return false;
  }
}

template <typename Msg>
std::string serialize_or_empty(const Msg& msg) {
  std::string out;
  msg.SerializeToString(&out);
  return out;
}

template <typename Msg>
std::string serialize_error_with_success_field(Msg& msg, const std::string& err) {
  msg.set_success(false);
  msg.set_message(err);
  return serialize_or_empty(msg);
}

}  // namespace

std::unordered_map<daphne::MessageTypeV2, V2Handler> make_v2_handlers() {
  using daphne::MessageTypeV2;

  std::unordered_map<MessageTypeV2, V2Handler> handlers;

  handlers[daphne::MT2_CONFIGURE_FE_REQ] = [](const std::string& in, std::string& out, Daphne& d) {
    ConfigureRequest req;
    ConfigureResponse resp;
    if (!req.ParseFromString(in)) {
      resp.set_success(false);
      resp.set_message("Bad ConfigureRequest payload");
      out = serialize_or_empty(resp);
      return;
    }

    std::string msg;
    bool ok = configureDaphne(req, d, msg);
    if (ok && auto_align_enabled()) {
      cmd_alignAFEs a_req;
      cmd_alignAFEs_response a_resp;
      std::string align_msg;
      const bool ok_align = alignAFE(a_req, a_resp, d, align_msg);
      msg += "\n\n[ALIGN_AFE]\n" + align_msg;
      ok = ok && ok_align;
    } else if (!auto_align_enabled()) {
      msg += "\n\n[ALIGN_AFE] skipped (DAPHNE_SKIP_ALIGN_AFTER_CONFIGURE set)";
    }

    resp.set_success(ok);
    resp.set_message(msg);
    out = serialize_or_empty(resp);
  };

  handlers[daphne::MT2_CONFIGURE_CLKS_REQ] = [](const std::string& in, std::string& out, Daphne&) {
    ConfigureCLKsRequest req;
    ConfigureCLKsResponse resp;
    if (!req.ParseFromString(in)) {
      resp.set_success(false);
      resp.set_message("Bad ConfigureCLKsRequest payload");
      out = serialize_or_empty(resp);
      return;
    }

    std::string info;
    const bool ok_clk = set_clock_source_and_mmcm_reset(req.ctrl_ep_clk(), req.reset_mmcm1(), info);
    const bool ok_ep =
        set_endpoint_addr_and_reset(static_cast<uint16_t>(req.id()), req.reset_endpoint(), info);

    resp.set_success(ok_clk && ok_ep);
    resp.set_message(info);
    out = serialize_or_empty(resp);
  };

  handlers[daphne::MT2_READ_TRIGGER_COUNTERS_REQ] = [](const std::string& in, std::string& out, Daphne&) {
    ReadTriggerCountersRequest req;
    ReadTriggerCountersResponse resp;
    if (!req.ParseFromString(in)) {
      resp.set_success(false);
      resp.set_message("Bad ReadTriggerCountersRequest payload");
      out = serialize_or_empty(resp);
      return;
    }

    std::vector<uint32_t> chs;
    if (req.channels_size() == 0) {
      chs.resize(40);
      std::iota(chs.begin(), chs.end(), 0);
    } else {
      chs.assign(req.channels().begin(), req.channels().end());
    }

    uint32_t base = trigregs::PHYS_BASE;
    if (req.base_addr() != 0) base = static_cast<uint32_t>(req.base_addr());

    std::string err;
    const bool ok = read_counters_raw(base, chs, resp, err);
    resp.set_success(ok);
    resp.set_message(ok ? "OK" : (std::string("Counters read error: ") + err));
    out = serialize_or_empty(resp);
  };

  handlers[daphne::MT2_READ_TEST_REG_REQ] = [](const std::string&, std::string& out, Daphne&) {
    TestRegResponse resp;
    resp.set_value(0xDEADBEEF);
    resp.set_message("ok");
    out = serialize_or_empty(resp);
  };

  handlers[daphne::MT2_READ_GENERAL_INFO_REQ] = [](const std::string& in, std::string& out, Daphne& d) {
    InfoRequest req;
    if (!req.ParseFromString(in)) {
      GeneralInfo resp;
      out = serialize_or_empty(resp);
      return;
    }

    GeneralInfo resp;
    resp.set_v_bias_0(d._VBIAS_0_voltage.load());
    resp.set_v_bias_1(d._VBIAS_1_voltage.load());
    resp.set_v_bias_2(d._VBIAS_2_voltage.load());
    resp.set_v_bias_3(d._VBIAS_3_voltage.load());
    resp.set_v_bias_4(d._VBIAS_4_voltage.load());
    resp.set_power_minus5v(d._n5VA_voltage.load());
    resp.set_power_plus2p5v(d._3V3PDS_voltage.load());
    resp.set_power_ce(d._1V8A_voltage.load());
    out = serialize_or_empty(resp);
  };

  handlers[daphne::MT2_READ_BIAS_VOLTAGE_MONITOR_REQ] = [](const std::string& in, std::string& out, Daphne& d) {
    cmd_readBiasVoltageMonitor req;
    cmd_readBiasVoltageMonitor_response resp;
    if (!req.ParseFromString(in)) {
      out = serialize_error_with_success_field(resp, "Bad cmd_readBiasVoltageMonitor payload");
      return;
    }

    std::string msg;
    const bool ok = readBiasVoltageMonitor(req, resp, d, msg);
    resp.set_success(ok);
    resp.set_message(msg);
    out = serialize_or_empty(resp);
  };

  handlers[daphne::MT2_READ_CURRENT_MONITOR_REQ] = [](const std::string& in, std::string& out, Daphne&) {
    cmd_readCurrentMonitor req;
    cmd_readCurrentMonitor_response resp;
    if (!req.ParseFromString(in)) {
      out = serialize_error_with_success_field(resp, "Bad cmd_readCurrentMonitor payload");
      return;
    }

    uint32_t current_value = 0;
    std::string msg;
    const bool ok = read_current_monitor_raw(req, current_value, msg);
    resp.set_success(ok);
    resp.set_message(msg);
    resp.set_currentmonitorchannel(req.currentmonitorchannel());
    resp.set_currentvalue(current_value);
    out = serialize_or_empty(resp);
  };

  handlers[daphne::MT2_READ_AFE_REG_REQ] = [](const std::string& in, std::string& out, Daphne& d) {
    cmd_readAFEReg req;
    cmd_readAFEReg_response resp;
    if (!req.ParseFromString(in)) {
      out = serialize_error_with_success_field(resp, "Bad cmd_readAFEReg payload");
      return;
    }
    std::string msg;
    const bool ok = readAFEReg(req, resp, d, msg);
    resp.set_success(ok);
    resp.set_message(msg);
    out = serialize_or_empty(resp);
  };

  handlers[daphne::MT2_READ_AFE_VGAIN_REQ] = [](const std::string& in, std::string& out, Daphne& d) {
    cmd_readAFEVgain req;
    cmd_readAFEVgain_response resp;
    if (!req.ParseFromString(in)) {
      out = serialize_error_with_success_field(resp, "Bad cmd_readAFEVgain payload");
      return;
    }
    std::string msg;
    const bool ok = readAFEVgain(req, resp, d, msg);
    resp.set_success(ok);
    resp.set_message(msg);
    out = serialize_or_empty(resp);
  };

  handlers[daphne::MT2_READ_AFE_BIAS_SET_REQ] = [](const std::string& in, std::string& out, Daphne& d) {
    cmd_readAFEBiasSet req;
    cmd_readAFEBiasSet_response resp;
    if (!req.ParseFromString(in)) {
      out = serialize_error_with_success_field(resp, "Bad cmd_readAFEBiasSet payload");
      return;
    }
    std::string msg;
    const bool ok = readAFEBiasSet(req, resp, d, msg);
    resp.set_success(ok);
    resp.set_message(msg);
    out = serialize_or_empty(resp);
  };

  handlers[daphne::MT2_READ_TRIM_ALL_CH_REQ] = [](const std::string& in, std::string& out, Daphne& d) {
    cmd_readTrim_allChannels req;
    cmd_readTrim_allChannels_response resp;
    if (!req.ParseFromString(in)) {
      out = serialize_error_with_success_field(resp, "Bad cmd_readTrim_allChannels payload");
      return;
    }
    std::string msg;
    const bool ok = readTrimAllChannels(req, resp, d, msg);
    resp.set_success(ok);
    resp.set_message(msg);
    out = serialize_or_empty(resp);
  };

  handlers[daphne::MT2_READ_TRIM_ALL_AFE_REQ] = [](const std::string& in, std::string& out, Daphne& d) {
    cmd_readTrim_allAFE req;
    cmd_readTrim_allAFE_response resp;
    if (!req.ParseFromString(in)) {
      out = serialize_error_with_success_field(resp, "Bad cmd_readTrim_allAFE payload");
      return;
    }
    std::string msg;
    const bool ok = readTrimAllAFE(req, resp, d, msg);
    resp.set_success(ok);
    resp.set_message(msg);
    out = serialize_or_empty(resp);
  };

  handlers[daphne::MT2_READ_TRIM_CH_REQ] = [](const std::string& in, std::string& out, Daphne& d) {
    cmd_readTrim_singleChannel req;
    cmd_readTrim_singleChannel_response resp;
    if (!req.ParseFromString(in)) {
      out = serialize_error_with_success_field(resp, "Bad cmd_readTrim_singleChannel payload");
      return;
    }
    std::string msg;
    const bool ok = readTrimSingleChannel(req, resp, d, msg);
    resp.set_success(ok);
    resp.set_message(msg);
    out = serialize_or_empty(resp);
  };

  handlers[daphne::MT2_READ_OFFSET_ALL_CH_REQ] = [](const std::string& in, std::string& out, Daphne& d) {
    cmd_readOffset_allChannels req;
    cmd_readOffset_allChannels_response resp;
    if (!req.ParseFromString(in)) {
      out = serialize_error_with_success_field(resp, "Bad cmd_readOffset_allChannels payload");
      return;
    }
    std::string msg;
    const bool ok = readOffsetAllChannels(req, resp, d, msg);
    resp.set_success(ok);
    resp.set_message(msg);
    out = serialize_or_empty(resp);
  };

  handlers[daphne::MT2_READ_OFFSET_ALL_AFE_REQ] = [](const std::string& in, std::string& out, Daphne& d) {
    cmd_readOffset_allAFE req;
    cmd_readOffset_allAFE_response resp;
    if (!req.ParseFromString(in)) {
      out = serialize_error_with_success_field(resp, "Bad cmd_readOffset_allAFE payload");
      return;
    }
    std::string msg;
    const bool ok = readOffsetAllAFE(req, resp, d, msg);
    resp.set_success(ok);
    resp.set_message(msg);
    out = serialize_or_empty(resp);
  };

  handlers[daphne::MT2_READ_OFFSET_CH_REQ] = [](const std::string& in, std::string& out, Daphne& d) {
    cmd_readOffset_singleChannel req;
    cmd_readOffset_singleChannel_response resp;
    if (!req.ParseFromString(in)) {
      out = serialize_error_with_success_field(resp, "Bad cmd_readOffset_singleChannel payload");
      return;
    }
    std::string msg;
    const bool ok = readOffsetSingleChannel(req, resp, d, msg);
    resp.set_success(ok);
    resp.set_message(msg);
    out = serialize_or_empty(resp);
  };

  handlers[daphne::MT2_READ_VBIAS_CONTROL_REQ] = [](const std::string& in, std::string& out, Daphne& d) {
    cmd_readVbiasControl req;
    cmd_readVbiasControl_response resp;
    if (!req.ParseFromString(in)) {
      out = serialize_error_with_success_field(resp, "Bad cmd_readVbiasControl payload");
      return;
    }
    std::string msg;
    const bool ok = readVbiasControl(req, resp, d, msg);
    resp.set_success(ok);
    resp.set_message(msg);
    out = serialize_or_empty(resp);
  };

  handlers[daphne::MT2_WRITE_AFE_REG_REQ] = [](const std::string& in, std::string& out, Daphne& d) {
    cmd_writeAFEReg req;
    cmd_writeAFEReg_response resp;
    if (!req.ParseFromString(in)) {
      out = serialize_error_with_success_field(resp, "Bad cmd_writeAFEReg payload");
      return;
    }

    std::string msg;
    uint32_t rb = 0;
    const bool ok = writeAFERegister(req, d, msg, rb);
    resp.set_success(ok);
    resp.set_message(msg);
    resp.set_afeblock(req.afeblock());
    resp.set_regaddress(req.regaddress());
    resp.set_regvalue(rb);
    out = serialize_or_empty(resp);
  };

  handlers[daphne::MT2_WRITE_AFE_VGAIN_REQ] = [](const std::string& in, std::string& out, Daphne& d) {
    cmd_writeAFEVGAIN req;
    cmd_writeAFEVGAIN_response resp;
    if (!req.ParseFromString(in)) {
      out = serialize_error_with_success_field(resp, "Bad cmd_writeAFEVGAIN payload");
      return;
    }

    std::string msg;
    uint32_t rb = 0;
    const bool ok = writeAFEVgain(req, d, msg, rb);
    resp.set_success(ok);
    resp.set_message(msg);
    resp.set_afeblock(req.afeblock());
    resp.set_vgainvalue(rb);
    out = serialize_or_empty(resp);
  };

  handlers[daphne::MT2_WRITE_AFE_ATTENUATION_REQ] = [](const std::string& in, std::string& out, Daphne& d) {
    cmd_writeAFEAttenuation req;
    cmd_writeAFEAttenuation_response resp;
    if (!req.ParseFromString(in)) {
      out = serialize_error_with_success_field(resp, "Bad cmd_writeAFEAttenuation payload");
      return;
    }

    std::string msg;
    uint32_t rb = 0;
    const bool ok = writeAFEAttenuation(req, d, msg, rb);
    resp.set_success(ok);
    resp.set_message(msg);
    resp.set_afeblock(req.afeblock());
    resp.set_attenuation(rb);
    out = serialize_or_empty(resp);
  };

  handlers[daphne::MT2_WRITE_AFE_BIAS_SET_REQ] = [](const std::string& in, std::string& out, Daphne& d) {
    cmd_writeAFEBiasSet req;
    cmd_writeAFEBiasSet_response resp;
    if (!req.ParseFromString(in)) {
      out = serialize_error_with_success_field(resp, "Bad cmd_writeAFEBiasSet payload");
      return;
    }

    std::string msg;
    uint32_t rb = 0;
    const bool ok = writeAFEBiasVoltage(req, d, msg, rb);
    resp.set_success(ok);
    resp.set_message(msg);
    resp.set_afeblock(req.afeblock());
    resp.set_biasvalue(rb);
    out = serialize_or_empty(resp);
  };

  handlers[daphne::MT2_WRITE_TRIM_CH_REQ] = [](const std::string& in, std::string& out, Daphne& d) {
    cmd_writeTrim_singleChannel req;
    cmd_writeTrim_singleChannel_response resp;
    if (!req.ParseFromString(in)) {
      out = serialize_error_with_success_field(resp, "Bad cmd_writeTrim_singleChannel payload");
      return;
    }

    std::string msg;
    uint32_t rb = 0;
    const bool ok = writeChannelTrim(req, d, msg, rb);
    resp.set_success(ok);
    resp.set_message(msg);
    resp.set_trimchannel(req.trimchannel());
    resp.set_trimvalue(rb);
    resp.set_trimgain(req.trimgain());
    out = serialize_or_empty(resp);
  };

  handlers[daphne::MT2_WRITE_TRIM_ALL_CH_REQ] = [](const std::string& in, std::string& out, Daphne& d) {
    cmd_writeTRIM_allChannels req;
    cmd_writeTRIM_allChannels_response resp;
    if (!req.ParseFromString(in)) {
      out = serialize_error_with_success_field(resp, "Bad cmd_writeTRIM_allChannels payload");
      return;
    }

    try {
      if (req.trimvalue() > 4095) throw std::invalid_argument("trimValue out of range (0..4095)");
      for (uint32_t ch = 0; ch < 40; ++ch) {
        const uint32_t afe_block = afe_definitions::AFE_board2PL_map.at(ch / 8);
        d.getDac()->setDacTrim(afe_block, ch % 8, req.trimvalue(), req.trimgain(), false);
        d.setChTrimDictValue(ch, req.trimvalue());
      }
      resp.set_success(true);
      resp.set_message("OK");
    } catch (const std::exception& e) {
      resp.set_success(false);
      resp.set_message(e.what());
    }
    resp.set_trimvalue(req.trimvalue());
    resp.set_trimgain(req.trimgain());
    out = serialize_or_empty(resp);
  };

  handlers[daphne::MT2_WRITE_TRIM_ALL_AFE_REQ] = [](const std::string& in, std::string& out, Daphne& d) {
    cmd_writeTrim_allAFE req;
    cmd_writeTrim_allAFE_response resp;
    if (!req.ParseFromString(in)) {
      out = serialize_error_with_success_field(resp, "Bad cmd_writeTrim_allAFE payload");
      return;
    }

    try {
      if (req.trimvalue() > 4095) throw std::invalid_argument("trimValue out of range (0..4095)");
      const uint32_t afe_board = req.afeblock();
      if (afe_board > 4) throw std::invalid_argument("afeBlock out of range (0..4)");
      const uint32_t afe_block = afe_definitions::AFE_board2PL_map.at(afe_board);
      for (uint32_t idx = 0; idx < 8; ++idx) {
        d.getDac()->setDacTrim(afe_block, idx, req.trimvalue(), req.trimgain(), false);
        d.setChTrimDictValue(afe_board * 8 + idx, req.trimvalue());
      }
      resp.set_success(true);
      resp.set_message("OK");
    } catch (const std::exception& e) {
      resp.set_success(false);
      resp.set_message(e.what());
    }
    resp.set_afeblock(req.afeblock());
    resp.set_trimvalue(req.trimvalue());
    resp.set_trimgain(req.trimgain());
    out = serialize_or_empty(resp);
  };

  handlers[daphne::MT2_WRITE_OFFSET_CH_REQ] = [](const std::string& in, std::string& out, Daphne& d) {
    cmd_writeOFFSET_singleChannel req;
    cmd_writeOFFSET_singleChannel_response resp;
    if (!req.ParseFromString(in)) {
      out = serialize_error_with_success_field(resp, "Bad cmd_writeOFFSET_singleChannel payload");
      return;
    }

    std::string msg;
    uint32_t rb = 0;
    const bool ok = writeChannelOffset(req, d, msg, rb);
    resp.set_success(ok);
    resp.set_message(msg);
    resp.set_offsetchannel(req.offsetchannel());
    resp.set_offsetvalue(rb);
    resp.set_offsetgain(req.offsetgain());
    out = serialize_or_empty(resp);
  };

  handlers[daphne::MT2_WRITE_OFFSET_ALL_CH_REQ] = [](const std::string& in, std::string& out, Daphne& d) {
    cmd_writeOFFSET_allChannels req;
    cmd_writeOFFSET_allChannels_response resp;
    if (!req.ParseFromString(in)) {
      out = serialize_error_with_success_field(resp, "Bad cmd_writeOFFSET_allChannels payload");
      return;
    }

    try {
      if (req.offsetvalue() > 4095) throw std::invalid_argument("offsetValue out of range (0..4095)");
      for (uint32_t ch = 0; ch < 40; ++ch) {
        const uint32_t afe_block = afe_definitions::AFE_board2PL_map.at(ch / 8);
        d.getDac()->setDacOffset(afe_block, ch % 8, req.offsetvalue(), req.offsetgain(), false);
        d.setChOffsetDictValue(ch, req.offsetvalue());
      }
      resp.set_success(true);
      resp.set_message("OK");
    } catch (const std::exception& e) {
      resp.set_success(false);
      resp.set_message(e.what());
    }
    resp.set_offsetvalue(req.offsetvalue());
    resp.set_offsetgain(req.offsetgain());
    out = serialize_or_empty(resp);
  };

  handlers[daphne::MT2_WRITE_OFFSET_ALL_AFE_REQ] = [](const std::string& in, std::string& out, Daphne& d) {
    cmd_writeOFFSET_allAFE req;
    cmd_writeOFFSET_allAFE_response resp;
    if (!req.ParseFromString(in)) {
      out = serialize_error_with_success_field(resp, "Bad cmd_writeOFFSET_allAFE payload");
      return;
    }

    try {
      if (req.offsetvalue() > 4095) throw std::invalid_argument("offsetValue out of range (0..4095)");
      const uint32_t afe_board = req.afeblock();
      if (afe_board > 4) throw std::invalid_argument("afeBlock out of range (0..4)");
      const uint32_t afe_block = afe_definitions::AFE_board2PL_map.at(afe_board);
      for (uint32_t idx = 0; idx < 8; ++idx) {
        d.getDac()->setDacOffset(afe_block, idx, req.offsetvalue(), req.offsetgain(), false);
        d.setChOffsetDictValue(afe_board * 8 + idx, req.offsetvalue());
      }
      resp.set_success(true);
      resp.set_message("OK");
    } catch (const std::exception& e) {
      resp.set_success(false);
      resp.set_message(e.what());
    }
    resp.set_afeblock(req.afeblock());
    resp.set_offsetvalue(req.offsetvalue());
    resp.set_offsetgain(req.offsetgain());
    out = serialize_or_empty(resp);
  };

  handlers[daphne::MT2_WRITE_VBIAS_CONTROL_REQ] = [](const std::string& in, std::string& out, Daphne& d) {
    cmd_writeVbiasControl req;
    cmd_writeVbiasControl_response resp;
    if (!req.ParseFromString(in)) {
      out = serialize_error_with_success_field(resp, "Bad cmd_writeVbiasControl payload");
      return;
    }

    std::string msg;
    uint32_t rb = 0;
    const bool ok = writeBiasVoltageControl(req, d, msg, rb);
    resp.set_success(ok);
    resp.set_message(msg);
    resp.set_vbiascontrolvalue(rb);
    out = serialize_or_empty(resp);
  };

  handlers[daphne::MT2_DUMP_SPYBUFFER_REQ] = [](const std::string& in, std::string& out, Daphne& d) {
    DumpSpyBuffersRequest req;
    DumpSpyBuffersResponse resp;
    if (!req.ParseFromString(in)) {
      resp.set_success(false);
      resp.set_message("Bad DumpSpyBuffersRequest payload");
      out = serialize_or_empty(resp);
      return;
    }

    std::string msg;
    const bool ok = dumpSpybuffer(req, resp, d, msg);
    resp.set_success(ok);
    resp.set_message(msg);
    out = serialize_or_empty(resp);
  };

  handlers[daphne::MT2_ALIGN_AFE_REQ] = [](const std::string& in, std::string& out, Daphne& d) {
    cmd_alignAFEs req;
    cmd_alignAFEs_response resp;
    if (!req.ParseFromString(in)) {
      out = serialize_error_with_success_field(resp, "Bad cmd_alignAFEs payload");
      return;
    }

    std::string msg;
    const bool ok = alignAFE(req, resp, d, msg);
    resp.set_success(ok);
    resp.set_message(msg);
    out = serialize_or_empty(resp);
  };

  handlers[daphne::MT2_WRITE_AFE_FUNCTION_REQ] = [](const std::string& in, std::string& out, Daphne& d) {
    cmd_writeAFEFunction req;
    cmd_writeAFEFunction_response resp;
    if (!req.ParseFromString(in)) {
      out = serialize_error_with_success_field(resp, "Bad cmd_writeAFEFunction payload");
      return;
    }

    std::string msg;
    const bool ok = writeAFEFunction(req, resp, d, msg);
    resp.set_success(ok);
    resp.set_message(msg);
    out = serialize_or_empty(resp);
  };

  handlers[daphne::MT2_SET_AFE_RESET_REQ] = [](const std::string& in, std::string& out, Daphne& d) {
    cmd_setAFEReset req;
    cmd_setAFEReset_response resp;
    if (!req.ParseFromString(in)) {
      out = serialize_error_with_success_field(resp, "Bad cmd_setAFEReset payload");
      return;
    }

    std::string msg;
    const bool ok = setAFEReset(req, resp, d, msg);
    resp.set_success(ok);
    resp.set_message(msg);
    out = serialize_or_empty(resp);
  };

  handlers[daphne::MT2_DO_AFE_RESET_REQ] = [](const std::string& in, std::string& out, Daphne& d) {
    cmd_doAFEReset req;
    cmd_doAFEReset_response resp;
    if (!req.ParseFromString(in)) {
      out = serialize_error_with_success_field(resp, "Bad cmd_doAFEReset payload");
      return;
    }

    std::string msg;
    const bool ok = doAFEReset(req, resp, d, msg);
    resp.set_success(ok);
    resp.set_message(msg);
    out = serialize_or_empty(resp);
  };

  handlers[daphne::MT2_SET_AFE_POWERSTATE_REQ] = [](const std::string& in, std::string& out, Daphne& d) {
    cmd_setAFEPowerState req;
    cmd_setAFEPowerState_response resp;
    if (!req.ParseFromString(in)) {
      out = serialize_error_with_success_field(resp, "Bad cmd_setAFEPowerState payload");
      return;
    }

    std::string msg;
    const bool ok = setAFEPowerState(req, resp, d, msg);
    resp.set_success(ok);
    resp.set_message(msg);
    out = serialize_or_empty(resp);
  };

  handlers[daphne::MT2_DO_SOFTWARE_TRIGGER_REQ] = [](const std::string& in, std::string& out, Daphne& d) {
    cmd_doSoftwareTrigger req;
    cmd_doSoftwareTrigger_response resp;
    if (!req.ParseFromString(in)) {
      out = serialize_error_with_success_field(resp, "Bad cmd_doSoftwareTrigger payload");
      return;
    }

    std::string msg;
    const bool ok = doSoftwareTrigger(req, resp, d, msg);
    resp.set_success(ok);
    resp.set_message(msg);
    out = serialize_or_empty(resp);
  };

  handlers[daphne::MT2_SET_HDMEZZ_BLOCK_ENABLE_REQ] = [](const std::string& in, std::string& out, Daphne& d) {
    cmd_setHDMezzBlockEnable req;
    cmd_setHDMezzBlockEnable_response resp;
    if (!req.ParseFromString(in)) {
      out = serialize_error_with_success_field(resp, "Bad cmd_setHDMezzBlockEnable payload");
      return;
    }

    std::string msg;
    const bool ok = setHDMezzBlockEnable(req, resp, d, msg);
    resp.set_success(ok);
    resp.set_message(msg);
    out = serialize_or_empty(resp);
  };

  handlers[daphne::MT2_CONFIGURE_HDMEZZ_BLOCK_REQ] = [](const std::string& in, std::string& out, Daphne& d) {
    cmd_configureHDMezzBlock req;
    cmd_configureHDMezzBlock_response resp;
    if (!req.ParseFromString(in)) {
      out = serialize_error_with_success_field(resp, "Bad cmd_configureHDMezzBlock payload");
      return;
    }

    std::string msg;
    const bool ok = configureHDMezzBlock(req, resp, d, msg);
    resp.set_success(ok);
    resp.set_message(msg);
    out = serialize_or_empty(resp);
  };

  handlers[daphne::MT2_READ_HDMEZZ_BLOCK_CONFIG_REQ] = [](const std::string& in, std::string& out, Daphne& d) {
    cmd_readHDMezzBlockConfig req;
    cmd_readHDMezzBlockConfig_response resp;
    if (!req.ParseFromString(in)) {
      out = serialize_error_with_success_field(resp, "Bad cmd_readHDMezzBlockConfig payload");
      return;
    }

    std::string msg;
    const bool ok = readHDMezzBlockConfig(req, resp, d, msg);
    resp.set_success(ok);
    resp.set_message(msg);
    out = serialize_or_empty(resp);
  };

  handlers[daphne::MT2_SET_HDMEZZ_POWER_STATES_REQ] = [](const std::string& in, std::string& out, Daphne& d) {
    cmd_setHDMezzPowerStates req;
    cmd_setHDMezzPowerStates_response resp;
    if (!req.ParseFromString(in)) {
      out = serialize_error_with_success_field(resp, "Bad cmd_setHDMezzPowerStates payload");
      return;
    }

    std::string msg;
    const bool ok = setHDMezzPowerStates(req, resp, d, msg);
    resp.set_success(ok);
    resp.set_message(msg);
    out = serialize_or_empty(resp);
  };

  handlers[daphne::MT2_READ_HDMEZZ_STATUS_REQ] = [](const std::string& in, std::string& out, Daphne& d) {
    cmd_readHDMezzStatus req;
    cmd_readHDMezzStatus_response resp;
    if (!req.ParseFromString(in)) {
      out = serialize_error_with_success_field(resp, "Bad cmd_readHDMezzStatus payload");
      return;
    }

    std::string msg;
    const bool ok = readHDMezzStatus(req, resp, d, msg);
    resp.set_success(ok);
    resp.set_message(msg);
    out = serialize_or_empty(resp);
  };

  handlers[daphne::MT2_CLEAR_HDMEZZ_ALERT_FLAG_REQ] = [](const std::string& in, std::string& out, Daphne& d) {
    cmd_clearHDMezzAlertFlag req;
    cmd_clearHDMezzAlertFlag_response resp;
    if (!req.ParseFromString(in)) {
      out = serialize_error_with_success_field(resp, "Bad cmd_clearHDMezzAlertFlag payload");
      return;
    }

    std::string msg;
    const bool ok = clearHDMezzAlertFlag(req, resp, d, msg);
    resp.set_success(ok);
    resp.set_message(msg);
    out = serialize_or_empty(resp);
  };

  return handlers;
}

}  // namespace daphne_sc
