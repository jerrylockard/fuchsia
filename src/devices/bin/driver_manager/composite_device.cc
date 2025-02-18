// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "composite_device.h"

#include <lib/fidl/cpp/wire/arena.h>
#include <zircon/status.h>

#include <string_view>
#include <utility>

#include "binding_internal.h"
#include "coordinator.h"
#include "src/devices/lib/log/log.h"

namespace fdm = fuchsia_device_manager;

namespace {

fbl::Array<StrProperty> ConvertStringProperties(
    fidl::VectorView<fdm::wire::DeviceStrProperty> str_props) {
  fbl::Array<StrProperty> str_properties(new StrProperty[str_props.count()], str_props.count());
  for (size_t i = 0; i < str_props.count(); i++) {
    str_properties[i].key = str_props[i].key.get();
    if (str_props[i].value.is_int_value()) {
      str_properties[i].value.emplace<StrPropValueType::Integer>(str_props[i].value.int_value());
    } else if (str_props[i].value.is_str_value()) {
      str_properties[i].value.emplace<StrPropValueType::String>(
          std::string(str_props[i].value.str_value().get()));
    } else if (str_props[i].value.is_bool_value()) {
      str_properties[i].value.emplace<StrPropValueType::Bool>(str_props[i].value.bool_value());
    } else if (str_props[i].value.is_enum_value()) {
      str_properties[i].value.emplace<StrPropValueType::Enum>(
          std::string(str_props[i].value.enum_value().get()));
    }
  }

  return str_properties;
}

}  // namespace

// CompositeDevice methods

CompositeDevice::CompositeDevice(fbl::String name, fbl::Array<const zx_device_prop_t> properties,
                                 fbl::Array<const StrProperty> str_properties,
                                 uint32_t fragments_count, uint32_t primary_fragment_index,
                                 bool spawn_colocated,
                                 fbl::Array<std::unique_ptr<Metadata>> metadata,
                                 bool from_driver_index)
    : name_(std::move(name)),
      properties_(std::move(properties)),
      str_properties_(std::move(str_properties)),
      fragments_count_(fragments_count),
      primary_fragment_index_(primary_fragment_index),
      spawn_colocated_(spawn_colocated),
      metadata_(std::move(metadata)),
      from_driver_index_(from_driver_index) {}

CompositeDevice::~CompositeDevice() = default;

zx_status_t CompositeDevice::Create(std::string_view name,
                                    fdm::wire::CompositeDeviceDescriptor comp_desc,
                                    std::unique_ptr<CompositeDevice>* out) {
  fbl::String name_obj(name);
  fbl::Array<zx_device_prop_t> properties(new zx_device_prop_t[comp_desc.props.count() + 1],
                                          comp_desc.props.count() + 1);
  memcpy(properties.data(), comp_desc.props.data(),
         comp_desc.props.count() * sizeof(comp_desc.props.data()[0]));

  // Set a property unique to composite devices.
  properties[comp_desc.props.count()].id = BIND_COMPOSITE;
  properties[comp_desc.props.count()].value = 1;

  fbl::Array<std::unique_ptr<Metadata>> metadata(
      new std::unique_ptr<Metadata>[comp_desc.metadata.count()], comp_desc.metadata.count());

  fbl::Array<StrProperty> str_properties = ConvertStringProperties(comp_desc.str_props);

  for (size_t i = 0; i < comp_desc.metadata.count(); i++) {
    std::unique_ptr<Metadata> md;
    zx_status_t status = Metadata::Create(comp_desc.metadata[i].data.count(), &md);
    if (status != ZX_OK) {
      return status;
    }

    md->type = comp_desc.metadata[i].key;
    md->length = comp_desc.metadata[i].data.count();
    memcpy(md->Data(), comp_desc.metadata[i].data.data(), md->length);
    metadata[i] = std::move(md);
  }

  auto dev = std::make_unique<CompositeDevice>(
      std::move(name), std::move(properties), std::move(str_properties),
      comp_desc.fragments.count(), comp_desc.primary_fragment_index, comp_desc.spawn_colocated,
      std::move(metadata), false);
  for (uint32_t i = 0; i < comp_desc.fragments.count(); ++i) {
    const auto& fidl_fragment = comp_desc.fragments[i];
    size_t parts_count = fidl_fragment.parts.count();
    if (parts_count != 1) {
      LOGF(ERROR, "Composite fragments with multiple parts are deprecated. %s has %zd parts.",
           name.data(), parts_count);
      return ZX_ERR_INVALID_ARGS;
    }

    const auto& fidl_part = fidl_fragment.parts[0];
    size_t program_count = fidl_part.match_program.count();
    fbl::Array<zx_bind_inst_t> bind_rules(new zx_bind_inst_t[program_count], program_count);
    for (size_t j = 0; j < program_count; ++j) {
      bind_rules[j] = zx_bind_inst_t{
          .op = fidl_part.match_program[j].op,
          .arg = fidl_part.match_program[j].arg,
      };
    }
    std::string name(fidl_fragment.name.data(), fidl_fragment.name.size());
    auto fragment =
        std::make_unique<CompositeDeviceFragment>(dev.get(), name, i, std::move(bind_rules));
    dev->unbound_fragments_.push_back(std::move(fragment));
  }
  *out = std::move(dev);
  return ZX_OK;
}

