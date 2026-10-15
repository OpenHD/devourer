// Register-model regression: no USB hardware or RF output is simulated.
#include "jaguar1/RadioManagementModule.h"
#include "Transport.h"
#include <array>
#include <cstdio>
#include <memory>

class RamRegs : public devourer::ITransport {
public:
  std::array<uint8_t, 65536> mem{};
  int txagc_writes = 0;
  bool reject_txagc = false;
  bool is_usb() const override { return true; }
  uint8_t read8(uint16_t a) override {
    if (a==0x30) return 0xff; // blank EFUSE
    if (a==0x33) return 0x80; // read completed
    return mem[a];
  }
  uint16_t read16(uint16_t a) override { return read8(a) | (read8(a+1) << 8); }
  uint32_t read32(uint16_t a) override { return uint32_t(read16(a)) | (uint32_t(read16(a+2)) << 16); }
  bool write8(uint16_t a, uint8_t v) override { mem[a]=v; return true; }
  bool write16(uint16_t a, uint16_t v) override { write8(a,v); write8(a+1,v>>8); return true; }
  bool write32(uint16_t a, uint32_t v) override {
    if ((a>=0xc20 && a<=0xc4c) || (a>=0xe20 && a<=0xe4c)) {
      ++txagc_writes;
      if (reject_txagc) return false;
    }
    write16(a,v); write16(a+2,v>>16); return true;
  }
  bool write_bytes(uint16_t, const uint8_t*, size_t) override { return true; }
  bool tx_async(uint8_t,uint8_t*,size_t,unsigned) override { return true; }
  int tx_sync(uint8_t,uint8_t*,size_t n,int) override { return n; }
  void rx_loop(int,int,const std::function<void(const uint8_t*,int)>&,
               const std::function<bool()>&) override {}
};

int main() {
  auto regs=std::make_shared<RamRegs>();
  regs->write32(0xf0, 0x04411135);
  auto logger=std::make_shared<Logger>();
  logger->set_level(Logger::Level::Warn);
  devourer::DeviceConfig cfg;
  cfg.tuning.disable_iqk=true;
  RtlAdapter dev(regs,logger,cfg);
  auto ee=std::make_shared<EepromManager>(dev,logger,cfg);
  ee->numTotalRfPath=2;
  ee->version_id.ICType=CHIP_8812;
  RadioManagementModule radio(dev,ee,logger,cfg);
  radio.init_hw_mlme_ext({36,0,CHANNEL_WIDTH_20});
  for (auto a : {0xc2c,0xc30,0xe2c,0xe30}) regs->write32(a,0x12121212);
  regs->txagc_writes=0;
  radio.SetTxPowerOverride(34);
  radio.SetTxPowerOffsetSteps(0);
  radio.ApplyTxPower();
  std::printf("override34: A=%08x B=%08x writes=%d\n",regs->read32(0xc2c),regs->read32(0xe2c),regs->txagc_writes);
  radio.SetTxPowerOffsetSteps(-32);
  radio.ApplyTxPower();
  std::printf("offset-32: A=%08x B=%08x writes=%d\n",regs->read32(0xc2c),regs->read32(0xe2c),regs->txagc_writes);
  if (regs->read32(0xc2c)!=0x02020202 || regs->read32(0xe2c)!=0x02020202 ||
      radio.TxPowerWritesAttempted()==0 || radio.TxPowerWritesFailed()!=0) return 1;
  const auto previous_training=regs->read32(0xc54);
  for (auto a : {0xc2c,0xc30,0xe2c,0xe30}) regs->write32(a,0x12121212);
  regs->reject_txagc=true;
  radio.SetTxPowerOffsetSteps(0);
  radio.ApplyTxPower();
  // Reproduce the tester's signature: TXAGC remains at init, while the
  // independent training write moves. Failed control writes must be visible.
  if (regs->read32(0xc2c)!=0x12121212 || regs->read32(0xe2c)!=0x12121212 ||
      regs->read32(0xc54)==previous_training ||
      radio.TxPowerWritesAttempted()==0 ||
      radio.TxPowerWritesFailed()!=radio.TxPowerWritesAttempted()) return 2;
  regs->reject_txagc=false;
  radio.ApplyTxPower();
  if (regs->read32(0xc2c)!=0x22222222 || regs->read32(0xe2c)!=0x22222222 ||
      radio.TxPowerWritesFailed()!=0) return 3;
  // Also exercise EFUSE-derived gain, rather than leaving the flat test knob
  // as the only covered path. Compute the independent expected target first.
  radio.SetTxPowerOverride(-1);
  radio.SetTxPowerOffsetSteps(-8);
  const auto expectedA=radio.ComputeTxPowerIndex(0,0x80,0);
  const auto expectedB=radio.ComputeTxPowerIndex(1,0x80,0);
  radio.ApplyTxPower();
  if ((regs->read32(0xc2c)&0xff)!=expectedA ||
      (regs->read32(0xe2c)&0xff)!=expectedB ||
      radio.TxPowerWritesFailed()!=0) return 4;
  std::puts("PASS: relative/flat power writes, failed-write reporting, training isolation, recovery");
  return 0;
}
