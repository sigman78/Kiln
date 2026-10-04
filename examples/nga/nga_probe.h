// examples/nga/nga_probe.h — why gpu::create_device() found no device.
#pragma once

namespace kiln::nga {

/// Logs each Vulkan device with its version, its driver and what NoGraphicsAPI needs that it lacks.
/// `windowed` adds the swapchain requirements.
void log_device_support(bool windowed);

} // namespace kiln::nga