std::unique_ptr<CompositeDevice> CompositeDevice::CreateFromDriverIndex(
    MatchedCompositeDriverInfo driver) {
  fbl::String name(driver.composite.name);
  auto dev = std::make_unique<CompositeDevice>(
      std::move(name), fbl::Array<const zx_device_prop_t>(), fbl::Array<const StrProperty>(),
      driver.composite.num_nodes, 0, driver.driver_info.colocate,
      fbl::Array<std::unique_ptr<Metadata>>(), true);

  for (uint32_t i = 0; i < driver.composite.num_nodes; ++i) {
    std::string name = driver.composite.node_names[i];
    auto fragment = std::make_unique<CompositeDeviceFragment>(dev.get(), std::string(name), i,
                                                              fbl::Array<const zx_bind_inst_t>());
    dev->unbound_fragments_.push_back(std::move(fragment));
  }
  dev->driver_index_driver_ = driver.driver_info;
  return dev;
}

bool CompositeDevice::IsFragmentMatch(const fbl::RefPtr<Device>& dev, size_t* index_out) const {
  if (from_driver_index_) {
    return false;
  }

  for (auto itr = bound_fragments_.begin(); itr != bound_fragments_.end(); ++itr) {
    if (itr->TryMatch(dev)) {
      LOGF(ERROR, "Ambiguous bind for composite device %p '%s': device 1 '%s', device 2 '%s'", this,
           name_.data(), itr->bound_device()->name().data(), dev->name().data());
      return false;
    }
  }
  for (auto itr = unbound_fragments_.begin(); itr != unbound_fragments_.end(); ++itr) {
    if (itr->TryMatch(dev)) {
      VLOGF(1, "Found a match for composite device %p '%s': device '%s'", this, name_.data(),
            dev->name().data());
      *index_out = itr->index();
      return true;
    }
  }
  VLOGF(1, "No match for composite device %p '%s': device '%s'", this, name_.data(),
        dev->name().data());
  return false;
}

zx_status_t CompositeDevice::TryMatchBindFragments(const fbl::RefPtr<Device>& dev) {
  size_t index;
  if (!IsFragmentMatch(dev, &index)) {
    return ZX_OK;
  }

  if (dev->name() == "sysmem-fidl" || dev->name() == "sysmem-banjo") {
    VLOGF(1, "Device '%s' matched fragment %zu of composite '%s'", dev->name().data(), index,
          name().data());
  } else {
    LOGF(INFO, "Device '%s' matched fragment %zu of composite '%s'", dev->name().data(), index,
         name().data());
  }
  auto status = BindFragment(index, dev);
  if (status != ZX_OK) {
    LOGF(ERROR, "Device '%s' failed to bind fragment %zu of composite '%s': %s", dev->name().data(),
         index, name().data(), zx_status_get_string(status));
  }
  return status;
}

zx_status_t CompositeDevice::BindFragment(size_t index, const fbl::RefPtr<Device>& dev) {
  // Find the fragment we're binding
  CompositeDeviceFragment* fragment = nullptr;
  for (auto& unbound_fragment : unbound_fragments_) {
    if (unbound_fragment.index() == index) {
      fragment = &unbound_fragment;
      break;
    }
  }

  if (!fragment) {
    LOGF(ERROR, "Attempted to bind bound fragment %zu in composite device %p", index, name_.data());
    return ZX_OK;
  }

  zx_status_t status = fragment->Bind(dev);
  if (status != ZX_OK) {
    return status;
  }
  bound_fragments_.push_back(unbound_fragments_.erase(*fragment));
  if (dev->has_outgoing_directory()) {
    status = TryAssemble();
    if (status != ZX_OK && status != ZX_ERR_SHOULD_WAIT) {
      LOGF(ERROR, "Failed to assemble composite device: %s", zx_status_get_string(status));
      return status;
    }
  }

  return ZX_OK;
}

