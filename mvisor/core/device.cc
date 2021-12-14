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

#include "device.h"
#include <cstring>
#include "logger.h"
#include "utilities.h"
#include "machine.h"
#include "device_manager.h"

Device::Device() {
  strcpy(name_, "unknown");
}

Device::~Device() {
}

void Device::Reset() {
  /* Don't add anything here */
}

/* Connect() is called when device manager initialize */
void Device::Connect() {
  MV_ASSERT(manager_);

  for (auto child : children_) {
    auto device = dynamic_cast<Device*>(child);
    if (device) {
      device->manager_ = manager_;
      device->Connect();
    }
  }

  connected_ = true;
  manager_->RegisterDevice(this);
  for (auto ir : io_resources_) {
    manager_->RegisterIoHandler(this, ir);
  }
  if (parent_ && manager_->machine()->debug()) {
    MV_LOG("%s <= %s", parent_->name(), name_);
  }
}

/* Disconnect() is called when device manager is being destroyed */
void Device::Disconnect() {
  if (!connected_) {
    return;
  }
  connected_ = false;
  for (auto child : children_) {
    auto device = dynamic_cast<Device*>(child);
    if (device) {
      device->Disconnect();
    }
  }

  for (auto &io_resource : io_resources_) {
    manager_->UnregisterIoHandler(this, io_resource);
  }
  manager_->UnregisterDevice(this);
}

void Device::AddIoResource(IoResourceType type, uint64_t base, uint64_t length, const char* name) {
  IoResource io_resource = {
    .type = type,
    .base = base,
    .length = length,
    .name = name
  };
  io_resources_.push_back(std::move(io_resource));
  if (connected_) {
    manager_->RegisterIoHandler(this, io_resource);
  }
}

void Device::RemoveIoResource(IoResourceType type, const char* name) {
  for (auto it = io_resources_.begin(); it != io_resources_.end(); it++) {
    if (it->type == type &&
        (it->name == name ||
          (name && it->name && strcmp(it->name, name) == 0)
        )
      ) {
      if (connected_) {
        manager_->UnregisterIoHandler(this, *it);
      }
      io_resources_.erase(it);
      break;
    }
  }
}

void Device::RemoveIoResource(IoResourceType type, uint64_t base) {
  for (auto it = io_resources_.begin(); it != io_resources_.end(); it++) {
    if (it->type == type && it->base == base) {
      if (connected_) {
        manager_->UnregisterIoHandler(this, *it);
      }
      io_resources_.erase(it);
      break;
    }
  }
}

void Device::Read(const IoResource& ir, uint64_t offset, uint8_t* data, uint32_t size) {
  MV_PANIC("not implemented %s base=0x%lx offset=0x%lx size=%d",
    name_, ir.base, offset, size);
}

void Device::Write(const IoResource& ir, uint64_t offset, uint8_t* data, uint32_t size) {
  MV_PANIC("not implemented %s base=0x%lx offset=0x%lx size=%d data=0x%lx",
    name_, ir.base, offset, size, *(uint64_t*)data);
}
