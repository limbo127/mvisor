/* 
 * MVisor
 * Copyright (C) 2021 Terrence <terrence@tenclass.com>
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "ide_storage.h"
#include <cstring>
#include "logger.h"
#include "device_manager.h"
#include "ahci_port.h"
#include "ata_interval.h"


IdeStorageDevice::IdeStorageDevice() {
  bzero(&drive_info_, sizeof(drive_info_));

  ata_handlers_[0x00] = [=] () { // NOP
    MV_PANIC("nop");
  };
  
  ata_handlers_[0x08] = [=] () { // ATA_CMD_DEVICE_RESET
    regs_.error &= ~ATA_CB_ER_BBK;
    regs_.error = ATA_CB_ER_NDAM;
    regs_.status = 0; // ?
    Ata_ResetSignature();
  };
  
  ata_handlers_[0x2F] = [=] () { // READ_LOG
    AbortCommand();
  };
  
  ata_handlers_[0xEC] = [=] () { // ATA_CMD_IDENTIFY_DEVICE
    Ata_IdentifyDevice();
  };
  
  ata_handlers_[0xEF] = [=] () { // ATA_CMD_SET_FEATURES
    Ata_SetFeatures();
  };
}

IdeStorageDevice::~IdeStorageDevice() {
  
}

void IdeStorageDevice::StartCommand() {
  regs_.status = ATA_SR_DRDY;
  regs_.error = 0;
  io_.dma_status = 0;
  io_.nbytes = 0;

  auto handler = ata_handlers_[regs_.command];
  if (handler) {
    handler();
  } else {
    MV_PANIC("unknown command 0x%x", regs_.command);
  }
}

/* Set Error and end this command */
void IdeStorageDevice::AbortCommand() {
  regs_.status = ATA_SR_DRDY | ATA_SR_ERR;
  regs_.error = ATA_CB_ER_ABRT;
}

void IdeStorageDevice::BindPort(AhciPort* port) {
  port_ = port;
}

void IdeStorageDevice::Reset() {
  regs_.status = ATA_SR_DRDY;
  Ata_ResetSignature();
}

void IdeStorageDevice::Ata_ResetSignature() {
  regs_.device = ~0xF;
  regs_.count0 = 1;
  regs_.lba0 = 1;
  if (type_ == kIdeStorageTypeCdrom) {
    regs_.lba1 = 0x14;
    regs_.lba2 = 0xEB;
  } else {
    regs_.lba1 = 0;
    regs_.lba2 = 0;
  }
}

void IdeStorageDevice::Ata_IdentifyDevice() {
  if (type_ != kIdeStorageTypeCdrom) {
    MV_PANIC("Not implemented. Harddisk should override this.");
  } else {
    if (type_ == kIdeStorageTypeCdrom) {
      Ata_ResetSignature();
    }
    AbortCommand();
  }
}

void IdeStorageDevice::Ata_SetFeatures() {
  switch (regs_.feature0)
  {
  case 0x03: { // set transfer mode
    uint8_t value = regs_.count0 & 0b111;
    switch (regs_.count0 >> 3)
    {
    case 0: // PIO default
    case 1: // PIO
      MV_PANIC("not supported PIO mode");
      break;
    case 2: // Single world DMA
      MV_PANIC("not supported Single world DMA mode");
      break;
    case 4: // MDMA
      MV_PANIC("not supported MDMA mode");
      break;
    case 8: // UDMA
      MV_LOG("udma = %x", value);
      break;
    default:
      MV_PANIC("unknown trasfer mode 0x%x", regs_.count0);
      break;
    }
    break;
  }
  case 0xcc: /* reverting to power-on defaults enable */
  case 0x66: /* reverting to power-on defaults disable */
    break;
  default:
    MV_LOG("unknown set features 0x%x", regs_.feature0);
    AbortCommand();
    break;
  }
}