zx_status_t CompositeDevice::TryAssemble() {
  ZX_ASSERT(device_ == nullptr);
  if (!unbound_fragments_.is_empty()) {
    return ZX_ERR_SHOULD_WAIT;
  }

  for (auto& fragment : bound_fragments_) {
    // Make sure the fragment driver has created its device
    // or that it didn't need one.
    if (fragment.fragment_device() == nullptr &&
        !fragment.bound_device()->has_outgoing_directory()) {
      return ZX_ERR_SHOULD_WAIT;
    }
  }

  // Find the driver_host to put everything in, nullptr means "a new driver_host".
  fbl::RefPtr<DriverHost> driver_host;
  if (spawn_colocated_) {
    for (auto& fragment : bound_fragments_) {
      if (fragment.index() == primary_fragment_index_) {
        driver_host = fragment.bound_device()->host();
      }
    }
  }

  Coordinator* coordinator = nullptr;

  fidl::Arena allocator;
  fidl::VectorView<fdm::wire::Fragment> fragments(allocator, bound_fragments_.size_slow());

  // Create all of the proxies for the fragment devices, in the same process
  for (auto& fragment : bound_fragments_) {
    const auto& fragment_dev = fragment.fragment_device();
    auto bound_dev = fragment.bound_device();
    coordinator = bound_dev->coordinator;

    // If the device we're bound to is proxied, we care about its proxy
    // rather than it, since that's the side that we communicate with.
    if (bound_dev->proxy()) {
      bound_dev = bound_dev->proxy();
    }

    // Check if we need to use the proxy.  If not, share a reference to
    // the instance of the fragment device.
    // We always use a proxy when there is an outgoing directory involved.
    if (bound_dev->host() == driver_host && !bound_dev->has_outgoing_directory()) {
      fragments[fragment.index()].name = fidl::StringView::FromExternal(fragment.name());
      fragments[fragment.index()].id = fragment_dev->local_id();
      continue;
    }

    // We need to create it.  Double check that we haven't ended up in a state
    // where the proxies would need to be in different processes.
    fbl::RefPtr<Device> proxy;
    zx_status_t status;
    if (bound_dev->has_outgoing_directory()) {
      VLOGF(1, "Preparing new proxy for %s", bound_dev->name().data());
      fbl::RefPtr<Device> new_proxy;
      status = coordinator->PrepareNewProxy(bound_dev, driver_host, &new_proxy);
      if (status != ZX_OK) {
        return status;
      }
      proxy = std::move(new_proxy);
    } else {
      if (driver_host != nullptr && fragment_dev != nullptr && fragment_dev->proxy() != nullptr &&
          fragment_dev->proxy()->host() != nullptr &&
          fragment_dev->proxy()->host() != driver_host) {
        LOGF(ERROR, "Cannot create composite device, device proxies are in different driver_hosts");
        return ZX_ERR_BAD_STATE;
      }

      VLOGF(1, "Preparing old proxy for %s", fragment_dev->name().data());
      status = coordinator->PrepareProxy(fragment_dev, driver_host);
      if (status != ZX_OK) {
        return status;
      }

      proxy = fragment_dev->proxy();
    }

    // If we hadn't picked a driver_host, use the one that was created just now.
    if (driver_host == nullptr) {
      driver_host = proxy->host();
      ZX_ASSERT(driver_host != nullptr);
    }

    // Stash the local ID after the proxy has been created
    fragments[fragment.index()].name = fidl::StringView::FromExternal(fragment.name());
    fragments[fragment.index()].id = proxy->local_id();
  }

  auto coordinator_endpoints = fidl::CreateEndpoints<fdm::Coordinator>();
  if (coordinator_endpoints.is_error()) {
    return coordinator_endpoints.error_value();
  }

  auto device_controller_endpoints = fidl::CreateEndpoints<fdm::DeviceController>();
  if (device_controller_endpoints.is_error()) {
    return device_controller_endpoints.error_value();
  }

  fbl::RefPtr<Device> new_device;
  auto status = Device::CreateComposite(
      coordinator, driver_host, *this, std::move(coordinator_endpoints->server),
      std::move(device_controller_endpoints->client), &new_device);
  if (status != ZX_OK) {
    return status;
  }
  coordinator->device_manager()->AddToDevices(new_device);

  // Create the composite device in the driver_host
  fdm::wire::CompositeDevice composite{fragments, fidl::StringView::FromExternal(name())};
  auto type = fdm::wire::DeviceType::WithComposite(allocator, composite);

  driver_host->controller()
      ->CreateDevice(std::move(coordinator_endpoints->client),
                     std::move(device_controller_endpoints->server), std::move(type),
                     new_device->local_id())
      .ThenExactlyOnce(
          [](fidl::WireUnownedResult<fdm::DriverHostController::CreateDevice>& result) {
            if (!result.ok()) {
              LOGF(ERROR, "Failed to create composite device: %s",
                   result.error().FormatDescription().c_str());
              return;
            }
            if (result.value().status != ZX_OK) {
              LOGF(ERROR, "Failed to create composite device: %s",
                   zx_status_get_string(result.value().status));
            }
          });

  device_ = std::move(new_device);
  device_->set_composite(this);

  // Add metadata
  for (size_t i = 0; i < metadata_.size(); i++) {
    // Making a copy of metadata, instead of transfering ownership, so that
    // metadata can be added again if device is recreated
    status = coordinator->AddMetadata(device_, metadata_[i]->type, metadata_[i]->Data(),
                                      metadata_[i]->length);
    if (status != ZX_OK) {
      LOGF(ERROR, "Failed to add metadata to device %p '%s': %s", device_.get(),
           device_->name().data(), zx_status_get_string(status));
      return status;
    }
  }

  status = device_->SignalReadyForBind();
  if (status != ZX_OK) {
    return status;
  }
  if (from_driver_index_) {
    zx_status_t status = coordinator->AttemptBind(driver_index_driver_, device_);
    if (status != ZX_OK) {
      LOGF(ERROR, "%s: Failed to bind composite driver '%s' to device '%s': %s", __func__,
           driver_index_driver_.name(), device_->name().data(), zx_status_get_string(status));
    }
    return status;
  }

  return ZX_OK;
}

void CompositeDevice::UnbindFragment(CompositeDeviceFragment* fragment) {
  // If the composite was fully instantiated, disassociate from it.  It will be
  // reinstantiated when this fragment is re-bound.
  if (device_ != nullptr) {
    Remove();
  }
  ZX_ASSERT(device_ == nullptr);
  ZX_ASSERT(fragment->composite() == this);
  unbound_fragments_.push_back(bound_fragments_.erase(*fragment));
}

void CompositeDevice::Remove() {
  if (device_ != nullptr) {
    device_->disassociate_from_composite();
    device_ = nullptr;
  }
}

// CompositeDeviceFragment methods

CompositeDeviceFragment::CompositeDeviceFragment(CompositeDevice* composite, std::string name,
                                                 uint32_t index,
                                                 fbl::Array<const zx_bind_inst_t> bind_rules)
    : composite_(composite), name_(name), index_(index), bind_rules_(std::move(bind_rules)) {}

CompositeDeviceFragment::~CompositeDeviceFragment() = default;

bool CompositeDeviceFragment::TryMatch(const fbl::RefPtr<Device>& dev) const {
  return internal::EvaluateBindProgram(dev, "composite_binder", bind_rules_, true /* autobind */);
}

zx_status_t CompositeDeviceFragment::Bind(const fbl::RefPtr<Device>& dev) {
  ZX_ASSERT(bound_device_ == nullptr);

  if (!dev->has_outgoing_directory()) {
    zx_status_t status = dev->coordinator->bind_driver_manager()->MatchAndBind(
        dev, dev->coordinator->fragment_driver(), false /* autobind */);
    if (status != ZX_OK) {
      return status;
    }
    bound_device_ = dev;
    dev->push_fragment(this);
  } else {
    bound_device_ = dev;
    dev->push_fragment(this);
    dev->flags |= DEV_CTX_BOUND;
  }

  return ZX_OK;
}

void CompositeDeviceFragment::Unbind() {
  ZX_ASSERT(bound_device_ != nullptr);
  composite_->UnbindFragment(this);
  // Drop our reference to the device added by the fragment driver
  fragment_device_ = nullptr;
  bound_device_->disassociate_from_composite();
  bound_device_ = nullptr;
}
